#include <seek_bar.hpp>
#include <cassert>
#include <vector>
#include <iostream>
int main(){
 std::vector<double> sent;
 smusic::OverlaySeekBar bar([&](double value){sent.push_back(value);});
 bar.setBoundaries(10,20,400,88);bar.setFocused(true);bar.setState(50,100,"a");
 assert(bar.requestFocus(nullptr,tsl::FocusDirection::Down)==&bar);
 assert(bar.onClick(HidNpadButton_Right)&&bar.displayedPosition()==55&&sent.empty());
 bar.setState(52,100,"a");assert(bar.displayedPosition()==55);
 assert(bar.onClick(HidNpadButton_A)&&sent.size()==1&&sent.back()==55);
 bar.onClick(HidNpadButton_Left);assert(bar.onClick(HidNpadButton_B));assert(bar.displayedPosition()==55&&sent.size()==1);
 bar.onClick(HidNpadButton_Right);bar.setFocused(false);assert(bar.displayedPosition()==55);
 bar.setState(0,100,"a");bar.onClick(HidNpadButton_Left);assert(bar.displayedPosition()==0);
 bar.setState(99,100,"b");bar.onClick(HidNpadButton_Right);assert(bar.displayedPosition()==100);
 bar.setState(50,100,"c");assert(bar.displayedPosition()==50);
 tsl::gfx::Renderer renderer;bar.draw(&renderer);assert(renderer.thumbX==210);
 using Event=tsl::elm::TouchEvent;
 assert(bar.onTouch(Event::Touch,210,62,0,0,210,62));
 assert(bar.onTouch(Event::Hold,386,62,210,62,210,62));assert(sent.size()==1);
 assert(bar.onTouch(Event::Release,386,62,386,62,210,62));assert(sent.size()==2&&sent.back()==100);
 assert(!bar.onTouch(Event::Touch,210,22,0,0,210,22));
 bar.setState(0,0,"");bar.onClick(HidNpadButton_Right);bar.onClick(HidNpadButton_A);
 assert(!bar.onTouch(Event::Release,210,62,0,0,210,62)&&sent.size()==2);
 std::cout<<"Production overlay seek row: focus, preview/apply/cancel, poll stability, bounds, touch drag and disabled state passed\n";
}
