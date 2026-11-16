#include <switchmusic/client.hpp>
#include <switchmusic/ipc.hpp>
#ifdef __SWITCH__
#include <switch.h>
#include <vector>
#endif

namespace smusic {
Json control(const Config &config, Json request) {
    try {
        auto input = encodeControlRequest(std::move(request), config.session);
#ifdef __SWITCH__
        // Named-port connect returns immediately if the module is disabled. Open
        // per call so a live Toolbox restart never leaves a cached dead session.
        Handle handle = INVALID_HANDLE;
        Result rc = svcConnectToNamedPort(&handle, ControlServiceName);
        if (R_FAILED(rc))
            return error("SwitchMusic sysmodule unavailable; enable it in Toolbox");
        struct Session {
            Handle handle;
            ~Session() { svcCloseHandle(handle); }
        } session{handle};
        Service service{};
        serviceCreate(&service, handle);
        std::vector<char> output(MaxControlBytes);
        u32 size = static_cast<u32>(input.size()), written = 0;
        rc = serviceDispatchInOut(&service, ControlCommand, size, written,
                                  .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_In,
                                                   SfBufferAttr_HipcMapAlias | SfBufferAttr_Out},
                                  .buffers = {{input.data(), input.size()}, {output.data(), output.size()}});
        if (R_FAILED(rc))
            return error("SwitchMusic IPC request failed; retry after enabling the sysmodule");
        if (!written || written > output.size())
            return error("Invalid IPC response size");
        return decodeControlMessage(std::string_view(output.data(), written));
#else
        (void)input;
        return error("Switch IPC is only available on Nintendo Switch");
#endif
    } catch (const std::exception &exception) {
        return error(exception.what());
    }
}
} // namespace smusic
