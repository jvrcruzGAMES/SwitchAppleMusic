#pragma once
#include <switchmusic/client.hpp>
#include <atomic>
#include <functional>
#include <memory>
#include <cstdint>
namespace smusic {
using Cancel=std::function<bool()>;
class Cancelled:public std::runtime_error {public:Cancelled():std::runtime_error("Playback operation cancelled"){};};
// One connection, one bounded range cache; never writes audio to SD.
class HttpInput {
    struct Impl;std::unique_ptr<Impl> impl;
public:
    HttpInput(Config config,Cancel cancel={});~HttpInput();
    std::string text(const std::string& path,size_t limit=32*1024);
    void open(const std::string& path);
    int read(uint8_t* output,int count);
    int64_t seek(int64_t offset,int whence);
};
class AudioDecoder {
    struct Impl;std::unique_ptr<Impl> impl;
public:
    AudioDecoder(Config config,const std::string& manifest,Cancel cancel={});~AudioDecoder();
    int read(int16_t* pcm,int frames); // 48-kHz interleaved stereo
    void seek(double seconds);
    double duration() const;
};
}
