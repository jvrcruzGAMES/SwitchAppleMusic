#include <switchmusic/client.hpp>
#include <switchmusic/ipc.hpp>
#include <switch.h>
#include <cassert>
#include <cstring>
#include <iostream>
#include <set>
namespace {
u64 ticks=1;unsigned connects=0,dispatches=0,sleeps=0,failConnect=0,lostReplies=0,toggles=0;
Result connectError=KERNELRESULT(OutOfResource);
bool available=true;Handle next=100;std::set<Handle> live;
}
u64 armGetSystemTick(){return ticks;}
u64 armNsToTicks(u64 value){return value;}
Result svcSleepThread(int64_t value){++sleeps;ticks+=value;return 0;}
Result svcCloseHandle(Handle handle){assert(live.erase(handle)==1);return 0;}
// The NRO has no spare caller-owned session quota. Only SM brokerage works.
Result svcConnectToNamedPort(Handle*,const char*){return KERNELRESULT(OutOfResource);}
TipcService* smGetServiceSessionTipc(){static TipcService service;return &service;}
Result testHasService(TipcService*,u32 command,SmServiceName name,u8* out){
    assert(command==65100&&std::string(name.name)==smusic::ControlServiceName);*out=available;return 0;
}
Result smGetServiceOriginal(Handle* handle,SmServiceName name){
    assert(std::string(name.name)==smusic::ControlServiceName);++connects;
    if(failConnect){--failConnect;return connectError;}
    *handle=next++;assert(live.empty());live.insert(*handle);return 0;
}
Result testClientDispatch(Service* service,u32 command,u32 size,u32* written,TestDispatchParams parameters){
    assert(live.count(service->session)&&command==smusic::ControlCommand);++dispatches;
    auto request=smusic::decodeControlMessage(std::string_view(static_cast<const char*>(parameters.buffers[0].data),size));
    auto op=request.at("op").get<std::string>();if(op=="toggle")++toggles;
    if(lostReplies){--lostReplies;return KERNELRESULT(ConnectionClosed);}
    std::string response=op=="state"?R"({"status":"playing"})":R"({"accepted":true})";
    assert(response.size()<=parameters.buffers[1].size);*written=response.size();
    std::memcpy(const_cast<void*>(parameters.buffers[1].data),response.data(),response.size());ticks+=100000000;return 0;
}
int main(){
    smusic::Config config{"https://backend.example","admin",std::string(43,'a')};
    auto state=[&]{return smusic::control(config,{{"op","state"}});};
    assert(state().at("status")=="playing");
    for(unsigned i=0;i<100;++i)assert(state().at("status")=="playing");
    assert(connects==1&&live.size()==1);
    ticks+=2000000000;assert(state().at("status")=="playing");assert(connects==2&&live.size()==1);
    config.session=std::string(43,'b');assert(state().at("status")=="playing");assert(connects==3&&live.size()==1);
    auto before=connects;lostReplies=1;assert(state().at("status")=="playing");assert(connects==before+1);
    auto sent=dispatches;before=connects;lostReplies=1;
    assert(smusic::control(config,{{"op","toggle"}}).contains("error"));
    assert(dispatches==sent+1&&connects==before&&toggles==1&&live.empty());
    failConnect=2;before=connects;assert(state().at("status")=="playing");assert(connects==before+3&&sleeps==2);
    config.session=std::string(43,'d');connectError=MAKERESULT(21,3);failConnect=1;
    before=connects;assert(state().at("status")=="playing");assert(connects==before+2);
    connectError=KERNELRESULT(OutOfResource);
    config.session=std::string(43,'c');failConnect=9;before=connects;
    auto failed=state();assert(failed.at("error").get<std::string>().find("session resources exhausted")!=std::string::npos);
    assert(connects==before+5&&live.empty());
    failConnect=0;available=false;before=connects;assert(state().contains("error"));assert(connects==before);
    available=true;assert(state().at("status")=="playing");
    std::cout<<"SM-brokered IPC works without caller session quota, reuses one session, renews idle/account handles, reconnects reads, bounds quota retry and never replays toggle\n";
}
