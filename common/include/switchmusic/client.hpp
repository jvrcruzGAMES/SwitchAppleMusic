#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <stdexcept>
#include <vector>
#include <functional>
namespace smusic {
using Json = nlohmann::json;
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
std::vector<uint8_t> apiBytes(const Config&, const std::string& path,size_t limit=256*1024,const std::function<bool()>& cancel={});
Json post(const Config&, const std::string& path, const Json& body);
Json signIn(Config, const std::string& password);
void signOut(const Config&);
Json control(const Config&, Json request);
}
