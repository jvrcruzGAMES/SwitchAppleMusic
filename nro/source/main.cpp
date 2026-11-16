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
#include <cmath>
using namespace smusic;
using Clock=std::chrono::steady_clock;
static std::string pageBase(std::string path){auto offset=path.find("&offset=");if(offset==std::string::npos)offset=path.find("?offset=");if(offset!=std::string::npos)path.resize(offset);return path;}
static std::string withOffset(const std::string& path,int offset){return pageBase(path)+(pageBase(path).find('?')==std::string::npos?"?":"&")+"offset="+std::to_string(offset);}
class QRView:public brls::Box {
    std::vector<std::string> rows;
public:
    explicit QRView(const Json& matrix):brls::Box(brls::Axis::COLUMN){
        if(!matrix.is_array()||matrix.size()<29||matrix.size()>185)throw std::runtime_error("Invalid server QR matrix");
        for(const auto& value:matrix){
            if(!value.is_string())throw std::runtime_error("Invalid server QR row");auto row=value.get<std::string>();
            if(row.size()!=matrix.size()||row.find_first_not_of("01")!=std::string::npos)throw std::runtime_error("Invalid server QR row");rows.push_back(std::move(row));
        }
        setWidth(340);setHeight(340);setMarginBottom(12);
    }
    void draw(NVGcontext* vg,float x,float y,float width,float height,brls::Style,brls::FrameContext*) override{
        float cell=std::floor(std::min(width,height)/rows.size());if(cell<1)return;
        float left=std::round(x+(width-cell*rows.size())/2),top=std::round(y+(height-cell*rows.size())/2);
        nvgSave(vg);nvgShapeAntiAlias(vg,0);nvgBeginPath(vg);nvgRect(vg,left,top,cell*rows.size(),cell*rows.size());nvgFillColor(vg,nvgRGB(255,255,255));nvgFill(vg);
        nvgBeginPath(vg);for(size_t row=0;row<rows.size();++row)for(size_t col=0;col<rows.size();++col)if(rows[row][col]=='1')nvgRect(vg,left+col*cell,top+row*cell,cell,cell);
        nvgFillColor(vg,nvgRGB(0,0,0));nvgFill(vg);nvgRestore(vg);
    }
};
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
    void configure(Config c){std::lock_guard guard(mutex);config=std::move(c);pending.erase(std::remove_if(pending.begin(),pending.end(),[](const Job& job){return job.type!="logout"&&job.type!="qr_cancel";}),pending.end());finished.clear();}
    void run(){while(true){Job job;Config active;{std::unique_lock guard(mutex);cv.wait(guard,[&]{return stopped||!pending.empty();});if(stopped)return;job=std::move(pending.front());pending.pop_front();active=(job.type=="logout"||job.type=="qr_cancel")?job.credentials:config;}
        Json result;try{
            if(job.type=="login")result=signIn(active,job.request.at("password"));
            else if(job.type=="logout"){signOut(active);result=Json::object();}
            else if(job.type.rfind("qr_",0)==0)result=post(active,job.path,job.request);
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
    Json currentUser;bool inSettings=false,qrActive=false,qrPending=false;std::string qrCode;Clock::time_point qrUntil{},lastQR{};brls::Label* qrTimer=nullptr;
    Json state,items=Json::array(),lines=Json::array();std::string currentPath,kind,lyricId,continuation;
    std::vector<brls::Button*> personalButtons;
    brls::Button* navigationFocus=nullptr;std::function<void()> navigation;
    unsigned generation=0,authGeneration=0;bool now=false,polling=false;Clock::time_point lastPoll{};
    brls::Button* button(brls::Box* box,const std::string& text,std::function<void()> fn) {
        auto* b=new brls::Button();b->setText(text);b->setHeight(62);b->setGrow(1);b->setMarginBottom(8);
        b->registerClickAction([this,fn](brls::View*){navigation=fn;return true;});box->addView(b);return b;
    }
    brls::Label* label(const std::string& text,float size=24){auto* l=new brls::Label();l->setText(text);l->setFontSize(size);l->setMarginBottom(16);body->addView(l);return l;}
    void clear(){brls::Application::giveFocus(navigationFocus);body->clearViews();track=progress=lyric=nearby=credit=nullptr;qrTimer=nullptr;}
    void focus(){brls::Application::giveFocus(body);scroll->setContentOffsetY(0,false);}
    void command(Json j){if(!net.submit({"command","",std::move(j),generation}))notice->setText("Network queue busy; try again");}
public:
    explicit Library(Network& n):net(n) {
        auto* root=new brls::Box(brls::Axis::COLUMN);root->setPadding(20);
        auto* account=new brls::Box(brls::Axis::ROW);account->setHeight(60);
        navigationFocus=button(account,"Account",[this]{if(loggedIn)settings();else loginScreen();});root->addView(account);
        auto* menu=new brls::Box(brls::Axis::ROW);menu->setHeight(70);libraryMenu=menu;
        for(auto k:{"songs","albums","playlists"}){auto* b=button(menu,k,[this,k]{browse("/v1/library/"+std::string(k),k);});personalButtons.push_back(b);}
        button(menu,"Search",[this]{brls::Application::getPlatform()->getImeManager()->openForText([this](std::string text){if(!text.empty())browse("/v1/catalog/search?term="+encode(text),"catalog");},"Search Apple Music","",200,"");});
        button(menu,"Now playing",[this]{playing();});root->addView(menu);
        notice=new brls::Label();notice->setText("Connecting to your music server");notice->setFontSize(18);root->addView(notice);
        scroll=new brls::ScrollingFrame();scroll->setGrow(1);body=new brls::Box(brls::Axis::COLUMN);body->setPadding(16);scroll->setContentView(body);root->addView(scroll);
        frame=new brls::AppletFrame(root);frame->setTitle("SwitchMusic");brls::Application::pushActivity(new brls::Activity(frame));
        try{config=Config::load(false);}catch(...){}
        net.configure(config);
        if(!config.session.empty()){checking=true;authGeneration=++generation;authPending=net.submit({"auth","/auth/me",{},generation});clear();label("Checking your saved login…");focus();}
        else if(!config.backend.empty())guest();else loginScreen();
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
        endQR();inSettings=false;
        loggedIn=false;checking=false;authPending=false;polling=false;now=false;++generation;
        navigationFocus->setText("Account");libraryMenu->setVisibility(brls::Visibility::GONE);clear();
        notice->setText(message.empty()?"Sign in with the account created by your server administrator":message);
        label("Sign in to SwitchMusic",32);
        button(body,"Server URL: "+(config.backend.empty()?std::string("Enter address"):config.backend),[this]{keyboard("Server URL",config.backend,1024);});
        button(body,"Username: "+(config.username.empty()?std::string("Enter username"):config.username),[this]{keyboard("Username",config.username,32);});
        button(body,password.empty()?"Password: Enter password":"Password: ••••••••",[this]{keyboard("Account password",password,256,true);});
        button(body,"Sign in",[this]{login();});
        button(body,"Browse without signing in",[this]{guest();});
        label("After signing in, link Apple Music in Settings with a QR code or on the server website. Catalog playback does not require a library link.",18);focus();
    }
    void guest(){
        try{config.validate(false);}catch(const std::exception& e){notice->setText(e.what());return;}
        endQR();config.session.clear();password.clear();loggedIn=false;checking=false;authPending=false;
        inSettings=false;now=false;polling=false;++generation;currentUser=Json::object();
        net.configure(config);try{config.save();}catch(const std::exception& e){notice->setText(e.what());return;}
        navigationFocus->setText("Account");libraryMenu->setVisibility(brls::Visibility::VISIBLE);
        for(auto* b:personalButtons)b->setVisibility(brls::Visibility::GONE);clear();
        notice->setText("Browsing the public Apple Music catalog");label("Apple Music catalog",32);
        label("Search songs, albums and playlists without signing in. Sign in to play music or access your personal library.",20);
        button(body,"Search catalog",[this]{brls::Application::getPlatform()->getImeManager()->openForText([this](std::string text){if(!text.empty())browse("/v1/catalog/search?term="+encode(text),"catalog");},"Search Apple Music","",200,"");});
        button(body,"Sign in",[this]{loginScreen();});focus();
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
        endQR();inSettings=false;
        ++generation;net.configure(config);net.submit({"logout","",{},generation,config});
        config.session.clear();password.clear();
        try{Config::clear();}catch(const std::exception& e){loginScreen(e.what());return;}
        loginScreen("Signed out");
    }
    void authenticated(const Json& result,bool fresh){
        authPending=false;checking=false;
        if(fresh){config.session=result.at("session");try{config.save();}catch(const std::exception& e){config.session.clear();loginScreen(e.what());return;}net.configure(config);}
        loggedIn=true;currentUser=result.at("user");navigationFocus->setText("Settings");lastAuth=Clock::now();libraryMenu->setVisibility(brls::Visibility::VISIBLE);
        for(auto* b:personalButtons)b->setVisibility(currentUser.value("apple_linked",false)?brls::Visibility::VISIBLE:brls::Visibility::GONE);
        notice->setText("Signed in as "+config.username);
        if(result.at("user").value("apple_linked",false))browse("/v1/library/songs","songs");
        else{clear();label("Search the Apple Music catalog",30);label("Your Apple Music library is not linked. Open Settings to link it with a QR code, or use the server website.",20);button(body,"Settings",[this]{settings();});button(body,"Search catalog",[this]{brls::Application::getPlatform()->getImeManager()->openForText([this](std::string text){if(!text.empty())browse("/v1/catalog/search?term="+encode(text),"catalog");},"Search Apple Music","",200,"");});focus();}
    }
    void endQR(){
        if(!qrCode.empty()&&!config.session.empty())net.submit({"qr_cancel","/auth/apple/qr/cancel",{{"code",qrCode}},generation,config});
        qrActive=qrPending=false;qrCode.clear();qrTimer=nullptr;
    }
    void settings(){
        if(!loggedIn)return;endQR();inSettings=true;now=false;++generation;clear();label("Loading settings…");focus();
        if(!net.submit({"settings","/auth/me",{},generation})){showSettings(currentUser,"Network queue busy; try again");}
    }
    void showSettings(const Json& user,const std::string& message=""){
        currentUser=user;for(auto* b:personalButtons)b->setVisibility(user.value("apple_linked",false)?brls::Visibility::VISIBLE:brls::Visibility::GONE);clear();notice->setText(message.empty()?"Manage your Apple Music library connection":message);
        label("Settings",32);label("Server: "+config.backend,18);label("Backend account: "+config.username,18);
        bool linked=user.value("apple_linked",false);label(linked?"Apple Music library connected":"Apple Music library not linked",24);
        button(body,linked?"Relink Apple Music with QR code":"Link Apple Music with QR code",[this]{startQR();});
        label("Scan with your phone and authorize Apple Music on the server website. Sign in there with the same backend account.",18);
        button(body,"Browse music",[this]{browse("/v1/library/songs","songs");});button(body,"Sign out of backend account",[this]{logout();});focus();
    }
    void startQR(){
        endQR();inSettings=true;now=false;qrActive=true;++generation;clear();label("Requesting Apple Music QR code…",26);
        button(body,"Cancel",[this]{settings();});focus();
        if(!net.submit({"qr_start","/auth/apple/qr/start",Json::object(),generation,config})){qrActive=false;showSettings(currentUser,"Network queue busy; try again");}
    }
    void showQR(const Json& result){
        auto code=result.at("code").get<std::string>(),confirmation=result.at("confirmation").get<std::string>();
        int expires=std::clamp(result.value("expires_in",300),1,300);
        if(code.size()!=43||code.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-")!=std::string::npos||confirmation.size()!=7)
            throw std::runtime_error("Invalid server QR request");
        auto* qr=new QRView(result.at("matrix"));qrCode=std::move(code);
        qrUntil=Clock::now()+std::chrono::seconds(expires);lastQR=Clock::now();
        clear();notice->setText("Scan this code to link your Apple Music library");label("Apple Music",30);
        auto* row=new brls::Box(brls::Axis::ROW);row->setHeight(350);row->addView(qr);body->addView(row);
        auto* instructions=new brls::Box(brls::Axis::COLUMN);instructions->setPadding(20);instructions->setGrow(1);
        auto add=[&](const std::string& text,float size){auto* item=new brls::Label();item->setText(text);item->setFontSize(size);item->setMarginBottom(16);instructions->addView(item);return item;};
        add("Match this code on your phone:",22);add(confirmation,36);add("Sign in as "+config.username+" on the website, then authorize Apple Music.",20);
        qrTimer=add("Expires in 5:00",20);row->addView(instructions);
        button(body,"Cancel",[this]{settings();});focus();
    }
    void browse(std::string path,std::string type) {
        if(path.rfind("/v1/library/",0)==0){
            if(!loggedIn){loginScreen("Sign in to access your personal library. Public catalog browsing is available without login.");return;}
            if(!currentUser.value("apple_linked",false)){settings();return;}
        }
        endQR();inSettings=false;now=false;++generation;currentPath=path;kind=type;clear();label("Loading music…");focus();
        if(!net.submit({"page",path,{},generation})){clear();label("Network queue busy");button(body,"Retry",[this]{browse(currentPath,kind);});focus();}
    }
    void showPage(const Json& page) {
        clear();items=page.value("items",Json::array());
        if(kind=="tracks"&&currentPath.rfind("/v1/library/",0)==0)button(body,"Back to collections",[this]{browse("/v1/library/albums","albums");});
        if(items.empty())label("No items in this collection.");
        continuation.clear();
        if(page.contains("next_offset")&&!page["next_offset"].is_null())continuation=withOffset(currentPath,page["next_offset"].get<int>());
        auto snapshot=items;auto nextPath=continuation;
        for(size_t i=0;i<items.size();++i){auto item=items[i];auto title=item.value("title",std::string("Untitled"));auto artist=item.value("artist",std::string{});
            button(body,title+(artist.empty()?"":"  ·  "+artist),[this,item,i,snapshot,nextPath]{
                auto type=item.value("type",std::string{});
                if(type=="songs"||type=="library-songs") {
                    if(!loggedIn){loginScreen("Sign in to play music. You can browse the catalog without signing in.");return;}
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
    void playing(){if(!loggedIn){loginScreen("Sign in to use playback controls.");return;}endQR();inSettings=false;now=true;++generation;clear();track=label("Nothing playing",30);progress=label("0:00 / 0:00");
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
            if(d.job.generation!=generation){
                if(d.job.type=="qr_start"&&d.result.contains("code"))net.submit({"qr_cancel","/auth/apple/qr/cancel",{{"code",d.result["code"]}},generation,d.job.credentials});
                continue;
            }
            if(d.job.type=="logout"||d.job.type=="qr_cancel")continue;
            if(d.result.value("http_status",0)==401 && d.job.type!="login"){
                config.session.clear();password.clear();try{Config::clear();}catch(...){}
                net.configure(config);loginScreen("Your login expired or was revoked. Please sign in again.");continue;
            }
            if(d.job.type=="settings"||d.job.type=="qr_start"||d.job.type=="qr_status"){
                if(d.job.type=="qr_status")qrPending=false;
                if(d.result.contains("error")){
                    if(d.job.type=="qr_status"&&d.result.value("http_status",0)==429)continue;
                    endQR();showSettings(currentUser,d.result["error"]);
                }else try{
                    if(d.job.type=="settings")showSettings(d.result.at("user"));
                    else if(d.job.type=="qr_start")showQR(d.result);
                    else if(d.result.value("status",std::string{})=="linked"){
                        qrActive=qrPending=false;qrCode.clear();showSettings(d.result.at("user"),"Apple Music connected. You can browse your library now.");
                    }
                }catch(const std::exception& e){endQR();showSettings(currentUser,e.what());}
                continue;
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
        auto current=Clock::now();
        if(qrActive&&!qrCode.empty()){
            if(current>=qrUntil){endQR();showSettings(currentUser,"QR code expired. Request another to continue.");}
            else{
                if(qrTimer)qrTimer->setText("Expires in "+timeText(std::chrono::duration<double>(qrUntil-current).count()));
                if(!qrPending&&current-lastQR>std::chrono::seconds(3)){lastQR=current;qrPending=net.submit({"qr_status","/auth/apple/qr/status",{{"code",qrCode}},generation});}
            }
        }
        if(loggedIn&&!authPending&&current-lastAuth>std::chrono::seconds(30)){lastAuth=current;authGeneration=generation;authPending=net.submit({"auth","/auth/me",{},generation});}
        if(loggedIn&&!inSettings&&!polling&&current-lastPoll>std::chrono::milliseconds(500)){lastPoll=current;polling=net.submit({"state","",{{"op","state"}},generation});}
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
