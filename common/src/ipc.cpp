#include <switchmusic/client.hpp>
#include <switchmusic/ipc.hpp>
#include <cstdio>
#ifdef __SWITCH__
#include <switch.h>
#include <mutex>
#include <vector>
#endif

namespace smusic {
#ifdef __SWITCH__
namespace {
constexpr Result SmOutOfSessions=MAKERESULT(21,3);
constexpr u32 AtmosphereHasService=65100;
// One process-wide connection, serialized across callers. Reusing it avoids
// consuming a new kernel session for every state poll and artwork chunk.
struct ControlConnection {
    std::mutex mutex;
    Service service{};
    u64 lastUse=0;
    std::string backend,session;
    void close(){
        if(service.session!=INVALID_HANDLE)svcCloseHandle(service.session);
        service={};lastUse=0;backend.clear();session.clear();
    }
    ~ControlConnection(){close();}
    Result connect(const Config& config){
        auto now=armGetSystemTick();
        // Renew before the server's two-second idle timeout. This also handles
        // suspend/resume without assuming that a cached handle is still live.
        if(service.session!=INVALID_HANDLE && (backend!=config.backend || session!=config.session || now-lastUse>=armNsToTicks(1000000000)))close();
        if(service.session!=INVALID_HANDLE)return 0;
        // Direct named-port connections allocate against the caller's session
        // quota. Homebrew applications can have no spare quota (0x10801).
        // SM creates the session and transfers its handle to us instead.
        // Atmosphere's availability query avoids a deferred GetService call
        // when Toolbox has disabled the sysmodule.
        auto name=smEncodeName(ControlServiceName);
        u8 available=0;
        Result rc=tipcDispatchInOut(smGetServiceSessionTipc(),AtmosphereHasService,name,available);
        if(R_FAILED(rc))return rc;
        if(!available)return KERNELRESULT(NotFound);
        Handle handle=INVALID_HANDLE;
        for(unsigned attempt=0;attempt<5;++attempt){
            rc=smGetServiceOriginal(&handle,name);
            if(R_SUCCEEDED(rc))break;
            if(R_VALUE(rc)!=KERNELRESULT(OutOfResource)&&R_VALUE(rc)!=KERNELRESULT(OutOfSessions)&&R_VALUE(rc)!=SmOutOfSessions)break;
            if(attempt<4)svcSleepThread(10000000);
        }
        if(R_SUCCEEDED(rc)){serviceCreate(&service,handle);backend=config.backend;session=config.session;}
        return rc;
    }
};
std::string resultCode(Result rc){char code[16];std::snprintf(code,sizeof(code),"0x%08x",unsigned(R_VALUE(rc)));return code;}
}
#endif
Json control(const Config& config,Json request){
    try{
        auto input=encodeControlRequest(request,config.session);
#ifdef __SWITCH__
        static ControlConnection connection;
        std::lock_guard guard(connection.mutex);
        std::vector<char> output(MaxControlBytes);
        const auto operation=request.value("op",std::string{});
        const bool readOnly=operation=="state"||operation=="artwork"||operation=="lyrics";
        for(unsigned attempt=0;attempt<2;++attempt){
            Result rc=connection.connect(config);
            if(R_FAILED(rc)){
                if(R_VALUE(rc)==KERNELRESULT(NotFound)||R_VALUE(rc)==KERNELRESULT(ConnectionRefused))
                    return error("SwitchMusic IPC service unavailable ("+resultCode(rc)+"); restart the sysmodule in Toolbox");
                if(R_VALUE(rc)==KERNELRESULT(OutOfResource))
                    return error("SwitchMusic IPC session resources exhausted ("+resultCode(rc)+")");
                return error("SwitchMusic IPC connection failed ("+resultCode(rc)+")");
            }
            u32 size=static_cast<u32>(input.size()),written=0;
            rc=serviceDispatchInOut(&connection.service,ControlCommand,size,written,
                .buffer_attrs={SfBufferAttr_HipcMapAlias|SfBufferAttr_In,SfBufferAttr_HipcMapAlias|SfBufferAttr_Out},
                .buffers={{input.data(),input.size()},{output.data(),output.size()}});
            connection.lastUse=armGetSystemTick();
            if(R_FAILED(rc)){
                connection.close();
                // Only read-only operations may be replayed after a lost reply.
                // Never accidentally toggle twice or skip two tracks.
                if(attempt==0&&readOnly&&(R_VALUE(rc)==KERNELRESULT(ConnectionClosed)||R_VALUE(rc)==KERNELRESULT(InvalidHandle)))continue;
                return error("SwitchMusic IPC request failed ("+resultCode(rc)+")");
            }
            if(!written||written>output.size()){connection.close();return error("Invalid IPC response size");}
            return decodeControlMessage(std::string_view(output.data(),written));
        }
        return error("SwitchMusic IPC service unavailable");
#else
        (void)input;
        return error("Switch IPC is only available on Nintendo Switch");
#endif
    }catch(const std::exception& exception){return error(exception.what());}
}
}
