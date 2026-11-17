#pragma once
#include <switchmusic/client.hpp>
#include <array>
#include <algorithm>
namespace smusic {
// Both UIs keep the active line in slot 1: previous, current, next three.
inline std::array<std::string,5> lyricWindow(const Json& lines,double seconds){
    long long milliseconds=static_cast<long long>(std::max(0.0,seconds)*1000);
    int active=-1;
    for(size_t i=0;i<lines.size();++i){if(lines[i].value("time_ms",0LL)<=milliseconds)active=int(i);else break;}
    std::array<std::string,5> result{};
    for(int slot=0;slot<5;++slot){int index=active+slot-1;
        if(index>=0&&index<int(lines.size()))result[slot]=lines[index].value("text",std::string{});
    }
    if(result[1].empty())result[1]="♪";
    return result;
}
}
