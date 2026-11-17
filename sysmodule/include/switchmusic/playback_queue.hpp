#pragma once
#include <nlohmann/json.hpp>
#include <algorithm>
#include <numeric>
#include <random>
#include <string>
#include <vector>
namespace smusic {
// Only IDs/types are retained. A bounded history window and the first page let
// repeat-all restart a paginated queue without storing the entire remote library.
class PlaybackQueue {
    using Json=nlohmann::json;
    Json tracks=Json::array(), first=Json::array();
    std::string continuation, firstContinuation;
    std::vector<size_t> order;
    size_t cursor=0;
    bool shuffled=false;
    std::string repeatMode="off";
    std::mt19937 random;
    void arrange(size_t selected) {
        order.resize(tracks.size());std::iota(order.begin(),order.end(),0);
        cursor=selected;
        if(shuffled&&!order.empty()) {
            std::swap(order[0],order[selected]);cursor=0;
            std::shuffle(order.begin()+1,order.end(),random);
        }
    }
public:
    explicit PlaybackQueue(uint32_t seed):random(seed){}
    bool empty()const{return tracks.empty();}
    size_t index()const{return order.empty()?0:order[cursor];}
    size_t size()const{return tracks.size();}
    const Json& current()const{return tracks.at(index());}
    bool shuffle()const{return shuffled;}
    const std::string& repeat()const{return repeatMode;}
    void setShuffle(bool value){auto selected=index();shuffled=value;arrange(selected);}
    void setRepeat(std::string value){repeatMode=std::move(value);}
    void clear(){tracks=first=Json::array();order.clear();continuation.clear();firstContinuation.clear();cursor=0;}
    void start(Json items,size_t selected,std::string next){tracks=std::move(items);first=tracks;continuation=firstContinuation=std::move(next);arrange(selected);}
    bool previous(){if(empty())return false;if(cursor)--cursor;else if(repeatMode=="queue")cursor=order.size()-1;return true;}
    template<class Fetch> bool advance(Fetch fetch,bool automatic=false) {
        if(empty())return false;
        if(automatic&&repeatMode=="single")return true;
        if(cursor+1<order.size()){++cursor;return true;}
        for(unsigned skipped=0;!continuation.empty()&&skipped<16;++skipped){
            Json page=fetch(continuation);auto base=continuation;
            auto offset=base.find("&offset=");if(offset==std::string::npos)offset=base.find("?offset=");if(offset!=std::string::npos)base.resize(offset);
            continuation=page.contains("next_offset")&&!page["next_offset"].is_null()?base+(base.find('?')==std::string::npos?"?":"&")+"offset="+std::to_string(page["next_offset"].get<int>()):"";
            Json next=Json::array();
            for(auto& item:page.at("items")){auto type=item.value("type",std::string{});if(type=="songs"||type=="library-songs")next.push_back({{"id",item.at("id")},{"type",type}});}
            if(!next.empty()) {
                // Preserve the last 28 played IDs in playback order for Previous.
                Json history=Json::array();
                size_t begin=order.size()>28?order.size()-28:0;
                for(size_t i=begin;i<order.size();++i)history.push_back(tracks[order[i]]);
                size_t selected=history.size();for(auto& item:next)history.push_back(std::move(item));
                tracks=std::move(history);order.resize(tracks.size());std::iota(order.begin(),order.end(),0);cursor=selected;
                if(shuffled)std::shuffle(order.begin()+selected,order.end(),random);
                return true;
            }
        }
        if(repeatMode=="queue"){
            tracks=first;continuation=firstContinuation;arrange(0);
            if(shuffled)std::shuffle(order.begin(),order.end(),random);
            return !empty();
        }
        return false;
    }
};
}
