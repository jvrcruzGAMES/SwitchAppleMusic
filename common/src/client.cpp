#include <switchmusic/client.hpp>
#include <curl/curl.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fstream>
#include <stdexcept>
#include <memory>
#include <cstdio>
#include <sys/stat.h>
#include <fcntl.h>
#include <cerrno>
#include <algorithm>
namespace smusic {
Json error(const std::string& message) { return {{"error", message}}; }
void Config::validate(bool requireSession) {
    while (!backend.empty() && backend.back()=='/') backend.pop_back();
    std::unique_ptr<CURLU,decltype(&curl_url_cleanup)> url(curl_url(),curl_url_cleanup);
    if(!url || backend.size()>1024 || std::any_of(backend.begin(),backend.end(),[](unsigned char c){return c<=32||c==127;}) || curl_url_set(url.get(),CURLUPART_URL,backend.c_str(),0)!=CURLUE_OK)
        throw std::runtime_error("Enter a valid server URL");
    auto part=[&](CURLUPart key){char* value=nullptr;curl_url_get(url.get(),key,&value,0);std::string text=value?value:"";curl_free(value);return text;};
    auto scheme=part(CURLUPART_SCHEME),path=part(CURLUPART_PATH);
    if((scheme!="https"&&scheme!="http")||part(CURLUPART_HOST).empty()||!part(CURLUPART_USER).empty()||!part(CURLUPART_PASSWORD).empty()||!part(CURLUPART_QUERY).empty()||!part(CURLUPART_FRAGMENT).empty()||(path!=""&&path!="/"))
        throw std::runtime_error("Use the server's http:// or https:// address, without a path or credentials");
    if(username.size()<3||username.size()>32||username[0]<'a'||username[0]>'z'||
       !std::all_of(username.begin(),username.end(),[](unsigned char c){return (c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='.'||c=='_'||c=='-';}))
        throw std::runtime_error("Enter your backend account username");
    if((requireSession||!session.empty()) && (session.size()<32||session.size()>128||!std::all_of(session.begin(),session.end(),[](unsigned char c){return (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_';})))
        throw std::runtime_error("Open SwitchMusic to sign in");
}
Config Config::load(bool requireSession,const std::string& path) {
    std::ifstream f(path);auto j=Json::parse(f,nullptr,false);
    if(!j.is_object())throw std::runtime_error("Open SwitchMusic to sign in");
    Config c{j.value("backend_url",std::string{}),j.value("username",std::string{}),j.value("session",std::string{})};
    c.validate(requireSession);return c;
}
void Config::save(const std::string& path) const {
    auto c=*this;c.validate(!c.session.empty());
    auto raw=Json{{"backend_url",c.backend},{"username",c.username},{"session",c.session}}.dump(2)+"\n";
    auto temporary=path+".tmp";
    int fd=open(temporary.c_str(),O_WRONLY|O_CREAT|O_TRUNC,0600);
    if(fd<0)throw std::runtime_error("Cannot save login on SD card");
    size_t offset=0;bool ok=true;
    while(offset<raw.size()){auto written=write(fd,raw.data()+offset,raw.size()-offset);if(written<=0){ok=false;break;}offset+=written;}
    if(fsync(fd)!=0)ok=false;if(close(fd)!=0)ok=false;
    if(ok){
        if(rename(temporary.c_str(),path.c_str())==0)return;
        // Horizon's RenameFile does not replace an existing destination.
        if(errno==EEXIST&&unlink(path.c_str())==0&&rename(temporary.c_str(),path.c_str())==0)return;
    }
    unlink(temporary.c_str());throw std::runtime_error("Cannot save login on SD card");
}
void Config::clear(const std::string& path) {
    // Keep the URL/username as a convenience, but remove all authorization.
    try{auto c=load(false,path);c.session.clear();c.save(path);}
    catch(...){if(unlink(path.c_str())!=0&&errno!=ENOENT)throw std::runtime_error("Cannot remove saved login");}
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
static std::string request(const Config& c,const std::string& path,void* out,curl_write_callback write,long timeout,const Json* body=nullptr,const std::string& csrf="") {
    if(path.empty()||path[0]!='/'||path.rfind("//",0)==0) throw std::runtime_error("Invalid API path");
    std::unique_ptr<CURL,decltype(&curl_easy_cleanup)> h(curl_easy_init(),curl_easy_cleanup);
    if(!h) throw std::runtime_error("HTTP allocation failed");
    auto url=c.backend+path;
    curl_easy_setopt(h.get(),CURLOPT_URL,url.c_str());
    curl_easy_setopt(h.get(),CURLOPT_COOKIEFILE,"");
    if(!c.session.empty())curl_easy_setopt(h.get(),CURLOPT_COOKIE,("switchmusic_session="+c.session).c_str());
    std::string payload;
    curl_slist* rawHeaders=nullptr;
    if(body){payload=body->dump();rawHeaders=curl_slist_append(rawHeaders,"Content-Type: application/json");curl_easy_setopt(h.get(),CURLOPT_POSTFIELDS,payload.c_str());}
    if(!csrf.empty())rawHeaders=curl_slist_append(rawHeaders,("X-CSRF-Token: "+csrf).c_str());
    std::unique_ptr<curl_slist,decltype(&curl_slist_free_all)> headers(rawHeaders,curl_slist_free_all);
    curl_easy_setopt(h.get(),CURLOPT_HTTPHEADER,headers.get());
    curl_easy_setopt(h.get(),CURLOPT_CONNECTTIMEOUT,5L);
    curl_easy_setopt(h.get(),CURLOPT_TIMEOUT,timeout);
    curl_easy_setopt(h.get(),CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(h.get(),CURLOPT_FOLLOWLOCATION,0L);
    curl_easy_setopt(h.get(),CURLOPT_WRITEFUNCTION,write);
    curl_easy_setopt(h.get(),CURLOPT_WRITEDATA,out);
#ifdef __SWITCH__
    curl_easy_setopt(h.get(),CURLOPT_CAINFO,"romfs:/certs/cacert.pem");
#endif
    curl_easy_setopt(h.get(),CURLOPT_SSL_VERIFYPEER,1L);
    curl_easy_setopt(h.get(),CURLOPT_SSL_VERIFYHOST,2L);
    auto rc=curl_easy_perform(h.get());long status=0;
    curl_easy_getinfo(h.get(),CURLINFO_RESPONSE_CODE,&status);
    if(rc!=CURLE_OK) throw std::runtime_error(std::string("Backend: ")+curl_easy_strerror(rc));
    if(status!=200&&status!=204) {
        std::string message="Backend HTTP "+std::to_string(status);
        if(write==collect){auto j=Json::parse(*static_cast<std::string*>(out),nullptr,false);
            if(j.is_object()&&j.contains("detail")&&j["detail"].is_string())message+=": "+j["detail"].get<std::string>();}
        throw HttpError(status,message);
    }
    curl_slist* cookies=nullptr;curl_easy_getinfo(h.get(),CURLINFO_COOKIELIST,&cookies);
    std::unique_ptr<curl_slist,decltype(&curl_slist_free_all)> cookieOwner(cookies,curl_slist_free_all);
    for(auto* item=cookies;item;item=item->next){std::string line=item->data;std::string marker="\tswitchmusic_session\t";auto name=line.rfind(marker);if(name!=std::string::npos)return line.substr(name+marker.size());}
    return "";
}
Json signIn(Config c,const std::string& password) {
    c.session.clear();c.validate(false);
    Json body={{"username",c.username},{"password",password}};std::string raw;
    auto session=request(c,"/auth/login",&raw,collect,35,&body);
    c.session=session;c.validate();auto result=Json::parse(raw);result["session"]=session;return result;
}
Json post(const Config& c,const std::string& path,const Json& body) {
    auto me=api(c,"/auth/me");std::string raw;
    request(c,path,&raw,collect,35,&body,me.at("csrf"));
    if(raw.empty())return Json::object();
    auto result=Json::parse(raw,nullptr,false);
    if(result.is_discarded())throw std::runtime_error("Invalid backend JSON");return result;
}
void signOut(const Config& c) {
    auto me=api(c,"/auth/me");Json body=Json::object();std::string raw;
    request(c,"/auth/logout",&raw,collect,35,&body,me.at("csrf"));
}
Json api(const Config& c,const std::string& path) {
    std::string raw;request(c,path,&raw,collect,35);
    auto j=Json::parse(raw,nullptr,false);
    if(j.is_discarded()) throw std::runtime_error("Invalid backend JSON");return j;
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
    req["version"]=2;req["session"]=c.session;
    bool ok=connect(fd,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))==0&&sendFrame(fd,req)&&receiveFrame(fd,result);
    close(fd);return ok?result:error("SwitchMusic sysmodule unavailable");
}
}
