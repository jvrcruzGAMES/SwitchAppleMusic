// Cross-linked budget probe: retain decoder, HOS audio and real HTTP/control code.
// This executable is a measurement fixture, not an installable music player.
#include <switch.h>
#include <switchmusic/client.hpp>
#include <sys/select.h>
#include <curl/curl.h>
extern "C" int switchmusic_audio_probe(int argc,char **argv);
extern "C" {
u32 __nx_applet_type=AppletType_None;
void __libnx_initheap() {
    alignas(4096) static u8 heap[8 * 1024 * 1024];
    extern void *fake_heap_start,*fake_heap_end;
    fake_heap_start=heap;fake_heap_end=heap+sizeof(heap);
}
}
int main(int argc,char **argv) {
    // argc branches prevent garbage collection of services used by the real player.
    if(argc>2) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        socketInitializeDefault();
        auto config=smusic::Config::load();
        smusic::api(config,"/health");smusic::control(config,{{"op","state"}});
        AudioRendererConfig ar{};ar.output_rate=AudioRendererOutputRate_48kHz;
        ar.num_voices=2;ar.num_sinks=1;ar.num_mix_objs=1;ar.num_mix_buffers=2;
        audrenInitialize(&ar);AudioDriver driver{};audrvCreate(&driver,&ar,2);
        void *pcm=aligned_alloc(4096,256*1024);
        int pool=audrvMemPoolAdd(&driver,pcm,256*1024);audrvMemPoolAttach(&driver,pool);
        const u8 channels[]={0,1};audrvDeviceSinkAdd(&driver,AUDREN_DEFAULT_DEVICE_NAME,2,channels);
        audrvVoiceInit(&driver,0,2,PcmFormat_Int16,48000);
        audrvVoiceSetDestinationMix(&driver,0,AUDREN_FINAL_MIX_ID);
        audrvVoiceStart(&driver,0);audrvUpdate(&driver);audrenStartAudioRenderer();
        audrvClose(&driver);audrenExit();free(pcm);socketExit();
    }
    return switchmusic_audio_probe(argc,argv);
}
