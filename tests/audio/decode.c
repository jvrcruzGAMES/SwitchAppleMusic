/* Decoder validation and link-budget harness, not a production sysmodule. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/tx.h>

static int emit(AVFrame *frame, SwrContext *swr, FILE *output) {
    uint8_t pcm[4096 * 2 * sizeof(int16_t)];
    const int planar = av_sample_fmt_is_planar(frame->format);
    const int bytes = av_get_bytes_per_sample(frame->format);
    const int channels = frame->ch_layout.nb_channels;
    if (channels < 1 || channels > 8 || bytes < 1) return AVERROR(EINVAL);
    for (int offset = 0; offset < frame->nb_samples; offset += 512) {
        const uint8_t *input[8] = {0};
        int samples = FFMIN(512, frame->nb_samples - offset);
        for (int i = 0; i < (planar ? channels : 1); ++i)
            input[i] = frame->extended_data[i] + offset * bytes * (planar ? 1 : channels);
        uint8_t *destination = pcm;
        int count = swr_convert(swr, &destination, 4096, input, samples);
        if (count < 0) return count;
        if (count && fwrite(pcm, 4, count, output) != (size_t)count) return AVERROR(EIO);
    }
    return 0;
}

static int decode(const char *path) {
    AVFormatContext *format = avformat_alloc_context();
    AVCodecContext *decoder = NULL;
    AVPacket *packet = NULL;
    AVFrame *frame = NULL;
    SwrContext *swr = NULL;
    int result = 1, audio = -1, rc;
    int64_t remaining = INT64_MAX;
    if (!format) goto cleanup;
    format->probesize = 256 * 1024;
    format->max_analyze_duration = 2 * AV_TIME_BASE;
    format->max_index_size = 128 * 1024;
    format->max_streams = 8;
    if (avformat_open_input(&format, path, NULL, NULL) < 0) goto cleanup;
    if (avformat_find_stream_info(format, NULL) < 0) goto cleanup;
    for (unsigned i = 0; i < format->nb_streams; ++i)
        if (format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) { audio = i; break; }
    if (audio < 0) goto cleanup;
    AVCodecParameters *parameters = format->streams[audio]->codecpar;
    if (parameters->codec_id != AV_CODEC_ID_AAC && parameters->codec_id != AV_CODEC_ID_ALAC) goto cleanup;
    const AVCodec *codec = avcodec_find_decoder(parameters->codec_id);
    decoder = avcodec_alloc_context3(codec);
    if (!decoder || avcodec_parameters_to_context(decoder, parameters) < 0) goto cleanup;
    if (decoder->sample_rate < 8000 || decoder->sample_rate > 192000 ||
        decoder->ch_layout.nb_channels < 1 || decoder->ch_layout.nb_channels > 8) goto cleanup;
    decoder->thread_count = 1;
    decoder->max_samples = 8 * 32768;
    if (avcodec_open2(decoder, codec, NULL) < 0) goto cleanup;
    if (format->streams[audio]->duration != AV_NOPTS_VALUE)
        remaining = av_rescale_q(format->streams[audio]->duration,
            format->streams[audio]->time_base, (AVRational){1, decoder->sample_rate});
    packet = av_packet_alloc(); frame = av_frame_alloc();
    if (!packet || !frame) goto cleanup;
    while ((rc = av_read_frame(format, packet)) >= 0) {
        if (packet->size > 1024 * 1024) goto cleanup;
        if (packet->stream_index == audio) {
            if (avcodec_send_packet(decoder, packet) < 0) goto cleanup;
            while ((rc = avcodec_receive_frame(decoder, frame)) >= 0) {
                if (!swr) {
                    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
                    if (swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, 48000,
                        &frame->ch_layout, frame->format, frame->sample_rate, 0, NULL) < 0 || swr_init(swr) < 0) goto cleanup;
                }
                if (frame->nb_samples > remaining) frame->nb_samples = remaining;
                remaining -= frame->nb_samples;
                if (frame->nb_samples && emit(frame, swr, stdout) < 0) goto cleanup;
                av_frame_unref(frame);
            }
            if (rc != AVERROR(EAGAIN) && rc != AVERROR_EOF) goto cleanup;
        }
        av_packet_unref(packet);
    }
    if (rc != AVERROR_EOF) goto cleanup;
    if (avcodec_send_packet(decoder, NULL) < 0) goto cleanup;
    while (avcodec_receive_frame(decoder, frame) >= 0)
        if (swr && emit(frame, swr, stdout) < 0) goto cleanup;
    if (swr) {
        uint8_t pcm[4096 * 4]; uint8_t *out = pcm;
        while ((rc = swr_convert(swr, &out, 4096, NULL, 0)) > 0)
            if (fwrite(pcm, 4, rc, stdout) != (size_t)rc) goto cleanup;
        if (rc < 0) goto cleanup;
    }
    result = 0;
cleanup:
    swr_free(&swr); av_frame_free(&frame); av_packet_free(&packet);
    avcodec_free_context(&decoder); avformat_close_input(&format);
    return result;
}

int switchmusic_audio_probe(int argc, char **argv) {
    av_log_set_level(AV_LOG_ERROR);
    if (argc == 2 && !strcmp(argv[1], "--codecs")) {
        void *opaque = NULL; const AVCodec *codec;
        while ((codec = av_codec_iterate(&opaque)))
            if (av_codec_is_decoder(codec)) puts(codec->name);
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--transforms")) {
        const int sizes[] = {120, 128, 480, 512, 960, 1024, 1920, 2048, 4096};
        float scale = 1.0f;
        for (unsigned i = 0; i < sizeof(sizes)/sizeof(sizes[0]); ++i) {
            AVTXContext *ctx = NULL; av_tx_fn function;
            if (av_tx_init(&ctx, &function, AV_TX_FLOAT_MDCT, 1, sizes[i], &scale, 0) < 0) return 2;
            av_tx_uninit(&ctx);
        }
        AVTXContext *ctx = NULL; av_tx_fn function;
        if (av_tx_init(&ctx, &function, AV_TX_FLOAT_FFT, 0, 262144, NULL, 0) >= 0) return 3;
        puts("AAC transform sizes supported; oversized transforms rejected"); return 0;
    }
    return argc == 2 ? decode(argv[1]) : 1;
}
#ifndef SWITCHMUSIC_BUDGET_PROBE
int main(int argc, char **argv) { return switchmusic_audio_probe(argc, argv); }
#endif
