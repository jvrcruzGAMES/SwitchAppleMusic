#include <switchmusic/audio.hpp>
#include <curl/curl.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <cstdio>
#include <limits>
#include <cctype>
#include <vector>
extern "C" {
#include <libavformat/avio.h>
#include <libavutil/error.h>
}
namespace smusic {
namespace {
constexpr size_t BlockSize=256*1024;
constexpr int64_t MaxTrack=512LL*1024*1024;
bool safePath(const std::string& path){return path.rfind("/v1/",0)==0&&path.size()<2048&&path.find_first_of("\r\n#") == std::string::npos&&path.substr(0,path.find('?')).find("..") == std::string::npos;}
}
struct HttpInput::Impl {
    Config config;Cancel cancel;CURL* curl=nullptr;
    std::array<uint8_t,BlockSize> block{};
    std::string path,etag,replyEtag;
    uint8_t* output=nullptr;size_t capacity=0,written=0;
    int64_t position=0,total=-1,blockStart=-1,replyStart=-1,replyEnd=-1,replyTotal=-1;
    size_t blockLength=0;
    Impl(Config c,Cancel fn):config(std::move(c)),cancel(std::move(fn)){
        config.validate();curl=curl_easy_init();if(!curl)throw std::runtime_error("HTTP allocation failed");
    }
    ~Impl(){curl_easy_cleanup(curl);}
    static size_t collect(char* p,size_t a,size_t b,void* data){
        auto& self=*static_cast<Impl*>(data);auto n=a*b;
        if(n>self.capacity-self.written)return 0;
        memcpy(self.output+self.written,p,n);self.written+=n;return n;
    }
    static size_t header(char* p,size_t a,size_t b,void* data){
        auto& self=*static_cast<Impl*>(data);size_t n=a*b;
        try{
        // Header parsing stays bounded; content is never used as a URL.
        if(n>4096)return 0;std::string line(p,n),lower=line;
        std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c){return std::tolower(c);});
        if(lower.rfind("http/",0)==0){self.replyStart=self.replyEnd=self.replyTotal=-1;self.replyEtag.clear();}
        if(lower.rfind("content-range:",0)==0){long long start,end,total;
            if(sscanf(lower.c_str(),"content-range: bytes %lld-%lld/%lld",&start,&end,&total)==3){self.replyStart=start;self.replyEnd=end;self.replyTotal=total;}}
        if(lower.rfind("etag:",0)==0){auto begin=line.find_first_not_of(" \t",5);auto end=line.find_last_not_of("\r\n \t");if(begin!=std::string::npos&&end>=begin)self.replyEtag=line.substr(begin,end-begin+1);}
        return n;
        }catch(...){return 0;} // Never unwind a C++ exception through libcurl.
    }
    static int progress(void* data,curl_off_t,curl_off_t,curl_off_t,curl_off_t){
        auto& self=*static_cast<Impl*>(data);try{return self.cancel&&self.cancel()?1:0;}catch(...){return 1;}
    }
    long get(const std::string& resource,uint8_t* data,size_t limit,const std::string& range=""){
        if(!safePath(resource))throw std::runtime_error("Invalid playback path");
        if(cancel&&cancel())throw Cancelled();
        output=data;capacity=limit;written=0;replyStart=replyEnd=replyTotal=-1;replyEtag.clear();
        curl_easy_reset(curl);auto url=config.backend+resource;
        curl_easy_setopt(curl,CURLOPT_URL,url.c_str());
        curl_easy_setopt(curl,CURLOPT_COOKIE,("switchmusic_session="+config.session).c_str());
        curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,0L);
        curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,5L);
        curl_easy_setopt(curl,CURLOPT_TIMEOUT,range.empty()?190L:20L);
        curl_easy_setopt(curl,CURLOPT_LOW_SPEED_LIMIT,1L);curl_easy_setopt(curl,CURLOPT_LOW_SPEED_TIME,10L);
        curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);
        curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,collect);curl_easy_setopt(curl,CURLOPT_WRITEDATA,this);
        curl_easy_setopt(curl,CURLOPT_HEADERFUNCTION,header);curl_easy_setopt(curl,CURLOPT_HEADERDATA,this);
        curl_easy_setopt(curl,CURLOPT_XFERINFOFUNCTION,progress);curl_easy_setopt(curl,CURLOPT_XFERINFODATA,this);curl_easy_setopt(curl,CURLOPT_NOPROGRESS,0L);
        curl_easy_setopt(curl,CURLOPT_SSL_VERIFYPEER,1L);curl_easy_setopt(curl,CURLOPT_SSL_VERIFYHOST,2L);
#ifdef __SWITCH__
        curl_easy_setopt(curl,CURLOPT_CAINFO,"romfs:/certs/cacert.pem");
#endif
        curl_slist* list=nullptr;
        if(!range.empty()){curl_easy_setopt(curl,CURLOPT_RANGE,range.c_str());if(!etag.empty())list=curl_slist_append(list,("If-Range: "+etag).c_str());}
        std::unique_ptr<curl_slist,decltype(&curl_slist_free_all)> headers(list,curl_slist_free_all);curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers.get());
        auto rc=curl_easy_perform(curl);long status=0;curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status);
        if(cancel&&cancel())throw Cancelled();
        if(status==401||status==403)throw HttpError(status,"Backend account authorization expired or denied");
        if(rc!=CURLE_OK)throw std::runtime_error("Playback transport failed or response exceeded its bound");
        if(status!=200&&status!=206)throw HttpError(status,"Playback HTTP "+std::to_string(status));
        return status;
    }
    void fetch(int64_t offset){
        auto end=std::min(offset+int64_t(BlockSize)-1,MaxTrack-1);
        auto status=get(path,block.data(),block.size(),std::to_string(offset)+"-"+std::to_string(end));
        if(status!=206||replyStart!=offset||replyTotal<=0||replyTotal>MaxTrack||replyEnd<replyStart||replyEnd>=replyTotal||replyEnd-replyStart+1!=int64_t(written)||!written)
            throw std::runtime_error("Backend must support bounded HTTP byte ranges");
        if(total>=0&&(replyTotal!=total||(!etag.empty()&&replyEtag!=etag)))throw std::runtime_error("Playback cache changed; restart this track");
        total=replyTotal;etag=replyEtag;blockStart=offset;blockLength=written;
    }
};
HttpInput::HttpInput(Config config,Cancel cancel):impl(new Impl(std::move(config),std::move(cancel))){}
HttpInput::~HttpInput()=default;
std::string HttpInput::text(const std::string& path,size_t limit){if(limit>256*1024)throw std::runtime_error("Playback metadata exceeds its bound");std::vector<uint8_t> body(limit);auto status=impl->get(path,body.data(),body.size());if(status!=200)throw std::runtime_error("Invalid playback response");return std::string(reinterpret_cast<char*>(body.data()),impl->written);}
void HttpInput::open(const std::string& path){impl->path=path;impl->position=0;impl->total=-1;impl->etag.clear();impl->blockStart=-1;impl->fetch(0);}
int HttpInput::read(uint8_t* output,int count){
    if(impl->cancel&&impl->cancel())throw Cancelled();
    if(count<=0)return AVERROR(EINVAL);if(impl->position>=impl->total)return AVERROR_EOF;
    if(impl->position<impl->blockStart||impl->position>=impl->blockStart+int64_t(impl->blockLength))impl->fetch(impl->position);
    auto offset=size_t(impl->position-impl->blockStart);auto n=std::min(size_t(count),impl->blockLength-offset);
    memcpy(output,impl->block.data()+offset,n);impl->position+=n;return int(n);
}
int64_t HttpInput::seek(int64_t offset,int whence){
    if(whence&AVSEEK_SIZE)return impl->total;whence&=~AVSEEK_FORCE;
    int64_t base=whence==SEEK_SET?0:whence==SEEK_CUR?impl->position:whence==SEEK_END?impl->total:-1;
    if(base<0||offset < -base||offset>impl->total-base)return AVERROR(EINVAL);
    return impl->position=base+offset;
}
}
