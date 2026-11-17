#pragma once
#include <tesla.hpp>
#include <switchmusic/client.hpp>
#include <array>
#include <chrono>
namespace smusic {
class OverlayLyricsPanel : public tsl::elm::Element {
    Json state=Json::object();std::array<std::string,5> lines{};
    std::string message="Loading synchronized lyrics…";
public:
    void setState(Json value){state=std::move(value);}
    void setLyrics(const Json& reply){
        lines={};message=reply.value("error",std::string{});
        if(!reply.value("ready",false)){message="Loading synchronized lyrics…";return;}
        if(reply.contains("lines")&&reply["lines"].is_array()&&reply["lines"].size()==5)
            for(unsigned i=0;i<5;++i)lines[i]=reply["lines"][i].get<std::string>();
    }
    tsl::elm::Element* requestFocus(tsl::elm::Element*,tsl::FocusDirection)override{return this;}
    void layout(u16,u16,u16,u16)override{setBoundaries(getX(),getY(),getWidth(),560);}
    void drawHighlight(tsl::gfx::Renderer*)override{}
    void drawFocusBackground(tsl::gfx::Renderer*)override{}
    void draw(tsl::gfx::Renderer* r)override{
        auto song=state.value("track",Json::object());int left=getX()+22,width=getWidth()-44;
        auto drawText=[&](const std::string& text,int y,unsigned size,tsl::Color color,unsigned limit){
            auto wrapped=tsl::wrapText(text,float(width-18),"auto",false,"",0,size);
            for(unsigned i=0;i<std::min<size_t>(limit,wrapped.size());++i){
                auto line=wrapped[i];if(i+1==limit&&wrapped.size()>limit)line+="…";
                r->drawString(line,false,left,y+int(i)*(size+5),size,color);
            }
        };
        drawText(song.value("title",std::string("Nothing playing")),getY()+28,23,tsl::Color{15,15,15,15},1);
        drawText(song.value("artist",std::string{}),getY()+54,17,tsl::Color{10,10,11,15},1);
        for(unsigned i=0;i<5;++i){
            int top=getY()+90+i*84;
            if(i==1)r->drawRoundedRect(getX()+10,top-24,getWidth()-20,78,10,tsl::Color{3,2,3,15});
            drawText(i==1&&!message.empty()?message:lines[i],top,i==1?24:20,
                     i==1?tsl::Color{15,15,15,15}:i==0?tsl::Color{7,7,8,15}:tsl::Color{10,10,11,15},2);
        }
        r->drawString("L back · Y play / pause",false,left,getY()+548,16,tsl::Color{10,10,11,15});
    }
};
class OverlayLyricsScreen : public tsl::Gui {
    Config config;OverlayLyricsPanel* panel=nullptr;std::chrono::steady_clock::time_point last{};
public:
    explicit OverlayLyricsScreen(Config value):config(std::move(value)){}
    tsl::elm::Element* createUI()override{
        auto* frame=new tsl::elm::OverlayFrame("SwitchMusic","Synced lyrics · L back");
        auto* list=new tsl::elm::List();panel=new OverlayLyricsPanel();list->addItem(panel,560);frame->setContent(list);return frame;
    }
    bool handleInput(u64 keys,u64,const HidTouchState&,HidAnalogStickState,HidAnalogStickState)override{
        if(keys&HidNpadButton_L){tsl::goBack();return true;}
        if(keys&HidNpadButton_R)return true;
        if(keys&HidNpadButton_Y){control(config,{{"op","toggle"}});return true;}
        return false;
    }
    void update()override{
        auto now=std::chrono::steady_clock::now();if(now-last<std::chrono::milliseconds(250))return;last=now;
        try{
            config=Config::load();auto state=control(config,{{"op","state"}});panel->setState(state);
            if(state.contains("error")&&!state["error"].get<std::string>().empty()){
                panel->setLyrics({{"ready",true},{"error",state["error"]}});return;
            }
            auto id=state.value("track",Json::object()).value("id",std::string{});
            if(id.empty()){panel->setLyrics({{"ready",true},{"error","Choose a song in the app"}});return;}
            auto reply=control(config,{{"op","lyrics"},{"track_id",id}});
            if(reply.value("track_id",std::string{})==id)panel->setLyrics(reply);
            else panel->setLyrics({{"ready",true},{"error",reply.value("error",std::string("Lyrics unavailable"))}});
        }catch(const std::exception& e){panel->setLyrics({{"ready",true},{"error",e.what()}});}
    }
};
}
