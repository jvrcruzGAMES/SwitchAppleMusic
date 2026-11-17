#include <switchmusic/client.hpp>
#include <curl/curl.h>
#include <cassert>
#include <fstream>
#include <iostream>
#include <unistd.h>
#include <cerrno>
using namespace smusic;
extern "C" int __real_rename(const char*,const char*);
extern "C" int __wrap_rename(const char* source,const char* destination){
    // Reproduce fsdev/Horizon semantics rather than Linux's replace-on-rename.
    if(access(destination,F_OK)==0){errno=EEXIST;return -1;}
    return __real_rename(source,destination);
}
int main(int argc,char** argv){
    assert(argc==3);curl_global_init(CURL_GLOBAL_DEFAULT);
    const std::string path=argv[2],password="Synthetic-password-12345";
    Config guest{argv[1],"",""};guest.validate(false);guest.save(path);
    auto anonymous=Config::load(false,path);assert(anonymous.username.empty()&&anonymous.session.empty());
    assert(api(anonymous,"/v1/catalog/search?term=test").at("items")[0].at("id")=="123");
    bool guestDenied=false;try{anonymous.validate();}catch(...){guestDenied=true;}assert(guestDenied);
    guestDenied=false;try{signIn(anonymous,password);}catch(...){guestDenied=true;}assert(guestDenied);
    Config config{argv[1],"admin",""};
    for(auto url:{"file:///tmp/test","https://user:password@example.com","https://example.com/path","https://example.com?token=secret","https://example.com#fragment"}){
        auto invalid=config;invalid.backend=url;bool rejected=false;try{invalid.validate(false);}catch(...){rejected=true;}assert(rejected);
    }
    bool denied=false;try{signIn(config,"wrong-password");}catch(const HttpError& e){denied=e.status==401;}assert(denied);
    auto login=signIn(config,password);config.session=login.at("session");assert(config.session.size()==43);
    config.save(path);auto restored=Config::load(true,path);assert(restored.session==config.session&&restored.volume==80);
    config.volume=37;config.save(path);assert(Config::load(true,path).volume==37);
    std::ifstream file(path);auto saved=Json::parse(file);assert(!saved.contains("password")&&!saved.contains("ca_bundle"));
    assert(api(restored,"/auth/me").at("user").at("username")=="admin");
    assert(post(restored,"/auth/apple/qr/start",Json::object()).at("confirmation")=="123-456");
    signOut(restored);denied=false;try{api(restored,"/auth/me");}catch(const HttpError& e){denied=e.status==401;}assert(denied);
    config.session=signIn(config,password).at("session");config.save(path);
    api(config,"/test/expire");denied=false;try{api(config,"/auth/me");}catch(const HttpError& e){denied=e.status==401;}assert(denied);
    Config::clear(path);assert(Config::load(false,path).backend==config.backend);assert(Config::load(false,path).session.empty());
    denied=false;try{Config::load(true,path);}catch(...){denied=true;}assert(denied);
    {std::ofstream legacy(path);legacy<<Json{{"backend_url",config.backend},{"username","admin"},{"password",password},{"ca_bundle","obsolete.pem"}};}
    assert(Config::load(false,path).session.empty()&&Config::load(false,path).volume==80);Config::clear(path);std::ifstream legacy(path);saved=Json::parse(legacy);assert(!saved.contains("password")&&!saved.contains("ca_bundle"));
    curl_global_cleanup();std::cout<<"Native login, cookie persistence, revocation, expiry, URL validation and legacy migration passed\n";
}
