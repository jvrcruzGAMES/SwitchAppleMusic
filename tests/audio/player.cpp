#include <switchmusic/audio.hpp>
#include <curl/curl.h>
#include <array>
#include <cstdio>
#include <iostream>
#include <cassert>
#include <chrono>
using namespace smusic;
int main(int argc,char** argv){
    curl_global_init(CURL_GLOBAL_DEFAULT);
    if(argc==3&&std::string(argv[2]).rfind("--reconnect-",0)==0){
        Config config{argv[1],"admin",std::string(43,'a')};std::string mode=argv[2];
        auto start=std::chrono::steady_clock::now();
        HttpInput input(config,[&]{return mode=="--reconnect-cancel"&&std::chrono::steady_clock::now()-start>std::chrono::milliseconds(100);});
        if(mode=="--reconnect-text")assert(input.text("/v1/recover")=="online");
        else if(mode=="--reconnect-range"){
            input.open("/v1/stream/recover",65536);std::array<uint8_t,4096> data;
            assert(input.read(data.data(),data.size())==int(data.size()));for(unsigned i=0;i<data.size();++i)assert(data[i]==i%251);
        }else if(mode=="--reconnect-cancel"){
            bool cancelled=false;try{input.text("/v1/recover");}catch(const Cancelled&){cancelled=true;}assert(cancelled);
            assert(std::chrono::steady_clock::now()-start<std::chrono::seconds(1));
        }else{
            bool failed=false;try{input.text("/v1/recover");}catch(const std::exception& e){failed=std::string(e.what()).find("Playback transport:")!=std::string::npos;}assert(failed);
            auto elapsed=std::chrono::steady_clock::now()-start;
            assert(elapsed>=std::chrono::milliseconds(10500)&&elapsed<std::chrono::seconds(20));
        }
        std::cout<<mode<<": passed\n";return 0;
    }
    if(argc==3&&std::string(argv[2])=="--http-checks"){
        Config config{argv[1],"admin",std::string(43,'a')};
        HttpInput input(config);
        assert(input.text("/v1/large-metadata").size()==600*1024);
        constexpr int64_t large=3LL*1024*1024*1024+123;
        input.open("/v1/stream/large",large);assert(input.seek(0,0x10000)==large);
        std::array<uint8_t,4096> bytes;
        for(int64_t offset:{0LL,2LL*1024*1024*1024+17}){
            assert(input.seek(offset,SEEK_SET)==offset);assert(input.read(bytes.data(),bytes.size())==int(bytes.size()));
            for(size_t i=0;i<bytes.size();++i)assert(bytes[i]==(offset+i)%251);
        }
        input.open("/v1/stream/chunked",600*1024);
        size_t streamed=0;int count;
        while((count=input.read(bytes.data(),bytes.size()))>0){
            for(int i=0;i<count;++i)assert(bytes[i]==(streamed+i)%251);
            streamed+=count;
        }
        assert(streamed==600*1024);
        assert(input.seek(400*1024,SEEK_SET)==400*1024);
        assert(input.read(bytes.data(),bytes.size())==int(bytes.size()));
        for(size_t i=0;i<bytes.size();++i)assert(bytes[i]==(400*1024+i)%251);
        bool reported=false;try{input.text("/v1/unavailable");}catch(const HttpError& error){reported=error.status==503;}assert(reported);
        reported=false;try{input.text("/v1/disconnect");}catch(const std::exception& error){reported=std::string(error.what()).find("Playback transport:")!=std::string::npos;}assert(reported);
        reported=false;try{input.open("/v1/stream/partial",65536);}catch(const std::exception& error){reported=std::string(error.what()).find("Playback transport:")!=std::string::npos;}assert(reported);
        std::cout<<"Large metadata, 3 GiB media seeks, chunked full-response fallback and precise transport errors passed\n";
        return 0;
    }
    if(argc!=6)return 2;
    try{
        Config config{argv[1],"admin",std::string(43,'a')};bool cancelled=false;
        AudioDecoder decoder(config,argv[2],[&]{return cancelled;});std::array<int16_t,4096*2> pcm;
        FILE* output=fopen(argv[3],"wb");if(!output)return 3;
        int count;while((count=decoder.read(pcm.data(),4096)))if(fwrite(pcm.data(),4,count,output)!=size_t(count))return 4;fclose(output);
        decoder.seek(1);output=fopen(argv[4],"wb");if(!output)return 3;
        while((count=decoder.read(pcm.data(),4096)))if(fwrite(pcm.data(),4,count,output)!=size_t(count))return 4;fclose(output);
        decoder.seek(2.5);output=fopen(argv[5],"wb");if(!output)return 3;
        while((count=decoder.read(pcm.data(),4096)))if(fwrite(pcm.data(),4,count,output)!=size_t(count))return 4;fclose(output);
        cancelled=true;bool interrupted=false;try{decoder.seek(0);decoder.read(pcm.data(),4096);}catch(const Cancelled&){interrupted=true;}
        if(!interrupted)throw std::runtime_error("Cancellation did not interrupt playback");
    }catch(const HttpError& error){std::cerr<<"HTTP "<<error.status<<"\n";return 10;}
    catch(const std::exception& error){std::cerr<<error.what()<<"\n";return 1;}
    curl_global_cleanup();return 0;
}
