#include <borealis.hpp>
#include <switchmusic/client.hpp>
#include <curl/curl.h>
#include <switch.h>
#include <thread>
#include <mutex>
#include <deque>
#include <condition_variable>
#include <chrono>
#include <sys/stat.h>
using namespace smusic;
using Clock=std::chrono::steady_clock;
static std::string pageBase(std::string path){auto offset=path.find("&offset=");if(offset==std::string::npos)offset=path.find("?offset=");if(offset!=std::string::npos)path.resize(offset);return path;}
static std::string withOffset(const std::string& path,int offset){return pageBase(path)+(pageBase(path).find('?')==std::string::npos?"?":"&")+"offset="+std::to_string(offset);}
struct Job {std::string type,path;Json request;unsigned generation=0;Config credentials{};};
struct Done {Job job;Json result;};
class Network {
    Config config;std::mutex mutex;std::condition_variable cv;
    std::deque<Job> pending;std::deque<Done> finished;bool stopped=false;std::thread worker;
public:
    explicit Network(Config c):config(std::move(c)),worker([this]{run();}){}
    ~Network() {{std::lock_guard guard(mutex);stopped=true;pending.clear();}cv.notify_all();worker.join();}
    bool submit(Job job) {std::lock_guard guard(mutex);if(pending.size()>=8)return false;pending.push_back(std::move(job));cv.notify_one();return true;}
    std::deque<Done> drain(){std::lock_guard guard(mutex);std::deque<Done> out;out.swap(finished);return out;}
    void configure(Config c){std::lock_guard guard(mutex);config=std::move(c);pending.erase(std::remove_if(pending.begin(),pending.end(),[](const Job& job){return job.type!="logout";}),pending.end());finished.clear();}
    void run(){while(true){Job job;Config active;{std::unique_lock guard(mutex);cv.wait(guard,[&]{return stopped||!pending.empty();});if(stopped)return;job=std::move(pending.front());pending.pop_front();active=job.type=="logout"?job.credentials:config;}
        Json result;try{
            if(job.type=="login")result=signIn(active,job.request.at("password"));
            else if(job.type=="logout"){signOut(active);result=Json::object();}
            else result=job.path.empty()?control(active,job.request):api(active,job.path);
            if(job.type=="login"||job.type=="auth"){
                if(!result.is_object()||!result.contains("user")||!result["user"].is_object()||
                   !result["user"].contains("username")||result["user"]["username"]!=active.username||
                   (result["user"].contains("apple_linked")&&!result["user"]["apple_linked"].is_boolean()))
                    throw std::runtime_error("This address did not return a valid SwitchMusic login");
            }
        }catch(const HttpError& e){result=error(e.what());result["http_status"]=e.status;}
        catch(const std::exception& e){result=error(e.what());}
        {std::lock_guard guard(mutex);finished.push_back({std::move(job),std::move(result)});}
    }}
};
static std::string timeText(double seconds){int s=std::max(0,int(seconds));return std::to_string(s/60)+":"+(s%60<10?"0":"")+std::to_string(s%60);}
class Library {
    Network& net;Config config;std::string password;bool loggedIn=false,authPending=false,checking=false;Clock::time_point lastAuth{};brls::Box* libraryMenu; brls::AppletFrame* frame; brls::ScrollingFrame* scroll;brls::Box* body;
    brls::Label *notice,*track=nullptr,*progress=nullptr,*lyric=nullptr,*nearby=nullptr,*credit=nullptr;
    Json state,items=Json::array(),lines=Json::array();std::string currentPath,kind,lyricId,continuation;
    brls::Button* navigationFocus=nullptr;std::function<void()> navigation;
    unsigned generation=0,authGeneration=0;bool now=false,polling=false;Clock::time_point lastPoll{};
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
        auto* account=new brls::Box(brls::Axis::ROW);account->setHeight(60);
        navigationFocus=button(account,"Account",[this]{if(loggedIn)logout();else loginScreen();});root->addView(account);
        auto* menu=new brls::Box(brls::Axis::ROW);menu->setHeight(70);libraryMenu=menu;
        for(auto k:{"songs","albums","playlists"}){auto* b=button(menu,k,[this,k]{browse("/v1/library/"+std::string(k),k);});if(!navigationFocus)navigationFocus=b;}
        button(menu,"Search",[this]{if(!loggedIn)return;brls::Application::getPlatform()->getImeManager()->openForText([this](std::string text){if(!text.empty())browse("/v1/catalog/search?term="+encode(text),"catalog");},"Search Apple Music","",200,"");});
        button(menu,"Now playing",[this]{playing();});root->addView(menu);
        notice=new brls::Label();notice->setText("Connecting to your music server");notice->setFontSize(18);root->addView(notice);
        scroll=new brls::ScrollingFrame();scroll->setGrow(1);body=new brls::Box(brls::Axis::COLUMN);body->setPadding(16);scroll->setContentView(body);root->addView(scroll);
        frame=new brls::AppletFrame(root);frame->setTitle("SwitchMusic");brls::Application::pushActivity(new brls::Activity(frame));
        try{config=Config::load(false);}catch(...){}
        net.configure(config);
        if(!config.session.empty()){checking=true;authGeneration=++generation;authPending=net.submit({"auth","/auth/me",{},generation});clear();label("Checking your saved login…");focus();}
        else loginScreen();
    }
    void keyboard(const std::string& title,std::string& value,int limit,bool secret=false){
        SwkbdConfig keyboard{};auto rc=swkbdCreate(&keyboard,0);
        if(R_FAILED(rc)){notice->setText("Nintendo Switch keyboard could not open");return;}
        swkbdConfigMakePresetDefault(&keyboard);swkbdConfigSetHeaderText(&keyboard,title.c_str());
        swkbdConfigSetStringLenMax(&keyboard,limit);swkbdConfigSetInitialText(&keyboard,value.c_str());
        swkbdConfigSetPasswordFlag(&keyboard,secret);swkbdConfigSetBlurBackground(&keyboard,true);
        std::vector<char> text(size_t(limit)*4+1);
        rc=swkbdShow(&keyboard,text.data(),text.size());swkbdClose(&keyboard);
        if(R_SUCCEEDED(rc)){value=text.data();loginScreen();}
    }
    void loginScreen(const std::string& message=""){
        loggedIn=false;checking=false;authPending=false;polling=false;now=false;++generation;
        navigationFocus->setText("Account");libraryMenu->setVisibility(brls::Visibility::GONE);clear();
        notice->setText(message.empty()?"Sign in with the account created by your server administrator":message);
        label("Sign in to SwitchMusic",32);
        button(body,"Server URL: "+(config.backend.empty()?std::string("Enter address"):config.backend),[this]{keyboard("Server URL",config.backend,1024);});
        button(body,"Username: "+(config.username.empty()?std::string("Enter username"):config.username),[this]{keyboard("Username",config.username,32);});
        button(body,password.empty()?"Password: Enter password":"Password: ••••••••",[this]{keyboard("Account password",password,256,true);});
        button(body,"Sign in",[this]{login();});
        label("Link your Apple Music library on the server website. Catalog playback does not require a library link.",18);focus();
    }
    void login(){
        if(authPending)return;
        try{config.validate(false);if(password.empty())throw std::runtime_error("Enter your account password");}
        catch(const std::exception& e){notice->setText(e.what());return;}
        net.configure(config);authGeneration=++generation;
        authPending=net.submit({"login","",{{"password",password}},generation});
        password.clear();notice->setText(authPending?"Signing in…":"Network queue busy; try again");
    }
    void logout(){
        ++generation;net.configure(config);net.submit({"logout","",{},generation,config});
        config.session.clear();password.clear();
        try{Config::clear();}catch(const std::exception& e){loginScreen(e.what());return;}
        loginScreen("Signed out");
    }
    void authenticated(const Json& result,bool fresh){
        authPending=false;checking=false;
        if(fresh){config.session=result.at("session");try{config.save();}catch(const std::exception& e){config.session.clear();loginScreen(e.what());return;}net.configure(config);}
        loggedIn=true;navigationFocus->setText("Sign out");lastAuth=Clock::now();libraryMenu->setVisibility(brls::Visibility::VISIBLE);
        notice->setText("Signed in as "+config.username);
        if(result.at("user").value("apple_linked",false))browse("/v1/library/songs","songs");
        else{clear();label("Search the Apple Music catalog",30);label("Your Apple Music library is not linked. Link it on the server website to browse personal songs, albums and playlists.",20);button(body,"Search catalog",[this]{brls::Application::getPlatform()->getImeManager()->openForText([this](std::string text){if(!text.empty())browse("/v1/catalog/search?term="+encode(text),"catalog");},"Search Apple Music","",200,"");});focus();}
    }
    void browse(std::string path,std::string type) {
        if(!loggedIn)return;
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
    void playing(){if(!loggedIn)return;now=true;++generation;clear();track=label("Nothing playing",30);progress=label("0:00 / 0:00");
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
            if(d.job.type=="state")polling=false;
            if((d.job.type=="auth"||d.job.type=="login")&&d.job.generation==authGeneration)authPending=false;
            if(d.job.generation!=generation)continue;
            if(d.job.type=="logout")continue;
            if(d.result.value("http_status",0)==401 && d.job.type!="login"){
                config.session.clear();password.clear();try{Config::clear();}catch(...){}
                net.configure(config);loginScreen("Your login expired or was revoked. Please sign in again.");continue;
            }
            if(d.job.type=="login"||d.job.type=="auth"){
                authPending=false;
                if(d.result.contains("error")){
                    if(d.job.type=="auth"&&!checking){notice->setText(d.result["error"]);lastAuth=Clock::now();}
                    else loginScreen(d.result["error"]);
                }else if(d.job.type=="login"||checking)authenticated(d.result,d.job.type=="login");
                continue;
            }
            if(d.job.type=="state"){polling=false;state=d.result;renderState();}
            else if(d.job.type=="command"){if(d.result.contains("error"))notice->setText(d.result["error"]);}
            else if(d.job.generation==generation) {
                if(d.result.contains("error")) {if(d.job.type=="lyrics"&&lyric)lyric->setText(d.result["error"]);else{clear();label(d.result["error"]);button(body,"Retry",[this]{browse(currentPath,kind);});focus();}}
                else if(d.job.type=="page")showPage(d.result);
                else if(d.job.type=="lyrics"&&lyric&&d.job.path=="/v1/lyrics/"+encode(lyricId)){lines=d.result.value("lines",Json::array());credit->setText(d.result.value("provider",std::string("syncedlyrics"))+"  "+d.result.value("copyright",std::string{}));if(lines.empty())lyric->setText("No synchronized lyrics available");renderState();}
            }
        }
        auto current=Clock::now();if(loggedIn&&!authPending&&current-lastAuth>std::chrono::seconds(30)){lastAuth=current;authGeneration=generation;authPending=net.submit({"auth","/auth/me",{},generation});}
        if(loggedIn&&!polling&&current-lastPoll>std::chrono::milliseconds(500)){lastPoll=current;polling=net.submit({"state","",{{"op","state"}},generation});}
    }
};
int main(){
    curl_global_init(CURL_GLOBAL_DEFAULT);
    if(!brls::Application::init())return 1;
    brls::Application::createWindow("SwitchMusic");brls::Application::getPlatform()->setThemeVariant(brls::ThemeVariant::DARK);
    brls::Application::setGlobalQuit(true);
    try {mkdir("sdmc:/config",0777);mkdir("sdmc:/config/switchmusic",0777);Network network(Config{});Library library(network);while(brls::Application::mainLoop())library.tick();}
    catch(const std::exception& e){auto* l=new brls::Label();l->setText(e.what());auto* frame=new brls::AppletFrame(l);frame->setTitle("SwitchMusic setup");brls::Application::pushActivity(new brls::Activity(frame));while(brls::Application::mainLoop()){} }
    curl_global_cleanup();return 0;
}
