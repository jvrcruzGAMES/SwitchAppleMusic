#pragma once
#include <nlohmann/json.hpp>
#include <string>
namespace smusic {
using Json = nlohmann::json;
constexpr unsigned ControlPort = 17837;
constexpr size_t MaxFrame = 64 * 1024;
struct Config {
    std::string backend, token, ca;
    static Config load();
};
Json error(const std::string& message);
std::string encode(const std::string& value);
Json api(const Config&, const std::string& path);
void download(const Config&, const std::string& path, const std::string& file);
Json control(const Config&, Json request);
bool receiveFrame(int fd, Json& value);
bool sendFrame(int fd, const Json& value);
}
