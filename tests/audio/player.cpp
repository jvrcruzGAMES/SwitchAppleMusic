#include <switchmusic/audio.hpp>
#include <curl/curl.h>
#include <array>
#include <cstdio>
#include <iostream>
using namespace smusic;
int main(int argc,char** argv){
    if(argc!=6)return 2;curl_global_init(CURL_GLOBAL_DEFAULT);
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
