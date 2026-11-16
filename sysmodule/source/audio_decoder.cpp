#include <switchmusic/audio.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <exception>
#include <sstream>
#include <numeric>
#include <cstdio>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}
namespace smusic {
struct AudioDecoder::Impl {
    HttpInput input;Cancel cancel;std::exception_ptr ioError;
    AVFormatContext* format=nullptr;AVIOContext* io=nullptr;AVCodecContext* codec=nullptr;
    AVPacket* packet=nullptr;AVFrame* frame=nullptr;SwrContext* swr=nullptr;
    int audio=-1,offset=0,scratchOffset=0,scratchFrames=0;
    bool sentEnd=false,ended=false,drained=false;double length=0,target=0;
    int64_t remaining=INT64_MAX,discardOutput=0;
    std::array<int16_t,4096*2> scratch{};
    Impl(Config c,Cancel fn):input(std::move(c),fn),cancel(std::move(fn)){}
    ~Impl(){swr_free(&swr);av_frame_free(&frame);av_packet_free(&packet);avcodec_free_context(&codec);avformat_close_input(&format);if(io){av_freep(&io->buffer);avio_context_free(&io);}}
    void check(int rc){if(rc>=0)return;if(ioError)std::rethrow_exception(ioError);if(cancel&&cancel())throw Cancelled();char message[AV_ERROR_MAX_STRING_SIZE];av_strerror(rc,message,sizeof(message));throw std::runtime_error(std::string("Audio decoder: ")+message);}
    static int read(void* opaque,uint8_t* bytes,int count){auto& self=*static_cast<Impl*>(opaque);try{return self.input.read(bytes,count);}catch(...){self.ioError=std::current_exception();return AVERROR_EXIT;}}
    static int64_t seek(void* opaque,int64_t offset,int whence){auto& self=*static_cast<Impl*>(opaque);try{return self.input.seek(offset,whence);}catch(...){self.ioError=std::current_exception();return AVERROR_EXIT;}}
    static int interrupt(void* opaque){auto& self=*static_cast<Impl*>(opaque);try{return self.cancel&&self.cancel();}catch(...){return 1;}}
    void open(const std::string& manifest){
        const auto text=input.text(manifest);
        std::istringstream lines(text);std::string line,map,media;long long init=0,count=0,start=0;bool end=false;
        if(text.rfind("#EXTM3U\n",0)!=0&&text.rfind("#EXTM3U\r\n",0)!=0)throw std::runtime_error("Invalid backend M3U8");
        while(std::getline(lines,line)){
            if(!line.empty()&&line.back()=='\r')line.pop_back();
            if(line.rfind("#EXT-X-KEY:",0)==0)throw std::runtime_error("Backend returned encrypted audio");
            if(line.rfind("#EXT-X-MAP:URI=\"",0)==0){auto close=line.find('"',16);if(close==std::string::npos)throw std::runtime_error("Invalid HLS initialization map");map=line.substr(16,close-16);auto range=line.find("BYTERANGE=\"");long long zero=-1;if(range==std::string::npos||sscanf(line.c_str()+range,"BYTERANGE=\"%lld@%lld\"",&init,&zero)!=2||zero!=0)throw std::runtime_error("Invalid HLS initialization range");}
            if(line.rfind("#EXT-X-BYTERANGE:",0)==0&&sscanf(line.c_str(),"#EXT-X-BYTERANGE:%lld@%lld",&count,&start)!=2)throw std::runtime_error("Invalid HLS media range");
            if(!line.empty()&&line[0]!='#'){if(!media.empty())throw std::runtime_error("Expected single-file backend VOD");media=line;}
            if(line=="#EXT-X-ENDLIST")end=true;
        }
        if(!end||media.rfind("/v1/stream/",0)!=0||media!=map||init<=0||start!=init||count<=0||init>INT64_MAX-count)throw std::runtime_error("Unsupported backend VOD layout");
        input.open(media,init+count);if(input.seek(0,AVSEEK_SIZE)!=init+count)throw std::runtime_error("HLS media length changed");
        av_log_set_level(AV_LOG_ERROR);
        auto* buffer=static_cast<unsigned char*>(av_malloc(32*1024));if(!buffer)throw std::bad_alloc();
        io=avio_alloc_context(buffer,32*1024,0,this,read,nullptr,seek);if(!io){av_free(buffer);throw std::bad_alloc();}
        io->seekable=AVIO_SEEKABLE_NORMAL;
        format=avformat_alloc_context();if(!format)throw std::bad_alloc();
        format->pb=io;format->flags|=AVFMT_FLAG_CUSTOM_IO;format->interrupt_callback={interrupt,this};
        format->probesize=256*1024;format->max_analyze_duration=2*AV_TIME_BASE;format->max_index_size=128*1024;format->max_streams=8;
        check(avformat_open_input(&format,nullptr,av_find_input_format("mov"),nullptr));check(avformat_find_stream_info(format,nullptr));
        for(unsigned i=0;i<format->nb_streams;++i)if(format->streams[i]->codecpar->codec_type==AVMEDIA_TYPE_AUDIO){audio=int(i);break;}
        if(audio<0)throw std::runtime_error("No audio track");auto* stream=format->streams[audio];auto* parameters=stream->codecpar;
        if(parameters->codec_id!=AV_CODEC_ID_AAC&&parameters->codec_id!=AV_CODEC_ID_ALAC)throw std::runtime_error("Only AAC and ALAC are supported");
        codec=avcodec_alloc_context3(avcodec_find_decoder(parameters->codec_id));if(!codec)throw std::bad_alloc();
        check(avcodec_parameters_to_context(codec,parameters));
        if(codec->sample_rate<8000||codec->sample_rate>192000||codec->ch_layout.nb_channels<1||codec->ch_layout.nb_channels>8)throw std::runtime_error("Unsupported audio layout or rate");
        codec->pkt_timebase=stream->time_base;codec->thread_count=1;check(avcodec_open2(codec,codec->codec,nullptr));
        length=stream->duration!=AV_NOPTS_VALUE?stream->duration*av_q2d(stream->time_base):format->duration!=AV_NOPTS_VALUE?double(format->duration)/AV_TIME_BASE:0;
        if(length>0)remaining=int64_t(std::llround(length*codec->sample_rate));
        packet=av_packet_alloc();frame=av_frame_alloc();if(!packet||!frame)throw std::bad_alloc();
    }
    bool nextFrame(){
        av_frame_unref(frame);offset=0;
        while(true){
            if(cancel&&cancel())throw Cancelled();
            auto rc=avcodec_receive_frame(codec,frame);
            if(rc>=0){
                if(frame->sample_rate!=codec->sample_rate||frame->ch_layout.nb_channels!=codec->ch_layout.nb_channels)throw std::runtime_error("Audio layout changed during playback");
                if(!swr){AVChannelLayout stereo=AV_CHANNEL_LAYOUT_STEREO;check(swr_alloc_set_opts2(&swr,&stereo,AV_SAMPLE_FMT_S16,48000,&frame->ch_layout,AVSampleFormat(frame->format),frame->sample_rate,0,nullptr));check(swr_init(swr));}
                if(target>0){
                    if(frame->best_effort_timestamp==AV_NOPTS_VALUE)throw std::runtime_error("Seek requires audio timestamps");
                    auto* stream=format->streams[audio];auto origin=stream->start_time==AV_NOPTS_VALUE?0:stream->start_time;
                    auto sample=av_rescale_q(frame->best_effort_timestamp-origin,stream->time_base,AVRational{1,codec->sample_rate});
                    // Anchor rational SRC phase to the track's original sample grid.
                    int quantum=codec->sample_rate/std::gcd(codec->sample_rate,48000);
                    offset=int((quantum-(sample%quantum+quantum)%quantum)%quantum);
                    auto first=sample+offset;
                    discardOutput=std::max<int64_t>(0,std::llround(target*48000)-av_rescale_q(first,AVRational{1,codec->sample_rate},AVRational{1,48000}));
                    remaining=length>0?int64_t(std::llround(length*codec->sample_rate))-first:INT64_MAX;target=0;
                }
                return true;
            }
            if(rc==AVERROR_EOF)return false;if(rc!=AVERROR(EAGAIN))check(rc);
            if(sentEnd)throw std::runtime_error("Decoder failed to drain");
            do{av_packet_unref(packet);rc=av_read_frame(format,packet);if(rc<0)break;}while(packet->stream_index!=audio);
            if(rc==AVERROR_EOF){check(avcodec_send_packet(codec,nullptr));sentEnd=true;}
            else{check(rc);check(avcodec_send_packet(codec,packet));av_packet_unref(packet);}
        }
    }
    bool convert(){
        scratchFrames=scratchOffset=0;
        while(!scratchFrames){
            if(ended){if(drained||!swr)return false;uint8_t* destination=reinterpret_cast<uint8_t*>(scratch.data());auto n=swr_convert(swr,&destination,4096,nullptr,0);check(n);scratchFrames=n;if(!n){drained=true;return false;}return true;}
            if(!frame->nb_samples||offset>=frame->nb_samples){if(!nextFrame()){ended=true;continue;}}
            int n=int(std::min<int64_t>({512,frame->nb_samples-offset,remaining}));if(n<=0){ended=true;continue;}
            const bool planar=av_sample_fmt_is_planar(AVSampleFormat(frame->format));int bytes=av_get_bytes_per_sample(AVSampleFormat(frame->format));
            const uint8_t* source[8]{};for(int i=0;i<(planar?frame->ch_layout.nb_channels:1);++i)source[i]=frame->extended_data[i]+offset*bytes*(planar?1:frame->ch_layout.nb_channels);
            uint8_t* destination=reinterpret_cast<uint8_t*>(scratch.data());auto converted=swr_convert(swr,&destination,4096,source,n);check(converted);offset+=n;remaining-=n;scratchFrames=converted;
        }
        return true;
    }
};
AudioDecoder::AudioDecoder(Config config,const std::string& manifest,Cancel cancel):impl(new Impl(std::move(config),std::move(cancel))){impl->open(manifest);}
AudioDecoder::~AudioDecoder()=default;
int AudioDecoder::read(int16_t* pcm,int frames){
    int written=0;while(written<frames){if(impl->scratchOffset>=impl->scratchFrames&&!impl->convert())break;if(impl->discardOutput>0){auto skip=std::min<int64_t>(impl->discardOutput,impl->scratchFrames-impl->scratchOffset);impl->discardOutput-=skip;impl->scratchOffset+=skip;continue;}int n=std::min(frames-written,impl->scratchFrames-impl->scratchOffset);memcpy(pcm+written*2,impl->scratch.data()+impl->scratchOffset*2,n*4);impl->scratchOffset+=n;written+=n;}return written;
}
void AudioDecoder::seek(double seconds){
    if(!std::isfinite(seconds))throw std::runtime_error("Invalid seek time");seconds=std::max(0.0,seconds);if(impl->length>0)seconds=std::min(seconds,impl->length);
    auto* stream=impl->format->streams[impl->audio];auto origin=stream->start_time==AV_NOPTS_VALUE?0:stream->start_time;
    // AAC needs preceding transform history; decode and discard preroll before the target.
    double seekPoint=std::max(0.0,seconds-1.0);
    auto timestamp=int64_t(seekPoint/av_q2d(stream->time_base))+origin;
    impl->ioError=nullptr;impl->check(avformat_seek_file(impl->format,impl->audio,INT64_MIN,timestamp,timestamp,0));
    av_frame_unref(impl->frame);av_packet_unref(impl->packet);swr_free(&impl->swr);
    // Recreate decoder state too, including AAC's noise-synthesis seed.
    avcodec_free_context(&impl->codec);auto* parameters=stream->codecpar;
    impl->codec=avcodec_alloc_context3(avcodec_find_decoder(parameters->codec_id));if(!impl->codec)throw std::bad_alloc();
    impl->check(avcodec_parameters_to_context(impl->codec,parameters));impl->codec->pkt_timebase=stream->time_base;
    impl->codec->thread_count=1;impl->check(avcodec_open2(impl->codec,impl->codec->codec,nullptr));
    impl->offset=impl->scratchOffset=impl->scratchFrames=0;impl->ended=impl->drained=impl->sentEnd=false;impl->target=seconds;impl->discardOutput=0;
    impl->remaining=impl->length>0?int64_t(std::llround((impl->length-seconds)*impl->codec->sample_rate)):INT64_MAX;
}
double AudioDecoder::duration()const{return impl->length;}
}
