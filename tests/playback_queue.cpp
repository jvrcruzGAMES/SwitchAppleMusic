#include <switchmusic/playback_queue.hpp>
#include <cassert>
#include <set>
#include <iostream>
using Json=nlohmann::json;
Json songs(int count){Json out=Json::array();for(int i=0;i<count;++i)out.push_back({{"id",std::to_string(i)},{"type","songs"}});return out;}
int main(){
    smusic::PlaybackQueue q(123);
    auto noFetch=[](const std::string&)->Json{assert(false);return {};};
    q.start(songs(12),4,"");q.setShuffle(true);
    assert(q.current()["id"]=="4");std::set<std::string> seen;std::vector<std::string> played;
    do{auto id=q.current()["id"].get<std::string>();assert(seen.insert(id).second);played.push_back(id);}while(q.advance(noFetch));
    assert(seen.size()==12);assert(q.previous());assert(q.current()["id"]==played[10]);
    auto current=q.current()["id"];q.setShuffle(false);assert(q.current()["id"]==current);
    q.start(songs(3),0,"");q.setRepeat("single");
    assert(q.advance(noFetch,true)&&q.index()==0);assert(q.advance(noFetch,false)&&q.index()==1);
    q.setRepeat("queue");assert(q.advance(noFetch)&&q.index()==2);assert(q.advance(noFetch,true)&&q.index()==0);
    assert(q.previous()&&q.index()==2);
    q.start(songs(2),0,"/v1/catalog/tracks?offset=2");unsigned fetches=0;
    auto fetch=[&](const std::string& path){assert(path=="/v1/catalog/tracks?offset=2");++fetches;return Json{{"items",Json::array({{{"id","2"},{"type","songs"}}})},{"next_offset",nullptr}};};
    assert(q.advance(fetch));assert(q.advance(fetch)&&q.current()["id"]=="2");
    assert(q.advance(fetch)&&q.current()["id"]=="0");assert(q.advance(fetch));assert(q.advance(fetch)&&q.current()["id"]=="2");assert(fetches==2);
    q.setRepeat("off");q.start(songs(1),0,"/v1/catalog/tracks?offset=1");
    unsigned page=1;
    auto manyPages=[&](const std::string&){return Json{{"items",Json::array({{{"id",std::to_string(page++)},{"type","songs"}}})},{"next_offset",int(page)}};};
    for(int i=0;i<100;++i){assert(q.advance(manyPages));assert(q.size()<=29);}
    q.clear();assert(q.empty()&&!q.previous()&&!q.advance(fetch));
    std::cout<<"Shuffle preserves current track and plays each ID once; previous, repeat-single, manual skip and paginated repeat-queue passed\n";
}
