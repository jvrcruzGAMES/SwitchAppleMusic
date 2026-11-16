#include <switchmusic/audio_output.hpp>
#include <deque>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>
static std::deque<AudioOutBuffer*> queued,released;
static u64 played=0;static bool resetOnStart=false,running=false;static float volume=0;
Result audoutInitialize(){queued.clear();released.clear();played=0;return 0;}
void audoutExit(){assert(queued.empty());}
u32 audoutGetSampleRate(){return 48000;}u32 audoutGetChannelCount(){return 2;}PcmFormat audoutGetPcmFormat(){return PcmFormat_Int16;}
Result audoutGetReleasedAudioOutBuffer(AudioOutBuffer** head,u32* count){*head=nullptr;*count=released.size();AudioOutBuffer* previous=nullptr;for(auto* b:released){if(previous)previous->next=b;else *head=b;previous=b;}if(previous)previous->next=nullptr;released.clear();return 0;}
Result audoutAppendAudioOutBuffer(AudioOutBuffer* b){assert(std::find(queued.begin(),queued.end(),b)==queued.end());assert(uintptr_t(b->buffer)%4096==0);assert(b->data_size<=b->buffer_size);queued.push_back(b);return 0;}
Result audoutStopAudioOut(){running=false;return 0;}Result audoutStartAudioOut(){running=true;if(resetOnStart)played=0;return 0;}
Result audoutFlushAudioOutBuffers(bool* flushed){*flushed=!queued.empty();while(!queued.empty()){released.push_back(queued.front());queued.pop_front();}return 0;}
Result audoutContainsAudioOutBuffer(AudioOutBuffer* b,bool* contains){*contains=std::find(queued.begin(),queued.end(),b)!=queued.end();return 0;}
Result audoutGetAudioOutPlayedSampleCount(u64* count){*count=played;return 0;}Result audoutSetAudioOutVolume(float v){volume=v;return 0;}
static void playOne(){assert(running&&!queued.empty());auto* b=queued.front();queued.pop_front();played+=b->data_size/4;released.push_back(b);}
int main(){
    for(bool policy:{false,true}){
        resetOnStart=policy;smusic::AudioOutput output;
        for(int i=0;i<4;++i){int slot=output.available();assert(slot>=0);output.append(slot,4096);}assert(output.available()==-1);
        output.pause(false);playOne();assert(std::abs(output.position()-4096.0/48000)<1e-9);
        int free=output.available();assert(free>=0);output.append(free,4096);assert(output.available()==-1);
        output.pause(true);auto paused=output.position();played+=100;assert(output.position()==paused);
        output.pause(false);playOne();assert(std::abs(output.position()-8192.0/48000)<1e-9);
        output.volume(30);assert(std::abs(volume-.3f)<.0001f);
        output.reset(10);assert(output.empty());assert(output.position()==10);
        output.append(output.available(),1000);output.pause(false);playOne();assert(output.empty());assert(std::abs(output.position()-(10+1000.0/48000))<1e-9);
    }
    std::cout<<"Audio buffer ownership, pause/resume counters, flush/seek and EOF draining passed\n";
}
