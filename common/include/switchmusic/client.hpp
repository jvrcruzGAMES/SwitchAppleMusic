#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <stdexcept>
namespace smusic {
using Json = nlohmann::json;
constexpr unsigned ControlPort = 17837;
constexpr size_t MaxFrame = 64 * 1024;
struct Config {
    std::string backend, username, session;
    static constexpr const char* Path = "sdmc:/config/switchmusic/config.json";
    static Config load(bool requireSession=true, const std::string& path=Path);
    void save(const std::string& path=Path) const;
    static void clear(const std::string& path=Path);
    void validate(bool requireSession=true);
};
class HttpError : public std::runtime_error {
public:
    long status;
    HttpError(long status, const std::string& message):std::runtime_error(message),status(status){}

};
Json error(const std::string& message);
std::string encode(const std::string& value);
Json api(const Config&, const std::string& path);
Json signIn(Config, const std::string& password);
void signOut(const Config&);
void download(const Config&, const std::string& path, const std::string& file);
Json control(const Config&, Json request);
bool receiveFrame(int fd, Json& value);
bool sendFrame(int fd, const Json& value);
}
