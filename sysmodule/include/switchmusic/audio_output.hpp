#pragma once
#include <switch.h>
#include <array>
#include <algorithm>
#include <stdexcept>
namespace smusic {
// Independently implemented audout buffer recycling, following sys-tune's design.
class AudioOutput {
public:
    static constexpr int Frames=4096,Count=4;
private:
    alignas(0x1000) int16_t pcm[Count][Frames*2]{};
    AudioOutBuffer buffers[Count]{};bool queued[Count]{};
    uint64_t submitted=0,base=0,playedBefore=0;double origin=0;bool initialized=false,running=false;
    void check(Result rc){if(R_FAILED(rc))throw std::runtime_error("Horizon audio output failed: "+std::to_string(rc));}
public:
    AudioOutput(){check(audoutInitialize());try{if(audoutGetSampleRate()!=48000||audoutGetChannelCount()!=2||audoutGetPcmFormat()!=PcmFormat_Int16)throw std::runtime_error("Unsupported Horizon audio output");for(int i=0;i<Count;++i){buffers[i].buffer=pcm[i];buffers[i].buffer_size=sizeof(pcm[i]);}reset();initialized=true;}catch(...){audoutExit();throw;}}
    ~AudioOutput(){if(initialized){audoutStopAudioOut();bool flushed;audoutFlushAudioOutBuffers(&flushed);audoutExit();}}
    void reclaim(){AudioOutBuffer* head=nullptr;u32 released=0;check(audoutGetReleasedAudioOutBuffer(&head,&released));for(auto* b=head;b;b=b->next)for(int i=0;i<Count;++i)if(b==&buffers[i])queued[i]=false;}
    int available(){reclaim();for(int i=0;i<Count;++i)if(!queued[i])return i;return -1;}
    int16_t* data(int index){return pcm[index];}
    void append(int index,int frames){buffers[index].data_size=frames*4;buffers[index].next=nullptr;check(audoutAppendAudioOutBuffer(&buffers[index]));queued[index]=true;submitted+=frames;}
    bool empty(){reclaim();for(bool used:queued)if(used)return false;return true;}
    void pause(bool value){
        if(value){if(running){u64 played=base;check(audoutGetAudioOutPlayedSampleCount(&played));playedBefore=std::min<uint64_t>(submitted,playedBefore+(played>=base?played-base:0));}check(audoutStopAudioOut());running=false;}
        else{check(audoutStartAudioOut());check(audoutGetAudioOutPlayedSampleCount(&base));running=true;}
    }
    void volume(double percent){check(audoutSetAudioOutVolume(float(percent/100)));}
    void reset(double seconds=0){check(audoutStopAudioOut());bool flushed=false;check(audoutFlushAudioOutBuffers(&flushed));reclaim();for(int i=0;i<Count;++i){bool contains=false;check(audoutContainsAudioOutBuffer(&buffers[i],&contains));if(contains)throw std::runtime_error("Audio buffer did not flush");queued[i]=false;}submitted=playedBefore=0;running=false;origin=seconds;check(audoutGetAudioOutPlayedSampleCount(&base));}
    double position(){u64 played=base;check(audoutGetAudioOutPlayedSampleCount(&played));return origin+double(std::min<uint64_t>(submitted,playedBefore+(running&&played>=base?played-base:0)))/48000;}
};
}
