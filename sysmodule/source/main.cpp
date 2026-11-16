#include <switch.h>
#include <switchmusic/client.hpp>
#include <mpv/client.h>
#include <curl/curl.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/stat.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <deque>
#include <fstream>
#include <algorithm>
#ifndef SWITCHMUSIC_HEAP_MB
#define SWITCHMUSIC_HEAP_MB 32
#endif
using namespace smusic;
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
    Result rc=socketInitializeDefault();if(R_FAILED(rc))diagAbortWithResult(rc);
}
void __appExit() {socketExit();timeExit();fsdevUnmountAll();fsExit();smExit();}
}
class Player {
    Config config;
    std::mutex lock;
    std::deque<Json> commands;
    Json state={{"status","stopped"},{"position",0.0},{"duration",0.0},{"volume",70.0},{"paused",false}};
    Json queue=Json::array();size_t index=0;int slot=0;std::string nextPath;
    mpv_handle* mpv=nullptr;int64_t activeEntry=-1;
    void check(int rc) { if(rc<0)throw std::runtime_error(mpv_error_string(rc)); }
    void command(std::initializer_list<const char*> args) {std::vector<const char*> v(args);v.push_back(nullptr);check(mpv_command(mpv,v.data()));}
    void update(Json values) {std::lock_guard guard(lock);state.update(values);}
    void load() {
        if(queue.empty()||index>=queue.size()) {command({"stop"});update({{"status","stopped"}});return;}
        update({{"status","loading"},{"error",""},{"queue_index",index},{"queue_size",queue.size()}});
        auto track=queue[index];
        auto resolved=api(config,"/v1/resolve/"+encode(track.at("type"))+"/"+encode(track.at("id")));
        slot=1-slot;auto file="sdmc:/config/switchmusic/audio-"+std::to_string(slot)+".m4a";
        download(config,resolved.at("stream_path"),file);
        command({"loadfile",file.c_str(),"replace"});
        check(mpv_get_property(mpv,"playlist/0/id",MPV_FORMAT_INT64,&activeEntry));
        int no=0;check(mpv_set_property(mpv,"pause",MPV_FORMAT_FLAG,&no));
        update({{"track",resolved},{"status","playing"},{"paused",false},{"position",0.0},{"duration",resolved.value("duration_ms",0)/1000.0}});
    }
    void advance() {
        if(index+1<queue.size()){++index;load();return;}
        for(unsigned skipped=0;!nextPath.empty()&&skipped<16;++skipped) {
            auto page=api(config,nextPath);auto base=nextPath.substr(0,nextPath.find('?'));
            nextPath=page.contains("next_offset")&&!page["next_offset"].is_null()?base+"?offset="+std::to_string(page["next_offset"].get<int>()):"";
            Json next=Json::array();for(auto& item:page.at("items")){auto type=item.value("type",std::string{});if(type=="songs"||type=="library-songs")next.push_back({{"id",item.at("id")},{"type",type}});}
            if(!next.empty()) {if(queue.size()>28){auto remove=queue.size()-28;queue.erase(queue.begin(),queue.begin()+remove);index-=remove;}
                index=queue.size();for(auto& item:next)queue.push_back(item);load();return;}

        }
        command({"stop"});update({{"status","stopped"}});
    }
    void act(const Json& j) {
        auto op=j.at("op").get<std::string>();
        if(op=="play") {queue=j.at("queue");index=j.value("index",size_t(0));nextPath=j.value("next_path",std::string{});load();}
        else if(op=="next") {advance();}
        else if(op=="previous") {if(index)--index;load();}
        else if(op=="toggle") {command({"cycle","pause"});}
        else if(op=="stop") {command({"stop"});update({{"status","stopped"},{"position",0.0}});}
        else if(op=="seek") {double value=std::max(0.0,j.at("seconds").get<double>());auto s=std::to_string(value);command({"seek",s.c_str(),"absolute"});}
        else if(op=="volume") {double value=std::clamp(j.at("value").get<double>(),0.0,100.0);check(mpv_set_property(mpv,"volume",MPV_FORMAT_DOUBLE,&value));}
    }
public:
    explicit Player(Config c):config(std::move(c)){}
    Json request(const Json& j) {
        std::lock_guard guard(lock);
        if(j.value("op",std::string{})=="state")return state;
        if(commands.size()>=8)return error("Playback command queue full");
        auto op=j.value("op",std::string{});
        if(op=="play") {
            if(!j.contains("queue")||!j["queue"].is_array()||j["queue"].empty()||j["queue"].size()>128)return error("Queue must contain 1 to 128 songs");
            if(!j.contains("index")||!j["index"].is_number_unsigned()||j["index"].get<size_t>()>=j["queue"].size())return error("Invalid queue index");
            if(j.contains("next_path")&&(!j["next_path"].is_string()||(!j["next_path"].get<std::string>().empty()&&j["next_path"].get<std::string>().rfind("/v1/library/",0)!=0)))return error("Invalid continuation path");
            for(auto& t:j["queue"])if(!t.is_object()||!t.contains("id")||!t["id"].is_string()||!t.contains("type")||!t["type"].is_string())return error("Invalid queue item");
        } else if(op=="seek"||op=="volume") {
            auto field=op=="seek"?"seconds":"value";
            if(!j.contains(field)||!j[field].is_number())return error("Numeric value required");
        } else if(op!="toggle"&&op!="next"&&op!="previous"&&op!="stop")return error("Unknown operation");
        commands.push_back(j);return {{"accepted",true}};
    }
    void run() {
        mpv=mpv_create();
        if(!mpv){update(error("mpv allocation failed"));return;}
        try {
            check(mpv_set_option_string(mpv,"config","no"));
            check(mpv_set_option_string(mpv,"vo","null"));
            check(mpv_set_option_string(mpv,"video","no"));
            check(mpv_set_option_string(mpv,"ao","hos"));
            check(mpv_set_option_string(mpv,"terminal","no"));
            check(mpv_set_option_string(mpv,"volume","70"));
            check(mpv_initialize(mpv));
            while(true) {
                Json next;
                {std::lock_guard guard(lock);if(!commands.empty()){next=std::move(commands.front());commands.pop_front();}}
                try {if(!next.is_null())act(next);}catch(const std::exception& e){update({{"status","error"},{"error",e.what()}});}
                auto* event=mpv_wait_event(mpv,0.05);
                if(event->event_id==MPV_EVENT_END_FILE) {
                    auto* end=static_cast<mpv_event_end_file*>(event->data);
                    if(end->playlist_entry_id!=activeEntry)continue;
                    if(end->reason==MPV_END_FILE_REASON_EOF) {try {advance();}catch(const std::exception& e){update({{"status","error"},{"error",e.what()}});}}
                    else if(end->reason==MPV_END_FILE_REASON_ERROR)update({{"status","error"},{"error",mpv_error_string(end->error)}});
                }
                double pos=0,dur=0,vol=70;int paused=0;
                Json values;
                if(mpv_get_property(mpv,"time-pos",MPV_FORMAT_DOUBLE,&pos)>=0)values["position"]=pos;
                if(mpv_get_property(mpv,"duration",MPV_FORMAT_DOUBLE,&dur)>=0)values["duration"]=dur;
                if(mpv_get_property(mpv,"volume",MPV_FORMAT_DOUBLE,&vol)>=0)values["volume"]=vol;
                if(mpv_get_property(mpv,"pause",MPV_FORMAT_FLAG,&paused)>=0)values["paused"]=bool(paused);
                update(values);
            }
        }catch(const std::exception& e){update({{"status","error"},{"error",e.what()}});}
        mpv_terminate_destroy(mpv);
    }
};
int main() {
    try {
        mkdir("sdmc:/config/switchmusic",0777);curl_global_init(CURL_GLOBAL_DEFAULT);
        auto config=Config::load();Player player(config);
        int server=socket(AF_INET,SOCK_STREAM,0);if(server<0)throw std::runtime_error("Control socket failed");
        sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons(ControlPort);addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        int yes=1;setsockopt(server,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
        if(bind(server,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))<0||listen(server,4)<0) {throw std::runtime_error("Control bind failed");}
        std::thread worker([&]{player.run();});
        while(true) {
            int fd=accept(server,nullptr,nullptr);if(fd<0){svcSleepThread(100000000);continue;}
            timeval tv{0,300000};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv));
            Json request;
            try {if(receiveFrame(fd,request)) {
                if(request.value("version",0)!=1||request.value("token",std::string{})!=config.token)sendFrame(fd,error("Invalid control credentials/version"));
                else sendFrame(fd,player.request(request));
            }}catch(...){sendFrame(fd,error("Malformed request"));}close(fd);
        }
    }catch(const std::exception& e){std::ofstream("sdmc:/config/switchmusic/fatal.log")<<e.what();return 1;}
}
