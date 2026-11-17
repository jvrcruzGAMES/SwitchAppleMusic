#include <switchmusic/audio_output.hpp>
#include <deque>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>
static std::deque<AudioOutBuffer*> queued,released;
static unsigned opens=0,closes=0,waits=0;static bool stuck=false,delayed=false;
static u64 played=0;static bool resetOnStart=false,running=false;static float volume=0;
Result audoutInitialize(){++opens;queued.clear();released.clear();played=0;return 0;}
void audoutExit(){++closes;queued.clear();released.clear();stuck=delayed=false;}
u32 audoutGetSampleRate(){return 48000;}u32 audoutGetChannelCount(){return 2;}PcmFormat audoutGetPcmFormat(){return PcmFormat_Int16;}
Result audoutGetReleasedAudioOutBuffer(AudioOutBuffer** head,u32* count){*head=nullptr;*count=0;if(!released.empty()){*head=released.front();released.pop_front();*count=1;}return 0;}
Result svcSleepThread(int64_t){++waits;if(delayed){while(!queued.empty()){released.push_back(queued.front());queued.pop_front();}delayed=false;}return 0;}

Result audoutAppendAudioOutBuffer(AudioOutBuffer* b){assert(std::find(queued.begin(),queued.end(),b)==queued.end());assert(uintptr_t(b->buffer)%4096==0);assert(b->data_size<=b->buffer_size);queued.push_back(b);return 0;}
Result audoutStopAudioOut(){running=false;return 0;}Result audoutStartAudioOut(){running=true;if(resetOnStart)played=0;return 0;}
Result audoutFlushAudioOutBuffers(bool* flushed){*flushed=!queued.empty();if(stuck||delayed)return 0;while(!queued.empty()){released.push_back(queued.front());queued.pop_front();}return 0;}
Result audoutContainsAudioOutBuffer(AudioOutBuffer* b,bool* contains){*contains=std::find(queued.begin(),queued.end(),b)!=queued.end()||std::find(released.begin(),released.end(),b)!=released.end();return 0;}
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
        assert(std::abs(volume-.7f)<.0001f);
        output.reset(10);assert(output.empty());assert(output.position()==10);
        output.append(output.available(),1000);output.pause(false);playOne();assert(output.empty());assert(std::abs(output.position()-(10+1000.0/48000))<1e-9);
    }
    {
        smusic::AudioOutput output;
        for(int i=0;i<4;++i)output.append(output.available(),4096);
        auto previousOpens=opens;delayed=true;output.reset();
        assert(waits>0&&opens==previousOpens&&output.empty());
        for(int i=0;i<4;++i)output.append(output.available(),4096);
        stuck=true;output.reset(25);
        assert(opens==previousOpens+1&&closes>0&&output.empty()&&output.position()==25);
        assert(std::abs(volume-.7f)<.0001f);
        output.append(output.available(),1000);output.pause(false);playOne();
        assert(output.empty()&&output.position()>25);
        for(int i=0;i<10;++i){output.append(output.available(),4096);output.reset();assert(output.empty());}
    }
    std::cout<<"Audio buffer ownership, pause/resume counters, single-tag draining, delayed flush, stuck-output recovery and replay after stop passed\n";
}
