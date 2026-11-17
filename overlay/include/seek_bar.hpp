#pragma once
#include <tesla.hpp>
#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <string>

namespace smusic {
// A real list focus target, independent of transport-button selection.
class OverlaySeekBar : public tsl::elm::Element {
    double position=0, duration=0;
    std::string track;
    std::optional<double> preview;
    std::function<void(double)> seek;
    static std::string time(double seconds){
        int s=std::max(0,int(seconds));
        return std::to_string(s/60)+":"+(s%60<10?"0":"")+std::to_string(s%60);
    }
    void commit(){if(preview&&seek){position=*preview;preview.reset();seek(position);}}
public:
    explicit OverlaySeekBar(std::function<void(double)> action):seek(std::move(action)){}
    void setState(double seconds,double total,const std::string& id){
        if(track!=id||total<=0)preview.reset();
        track=id;duration=std::max(0.0,total);position=std::clamp(seconds,0.0,duration);
        if(preview)*preview=std::clamp(*preview,0.0,duration);
    }
    double displayedPosition()const{return preview.value_or(position);}
    tsl::elm::Element* requestFocus(tsl::elm::Element*,tsl::FocusDirection)override{return this;}
    void setFocused(bool focused)override{tsl::elm::Element::setFocused(focused);if(!focused)preview.reset();}
    void layout(u16,u16,u16,u16)override{setBoundaries(getX(),getY(),getWidth(),88);}
    bool onClick(u64 keys)override{
        if(keys&(HidNpadButton_L|HidNpadButton_R))return false;
        if(keys&(HidNpadButton_Left|HidNpadButton_Right)){
            if(duration>0)preview=std::clamp(displayedPosition()+((keys&HidNpadButton_Right)?5:-5),0.0,duration);
            return true;
        }
        if(keys&HidNpadButton_A){commit();return true;}
        if((keys&HidNpadButton_B)&&preview){preview.reset();return true;}
        return false;
    }
    bool onTouch(tsl::elm::TouchEvent event,s32 x,s32,s32,s32,s32 initialX,s32 initialY)override{
        if(duration<=0||initialX<getX()+24||initialX>getX()+getWidth()-24||
            initialY<getY()+20||initialY>getY()+64)return false;
        preview=duration*std::clamp(double(x-getX()-24)/std::max(1,int(getWidth())-48),0.0,1.0);
        if(event==tsl::elm::TouchEvent::Release)commit();
        return true;
    }
    void draw(tsl::gfx::Renderer* r)override{
        const tsl::Color muted{10,10,11,15},accent{15,4,6,15},white{15,15,15,15};
        int left=getX()+24,width=std::max(1,int(getWidth())-48),cy=getY()+42;
        r->drawString(duration>0?(hasFocus()?"← / → seek · A apply · B cancel":"Seek playback"):"Play a track to seek",
                      false,left,getY()+20,14,muted);
        r->drawRoundedRect(left,cy-3,width,6,3,tsl::Color{4,4,5,15});
        int filled=duration>0?int(width*std::clamp(displayedPosition()/duration,0.0,1.0)):0;
        if(filled>0)r->drawRoundedRect(left,cy-3,filled,6,3,accent);
        if(duration>0)r->drawCircle(left+filled,cy,hasFocus()?8:6,true,hasFocus()?accent:white);
        r->drawString(time(displayedPosition()),false,left,getY()+74,16,muted);
        auto total=time(duration);
        auto length=r->drawString(total,false,0,0,16,tsl::Color{},0,false).first;
        r->drawString(total,false,left+width-length,getY()+74,16,muted);
    }
};
}
