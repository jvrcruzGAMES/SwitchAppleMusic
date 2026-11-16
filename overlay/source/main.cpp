#define TESLA_INIT_IMPL
#define TESLA_INIT_NO_SOCKET
#include <tesla.hpp>
#include <switchmusic/client.hpp>
#include <chrono>
using namespace smusic;
class Controls:public tsl::Gui {
    tsl::elm::ListItem *title=nullptr,*progress=nullptr;Config config;bool configured=false;
    Json state;std::chrono::steady_clock::time_point last{};
    void command(Json j){if(!configured)return;auto r=control(config,std::move(j));if(r.contains("error"))title->setText(r["error"]);}
    void add(tsl::elm::List* list,const std::string& text,std::function<Json()> request){auto* item=new tsl::elm::ListItem(text);item->setClickListener([this,request](u64 keys){if(keys&KEY_A){command(request());return true;}return false;});list->addItem(item);}
public:
    tsl::elm::Element* createUI() override {
        auto* frame=new tsl::elm::OverlayFrame("SwitchMusic","0.1.0");auto* list=new tsl::elm::List();
        title=new tsl::elm::ListItem("Connecting…");progress=new tsl::elm::ListItem("0:00");list->addItem(title);list->addItem(progress);
        try{config=Config::load();configured=true;}catch(const std::exception& e){title->setText(e.what());}
        add(list,"Play / Pause",[]{return Json{{"op","toggle"}};});add(list,"Previous",[]{return Json{{"op","previous"}};});add(list,"Next",[]{return Json{{"op","next"}};});
        add(list,"Seek −15s",[this]{return Json{{"op","seek"},{"seconds",std::max(0.0,state.value("position",0.0)-15)}};});
        add(list,"Seek +15s",[this]{return Json{{"op","seek"},{"seconds",state.value("position",0.0)+15}};});
        add(list,"Volume −5",[this]{return Json{{"op","volume"},{"value",state.value("volume",70.0)-5}};});
        add(list,"Volume +5",[this]{return Json{{"op","volume"},{"value",state.value("volume",70.0)+5}};});
        add(list,"Stop",[]{return Json{{"op","stop"}};});frame->setContent(list);return frame;
    }
    void update() override {auto now=std::chrono::steady_clock::now();if(now-last<std::chrono::milliseconds(750))return;last=now;
        try {config=Config::load();configured=true;state=control(config,{{"op","state"}});if(state.contains("error")&&!state["error"].get<std::string>().empty()){title->setText(state["error"]);return;}
            auto song=state.value("track",Json::object());title->setText(song.value("title",std::string("Nothing playing")));title->setValue(state.value("paused",false)?"Paused":state.value("status",std::string{}));
            int sec=int(state.value("position",0.0));progress->setText(song.value("artist",std::string{}));progress->setValue(std::to_string(sec/60)+":"+(sec%60<10?"0":"")+std::to_string(sec%60)+"  "+std::to_string(int(state.value("volume",70.0)))+"%");
        }catch(const std::exception& e){configured=false;title->setText(e.what());}
    }
};
class MusicOverlay:public tsl::Overlay {
    void initServices() override {socketInitializeDefault();}
    void exitServices() override {socketExit();}
    std::unique_ptr<tsl::Gui> loadInitialGui() override{return initially<Controls>();}
};
int main(int argc,char** argv){return tsl::loop<MusicOverlay>(argc,argv);}
