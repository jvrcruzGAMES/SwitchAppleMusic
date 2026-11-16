#pragma once
#include <nlohmann/json.hpp>
#include <functional>
#include <string>
#include <string_view>
#include <stdexcept>

namespace smusic {
// Kernel named port, not a TCP endpoint or a deferred SM lookup.
inline constexpr char ControlServiceName[] = "smusic";
inline constexpr unsigned ControlProtocolVersion = 3;
inline constexpr unsigned ControlCommand = 0;
inline constexpr size_t MaxControlBytes = 64 * 1024;
inline constexpr unsigned MaxControlSessions = 4;
using ControlHandler = std::function<nlohmann::json(const nlohmann::json &)>;

inline std::string encodeControlRequest(nlohmann::json request, const std::string &session) {
    if (!request.is_object() || session.empty())
        throw std::runtime_error("Open SwitchMusic to sign in");
    request["version"] = ControlProtocolVersion;
    request["session"] = session;
    auto raw = request.dump();
    if (raw.size() > MaxControlBytes)
        throw std::runtime_error("Playback request exceeds IPC buffer limit");
    return raw;
}
inline nlohmann::json decodeControlMessage(std::string_view raw) {
    if (raw.empty() || raw.size() > MaxControlBytes)
        throw std::runtime_error("Invalid IPC message size");
    auto value = nlohmann::json::parse(raw.begin(), raw.end(), nullptr, false);
    if (!value.is_object())
        throw std::runtime_error("Malformed IPC JSON object");
    return value;
}
inline std::string encodeControlReply(const nlohmann::json &value) {
    auto raw = value.dump();
    if (!value.is_object() || raw.size() > MaxControlBytes)
        return R"({"error":"Playback response exceeds IPC buffer limit"})";
    return raw;
}
inline std::string dispatchControlMessage(std::string_view raw, const std::string &currentSession,
                                          const ControlHandler &handler) {
    try {
        auto request = decodeControlMessage(raw);
        if (!request.contains("version") || !request["version"].is_number_unsigned() ||
            request["version"].get<uint64_t>() != ControlProtocolVersion || currentSession.empty() ||
            request.value("session", std::string{}) != currentSession)
            return R"({"error":"Open SwitchMusic to sign in again"})";
        return encodeControlReply(handler(request));
    } catch (const std::exception &) {
        return R"({"error":"Malformed IPC request"})";
    }
}
} // namespace smusic
