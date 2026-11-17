#pragma once
#include <cstdint>
using Result=uint32_t;
#define R_FAILED(value) ((value)!=0)
#define R_SUCCEEDED(value) ((value)==0)
enum NifmServiceType{NifmServiceType_System=1};
enum NifmRequestState{NifmRequestState_Invalid=0,NifmRequestState_Unknown1=1,NifmRequestState_OnHold=2,NifmRequestState_Available=3,NifmRequestState_Unknown4=4,NifmRequestState_Unknown5=5};
struct NifmRequest{unsigned id=0;};
Result nifmInitialize(NifmServiceType);void nifmExit();
Result nifmCreateRequest(NifmRequest*,bool);Result nifmRequestCancel(NifmRequest*);void nifmRequestClose(NifmRequest*);
Result nifmRequestSubmit(NifmRequest*);Result nifmGetRequestState(NifmRequest*,NifmRequestState*);Result nifmGetResult(NifmRequest*);
