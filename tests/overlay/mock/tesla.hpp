#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
using u16=uint16_t;using u64=uint64_t;using s32=int32_t;
struct HidTouchState{};struct HidAnalogStickState{};
constexpr u64 HidNpadButton_L=16,HidNpadButton_Y=32,HidNpadButton_R=64;
constexpr u64 HidNpadButton_Left=1,HidNpadButton_Right=2,HidNpadButton_A=4,HidNpadButton_B=8;
namespace tsl{
enum class FocusDirection{None,Up,Down};
struct Color{int r=0,g=0,b=0,a=0;};
namespace gfx{struct Renderer{
 struct Text{std::string text;int y,size;};std::vector<Text> texts;
 int thumbX=-1;
 std::pair<int,int> drawString(const std::string& text,bool,int,int y,int size,Color,int=0,bool=true){texts.push_back({text,y,size});return {20,16};}
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
 virtual void drawHighlight(gfx::Renderer*){}
 virtual void drawFocusBackground(gfx::Renderer*){}
 void setBoundaries(int a,int b,int c,int d){x=a;y=b;width=c;height=d;}
 int getX()const{return x;}int getY()const{return y;}int getWidth()const{return width;}int getHeight()const{return height;}
};
class List:public Element{public:void addItem(Element*,int){}};
class OverlayFrame:public Element{public:OverlayFrame(const char*,const char*){}void setContent(Element*){}};
}
inline std::vector<std::string> wrapText(const std::string& text,float,const std::string&,bool,const std::string&,float,size_t){return {text};}
inline void goBack(){}
class Gui{public:virtual ~Gui()=default;virtual elm::Element* createUI()=0;virtual void update(){}virtual bool handleInput(u64,u64,const HidTouchState&,HidAnalogStickState,HidAnalogStickState){return false;}};
}
