#include <switchmusic/client.hpp>
#include <curl/curl.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fstream>
#include <stdexcept>
#include <memory>
#include <cstdio>
namespace smusic {
Json error(const std::string& message) { return {{"error", message}}; }
Config Config::load() {
    std::ifstream f("sdmc:/config/switchmusic/config.json");
    auto j = Json::parse(f, nullptr, false);
    if (!j.is_object() || !j.contains("backend_url") || !j["backend_url"].is_string()
        || !j.contains("username") || !j["username"].is_string()
        || !j.contains("password") || !j["password"].is_string())
        throw std::runtime_error("Configure /config/switchmusic/config.json first");
    Config c{j["backend_url"], j["username"], j["password"], j.value("ca_bundle", std::string{})};
    while (!c.backend.empty() && c.backend.back() == '/') c.backend.pop_back();
    if (c.username.size() < 3 || c.username.size() > 32 || c.username.find_first_of(":\r\n") != std::string::npos ||
        c.password.size() < 12 || c.password.size() > 256 || c.password.find_first_of("\r\n") != std::string::npos ||
        (c.backend.rfind("https://",0) != 0 && c.backend.rfind("http://",0) != 0))
        throw std::runtime_error("Invalid backend URL or account credentials");
    return c;
}
std::string encode(const std::string& v) {
    std::string out; const char* hex="0123456789ABCDEF";
    for (unsigned char c : v) {
        if ((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='.'||c=='_') out+=c;
        else { out+='%'; out+=hex[c>>4]; out+=hex[c&15]; }
    } return out;
}
static size_t collect(char* p, size_t a, size_t b, void* data) {
    auto& out=*static_cast<std::string*>(data); size_t n=a*b;
    if (out.size()+n > 2*1024*1024) return 0;
    out.append(p,n); return n;
}
struct Sink { FILE* f; size_t bytes=0; };
static size_t save(char* p,size_t a,size_t b,void* data) {
    auto& s=*static_cast<Sink*>(data);size_t n=a*b;
    if(s.bytes+n>64*1024*1024) return 0;
    size_t written=fwrite(p,1,n,s.f);s.bytes+=written;return written;
}
static void request(const Config& c,const std::string& path,void* out,curl_write_callback write,long timeout) {
    if(path.empty()||path[0]!='/'||path.rfind("//",0)==0) throw std::runtime_error("Invalid API path");
    std::unique_ptr<CURL,decltype(&curl_easy_cleanup)> h(curl_easy_init(),curl_easy_cleanup);
    if(!h) throw std::runtime_error("HTTP allocation failed");
    auto url=c.backend+path;
    curl_easy_setopt(h.get(),CURLOPT_URL,url.c_str());
    curl_easy_setopt(h.get(),CURLOPT_HTTPAUTH,CURLAUTH_BASIC);
    curl_easy_setopt(h.get(),CURLOPT_USERNAME,c.username.c_str());
    curl_easy_setopt(h.get(),CURLOPT_PASSWORD,c.password.c_str());
    curl_easy_setopt(h.get(),CURLOPT_CONNECTTIMEOUT,5L);
    curl_easy_setopt(h.get(),CURLOPT_TIMEOUT,timeout);
    curl_easy_setopt(h.get(),CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(h.get(),CURLOPT_FOLLOWLOCATION,0L);
    curl_easy_setopt(h.get(),CURLOPT_WRITEFUNCTION,write);
    curl_easy_setopt(h.get(),CURLOPT_WRITEDATA,out);
    if(!c.ca.empty()) curl_easy_setopt(h.get(),CURLOPT_CAINFO,c.ca.c_str());
    auto rc=curl_easy_perform(h.get());long status=0;
    curl_easy_getinfo(h.get(),CURLINFO_RESPONSE_CODE,&status);
    if(rc!=CURLE_OK) throw std::runtime_error(std::string("Backend: ")+curl_easy_strerror(rc));
    if(status!=200) {
        std::string message="Backend HTTP "+std::to_string(status);
        if(write==collect){auto j=Json::parse(*static_cast<std::string*>(out),nullptr,false);
            if(j.is_object()&&j.contains("detail")&&j["detail"].is_string())message+=": "+j["detail"].get<std::string>();}
        throw std::runtime_error(message);
    }
}
Json api(const Config& c,const std::string& path) {
    std::string raw;request(c,path,&raw,collect,35);
    auto j=Json::parse(raw,nullptr,false);
    if(j.is_discarded()) throw std::runtime_error("Invalid backend JSON");return j;
}
void download(const Config& c,const std::string& path,const std::string& file) {
    std::unique_ptr<FILE,decltype(&fclose)> f(fopen(file.c_str(),"wb"),fclose);
    if(!f) throw std::runtime_error("Cannot create audio cache on SD card");
    Sink sink{f.get()};request(c,path,&sink,save,240);
    if(!sink.bytes) throw std::runtime_error("Empty audio stream");
}
static bool transfer(int fd,void* data,size_t n,bool sending) {
    auto* p=static_cast<char*>(data);
    while(n) {auto done=sending?send(fd,p,n,0):recv(fd,p,n,0); if(done<=0)return false;p+=done;n-=done;}
    return true;
}
bool receiveFrame(int fd,Json& value) {
    uint32_t n;if(!transfer(fd,&n,4,false))return false;n=ntohl(n);
    if(!n||n>MaxFrame)return false;std::string raw(n,'\0');
    if(!transfer(fd,raw.data(),n,false))return false;
    value=Json::parse(raw,nullptr,false);return value.is_object();
}
bool sendFrame(int fd,const Json& value) {
    auto raw=value.dump();if(raw.size()>MaxFrame)return false;uint32_t n=htonl(raw.size());
    return transfer(fd,&n,4,true)&&transfer(fd,raw.data(),raw.size(),true);
}
Json control(const Config& c,Json req) {
    int fd=socket(AF_INET,SOCK_STREAM,0);if(fd<0)return error("Socket unavailable");
    timeval tv{0,300000};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
    setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv));
    sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons(ControlPort);addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    Json result;
    req["version"]=1;req["username"]=c.username;req["password"]=c.password;
    bool ok=connect(fd,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))==0&&sendFrame(fd,req)&&receiveFrame(fd,result);
    close(fd);return ok?result:error("SwitchMusic sysmodule unavailable");
}
}
