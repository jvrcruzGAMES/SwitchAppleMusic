#pragma once
#include <switchmusic/lyrics.hpp>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <algorithm>
#include <mutex>
#include <thread>
namespace smusic {
// Lazily fetched only when an overlay requests lyrics. IPC returns five lines,
// never the whole LRC; network work stays off the control/audio threads.
class LyricsWorker {
    std::mutex mutex;std::condition_variable wake;std::atomic<unsigned> epoch{0};
    bool stopped=false,pending=false,requested=false,ready=false;
    unsigned failures=0;bool retryable=false;
    std::chrono::steady_clock::time_point retryAt{};
    Config config;std::string track,failure;Json lines=Json::array();std::thread worker;
    void run(){for(;;){
        Config active;std::string id;unsigned generation;
        {std::unique_lock lock(mutex);wake.wait(lock,[&]{return stopped||pending;});if(stopped)return;
         active=config;id=track;generation=epoch;pending=false;}
        Json loaded=Json::array();std::string error;bool transient=false;
        try{
            auto bytes=apiBytes(active,"/v1/lyrics/"+encode(id),256*1024,[&,generation]{return epoch.load()!=generation;},35);
            auto response=Json::parse(bytes);loaded=response.value("lines",Json::array());
            if(!loaded.is_array()||loaded.size()>2048)throw std::runtime_error("Invalid lyric lines");
            long long previous=-1;
            for(auto& line:loaded){
                if(!line.is_object()||!line.contains("time_ms")||!line["time_ms"].is_number_integer()||!line.contains("text")||!line["text"].is_string())throw std::runtime_error("Invalid lyric line");
                auto timestamp=line["time_ms"].get<long long>();if(timestamp<previous)throw std::runtime_error("Unsorted lyric lines");previous=timestamp;
            }
            if(loaded.empty())error="No synchronized lyrics available";
        }catch(const TransportError& e){loaded=Json::array();error=e.what();transient=e.retryable;}
        catch(const std::exception& e){loaded=Json::array();error=e.what();}
        {std::lock_guard lock(mutex);if(epoch==generation){lines=std::move(loaded);failure=std::move(error);ready=true;retryable=transient;
            if(transient){failures=std::min(failures+1,5u);retryAt=std::chrono::steady_clock::now()+std::chrono::seconds(1u<<failures);}
            else failures=0;}}
    }}
public:
    ~LyricsWorker(){{std::lock_guard lock(mutex);stopped=true;++epoch;}wake.notify_all();if(worker.joinable())worker.join();}
    void prepare(const Config& value,const Json& song){std::lock_guard lock(mutex);
        auto id=song.value("id",std::string{});
        if(id==track&&value.backend==config.backend&&value.session==config.session)return;
        ++epoch;config=value;track=id;lines=Json::array();failure.clear();ready=requested=pending=retryable=false;failures=0;
    }
    Json snapshot(const std::string& id,double position){std::lock_guard lock(mutex);
        if(id.empty()||id!=track)return {{"track_id",id},{"ready",false}};
        // Retry only while the overlay is polling this track. A temporary
        // connection failure must not become a permanent cached lyric result.
        if(ready&&retryable&&std::chrono::steady_clock::now()>=retryAt){requested=ready=retryable=false;}
        if(!requested){requested=pending=true;
            try{if(!worker.joinable())worker=std::thread([this]{run();});wake.notify_one();}
            catch(...){pending=false;ready=true;failure="Cannot start lyric worker";}
        }
        if(!ready)return {{"track_id",id},{"ready",false}};
        return {{"track_id",id},{"ready",true},{"lines",lyricWindow(lines,position)},{"error",failure}};
    }
};
}
