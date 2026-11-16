#pragma once
#include <cstdint>
using u32=uint32_t;using u64=uint64_t;using Result=uint32_t;
#define R_FAILED(value) ((value)!=0)
enum PcmFormat{PcmFormat_Int16};
struct AudioOutBuffer{AudioOutBuffer* next;void* buffer;u64 buffer_size,data_size,data_offset;};
Result audoutInitialize();void audoutExit();u32 audoutGetSampleRate();u32 audoutGetChannelCount();PcmFormat audoutGetPcmFormat();
Result audoutGetReleasedAudioOutBuffer(AudioOutBuffer**,u32*);Result audoutAppendAudioOutBuffer(AudioOutBuffer*);
Result audoutStopAudioOut();Result audoutStartAudioOut();Result audoutFlushAudioOutBuffers(bool*);
Result audoutContainsAudioOutBuffer(AudioOutBuffer*,bool*);Result audoutGetAudioOutPlayedSampleCount(u64*);Result audoutSetAudioOutVolume(float);
