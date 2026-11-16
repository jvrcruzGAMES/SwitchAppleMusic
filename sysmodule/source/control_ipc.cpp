#include <switchmusic/control_server.hpp>
#include <switchmusic/ipc.hpp>
#include <array>
#include <algorithm>
#include <cstring>

namespace smusic {
namespace {
constexpr size_t TlsMessageBytes = 0x100;
constexpr Result BadInput = MAKERESULT(Module_Libnx, LibnxError_BadInput);

// Kernel-translated handles are ours even when a caller sends an unsupported
// special header. Close them before rejecting it, avoiding a handle-table leak.
void discardHandles(const HipcHeader &header, const unsigned char *tls) {
    if (!header.has_special_header)
        return;
    HipcSpecialHeader special{};
    std::memcpy(&special, tls + sizeof(HipcHeader), sizeof(special));
    size_t offset = sizeof(HipcHeader) + sizeof(special) + (special.send_pid ? sizeof(u64) : 0);
    size_t count = special.num_copy_handles + special.num_move_handles;
    if (offset + count * sizeof(Handle) > TlsMessageBytes)
        return;
    for (size_t i = 0; i < count; ++i) {
        Handle handle;
        std::memcpy(&handle, tls + offset + i * sizeof(Handle), sizeof(handle));
        svcCloseHandle(handle);
    }
}
void reply(Result result, u32 written = 0) {
    void *tls = armGetTls();
    auto response = hipcMakeRequestInline(tls, .num_data_words = 9);
    auto *header = static_cast<CmifOutHeader *>(cmifGetAlignedDataStart(response.data_words, tls));
    *header = {CMIF_OUT_HEADER_MAGIC, 0, result, 0};
    std::memcpy(header + 1, &written, sizeof(written));
}
bool process(const std::function<std::string(std::string_view)> &handler) {
    auto *tls = static_cast<unsigned char *>(armGetTls());
    HipcHeader header{};
    std::memcpy(&header, tls, sizeof(header));
    discardHandles(header, tls);
    if (header.type == CmifCommandType_Close)
        return false;
    // Validate the layout before hipcParseRequest can follow descriptor offsets.
    const size_t bytes = sizeof(HipcHeader) + 2 * sizeof(HipcBufferDescriptor) + header.num_data_words * 4;
    if (header.type != CmifCommandType_Request || header.has_special_header || header.num_send_statics ||
        header.num_send_buffers != 1 || header.num_recv_buffers != 1 || header.num_exch_buffers ||
        header.recv_static_mode || bytes > TlsMessageBytes) {
        reply(BadInput);
        return true;
    }
    auto request = hipcParseRequest(tls);
    auto *cmif = static_cast<CmifInHeader *>(cmifGetAlignedDataStart(request.data.data_words, tls));
    auto *end = reinterpret_cast<unsigned char *>(request.data.data_words) + header.num_data_words * 4;
    if (reinterpret_cast<unsigned char *>(cmif) + sizeof(*cmif) + sizeof(u32) > end ||
        cmif->magic != CMIF_IN_HEADER_MAGIC || cmif->version || cmif->command_id != ControlCommand ||
        cmif->token) {
        reply(BadInput);
        return true;
    }
    u32 size;
    std::memcpy(&size, cmif + 1, sizeof(size));
    // Descriptors live in the thread's shared IPC/TLS buffer. The handler uses
    // FS/other services, which overwrite that buffer before the reply is sent.
    // Snapshot values and pointers before invoking any application code.
    const auto input = request.data.send_buffers[0];
    const auto output = request.data.recv_buffers[0];
    const auto *source = static_cast<const char *>(hipcGetBufferAddress(&input));
    auto *destination = hipcGetBufferAddress(&output);
    size_t capacity = hipcGetBufferSize(&output);
    if (input.mode != HipcBufferMode_Normal || output.mode != HipcBufferMode_Normal || !size ||
        size > MaxControlBytes || size != hipcGetBufferSize(&input) || !capacity ||
        capacity > MaxControlBytes || !source || !destination) {
        reply(BadInput);
        return true;
    }
    std::string result;
    try {
        // Own the request bytes across nested service calls as well.
        const std::string body(source, size);
        result = handler(body);
    } catch (...) {
        result = R"({"error":"Playback IPC request failed"})";
    }
    if (result.empty() || result.size() > capacity) {
        reply(BadInput);
        return true;
    }
    std::memcpy(destination, result.data(), result.size());
    reply(0, static_cast<u32>(result.size()));
    return true;
}
} // namespace
Result createControlPort(Handle *port) {
    Result rc = svcManageNamedPort(port, ControlServiceName, MaxControlSessions);
    if (R_SUCCEEDED(rc))
        return rc;
    // Named-port entries can outlive a force-terminated owner. Reclaim only
    // a proven closed server; never unregister another live instance.
    Handle probe = INVALID_HANDLE;
    Result status = svcConnectToNamedPort(&probe, ControlServiceName);
    if (R_SUCCEEDED(status)) {
        svcCloseHandle(probe);
        return rc;
    }
    if (R_VALUE(status) == KERNELRESULT(ConnectionRefused)) {
        Handle unused = INVALID_HANDLE;
        status = svcManageNamedPort(&unused, ControlServiceName, 0);
        if (R_FAILED(status))
            return status;
    } else if (R_VALUE(status) != KERNELRESULT(NotFound))
        return rc;
    *port = INVALID_HANDLE;
    return svcManageNamedPort(port, ControlServiceName, MaxControlSessions);
}
Result runControlServer(Handle port, const std::function<std::string(std::string_view)> &handler) {
    std::array<Handle, MaxControlSessions + 1> handles{};
    std::array<u64, MaxControlSessions + 1> touched{};
    handles[0] = port;
    size_t count = 1;
    Handle response = INVALID_HANDLE;
    auto remove = [&](size_t index) {
        svcCloseHandle(handles[index]);
        --count;
        handles[index] = handles[count];
        touched[index] = touched[count];
    };
    while (true) {
        u64 now = armGetSystemTick();
        for (size_t i = count; i-- > 1;)
            if (handles[i] != response && now - touched[i] > armNsToTicks(2000000000))
                remove(i);
        s32 index = -1;
        Handle replied = response;
        Result rc = svcReplyAndReceive(&index, handles.data(), static_cast<s32>(count), response, 100000000);
        response = INVALID_HANDLE;
        if (R_VALUE(rc) == KERNELRESULT(ConnectionClosed)) {
            if (index > 0 && static_cast<size_t>(index) < count)
                remove(index);
            else {
                for (size_t i = 1; i < count; ++i)
                    if (handles[i] == replied) {
                        remove(i);
                        break;
                    }
            }
            continue;
        }
        if (R_VALUE(rc) == KERNELRESULT(TimedOut))
            continue;
        if (R_FAILED(rc))
            return rc;
        if (index == 0) {
            Handle session;
            rc = svcAcceptSession(&session, port);
            if (R_SUCCEEDED(rc)) {
                if (count == handles.size())
                    svcCloseHandle(session);
                else {
                    handles[count] = session;
                    touched[count] = armGetSystemTick();
                    ++count;
                }
            }
        } else if (index > 0 && static_cast<size_t>(index) < count) {
            touched[index] = armGetSystemTick();
            if (process(handler))
                response = handles[index];
            else
                remove(index);
        }
    }
}
} // namespace smusic
