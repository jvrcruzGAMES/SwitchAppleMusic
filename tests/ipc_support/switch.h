#pragma once
// Host-only libnx test double. It models the documented HIPC descriptor layout
// and kernel calls so the actual production server runs against reusable TLS.
#include <cstdint>
#include <cstddef>
#include <cstring>
using u8 = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s32 = int32_t;
using Handle = u32;
using Result = u32;
inline constexpr Handle INVALID_HANDLE = 0;
inline constexpr u32 Module_Libnx = 345, LibnxError_BadInput = 11;
#define MAKERESULT(module, description) ((Result)((module) | ((description) << 9)))
#define R_VALUE(rc) (rc)
#define R_FAILED(rc) ((rc) != 0)
#define R_SUCCEEDED(rc) ((rc) == 0)
#define KERNELRESULT(name) MAKERESULT(1, KernelError_##name)
inline constexpr u32 KernelError_TimedOut = 117, KernelError_NotFound = 121,
                     KernelError_ConnectionClosed = 123, KernelError_ConnectionRefused = 131;
inline constexpr u32 CMIF_IN_HEADER_MAGIC = 0x49434653, CMIF_OUT_HEADER_MAGIC = 0x4f434653;
inline constexpr u32 CmifCommandType_Close = 2, CmifCommandType_Request = 4, HipcBufferMode_Normal = 0;
struct HipcHeader {
    u32 type : 16, num_send_statics : 4, num_send_buffers : 4, num_recv_buffers : 4, num_exch_buffers : 4;
    u32 num_data_words : 10, recv_static_mode : 4, padding : 6, recv_list_offset : 11, has_special_header : 1;
};
struct HipcSpecialHeader {
    u32 send_pid : 1, num_copy_handles : 4, num_move_handles : 4, padding : 23;
};
struct HipcBufferDescriptor {
    u32 size_low, address_low;
    u32 mode : 2, address_high : 22, size_high : 4, address_mid : 4;
};
struct HipcMetadata {
    u32 type = 0, num_send_statics = 0, num_send_buffers = 0, num_recv_buffers = 0, num_exch_buffers = 0,
        num_data_words = 0, num_recv_statics = 0, send_pid = 0, num_copy_handles = 0, num_move_handles = 0;
};
struct HipcRequest {
    HipcBufferDescriptor *send_buffers = nullptr;
    HipcBufferDescriptor *recv_buffers = nullptr;
    u32 *data_words = nullptr;
};
struct HipcParsedRequest {
    HipcMetadata meta;
    HipcRequest data;
};
struct CmifInHeader {
    u32 magic, version, command_id, token;
};
struct CmifOutHeader {
    u32 magic, version;
    Result result;
    u32 token;
};
static_assert(sizeof(HipcHeader) == 8 && sizeof(HipcSpecialHeader) == 4 &&
              sizeof(HipcBufferDescriptor) == 12);
inline void *hipcGetBufferAddress(const HipcBufferDescriptor *descriptor) {
    return reinterpret_cast<void *>(uintptr_t(descriptor->address_low) |
                                    (uintptr_t(descriptor->address_mid) << 32) |
                                    (uintptr_t(descriptor->address_high) << 36));
}
inline size_t hipcGetBufferSize(const HipcBufferDescriptor *descriptor) {
    return descriptor->size_low | (size_t(descriptor->size_high) << 32);
}
inline HipcBufferDescriptor testBuffer(const void *pointer, size_t size) {
    auto address = reinterpret_cast<uintptr_t>(pointer);
    HipcBufferDescriptor result{};
    result.size_low = u32(size);
    result.size_high = u32(size >> 32);
    result.address_low = u32(address);
    result.address_mid = u32(address >> 32);
    result.address_high = u32(address >> 36);
    return result;
}
inline HipcParsedRequest hipcParseRequest(void *tls) {
    auto *header = static_cast<HipcHeader *>(tls);
    auto *start = reinterpret_cast<unsigned char *>(tls) + 8;
    HipcParsedRequest request{};
    request.data.send_buffers = reinterpret_cast<HipcBufferDescriptor *>(start);
    request.data.recv_buffers = request.data.send_buffers + header->num_send_buffers;
    request.data.data_words = reinterpret_cast<u32 *>(request.data.recv_buffers + header->num_recv_buffers);
    return request;
}
inline void *cmifGetAlignedDataStart(u32 *words, void *tls) {
    auto offset = reinterpret_cast<unsigned char *>(words) - static_cast<unsigned char *>(tls);
    return static_cast<unsigned char *>(tls) + ((offset + 15) & ~size_t(15));
}
inline HipcRequest testMakeReply(void *tls, HipcMetadata metadata) {
    HipcHeader header{};
    header.type = metadata.type;
    header.num_data_words = metadata.num_data_words;
    std::memcpy(tls, &header, 8);
    HipcRequest result{};
    result.data_words = reinterpret_cast<u32 *>(static_cast<unsigned char *>(tls) + 8);
    return result;
}
#define hipcMakeRequestInline(base, ...) testMakeReply((base), HipcMetadata{__VA_ARGS__})
void *armGetTls();
u64 armGetSystemTick();
u64 armNsToTicks(u64);
Result svcCloseHandle(Handle);
Result svcConnectToNamedPort(Handle *, const char *);
Result svcManageNamedPort(Handle *, const char *, s32);
Result svcAcceptSession(Handle *, Handle);
Result svcReplyAndReceive(s32 *, const Handle *, s32, Handle, u64);
