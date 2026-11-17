#pragma once
#include <cstdint>
#include <string>
#include <utility>
using u16=uint16_t;using u64=uint64_t;using s32=int32_t;
constexpr u64 HidNpadButton_Left=1,HidNpadButton_Right=2,HidNpadButton_A=4,HidNpadButton_B=8;
namespace tsl{
enum class FocusDirection{None,Up,Down};
struct Color{int r=0,g=0,b=0,a=0;};
namespace gfx{struct Renderer{
 int thumbX=-1;
 std::pair<int,int> drawString(const std::string&,bool,int,int,int,Color,int=0,bool=true){return {20,16};}
 void drawRoundedRect(int,int,int,int,int,Color){}
 void drawCircle(int x,int,int,bool,Color){thumbX=x;}
};}
namespace elm{
enum class TouchEvent{Touch,Hold,Release};
class Element{
 int x=0,y=0,width=0,height=0;bool focused=false;
public:
 virtual ~Element()=default;
 virtual Element* requestFocus(Element*,FocusDirection){return this;}
 virtual void setFocused(bool value){focused=value;}
 bool hasFocus()const{return focused;}
 virtual void layout(u16,u16,u16,u16){}
 virtual bool onClick(u64){return false;}
 virtual bool onTouch(TouchEvent,s32,s32,s32,s32,s32,s32){return false;}
 virtual void draw(gfx::Renderer*){}
 void setBoundaries(int a,int b,int c,int d){x=a;y=b;width=c;height=d;}
 int getX()const{return x;}int getY()const{return y;}int getWidth()const{return width;}int getHeight()const{return height;}
};
}
}
