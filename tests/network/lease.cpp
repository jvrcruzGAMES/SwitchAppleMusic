#include <switchmusic/network_lease.hpp>
#include <cassert>
#include <iostream>
unsigned initializes=0,exits=0,creates=0,closes=0,submits=0;bool active=false;
Result initError=0,requestError=0;
NifmRequestState state=NifmRequestState_OnHold;
Result nifmInitialize(NifmServiceType type){assert(type==NifmServiceType_System);++initializes;return initError;}
void nifmExit(){assert(!active);++exits;}
Result nifmCreateRequest(NifmRequest* request,bool autoclear){assert(autoclear&&!active);request->id=++creates;active=true;return 0;}
Result nifmRequestCancel(NifmRequest*){assert(active);return 0;}
void nifmRequestClose(NifmRequest*){assert(active);active=false;++closes;}
Result nifmRequestSubmit(NifmRequest*){assert(active);++submits;return 0;}
Result nifmGetRequestState(NifmRequest*,NifmRequestState* out){assert(active);*out=state;return 0;}
Result nifmGetResult(NifmRequest*){return requestError;}
int main(){
 using Clock=std::chrono::steady_clock;auto now=Clock::now();
 {
  smusic::NetworkLease lease;assert(lease.poll(now)==0&&active&&submits==1);
  for(int i=1;i<5;++i)lease.poll(now+std::chrono::milliseconds(i*100));assert(submits==1);
  lease.poll(now+std::chrono::seconds(1));assert(active&&creates==1&&submits==1); // pending is retained
  state=NifmRequestState_Available;
  for(int i=2;i<20;++i)lease.poll(now+std::chrono::seconds(i));
  assert(active&&creates==1&&closes==0); // idle/stopped polling never releases connectivity
  state=NifmRequestState_Unknown1;requestError=123;
  assert(lease.poll(now+std::chrono::seconds(20))==123&&!active&&closes==1&&exits==1);
  lease.poll(now+std::chrono::seconds(21));assert(initializes==1);
  state=NifmRequestState_OnHold;requestError=0;
  assert(lease.poll(now+std::chrono::seconds(25))==0&&active&&creates==2);
 }
 assert(!active&&closes==2&&exits==2);
 {smusic::NetworkLease lease;initError=44;assert(lease.poll(now)==44);initError=0;
  lease.poll(now+std::chrono::seconds(1));assert(!active);
  assert(lease.poll(now+std::chrono::seconds(5))==0&&active);}
 std::cout<<"Persistent NIFM lease survives idle/stop, retains pending requests, recovers failures with backoff and closes on shutdown\n";
}
