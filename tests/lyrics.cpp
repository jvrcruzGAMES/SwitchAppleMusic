#include <curl/curl.h>
#include <switchmusic/lyrics_worker.hpp>
#include <cassert>
#include <chrono>
#include <thread>
#include <iostream>
using namespace smusic;
int main(int argc,char** argv){
    assert(argc==2||argc==3);curl_global_init(CURL_GLOBAL_DEFAULT);
    Json lines=Json::array();for(int i=0;i<6;++i)lines.push_back({{"time_ms",1000*(i+1)},{"text",std::to_string(i)}});
    auto before=lyricWindow(lines,0);assert(before[0].empty()&&before[1]=="♪"&&before[2]=="0"&&before[4]=="2");
    auto middle=lyricWindow(lines,2.5);assert((middle==std::array<std::string,5>{"0","1","2","3","4"}));
    auto end=lyricWindow(lines,7);assert(end[0]=="4"&&end[1]=="5"&&end[2].empty());
    LyricsWorker worker;Config config{argv[1],"admin",std::string(43,'a')};worker.prepare(config,{{"id","123"}});
    assert(!worker.snapshot("old",1).at("ready").get<bool>());
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(8);Json result;bool sawFailure=false;
    do{result=worker.snapshot("123",2.5);if(result.value("ready",false)){if(result.value("error",std::string{}).empty())break;sawFailure=true;}std::this_thread::sleep_for(std::chrono::milliseconds(10));}while(std::chrono::steady_clock::now()<deadline);
    assert(result.value("ready",false)&&result.at("lines")==Json(middle));
    if(argc==3)assert(sawFailure);
    worker.prepare(config,Json::object());assert(!worker.snapshot("123",0).at("ready").get<bool>());
    curl_global_cleanup();std::cout<<"Shared five-line timing, boundaries, lazy IPC lyric worker and stale-track rejection passed\n";
}
