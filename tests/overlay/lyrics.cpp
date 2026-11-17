#include <lyrics_panel.hpp>
#include <cassert>
#include <iostream>
int main(){
 smusic::OverlayLyricsPanel panel;panel.setBoundaries(0,0,400,560);
 panel.setState({{"track",{{"title","Song"},{"artist","Artist"}}}});
 panel.setLyrics({{"ready",true},{"error",""},{"lines",{"Previous","Current","Next 1","Next 2","Next 3"}}});
 tsl::gfx::Renderer renderer;panel.draw(&renderer);
 assert(renderer.texts.size()==8);
 const char* expected[]={"Previous","Current","Next 1","Next 2","Next 3"};
 for(unsigned i=0;i<5;++i){auto text=renderer.texts[i+2];assert(text.text==expected[i]);assert(text.size==(i==1?24:20));
  if(i)assert(text.y-renderer.texts[i+1].y==84);}
 panel.setLyrics({{"ready",false}});renderer.texts.clear();panel.draw(&renderer);
 assert(renderer.texts[3].text=="Loading synchronized lyrics…");
 std::cout<<"Production overlay renders previous/current/next-three lines with current in the second slot\n";
}
