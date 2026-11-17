#include <switch.h>
#include <switchmusic/client.hpp>
#include <switchmusic/ipc.hpp>
#include <switchmusic/control_server.hpp>
#include <switchmusic/artwork_worker.hpp>
#include <switchmusic/audio.hpp>
#include <switchmusic/audio_output.hpp>
#include <switchmusic/playback_queue.hpp>
#include <curl/curl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <deque>
#include <fstream>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#ifndef SWITCHMUSIC_HEAP_MB
#define SWITCHMUSIC_HEAP_MB 8
#endif
using namespace smusic;
static bool controlPortRegistered=false;
extern "C" {
void __libnx_init_time(void);
u32 __nx_applet_type=AppletType_None;
u32 __nx_fs_num_sessions=1;
void __libnx_initheap() {
    alignas(0x1000) static u8 heap[SWITCHMUSIC_HEAP_MB*1024*1024];
    extern void *fake_heap_start,*fake_heap_end;
    fake_heap_start=heap;fake_heap_end=heap+sizeof(heap);
}
void __appInit() {
    if(R_FAILED(smInitialize())) diagAbortWithResult(MAKERESULT(Module_Libnx,LibnxError_InitFail_SM));
    if(R_SUCCEEDED(setsysInitialize())) { SetSysFirmwareVersion f{};
        if(R_SUCCEEDED(setsysGetFirmwareVersion(&f)))hosversionSet(MAKEHOSVERSION(f.major,f.minor,f.micro));setsysExit(); }
    if(R_FAILED(fsInitialize())) diagAbortWithResult(MAKERESULT(Module_Libnx,LibnxError_InitFail_FS));
    fsdevMountSdmc();timeInitialize();__libnx_init_time();
    SocketInitConfig sockets{};
    sockets.tcp_tx_buf_size=16*1024;sockets.tcp_rx_buf_size=128*1024;
    sockets.udp_tx_buf_size=4096;sockets.udp_rx_buf_size=4096;
    sockets.sb_efficiency=2;sockets.num_bsd_sessions=2;sockets.bsd_service_type=BsdServiceType_User;
    Result rc=socketInitialize(&sockets);if(R_FAILED(rc))diagAbortWithResult(rc);
}
void __appExit() {if(controlPortRegistered){smUnregisterService(smEncodeName(ControlServiceName));}romfsExit();socketExit();timeExit();fsdevUnmountAll();fsExit();smExit();}
}
class Player {
    ArtworkWorker artwork;
    Config config;std::mutex lock;std::deque<Json> commands;
    Json state={{"status","stopped"},{"position",0.0},{"duration",0.0},{"paused",false},{"shuffle",false},{"repeat","off"}};
    PlaybackQueue queue{uint32_t(armGetSystemTick())};std::string manifest;
    std::unique_ptr<AudioDecoder> decoder;std::unique_ptr<AudioOutput> output;
    std::atomic<bool> shuttingDown{false};
    std::atomic<uint64_t> revision{0};uint64_t operation=0;
    bool paused=false,draining=false,started=false;double resumePosition=0;
    std::chrono::steady_clock::time_point lastCredentials{};
    void update(Json values){std::lock_guard guard(lock);state.update(values);}
    bool cancelled(){
        if(shuttingDown.load()||revision.load()!=operation)return true;
        auto now=std::chrono::steady_clock::now();
        if(now-lastCredentials>std::chrono::seconds(1)){
            lastCredentials=now;try{auto current=Config::load();if(current.session!=config.session||current.backend!=config.backend)return true;}catch(...){return true;}
        }
        return false;
    }
    void stop(){if(output)output->reset();decoder.reset();manifest.clear();paused=draining=started=false;resumePosition=0;update({{"status","stopped"},{"position",0.0},{"paused",false},{"error",""}});}
    void syncConfig(){
        Config next;try{next=Config::load();}catch(...){}
        if(next.backend!=config.backend||next.username!=config.username||next.session!=config.session){
            artwork.clear();stop();queue.clear();config=std::move(next);
            update({{"status",config.session.empty()?"signed out":"stopped"},{"track",Json::object()},{"duration",0.0},{"error",""},{"http_status",0}});
        }
    }
    Json get(const std::string& path){HttpInput http(config,[this]{return cancelled();});return Json::parse(http.text(path,256*1024));}
    void openDecoder(){decoder=std::make_unique<AudioDecoder>(config,manifest,[this]{return cancelled();});if(decoder->duration()>0)resumePosition=std::min(resumePosition,decoder->duration());output->reset(resumePosition);started=false;if(resumePosition>0)decoder->seek(resumePosition);draining=false;}
    void load(){
        if(queue.empty()){stop();return;}
        output->reset();decoder.reset();manifest.clear();paused=draining=started=false;resumePosition=0;
        update({{"status","loading"},{"error",""},{"http_status",0},{"paused",false},{"queue_index",queue.index()},{"queue_size",queue.size()}});
        auto track=queue.current();auto resolved=get("/v1/resolve/"+encode(track.at("type"))+"/"+encode(track.at("id")));
        if(!resolved.contains("playback_path")||!resolved["playback_path"].is_string())throw std::runtime_error("Upgrade the backend for M3U8 playback");
        artwork.request(config,resolved);
        manifest=resolved["playback_path"];update({{"track",resolved},{"position",0.0},{"duration",resolved.value("duration_ms",0)/1000.0}});
        openDecoder();if(decoder->duration()>0)update({{"duration",decoder->duration()}});update({{"status","playing"}});
    }
    void advance(bool automatic=false){
        if(queue.advance([this](const std::string& path){return get(path);},automatic))load();else stop();
    }
    void act(const Json& j){
        if(config.session.empty()||j.value("session",std::string{})!=config.session)return;
        auto op=j.at("op").get<std::string>();if(j.contains("revision"))operation=j["revision"];
        if(op=="play"){queue.start(j.at("queue"),j.value("index",size_t(0)),j.value("next_path",std::string{}));load();}
        else if(op=="next")advance();
        else if(op=="previous"){if(queue.previous())load();}
        else if(op=="stop"){stop();queue.clear();}
        else if(op=="toggle"){
            if(manifest.empty()){
                if(queue.empty())return;load();return;
            }
            paused=!paused;if(!paused&&!decoder)openDecoder();
            if(started)output->pause(paused);update({{"paused",paused},{"status",paused?"paused":"playing"}});
        }
        else if(op=="seek"&&!manifest.empty()){
            double seconds=std::max(0.0,j.at("seconds").get<double>());if(decoder&&decoder->duration()>0)seconds=std::min(seconds,decoder->duration());
            output->reset(seconds);started=false;resumePosition=seconds;
            // Reopen after cancellation rather than trusting a partially read MOV packet.
            if(!decoder)openDecoder();else decoder->seek(seconds);draining=false;update({{"position",seconds}});
        }
        else if(op=="shuffle"){queue.setShuffle(j.at("enabled").get<bool>());update({{"shuffle",queue.shuffle()}});}
        else if(op=="repeat"){queue.setRepeat(j.at("mode").get<std::string>());update({{"repeat",queue.repeat()}});}
    }
    void interrupted(){resumePosition=output->position();output->reset(resumePosition);decoder.reset();started=draining=false;}
public:
    void shutdown(){shuttingDown=true;++revision;}
    Json request(const Json& j) {
        if(j.value("op",std::string{})=="artwork")return artwork.chunk(j);
        std::lock_guard guard(lock);
        if(j.value("op",std::string{})=="state")return state;
        if(commands.size()>=8)return error("Playback command queue full");
        auto op=j.value("op",std::string{});
        if(op=="play") {
            if(!j.contains("queue")||!j["queue"].is_array()||j["queue"].empty()||j["queue"].size()>128)return error("Queue must contain 1 to 128 songs");
            if(!j.contains("index")||!j["index"].is_number_unsigned()||j["index"].get<size_t>()>=j["queue"].size())return error("Invalid queue index");
            if(j.contains("next_path")&&(!j["next_path"].is_string()||(!j["next_path"].get<std::string>().empty()&&j["next_path"].get<std::string>().rfind("/v1/",0)!=0)))return error("Invalid continuation path");
            for(auto& t:j["queue"])if(!t.is_object()||!t.contains("id")||!t["id"].is_string()||!t.contains("type")||!t["type"].is_string())return error("Invalid queue item");
        } else if(op=="shuffle") {
            if(!j.contains("enabled")||!j["enabled"].is_boolean())return error("Shuffle enabled must be boolean");
        } else if(op=="repeat") {
            if(!j.contains("mode")||!j["mode"].is_string()||(j["mode"]!="off"&&j["mode"]!="single"&&j["mode"]!="queue"))return error("Repeat must be off, single or queue");
        } else if(op=="seek") {
            auto field="seconds";
            if(!j.contains(field)||!j[field].is_number()||!std::isfinite(j[field].get<double>()))return error("Numeric value required");
        } else if(op!="toggle"&&op!="next"&&op!="previous"&&op!="stop")return error("Unknown operation");
        auto pending=j;
        if(op!="shuffle"&&op!="repeat")pending["revision"]=++revision;
        commands.push_back(std::move(pending));return {{"accepted",true}};
    }
    void run(){
        try{
            auto lastConfig=std::chrono::steady_clock::time_point{};
            while(!shuttingDown.load()){
                Json next;{std::lock_guard guard(lock);if(!commands.empty()){next=std::move(commands.front());commands.pop_front();}}
                try{
                    auto now=std::chrono::steady_clock::now();
                    if(!next.is_null()||now-lastConfig>std::chrono::seconds(1)){syncConfig();lastConfig=now;}
                    if(!output&&!next.is_null())output=std::make_unique<AudioOutput>();
                    if(!next.is_null())act(next);
                    if(decoder&&!paused){
                        int slot=output->available();
                        if(!draining&&slot>=0){int count=decoder->read(output->data(slot),AudioOutput::Frames);if(count){output->append(slot,count);if(!started){output->pause(false);started=true;}}else draining=true;}
                        if(draining&&output->empty()){if(revision.load()!=operation)throw Cancelled();advance(true);}
                    }
                    if(output&&!manifest.empty())update({{"position",output->position()}});
                }catch(const Cancelled&){try{interrupted();}catch(...){output.reset();decoder.reset();started=draining=false;}}
                catch(const HttpError& e){decoder.reset();output.reset();manifest.clear();paused=started=draining=false;update({{"status","error"},{"error",e.what()},{"http_status",e.status}});}
                catch(const std::exception& e){decoder.reset();output.reset();manifest.clear();paused=started=draining=false;update({{"status","error"},{"error",e.what()}});}
                svcSleepThread(5000000);
            }
            stop();
        }catch(const std::exception& e){update({{"status","error"},{"error",e.what()}});}
    }
};
int main() {
    try {
        mkdir("sdmc:/config/switchmusic",0777);curl_global_init(CURL_GLOBAL_DEFAULT);
        if(R_FAILED(romfsMountFromFsdev("sdmc:/atmosphere/contents/420000000000534D/romfs.bin",0,"romfs")))throw std::runtime_error("Reinstall SwitchMusic: bundled certificate RomFS is missing");
        Player player;
        // SM-registered Horizon IPC service; sockets are only for backend HTTP.
        Handle port=INVALID_HANDLE;
        Result rc=createControlPort(&port);
        if(R_FAILED(rc))throw std::runtime_error("Cannot register SwitchMusic IPC port");
        controlPortRegistered=true;
        std::thread worker([&]{player.run();});
        try{
            rc=runControlServer(port,[&](std::string_view raw){
                try {
                    auto config=Config::load();
                    return dispatchControlMessage(raw,config.session,[&](const Json& request){return player.request(request);});
                }catch(const std::exception& e){return encodeControlReply(error(e.what()));}
            });
        }catch(...){rc=MAKERESULT(Module_Libnx,LibnxError_BadInput);}
        // Exit this module cleanly on a server failure; never turn a recoverable
        // control-channel error into a fatal system report or unwind a live worker.
        smUnregisterService(smEncodeName(ControlServiceName));controlPortRegistered=false;
        player.shutdown();worker.join();svcCloseHandle(port);
        char message[96];std::snprintf(message,sizeof(message),"IPC server stopped: 0x%08x",unsigned(R_VALUE(rc)));
        std::ofstream("sdmc:/config/switchmusic/fatal.log")<<message;
        return 1;
    }catch(const std::exception& e){std::ofstream("sdmc:/config/switchmusic/fatal.log")<<e.what();return 1;}
}
