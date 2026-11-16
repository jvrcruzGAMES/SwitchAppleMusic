#include <borealis.hpp>
#include <switchmusic/client.hpp>
#include <curl/curl.h>
#include <switch.h>
#include <thread>
#include <mutex>
#include <deque>
#include <condition_variable>
#include <chrono>
using namespace smusic;
using Clock=std::chrono::steady_clock;
static std::string pageBase(std::string path){auto offset=path.find("&offset=");if(offset==std::string::npos)offset=path.find("?offset=");if(offset!=std::string::npos)path.resize(offset);return path;}
static std::string withOffset(const std::string& path,int offset){return pageBase(path)+(pageBase(path).find('?')==std::string::npos?"?":"&")+"offset="+std::to_string(offset);}
struct Job {std::string type,path;Json request;unsigned generation=0;};
struct Done {Job job;Json result;};
class Network {
    Config config;std::mutex mutex;std::condition_variable cv;
    std::deque<Job> pending;std::deque<Done> finished;bool stopped=false;std::thread worker;
public:
    explicit Network(Config c):config(std::move(c)),worker([this]{run();}){}
    ~Network() {{std::lock_guard guard(mutex);stopped=true;pending.clear();}cv.notify_all();worker.join();}
    bool submit(Job job) {std::lock_guard guard(mutex);if(pending.size()>=8)return false;pending.push_back(std::move(job));cv.notify_one();return true;}
    std::deque<Done> drain(){std::lock_guard guard(mutex);std::deque<Done> out;out.swap(finished);return out;}
    void run(){while(true){Job job;{std::unique_lock guard(mutex);cv.wait(guard,[&]{return stopped||!pending.empty();});if(stopped)return;job=std::move(pending.front());pending.pop_front();}
        Json result;try{result=job.path.empty()?control(config,job.request):api(config,job.path);}catch(const std::exception& e){result=error(e.what());}
        {std::lock_guard guard(mutex);finished.push_back({std::move(job),std::move(result)});}
    }}
};
static std::string timeText(double seconds){int s=std::max(0,int(seconds));return std::to_string(s/60)+":"+(s%60<10?"0":"")+std::to_string(s%60);}
class Library {
    Network& net;brls::AppletFrame* frame; brls::ScrollingFrame* scroll;brls::Box* body;
    brls::Label *notice,*track=nullptr,*progress=nullptr,*lyric=nullptr,*nearby=nullptr,*credit=nullptr;
    Json state,items=Json::array(),lines=Json::array();std::string currentPath,kind,lyricId,continuation;
    brls::Button* navigationFocus=nullptr;std::function<void()> navigation;
    unsigned generation=0;bool now=false,polling=false;Clock::time_point lastPoll{};
    brls::Button* button(brls::Box* box,const std::string& text,std::function<void()> fn) {
        auto* b=new brls::Button();b->setText(text);b->setHeight(62);b->setGrow(1);b->setMarginBottom(8);
        b->registerClickAction([this,fn](brls::View*){navigation=fn;return true;});box->addView(b);return b;
    }
    brls::Label* label(const std::string& text,float size=24){auto* l=new brls::Label();l->setText(text);l->setFontSize(size);l->setMarginBottom(16);body->addView(l);return l;}
    void clear(){brls::Application::giveFocus(navigationFocus);body->clearViews();track=progress=lyric=nearby=credit=nullptr;}
    void focus(){brls::Application::giveFocus(body);scroll->setContentOffsetY(0,false);}
    void command(Json j){if(!net.submit({"command","",std::move(j),generation}))notice->setText("Network queue busy; try again");}
public:
    explicit Library(Network& n):net(n) {
        auto* root=new brls::Box(brls::Axis::COLUMN);root->setPadding(20);
        auto* menu=new brls::Box(brls::Axis::ROW);menu->setHeight(70);
        for(auto k:{"songs","albums","playlists"}){auto* b=button(menu,k,[this,k]{browse("/v1/library/"+std::string(k),k);});if(!navigationFocus)navigationFocus=b;}
        button(menu,"Search",[this]{brls::Application::getPlatform()->getImeManager()->openForText([this](std::string text){if(!text.empty())browse("/v1/catalog/search?term="+encode(text),"catalog");},"Search Apple Music","",200,"");});
        button(menu,"Now playing",[this]{playing();});root->addView(menu);
        notice=new brls::Label();notice->setText("Connecting to your music server");notice->setFontSize(18);root->addView(notice);
        scroll=new brls::ScrollingFrame();scroll->setGrow(1);body=new brls::Box(brls::Axis::COLUMN);body->setPadding(16);scroll->setContentView(body);root->addView(scroll);
        frame=new brls::AppletFrame(root);frame->setTitle("SwitchMusic");brls::Application::pushActivity(new brls::Activity(frame));
        browse("/v1/library/songs","songs");
    }
    void browse(std::string path,std::string type) {
        now=false;++generation;currentPath=path;kind=type;clear();label("Loading your library…");focus();
        if(!net.submit({"page",path,{},generation})){clear();label("Network queue busy");button(body,"Retry",[this]{browse(currentPath,kind);});focus();}
    }
    void showPage(const Json& page) {
        clear();items=page.value("items",Json::array());
        if(kind=="tracks")button(body,"Back to collections",[this]{browse("/v1/library/albums","albums");});
        if(items.empty())label("No items in this collection.");
        continuation.clear();
        if(page.contains("next_offset")&&!page["next_offset"].is_null())continuation=withOffset(currentPath,page["next_offset"].get<int>());
        auto snapshot=items;auto nextPath=continuation;
        for(size_t i=0;i<items.size();++i){auto item=items[i];auto title=item.value("title",std::string("Untitled"));auto artist=item.value("artist",std::string{});
            button(body,title+(artist.empty()?"":"  ·  "+artist),[this,item,i,snapshot,nextPath]{
                auto type=item.value("type",std::string{});
                if(type=="songs"||type=="library-songs") {
                    Json queue=Json::array();size_t selected=0;
                    for(size_t x=0;x<snapshot.size();++x){auto t=snapshot[x].value("type",std::string{});if(t=="songs"||t=="library-songs"){if(x==i)selected=queue.size();queue.push_back({{"id",snapshot[x]["id"]},{"type",t}});}}
                    command({{"op","play"},{"queue",queue},{"index",selected},{"next_path",nextPath}});playing();
                }else if(type=="library-albums"||type=="library-playlists"||type=="albums"||type=="playlists") {
                    auto collection=(type=="library-albums"||type=="albums")?"albums":"playlists";
                    auto source=type.rfind("library-",0)==0?"library":"catalog";
                    browse("/v1/"+std::string(source)+"/"+std::string(collection)+"/"+encode(item.at("id"))+"/tracks","tracks");
                }else notice->setText("This item is not a playable song or collection");
            });
        }
        if(page.contains("next_offset")&&!page["next_offset"].is_null()) {
            auto base=pageBase(currentPath);auto next=page["next_offset"].get<int>();auto type=kind;
            button(body,"Next page →",[this,base,next,type]{browse(base+(base.find('?')==std::string::npos?"?":"&")+"offset="+std::to_string(next),type);});
        }
        if(currentPath.find('?')!=std::string::npos){auto base=pageBase(currentPath);auto type=kind;button(body,"First page",[this,base,type]{browse(base,type);});}
        focus();
    }
    void playing(){now=true;++generation;clear();track=label("Nothing playing",30);progress=label("0:00 / 0:00");
        button(body,"Play / Pause",[this]{command({{"op","toggle"}});});
        button(body,"Previous",[this]{command({{"op","previous"}});});button(body,"Next",[this]{command({{"op","next"}});});
        button(body,"Seek −15 seconds",[this]{command({{"op","seek"},{"seconds",std::max(0.0,state.value("position",0.0)-15)}});});
        button(body,"Seek +15 seconds",[this]{command({{"op","seek"},{"seconds",state.value("position",0.0)+15}});});
        button(body,"Volume −5",[this]{command({{"op","volume"},{"value",state.value("volume",70.0)-5}});});
        button(body,"Volume +5",[this]{command({{"op","volume"},{"value",state.value("volume",70.0)+5}});});
        button(body,"Stop",[this]{command({{"op","stop"}});});
        lyric=label("Synchronized lyrics",28);lyric->setTextColor(nvgRGB(250,55,90));nearby=label("");credit=label("syncedlyrics",16);
        lyricId.clear();lines=Json::array();focus();renderState();
    }
    void renderState(){
        notice->setText(state.contains("error")&&!state["error"].get<std::string>().empty()?state["error"].get<std::string>():state.value("status",std::string("Connecting…")));
        if(!now||!track)return;
        auto song=state.value("track",Json::object());track->setText(song.value("title",std::string("Nothing playing"))+"  ·  "+song.value("artist",std::string{}));
        progress->setText(timeText(state.value("position",0.0))+" / "+timeText(state.value("duration",0.0))+(state.value("paused",false)?"  Paused":"")+"  Volume "+std::to_string(int(state.value("volume",70.0)))+"%");
        auto id=song.value("id",std::string{});if(!id.empty()&&id!=lyricId){lyricId=id;lines=Json::array();lyric->setText("Loading lyrics…");net.submit({"lyrics","/v1/lyrics/"+encode(id),{},generation});}
        if(!lines.empty()) {long ms=long(state.value("position",0.0)*1000);int active=-1;
            for(size_t i=0;i<lines.size();++i){if(lines[i].value("time_ms",0L)<=ms)active=i;else break;}
            lyric->setText(active>=0?lines[active].value("text",std::string{}):"♪");
            nearby->setText(active+1<int(lines.size())?lines[active+1].value("text",std::string{}):"");
        }
    }
    void tick(){
        if(navigation){auto action=std::move(navigation);navigation={};action();}
        for(auto& d:net.drain()) {
            if(d.job.type=="state"){polling=false;state=d.result;renderState();}
            else if(d.job.type=="command"){if(d.result.contains("error"))notice->setText(d.result["error"]);}
            else if(d.job.generation==generation) {
                if(d.result.contains("error")) {if(d.job.type=="lyrics"&&lyric)lyric->setText(d.result["error"]);else{clear();label(d.result["error"]);button(body,"Retry",[this]{browse(currentPath,kind);});focus();}}
                else if(d.job.type=="page")showPage(d.result);
                else if(d.job.type=="lyrics"&&lyric&&d.job.path=="/v1/lyrics/"+encode(lyricId)){lines=d.result.value("lines",Json::array());credit->setText(d.result.value("provider",std::string("syncedlyrics"))+"  "+d.result.value("copyright",std::string{}));if(lines.empty())lyric->setText("No synchronized lyrics available");renderState();}
            }
        }
        auto current=Clock::now();if(!polling&&current-lastPoll>std::chrono::milliseconds(500)){lastPoll=current;polling=net.submit({"state","",{{"op","state"}},generation});}
    }
};
int main(){
    curl_global_init(CURL_GLOBAL_DEFAULT);
    if(!brls::Application::init())return 1;
    brls::Application::createWindow("SwitchMusic");brls::Application::getPlatform()->setThemeVariant(brls::ThemeVariant::DARK);
    brls::Application::setGlobalQuit(true);
    try {Network network(Config::load());Library library(network);while(brls::Application::mainLoop())library.tick();}
    catch(const std::exception& e){auto* l=new brls::Label();l->setText(e.what());auto* frame=new brls::AppletFrame(l);frame->setTitle("SwitchMusic setup");brls::Application::pushActivity(new brls::Activity(frame));while(brls::Application::mainLoop()){} }
    curl_global_cleanup();return 0;
}
