#pragma once
#include <switch.h>
#include <chrono>
namespace smusic {
// Owned/polled by the playback worker, including while stopped. Sockets alone
// do not express a persistent background connectivity request to Horizon.
class NetworkLease {
    NifmRequest request{};
    bool initialized=false,created=false;
    std::chrono::steady_clock::time_point next{};
    Result last=0;
    void closeRequest(){if(created){nifmRequestCancel(&request);nifmRequestClose(&request);created=false;}}
    void reset(){closeRequest();if(initialized){nifmExit();initialized=false;}}
public:
    NetworkLease()=default;
    NetworkLease(const NetworkLease&)=delete;
    NetworkLease& operator=(const NetworkLease&)=delete;
    ~NetworkLease(){reset();}
    Result poll(std::chrono::steady_clock::time_point now=std::chrono::steady_clock::now()){
        if(now<next)return last;
        next=now+std::chrono::seconds(1);
        if(!initialized){last=nifmInitialize(NifmServiceType_System);if(R_FAILED(last)){next=now+std::chrono::seconds(5);return last;}initialized=true;}
        if(!created){
            last=nifmCreateRequest(&request,true);
            if(R_FAILED(last)){reset();next=now+std::chrono::seconds(5);return last;}
            created=true;last=nifmRequestSubmit(&request);
        }else{
            NifmRequestState state=NifmRequestState_Invalid;
            last=nifmGetRequestState(&request,&state);
            if(R_SUCCEEDED(last)){
                if(state==NifmRequestState_Invalid){last=nifmGetResult(&request);closeRequest();}
                else if(state==NifmRequestState_Unknown1){last=nifmGetResult(&request);if(R_SUCCEEDED(last))last=nifmRequestSubmit(&request);}
                else if(state==NifmRequestState_Unknown5)last=nifmRequestSubmit(&request);
                // OnHold is a pending request, not a broken session. Keep it
                // alive while Horizon handles radio/profile/foreground policy.
            }
        }
        if(R_FAILED(last)){reset();next=now+std::chrono::seconds(5);}
        return last;
    }
};
}
