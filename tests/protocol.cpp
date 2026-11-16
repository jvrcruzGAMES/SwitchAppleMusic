#include <switchmusic/client.hpp>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <thread>
#include <cassert>
#include <iostream>
using namespace smusic;
int main(){
    assert(encode("a/b? x")=="a%2Fb%3F%20x");
    int sockets[2];assert(socketpair(AF_UNIX,SOCK_STREAM,0,sockets)==0);
    Json sent={{"version",2},{"session",std::string(43,'a')},{"op","play"},{"queue",Json::array({{{"id","i.123"},{"type","library-songs"}}})}};
    assert(sendFrame(sockets[0],sent));Json got;assert(receiveFrame(sockets[1],got));assert(got==sent);
    uint32_t huge=htonl(MaxFrame+1);assert(write(sockets[0],&huge,4)==4);assert(!receiveFrame(sockets[1],got));
    uint32_t n=htonl(3);assert(write(sockets[0],&n,4)==4);assert(write(sockets[0],"bad",3)==3);assert(!receiveFrame(sockets[1],got));
    std::string payload=sent.dump();n=htonl(payload.size());
    std::thread partial([&]{auto* p=reinterpret_cast<char*>(&n);for(unsigned i=0;i<4;++i)assert(write(sockets[0],p+i,1)==1);for(char c:payload)assert(write(sockets[0],&c,1)==1);});
    assert(receiveFrame(sockets[1],got));assert(got==sent);partial.join();close(sockets[0]);close(sockets[1]);
    std::cout<<"Protocol roundtrip, fragmented reads, invalid JSON and size limits passed\n";
}
