#include <switch.h>
#include <switchmusic/client.hpp>
#include <switchmusic/ipc.hpp>
#include <switchmusic/control_server.hpp>
#include <switchmusic/audio.hpp>
#include <switchmusic/audio_output.hpp>
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
void __appExit() {if(controlPortRegistered){Handle unused;svcManageNamedPort(&unused,ControlServiceName,0);}romfsExit();socketExit();timeExit();fsdevUnmountAll();fsExit();smExit();}
}
class Player {
    Config config;std::mutex lock;std::deque<Json> commands;
    Json state={{"status","stopped"},{"position",0.0},{"duration",0.0},{"volume",70.0},{"paused",false}};
    Json queue=Json::array();size_t index=0;std::string nextPath,manifest;
    std::unique_ptr<AudioDecoder> decoder;std::unique_ptr<AudioOutput> output;
    std::atomic<uint64_t> revision{0};uint64_t operation=0;
    bool paused=false,draining=false,started=false;double volume=70,resumePosition=0;
    std::chrono::steady_clock::time_point lastCredentials{};
    void update(Json values){std::lock_guard guard(lock);state.update(values);}
    bool cancelled(){
        if(revision.load()!=operation)return true;
        auto now=std::chrono::steady_clock::now();
        if(now-lastCredentials>std::chrono::seconds(1)){
            lastCredentials=now;try{auto current=Config::load();if(current.session!=config.session||current.backend!=config.backend)return true;}catch(...){return true;}
        }
        return false;
    }
    void stop(){output->reset();decoder.reset();manifest.clear();paused=draining=started=false;resumePosition=0;update({{"status","stopped"},{"position",0.0},{"paused",false}});}
    void syncConfig(){
        Config next;try{next=Config::load();}catch(...){}
        if(next.backend!=config.backend||next.username!=config.username||next.session!=config.session){
            stop();queue=Json::array();index=0;nextPath.clear();config=std::move(next);
            update({{"status",config.session.empty()?"signed out":"stopped"},{"track",Json::object()},{"duration",0.0},{"error",""},{"http_status",0}});
        }
    }
    Json get(const std::string& path){HttpInput http(config,[this]{return cancelled();});return Json::parse(http.text(path,256*1024));}
    void openDecoder(){decoder=std::make_unique<AudioDecoder>(config,manifest,[this]{return cancelled();});if(decoder->duration()>0)resumePosition=std::min(resumePosition,decoder->duration());output->reset(resumePosition);started=false;if(resumePosition>0)decoder->seek(resumePosition);draining=false;}
    void load(){
        if(queue.empty()||index>=queue.size()){stop();return;}
        output->reset();decoder.reset();manifest.clear();paused=draining=started=false;resumePosition=0;
        update({{"status","loading"},{"error",""},{"http_status",0},{"paused",false},{"queue_index",index},{"queue_size",queue.size()}});
        auto track=queue[index];auto resolved=get("/v1/resolve/"+encode(track.at("type"))+"/"+encode(track.at("id")));
        if(!resolved.contains("playback_path")||!resolved["playback_path"].is_string())throw std::runtime_error("Upgrade the backend for M3U8 playback");
        manifest=resolved["playback_path"];update({{"track",resolved},{"position",0.0},{"duration",resolved.value("duration_ms",0)/1000.0}});
        openDecoder();if(decoder->duration()>0)update({{"duration",decoder->duration()}});update({{"status","playing"}});
    }
    void advance(){
        if(index+1<queue.size()){++index;load();return;}
        for(unsigned skipped=0;!nextPath.empty()&&skipped<16;++skipped){
            auto page=get(nextPath);auto base=nextPath;auto offset=base.find("&offset=");if(offset==std::string::npos)offset=base.find("?offset=");if(offset!=std::string::npos)base.resize(offset);
            nextPath=page.contains("next_offset")&&!page["next_offset"].is_null()?base+(base.find('?')==std::string::npos?"?":"&")+"offset="+std::to_string(page["next_offset"].get<int>()):"";
            Json next=Json::array();for(auto& item:page.at("items")){auto type=item.value("type",std::string{});if(type=="songs"||type=="library-songs")next.push_back({{"id",item.at("id")},{"type",type}});}
            if(!next.empty()){if(queue.size()>28){auto remove=queue.size()-28;queue.erase(queue.begin(),queue.begin()+remove);}index=queue.size();for(auto& item:next)queue.push_back(item);load();return;}
        }
        stop();
    }
    void act(const Json& j){
        if(config.session.empty()||j.value("session",std::string{})!=config.session)return;
        auto op=j.at("op").get<std::string>();if(j.contains("revision"))operation=j["revision"];
        if(op=="play"){queue=j.at("queue");index=j.value("index",size_t(0));nextPath=j.value("next_path",std::string{});load();}
        else if(op=="next")advance();
        else if(op=="previous"){if(index)--index;load();}
        else if(op=="stop"){stop();queue=Json::array();nextPath.clear();}
        else if(op=="toggle"){
            if(manifest.empty()){
                if(queue.empty())return;paused=!paused;
                if(!paused)load();else update({{"paused",true},{"status","paused"}});return;
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
        else if(op=="volume"){volume=std::clamp(j.at("value").get<double>(),0.0,100.0);output->volume(volume);update({{"volume",volume}});}
    }
    void interrupted(){resumePosition=output->position();output->reset(resumePosition);decoder.reset();started=draining=false;}
public:
    Json request(const Json& j) {
        std::lock_guard guard(lock);
        if(j.value("op",std::string{})=="state")return state;
        if(commands.size()>=8)return error("Playback command queue full");
        auto op=j.value("op",std::string{});
        if(op=="play") {
            if(!j.contains("queue")||!j["queue"].is_array()||j["queue"].empty()||j["queue"].size()>128)return error("Queue must contain 1 to 128 songs");
            if(!j.contains("index")||!j["index"].is_number_unsigned()||j["index"].get<size_t>()>=j["queue"].size())return error("Invalid queue index");
            if(j.contains("next_path")&&(!j["next_path"].is_string()||(!j["next_path"].get<std::string>().empty()&&j["next_path"].get<std::string>().rfind("/v1/",0)!=0)))return error("Invalid continuation path");
            for(auto& t:j["queue"])if(!t.is_object()||!t.contains("id")||!t["id"].is_string()||!t.contains("type")||!t["type"].is_string())return error("Invalid queue item");
        } else if(op=="seek"||op=="volume") {
            auto field=op=="seek"?"seconds":"value";
            if(!j.contains(field)||!j[field].is_number()||!std::isfinite(j[field].get<double>()))return error("Numeric value required");
        } else if(op!="toggle"&&op!="next"&&op!="previous"&&op!="stop")return error("Unknown operation");
        auto pending=j;
        if(op!="volume")pending["revision"]=++revision;
        commands.push_back(std::move(pending));return {{"accepted",true}};
    }
    void run(){
        try{
            output=std::make_unique<AudioOutput>();output->volume(volume);
            auto lastConfig=std::chrono::steady_clock::time_point{};
            while(true){
                Json next;{std::lock_guard guard(lock);if(!commands.empty()){next=std::move(commands.front());commands.pop_front();}}
                try{
                    auto now=std::chrono::steady_clock::now();
                    if(!next.is_null()||now-lastConfig>std::chrono::seconds(1)){syncConfig();lastConfig=now;}
                    if(!next.is_null())act(next);
                    if(decoder&&!paused){
                        int slot=output->available();
                        if(!draining&&slot>=0){int count=decoder->read(output->data(slot),AudioOutput::Frames);if(count){output->append(slot,count);if(!started){output->pause(false);started=true;}}else draining=true;}
                        if(draining&&output->empty()){if(revision.load()!=operation)throw Cancelled();advance();}
                    }
                    if(!manifest.empty())update({{"position",output->position()}});
                }catch(const Cancelled&){interrupted();}
                catch(const HttpError& e){stop();update({{"status","error"},{"error",e.what()},{"http_status",e.status}});}
                catch(const std::exception& e){stop();update({{"status","error"},{"error",e.what()}});}
                svcSleepThread(5000000);
            }
        }catch(const std::exception& e){update({{"status","error"},{"error",e.what()}});}
    }
};
int main() {
    try {
        mkdir("sdmc:/config/switchmusic",0777);curl_global_init(CURL_GLOBAL_DEFAULT);
        if(R_FAILED(romfsMountFromFsdev("sdmc:/atmosphere/contents/420000000000534D/romfs.bin",0,"romfs")))throw std::runtime_error("Reinstall SwitchMusic: bundled certificate RomFS is missing");
        Player player;
        // Named Horizon IPC port; network sockets above are only for backend HTTP.
        Handle port=INVALID_HANDLE;
        Result rc=createControlPort(&port);
        if(R_FAILED(rc))throw std::runtime_error("Cannot register SwitchMusic IPC port");
        controlPortRegistered=true;
        std::thread worker([&]{player.run();});
        rc=runControlServer(port,[&](std::string_view raw){
            try {
                auto config=Config::load();
                return dispatchControlMessage(raw,config.session,[&](const Json& request){return player.request(request);});
            }catch(const std::exception& e){return encodeControlReply(error(e.what()));}
        });
        // A fatal kernel server error must stop the worker and all audio services
        // with the process; never unwind a live worker referencing stack objects.
        diagAbortWithResult(rc);
    }catch(const std::exception& e){std::ofstream("sdmc:/config/switchmusic/fatal.log")<<e.what();return 1;}
}
