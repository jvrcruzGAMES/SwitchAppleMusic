#include <switchmusic/control_server.hpp>
#include <switchmusic/ipc.hpp>
#include <array>
#include <cassert>
#include <iostream>
#include <string>
#include <vector>
namespace {
alignas(16) std::array<unsigned char, 256> tls{};
std::string input, expected;
std::array<char, smusic::MaxControlBytes> output;
unsigned step = 0, handled = 0;
Result expectedResult = 0;
bool malformed = false;
size_t outputCapacity = output.size();
constexpr Result Done = MAKERESULT(1, 999);
void makeRequest() {
    tls.fill(0);
    HipcHeader header{};
    header.type = CmifCommandType_Request;
    header.num_send_buffers = 1;
    header.num_recv_buffers = 1;
    header.num_data_words = 9;
    if (malformed)
        header.num_recv_buffers = 0;
    std::memcpy(tls.data(), &header, sizeof(header));
    auto in = testBuffer(input.data(), input.size()), out = testBuffer(output.data(), outputCapacity);
    std::memcpy(tls.data() + 8, &in, sizeof(in));
    std::memcpy(tls.data() + 20, &out, sizeof(out));
    auto *cmif = reinterpret_cast<CmifInHeader *>(tls.data() + 32);
    *cmif = {CMIF_IN_HEADER_MAGIC, 0, smusic::ControlCommand, 0};
    u32 size = input.size();
    std::memcpy(cmif + 1, &size, sizeof(size));
}
void verifyReply() {
    auto *header = reinterpret_cast<CmifOutHeader *>(tls.data() + 16);
    assert(header->magic == CMIF_OUT_HEADER_MAGIC);
    assert(header->result == expectedResult);
    u32 size = 0;
    std::memcpy(&size, header + 1, sizeof(size));
    if (!expectedResult) {
        assert(size == expected.size());
        assert(std::string(output.data(), size) == expected);
    } else {
        assert(size == 0);
        for (char c : output)
            assert(c == char(0x5a));
    }
}
void run(bool overwrite, bool badHeader = false, size_t capacity = smusic::MaxControlBytes) {
    input = smusic::encodeControlRequest({{"op", "state"}}, std::string(43, 'a'));
    expected = nlohmann::json{{"duration", 0.0},
                              {"error", ""},
                              {"http_status", 0},
                              {"paused", false},
                              {"position", 0.0},
                              {"status", "stopped"},
                              {"track", nlohmann::json::object()},
                              {"volume", 70.0}}
                   .dump();
    step = handled = 0;
    output.fill(char(0x5a));
    malformed = badHeader;
    outputCapacity = capacity;
    expectedResult =
        (badHeader || capacity < expected.size()) ? MAKERESULT(Module_Libnx, LibnxError_BadInput) : 0;
    auto result = smusic::runControlServer(10, [&](std::string_view raw) {
        ++handled;
        // Reproduce FS/libnx calls overwriting the current thread's message
        // buffer with a service response, exactly as in the Atmosphere report.
        if (overwrite) {
            tls.fill(0);
            auto *fsReply = reinterpret_cast<CmifOutHeader *>(tls.data() + 32);
            *fsReply = {CMIF_OUT_HEADER_MAGIC, 0, 0, 0};
        }
        assert(smusic::decodeControlMessage(raw).at("op") == "state");
        return expected;
    });
    assert(result == Done && step == 3);
    assert(handled == (badHeader ? 0u : 1u));
}
} // namespace
void *armGetTls() { return tls.data(); }
u64 armGetSystemTick() { return 0; }
u64 armNsToTicks(u64 n) { return n; }
Result svcCloseHandle(Handle) { return 0; }
Result svcConnectToNamedPort(Handle *, const char *) { return KERNELRESULT(NotFound); }
Result svcManageNamedPort(Handle *, const char *, s32) { return 0; }
Result svcAcceptSession(Handle *out, Handle port) {
    assert(port == 10);
    *out = 20;
    return 0;
}
Result svcReplyAndReceive(s32 *index, const Handle *handles, s32 count, Handle reply, u64) {
    assert(handles[0] == 10);
    if (step == 0) {
        assert(count == 1 && reply == INVALID_HANDLE);
        ++step;
        *index = 0;
        return 0;
    }
    if (step == 1) {
        assert(count == 2 && handles[1] == 20 && reply == INVALID_HANDLE);
        ++step;
        makeRequest();
        *index = 1;
        return 0;
    }
    assert(step == 2 && reply == 20);
    ++step;
    verifyReply();
    return Done;
}
int main() {
    Handle port=INVALID_HANDLE;assert(smusic::createControlPort(&port)==0&&port==10);
    run(false);
    run(true);
    run(true, false, 8);
    run(false, true);
    std::cout << "Production IPC server survives nested TLS overwrite; malformed descriptors and short "
                 "output rejected\n";
}

Result smRegisterService(Handle* out,SmServiceName name,bool light,s32 sessions){
    assert(std::string(name.name)==smusic::ControlServiceName&&!light&&sessions==smusic::MaxControlSessions);
    *out=10;return 0;
}
