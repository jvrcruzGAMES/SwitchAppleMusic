#include <borealis.hpp>
#include <switchmusic/client.hpp>
#include <curl/curl.h>
#ifdef __SWITCH__
#include <switch.h>
#endif
#include <optional>
#include <atomic>
#include <unordered_map>
#include <borealis/core/touch/tap_gesture.hpp>
#include <thread>
#include <mutex>
#include <deque>
#include <condition_variable>
#include <chrono>
#include <sys/stat.h>
#include <cmath>
using namespace smusic;
using Clock = std::chrono::steady_clock;
static std::string pageBase(std::string path) {
    auto offset = path.find("&offset=");
    if (offset == std::string::npos) offset = path.find("?offset=");
    if (offset != std::string::npos) path.resize(offset);
    return path;
}
static std::string withOffset(const std::string &path, int offset) {
    return pageBase(path) + (pageBase(path).find('?') == std::string::npos ? "?" : "&") +
           "offset=" + std::to_string(offset);
}
class QRView : public brls::Box {
    std::vector<std::string> rows;

  public:
    QRView() : brls::Box(brls::Axis::COLUMN) {}
    void setMatrix(const Json &matrix) {
        rows.clear();
        if (!matrix.is_array() || matrix.size() < 29 || matrix.size() > 185)
            throw std::runtime_error("Invalid server QR matrix");
        for (const auto &value : matrix) {
            if (!value.is_string()) throw std::runtime_error("Invalid server QR row");
            auto row = value.get<std::string>();
            if (row.size() != matrix.size() || row.find_first_not_of("01") != std::string::npos)
                throw std::runtime_error("Invalid server QR row");
            rows.push_back(std::move(row));
        }
    }
    void draw(NVGcontext *vg, float x, float y, float width, float height, brls::Style,
              brls::FrameContext *) override {
        float cell = std::floor(std::min(width, height) / rows.size());
        if (cell < 1) return;
        float left = std::round(x + (width - cell * rows.size()) / 2),
              top = std::round(y + (height - cell * rows.size()) / 2);
        nvgSave(vg);
        nvgShapeAntiAlias(vg, 0);
        nvgBeginPath(vg);
        nvgRect(vg, left, top, cell * rows.size(), cell * rows.size());
        nvgFillColor(vg, nvgRGB(255, 255, 255));
        nvgFill(vg);
        nvgBeginPath(vg);
        for (size_t row = 0; row < rows.size(); ++row)
            for (size_t col = 0; col < rows.size(); ++col)
                if (rows[row][col] == '1') nvgRect(vg, left + col * cell, top + row * cell, cell, cell);
        nvgFillColor(vg, nvgRGB(0, 0, 0));
        nvgFill(vg);
        nvgRestore(vg);
    }
};
// Vector transport glyphs stay sharp at any UI scale and need no bitmap assets.
// XML owns the buttons, sizing and focus; code only draws the changing glyph.
class TransportIcon : public brls::View {
    std::string icon = "play";
  public:
    TransportIcon() {
        registerStringXMLAttribute("icon", [this](const std::string& value) { icon = value; });
    }
    void setPlaying(bool playing) { icon = playing ? "pause" : "play"; }
    bool isPlaying() const { return icon == "pause"; }
    void draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style,
              brls::FrameContext*) override {
        nvgSave(vg);
        float size = std::min(width, height);
        nvgTranslate(vg, x + (width-size)/2, y + (height-size)/2);
        nvgScale(vg, size/24, size/24);
        nvgBeginPath(vg);
        if (icon == "pause") {
            nvgRect(vg, 5, 4, 5, 16); nvgRect(vg, 14, 4, 5, 16);
        } else if (icon == "previous") {
            nvgRect(vg, 4, 4, 3, 16);
            nvgMoveTo(vg, 19, 4); nvgLineTo(vg, 8, 12); nvgLineTo(vg, 19, 20); nvgClosePath(vg);
        } else if (icon == "next") {
            nvgRect(vg, 17, 4, 3, 16);
            nvgMoveTo(vg, 5, 4); nvgLineTo(vg, 16, 12); nvgLineTo(vg, 5, 20); nvgClosePath(vg);
        } else {
            nvgMoveTo(vg, 6, 3); nvgLineTo(vg, 21, 12); nvgLineTo(vg, 6, 21); nvgClosePath(vg);
        }
        nvgFillColor(vg, nvgRGB(244, 245, 247)); nvgFill(vg); nvgRestore(vg);
    }
};
struct Job {
    std::string type, path;
    Json request;
    unsigned generation = 0;
    Config credentials{};
};
struct Done {
    Job job;
    Json result;
};
class Network {
    std::atomic<unsigned> artworkGeneration{0};
    Config config;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Job> pending;
    std::deque<Done> finished;
    bool stopped = false;
    std::thread worker;

  public:
    explicit Network(Config c) : config(std::move(c)), worker([this] { run(); }) {}
    ~Network() {
        {
            std::lock_guard guard(mutex);
            stopped = true;
            ++artworkGeneration;
            pending.clear();
        }
        cv.notify_all();
        worker.join();
    }
    bool submit(Job job) {
        std::lock_guard guard(mutex);
        if (pending.size() >= 8) return false;
        pending.push_back(std::move(job));
        cv.notify_one();
        return true;
    }
    std::deque<Done> drain() {
        std::lock_guard guard(mutex);
        std::deque<Done> out;
        out.swap(finished);
        return out;
    }
    void configure(Config c) {
        std::lock_guard guard(mutex);
        config = std::move(c);
        pending.erase(
            std::remove_if(pending.begin(), pending.end(),
                           [](const Job &job) { return job.type != "logout" && job.type != "qr_cancel"; }),
            pending.end());
        finished.clear();
    }
    void cancelArtwork(unsigned generation) {
        artworkGeneration = generation;
        std::lock_guard guard(mutex);
        pending.erase(std::remove_if(pending.begin(), pending.end(),
                                     [&](const Job &job) {
                                         return job.type == "artwork" && job.generation != generation;
                                     }),
                      pending.end());
    }
    void run() {
        while (true) {
            Job job;
            Config active;
            {
                std::unique_lock guard(mutex);
                cv.wait(guard, [&] { return stopped || !pending.empty(); });
                if (stopped) return;
                job = std::move(pending.front());
                pending.pop_front();
                active = (job.type == "logout" || job.type == "qr_cancel") ? job.credentials : config;
            }
            Json result;
            try {
                if (job.type == "login")
                    result = signIn(active, job.request.at("password"));
                else if (job.type == "artwork") {
                    auto bytes = apiBytes(active, job.path, 256 * 1024, [this, epoch = job.generation] {
                        return artworkGeneration.load() != epoch;
                    });
                    result = {{"bytes", Json::binary(std::move(bytes))}};
                } else if (job.type == "logout") {
                    signOut(active);
                    result = Json::object();
                } else if (job.type.rfind("qr_", 0) == 0)
                    result = post(active, job.path, job.request);
                else
                    // Playback commands use Horizon IPC; paths use backend HTTP.
                    result = job.path.empty() ? control(active, job.request) : api(active, job.path);
                if (job.type == "login" || job.type == "auth") {
                    if (!result.is_object() || !result.contains("user") || !result["user"].is_object() ||
                        !result["user"].contains("username") ||
                        result["user"]["username"] != active.username ||
                        (result["user"].contains("apple_linked") &&
                         !result["user"]["apple_linked"].is_boolean()))
                        throw std::runtime_error("This address did not return a valid SwitchMusic login");
                }
            } catch (const HttpError &e) {
                result = error(e.what());
                result["http_status"] = e.status;
            } catch (const std::exception &e) {
                result = error(e.what());
            }
            {
                std::lock_guard guard(mutex);
                finished.push_back({std::move(job), std::move(result)});
            }
        }
    }
};
static std::string timeText(double seconds) {
    int s = std::max(0, int(seconds));
    return std::to_string(s / 60) + ":" + (s % 60 < 10 ? "0" : "") + std::to_string(s % 60);
}
// XML owns every static layout. C++ binds data/actions and creates only repeated
// XML track rows, the server-generated QR view and vector transport glyphs.
class Library {
    Network &net;
    Config config;
    std::string password;
    bool loggedIn = false, authPending = false, checking = false;
    Clock::time_point lastAuth{}, lastPoll{}, lastQR{}, qrUntil{};
    brls::AppletFrame *frame;
    brls::Box *body, *sidebarLibrary;
    brls::ScrollingFrame *scroll;
    brls::View *page = nullptr, *safeFocus, *pendingFocus = nullptr;
    brls::Label *notice, *account;
    brls::Label *track = nullptr, *artist = nullptr, *progress = nullptr, *lyric = nullptr, *nearby = nullptr,
                *credit = nullptr, *volume = nullptr, *album = nullptr, *qrTimer = nullptr;
    brls::View *fill = nullptr;
    Json currentUser = Json::object(), state = Json::object(), items = Json::array(), lines = Json::array();
    std::string screen, currentPath, kind, pageTitle, lyricId, qrCode, playerArtworkPath;
    bool inSettings = false, qrActive = false, qrPending = false, now = false, polling = false;
    unsigned generation = 0, authGeneration = 0;
    size_t selection = 0;
    float restoreOffset = 0;
    struct Location {
        std::string path, type, title;
        size_t row;
        float offset;
    };
    std::vector<Location> history;
    std::optional<Location> playerReturn;
    std::optional<float> pendingScroll;
    std::unordered_map<std::string, std::vector<brls::Image *>> artworkViews;
    std::unordered_map<std::string, std::shared_ptr<std::vector<uint8_t>>> artworkCache;
    std::deque<std::string> artworkQueue, artworkLRU;
    bool artworkPending = false;
    std::function<void()> navigation;
    template <class T = brls::View> static T *view(brls::View *root, const std::string &id) {
        auto *found = dynamic_cast<T *>(root->getView(id));
        if (!found) throw std::runtime_error("Missing XML view: " + id);
        return found;
    }
    static brls::View *xml(const std::string &name) {
        return brls::View::createFromXMLResource("switchmusic/" + name + ".xml");
    }
    void text(const std::string &id, const std::string &value) {
        view<brls::Label>(page, id)->setText(value);
    }
    void bind(brls::View *root, const std::string &id, std::function<void()> action) {
        view(root, id)->registerClickAction([this, action](brls::View *) {
            navigation = action;
            return true;
        });
    }
    void bind(const std::string &id, std::function<void()> action) {
        bind(page, id, std::move(action));
    }
    void selectNav(const std::string &id) {
        for (auto name : {"home", "search", "player", "songs", "albums", "playlists", "account"})
            view<brls::Button>(frame, "nav/" + std::string(name))
                ->setTextColor(id == name ? nvgRGB(250, 69, 102) : nvgRGB(169, 176, 191));
    }
    void updateAccount() {
        account->setText(loggedIn ? config.username : "Guest catalog");
        view(frame, "app")->setActionAvailable(brls::BUTTON_Y, loggedIn);
        sidebarLibrary->setVisibility(loggedIn && currentUser.value("apple_linked", false)
                                          ? brls::Visibility::VISIBLE
                                          : brls::Visibility::GONE);
        view<brls::Button>(frame, "nav/account")->setText(loggedIn ? "Settings" : "Account");
    }
    void setPage(const std::string &layout, const std::string &mode) {
        playerArtworkPath.clear();
        artworkViews.clear();
        artworkQueue.clear();
        artworkPending = false;
        net.cancelArtwork(generation);
        pendingFocus = nullptr;
        pendingScroll.reset();
        brls::Application::giveFocus(safeFocus);
        body->clearViews();
        page = nullptr;
        track = artist = progress = lyric = nearby = credit = volume = album = qrTimer = nullptr;
        fill = nullptr;
        page = xml(layout);
        body->addView(page);
        screen = mode;
        scroll->setContentOffsetY(0, false);
    }
    void focus(const std::string &id = "") {
        pendingFocus = id.empty() ? page : view(page, id);
    }
    void command(Json request) {
        if (!loggedIn) {
            notice->setText("Sign in to control playback");
            return;
        }
        if (!net.submit({"command", "", std::move(request), generation}))
            notice->setText("Network queue busy; try again");
    }
    Location location() {
        return {currentPath, kind, pageTitle, selection, scroll->getContentOffsetY()};
    }
    void message(const std::string &title, const std::string &detail, std::function<void()> retry = {},
                 const std::string &mode = "message") {
        setPage("message", mode);
        text("page/title", title);
        text("page/subtitle", detail);
        view(page, "message/spinner")
            ->setVisibility(retry ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
        view(page, "message/retry")
            ->setVisibility(retry ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
        if (retry) bind("message/retry", retry);
        bind("message/back", [this] { back(); });
        focus(retry ? "message/retry" : "message/back");
    }
    void endQR() {
        if (!qrCode.empty() && !config.session.empty())
            net.submit({"qr_cancel", "/auth/apple/qr/cancel", {{"code", qrCode}}, generation, config});
        qrActive = qrPending = false;
        qrCode.clear();
        qrTimer = nullptr;
    }
    void openCollection(const std::string &type) {
        history.clear();
        browse("/v1/library/" + type, type,
               type == "songs"    ? "Songs"
               : type == "albums" ? "Albums"
                                  : "Playlists",
               false);
    }

  public:
    explicit Library(Network &n) : net(n) {
        // Borealis theme tokens have no XML loader; set them once before inflation.
        auto &theme = brls::Theme::getDarkTheme();
        theme.addColor("brls/background", nvgRGB(18, 21, 28));
        theme.addColor("brls/accent", nvgRGB(250, 69, 102));
        theme.addColor("brls/highlight/color1", nvgRGB(250, 69, 102));
        theme.addColor("brls/highlight/color2", nvgRGB(255, 166, 185));
        theme.addColor("brls/click_pulse", nvgRGBA(250, 69, 102, 38));
        theme.addColor("brls/button/primary_enabled_background", nvgRGB(250, 69, 102));
        theme.addColor("brls/button/primary_enabled_text", nvgRGB(255, 255, 255));
        theme.addColor("brls/button/default_enabled_background", nvgRGB(42, 48, 61));
        theme.addColor("brls/button/enabled_border_color", nvgRGB(75, 84, 104));
        brls::Application::registerXMLView("SwitchMusicTransportIcon", [] { return new TransportIcon(); });
        brls::Application::registerXMLView("SwitchMusicQR", [] { return new QRView(); });
        frame = dynamic_cast<brls::AppletFrame *>(xml("shell"));
        if (!frame) throw std::runtime_error("Invalid shell XML");
        body = view<brls::Box>(frame, "content");
        scroll = view<brls::ScrollingFrame>(frame, "scroll");
        notice = view<brls::Label>(frame, "shell/status");
        account = view<brls::Label>(frame, "shell/account");
        sidebarLibrary = view<brls::Box>(frame, "nav/library");
        safeFocus = view(frame, "nav/home");
        bind(frame, "nav/home", [this] { home(); });
        bind(frame, "nav/search", [this] { searchScreen(); });
        bind(frame, "nav/player", [this] { playing(); });
        bind(frame, "nav/account", [this] {
            if (loggedIn)
                settings();
            else
                loginScreen();
        });
        for (std::string type : {"songs", "albums", "playlists"})
            bind(frame, "nav/" + type, [this, type] { openCollection(type); });
        // Register on the content, ahead of AppletFrame's default B=dismiss.
        auto *root = view(frame, "app");
        root->registerAction("Back", brls::BUTTON_B, [this](brls::View *) {
            navigation = [this] { back(); };
            return true;
        });
        root->registerAction("Search", brls::BUTTON_X, [this](brls::View *) {
            navigation = [this] { searchScreen(); };
            return true;
        });
        root->registerAction("Play / Pause", brls::BUTTON_Y, [this](brls::View *) {
            if (loggedIn) command({{"op", "toggle"}});
            return true;
        });
        brls::Application::pushActivity(new brls::Activity(frame));
        try {
            config = Config::load(false);
        } catch (...) {
        }
        net.configure(config);
        updateAccount();
        if (!config.session.empty()) {
            checking = true;
            authGeneration = ++generation;
            authPending = net.submit({"auth", "/auth/me", {}, generation});
            message("Welcome back", "Checking your saved login…");
        } else if (!config.backend.empty())
            guest();
        else
            loginScreen();
    }
    void back() {
        if (screen == "qr" || qrActive) {
            settings();
            return;
        }
        if (screen == "player" && playerReturn) {
            auto saved = *playerReturn;
            playerReturn.reset();
            browse(saved.path, saved.type, saved.title, false, saved.row, saved.offset);
            return;
        }
        if (screen.rfind("collection", 0) == 0 && !history.empty()) {
            auto saved = history.back();
            history.pop_back();
            browse(saved.path, saved.type, saved.title, false, saved.row, saved.offset);
            return;
        }
        if (screen == "home") {
            brls::Application::giveFocus(safeFocus);
            return;
        }
        if (config.backend.empty()) {
            loginScreen();
            return;
        }
        home();
    }
    void keyboard(const std::string &title, std::string &value, int limit, bool secret = false,
                  const std::string &next = "") {
#ifdef __SWITCH__
        SwkbdConfig keyboard{};
        auto rc = swkbdCreate(&keyboard, 0);
        if (R_FAILED(rc)) {
            notice->setText("Nintendo Switch keyboard could not open");
            return;
        }
        swkbdConfigMakePresetDefault(&keyboard);
        swkbdConfigSetHeaderText(&keyboard, title.c_str());
        swkbdConfigSetStringLenMax(&keyboard, limit);
        swkbdConfigSetInitialText(&keyboard, value.c_str());
        swkbdConfigSetPasswordFlag(&keyboard, secret);
        swkbdConfigSetBlurBackground(&keyboard, true);
        std::vector<char> input(size_t(limit) * 4 + 1);
        rc = swkbdShow(&keyboard, input.data(), input.size());
        swkbdClose(&keyboard);
        if (R_SUCCEEDED(rc)) {
            value = input.data();
            loginScreen();
            if (!next.empty()) focus(next);
        }
#else
        brls::Application::getPlatform()->getImeManager()->openForText(
            [this, &value, next](std::string input) {
                value = input;
                loginScreen();
                if (!next.empty()) focus(next);
            },
            title, "", limit, value);
#endif
    }
    void loginScreen(const std::string &detail = "") {
        artworkCache.clear();
        artworkLRU.clear();
        endQR();
        inSettings = false;
        loggedIn = false;
        checking = authPending = polling = now = false;
        ++generation;
        updateAccount();
        selectNav("account");
        setPage("login", "login");
        notice->setText(detail.empty() ? "Sign in, or browse the public catalog as a guest" : detail);
        view<brls::Button>(page, "login/server")
            ->setText("Server: " + (config.backend.empty() ? std::string("Enter address") : config.backend));
        view<brls::Button>(page, "login/username")
            ->setText("Username: " +
                      (config.username.empty() ? std::string("Enter username") : config.username));
        view<brls::Button>(page, "login/password")
            ->setText(password.empty() ? "Password: Enter password" : "Password: ••••••••");
        bind("login/server",
             [this] { keyboard("Server URL", config.backend, 1024, false, "login/username"); });
        bind("login/username",
             [this] { keyboard("Username", config.username, 32, false, "login/password"); });
        bind("login/password", [this] { keyboard("Account password", password, 256, true, "login/submit"); });
        bind("login/submit", [this] { login(); });
        bind("login/guest", [this] { guest(); });
        focus("login/server");
    }
    void guest() {
        artworkCache.clear();
        artworkLRU.clear();
        // Guest browsing validates only the server address, never a half-entered username.
        Config anonymous{config.backend, "", ""};
        try {
            anonymous.validate(false);
        } catch (const std::exception &e) {
            notice->setText(e.what());
            return;
        }
        endQR();
        config = anonymous;
        password.clear();
        loggedIn = checking = authPending = polling = now = false;
        currentUser = Json::object();
        ++generation;
        net.configure(config);
        try {
            config.save();
        } catch (const std::exception &e) {
            notice->setText(e.what());
            return;
        }
        home();
    }
    void home() {
        endQR();
        inSettings = now = false;
        ++generation;
        history.clear();
        playerReturn.reset();
        updateAccount();
        selectNav("home");
        setPage("home", "home");
        notice->setText(loggedIn ? "Signed in as " + config.username : "Public catalog · no sign-in needed");
        bool linked = loggedIn && currentUser.value("apple_linked", false);
        view(page, "home/connection")
            ->setVisibility(linked ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
        view(page, "home/library")
            ->setVisibility(linked ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
        text("home/link-title", linked     ? "Your library is connected"
                                : loggedIn ? "Bring your library along"
                                           : "Make it personal");
        text("home/link-detail", linked     ? "Your saved songs, albums and playlists are ready."
                                 : loggedIn ? "Open Settings to link Apple Music with your phone."
                                            : "Sign in and link Apple Music to see your saved music.");
        bind("home/search", [this] { searchScreen(); });
        bind("home/player", [this] { playing(); });
        bind("home/account", [this] {
            if (loggedIn)
                settings();
            else
                loginScreen();
        });
        for (std::string type : {"songs", "albums", "playlists"})
            bind("home/" + type, [this, type] { openCollection(type); });
        focus("home/search");
    }
    void searchScreen() {
        if (config.backend.empty()) {
            loginScreen("Enter a server URL to browse Apple Music");
            return;
        }
        endQR();
        inSettings = now = false;
        ++generation;
        history.clear();
        selectNav("search");
        setPage("search", "search");
        notice->setText("Search the public Apple Music catalog");
        bind("search/open", [this] { search(); });
        focus("search/open");
    }
    void search() {
        brls::Application::getPlatform()->getImeManager()->openForText(
            [this](std::string input) {
                if (!input.empty())
                    browse("/v1/catalog/search?term=" + encode(input), "catalog",
                           "Results for “" + input + "”", false);
            },
            "Search Apple Music", "", 200, "");
    }
    void login() {
        if (authPending) return;
        try {
            config.validate(false);
            if (config.username.empty()) throw std::runtime_error("Enter your account username");
            if (password.empty()) throw std::runtime_error("Enter your account password");
        } catch (const std::exception &e) {
            notice->setText(e.what());
            return;
        }
        net.configure(config);
        authGeneration = ++generation;
        authPending = net.submit({"login", "", {{"password", password}}, generation});
        password.clear();
        view<brls::Button>(page, "login/password")->setText("Password: Enter password");
        notice->setText(authPending ? "Signing in…" : "Network queue busy; try again");
    }
    void logout() {
        endQR();
        ++generation;
        net.configure(config);
        net.submit({"logout", "", {}, generation, config});
        config.session.clear();
        password.clear();
        try {
            Config::clear();
        } catch (const std::exception &e) {
            loginScreen(e.what());
            return;
        }
        loginScreen("Signed out. Guest browsing is still available.");
    }
    void authenticated(const Json &result, bool fresh) {
        authPending = checking = false;
        if (fresh) {
            config.session = result.at("session");
            try {
                config.save();
            } catch (const std::exception &e) {
                config.session.clear();
                loginScreen(e.what());
                return;
            }
            net.configure(config);
        }
        loggedIn = true;
        currentUser = result.at("user");
        lastAuth = Clock::now();
        home();
    }
    void settings() {
        if (!loggedIn) {
            loginScreen();
            return;
        }
        endQR();
        inSettings = true;
        now = false;
        ++generation;
        selectNav("account");
        message("Your account", "Loading settings…", {}, "settings");
        if (!net.submit({"settings", "/auth/me", {}, generation}))
            showSettings(currentUser, "Network queue busy; try again");
    }
    void showSettings(const Json &user, const std::string &detail = "") {
        currentUser = user;
        updateAccount();
        setPage("settings", "settings");
        notice->setText(detail.empty() ? "Manage your Apple Music connection" : detail);
        text("settings/server", "Server: " + config.backend);
        text("settings/user", "Account: " + config.username);
        bool linked = user.value("apple_linked", false);
        text("settings/apple", linked ? "Apple Music library connected" : "Apple Music library not linked");
        view<brls::Button>(page, "settings/link")
            ->setText(linked ? "Relink Apple Music with QR code" : "Link Apple Music with QR code");
        bind("settings/link", [this] { startQR(); });
        bind("settings/browse", [this] { home(); });
        bind("settings/logout", [this] { logout(); });
        focus("settings/link");
    }
    void startQR() {
        endQR();
        inSettings = true;
        now = false;
        qrActive = true;
        ++generation;
        message("Link Apple Music", "Requesting your secure QR code…", {}, "qr");
        if (!net.submit({"qr_start", "/auth/apple/qr/start", Json::object(), generation, config})) {
            qrActive = false;
            showSettings(currentUser, "Network queue busy; try again");
        }
    }
    void showQR(const Json &result) {
        auto code = result.at("code").get<std::string>(),
             confirmation = result.at("confirmation").get<std::string>();
        if (code.size() != 43 ||
            code.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") !=
                std::string::npos ||
            confirmation.size() != 7)
            throw std::runtime_error("Invalid server QR request");
        qrCode = std::move(code);
        qrUntil = Clock::now() + std::chrono::seconds(std::clamp(result.value("expires_in", 300), 1, 300));
        lastQR = Clock::now();
        setPage("qr", "qr");
        view<QRView>(page, "qr/image")->setMatrix(result.at("matrix"));
        text("qr/code", confirmation);
        text("qr/account", "Sign in as " + config.username);
        qrTimer = view<brls::Label>(page, "qr/timer");
        bind("qr/cancel", [this] { settings(); });
        notice->setText("Scan with your phone. Keep this screen open.");
        focus("qr/cancel");
    }
    void browse(std::string path, std::string type, std::string title = "Music", bool remember = true,
                size_t row = 0, float offset = 0) {
        if (path.rfind("/v1/library/", 0) == 0) {
            if (!loggedIn) {
                loginScreen("Sign in to access your personal library");
                return;
            }
            if (!currentUser.value("apple_linked", false)) {
                settings();
                return;
            }
        }
        if (remember && screen == "collection") {
            if (history.size() == 16) history.erase(history.begin());
            history.push_back(location());
        }
        endQR();
        inSettings = now = false;
        ++generation;
        currentPath = path;
        kind = type;
        pageTitle = title;
        selection = row;
        restoreOffset = offset;
        selectNav(path.rfind("/v1/library/", 0) == 0 ? (type == "tracks" ? "albums" : type) : "search");
        message(title, "Loading music…", {}, "collection-loading");
        if (!net.submit({"page", path, {}, generation}))
            message(
                "Server busy", "Network queue busy; try again",
                [this] { browse(currentPath, kind, pageTitle, false); }, "collection-error");
    }
    void showPage(const Json &result) {
        setPage("collection", "collection");
        text("page/title", pageTitle);
        items = result.value("items", Json::array());
        text("page/subtitle", kind == "catalog"
                                  ? "Songs, albums and playlists from Apple Music"
                                  : "Select a song to play. Press B to return to your previous page.");
        text("collection/count", std::to_string(items.size()) + " items on this page");
        bind("collection/search", [this] { searchScreen(); });
        view(page, "collection/empty")
            ->setVisibility(items.empty() ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
        auto *rows = view<brls::Box>(page, "collection/rows");
        std::vector<brls::View *> rowViews;
        std::string nextPath = result.contains("next_offset") && !result["next_offset"].is_null()
                                   ? withOffset(currentPath, result["next_offset"].get<int>())
                                   : "";
        auto snapshot = std::make_shared<Json>(items);
        for (size_t i = 0; i < items.size(); ++i) {
            auto item = items[i];
            auto *entry = xml("row");
            entry->setId("item/" + std::to_string(i));
            view<brls::Label>(entry, "row/index")->setText(std::to_string(i + 1));
            auto artwork = item.value("artwork_path", std::string{});
            if (artwork.rfind("/v1/artwork/", 0) == 0) {
                auto path = artwork + "?size=64";
                auto *image = view<brls::Image>(entry, "row/artwork");
                auto key = config.backend + "\n" + config.session + "\n" + path;
                auto cached = artworkCache.find(key);
                if (cached != artworkCache.end()) {
                    image->setImageFromMem(cached->second->data(), cached->second->size());
                    artworkLRU.erase(std::remove(artworkLRU.begin(), artworkLRU.end(), key),
                                     artworkLRU.end());
                    artworkLRU.push_back(key);
                } else {
                    if (!artworkViews.count(path)) artworkQueue.push_back(path);
                    artworkViews[path].push_back(image);
                }
            }
            view<brls::Label>(entry, "row/title")->setText(item.value("title", std::string("Untitled")));
            auto type = item.value("type", std::string{});
            bool song = type == "songs" || type == "library-songs";
            auto detail = item.value("artist", std::string{});
            if (detail.empty()) detail = song ? item.value("album", std::string{}) : "Apple Music collection";
            view<brls::Label>(entry, "row/detail")->setText(detail);
            view<brls::Label>(entry, "row/type")
                ->setText(song ? timeText(item.value("duration_ms", 0) / 1000.0)
                          : type.find("albums") != std::string::npos ? "ALBUM  ›"
                                                                     : "PLAYLIST  ›");
            entry->setCustomNavigationRoute(brls::FocusDirection::LEFT, safeFocus);
            entry->addGestureRecognizer(new brls::TapGestureRecognizer(entry));
            entry->getFocusEvent()->subscribe([this, i](brls::View *) { selection = i; });
            entry->registerClickAction([this, item, i, snapshot, nextPath, song](brls::View *) {
                navigation = [this, item, i, snapshot, nextPath, song] {
                    selection = i;
                    if (song) {
                        if (!loggedIn) {
                            loginScreen("Sign in to control playback. Catalog browsing remains available.");
                            return;
                        }
                        Json queue = Json::array();
                        size_t selected = 0;
                        for (size_t x = 0; x < snapshot->size(); ++x) {
                            auto type = (*snapshot)[x].value("type", std::string{});
                            if (type == "songs" || type == "library-songs") {
                                if (x == i) selected = queue.size();
                                queue.push_back({{"id", (*snapshot)[x]["id"]}, {"type", type}});
                            }
                        }
                        command(
                            {{"op", "play"}, {"queue", queue}, {"index", selected}, {"next_path", nextPath}});
                        playing();
                    } else {
                        auto type = item.value("type", std::string{});
                        auto collection = type.find("albums") != std::string::npos ? "albums" : "playlists";
                        auto source = type.rfind("library-", 0) == 0 ? "library" : "catalog";
                        browse("/v1/" + std::string(source) + "/" + collection + "/" + encode(item.at("id")) +
                                   "/tracks",
                               "tracks", item.value("title", std::string("Collection")));
                    }
                };
                return true;
            });
            rows->addView(entry);
            rowViews.push_back(entry);
        }
        view(page, "collection/next")
            ->setVisibility(nextPath.empty() ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
        auto base = pageBase(currentPath);
        view(page, "collection/first")
            ->setVisibility(base == currentPath ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
        bind("collection/next", [this, nextPath] { browse(nextPath, kind, pageTitle); });
        bind("collection/first", [this, base] { browse(base, kind, pageTitle); });
        if (rowViews.empty())
            focus("collection/search");
        else
            pendingFocus = rowViews[std::min(selection, rowViews.size() - 1)];
        pendingScroll = restoreOffset;
        notice->setText(loggedIn ? "A select  ·  B back  ·  X search"
                                 : "Guest catalog  ·  Sign in for your personal library");
    }
    void playing() {
        if (!loggedIn) {
            loginScreen("Sign in to control playback");
            return;
        }
        if (screen == "collection") playerReturn = location();
        endQR();
        inSettings = false;
        now = true;
        ++generation;
        selectNav("player");
        setPage("player", "player");
        track = view<brls::Label>(page, "page/title");
        artist = view<brls::Label>(page, "page/subtitle");
        album = view<brls::Label>(page, "player/album");
        progress = view<brls::Label>(page, "player/time");
        volume = view<brls::Label>(page, "player/volume");
        fill = view(page, "player/fill");
        lyric = view<brls::Label>(page, "player/lyric");
        nearby = view<brls::Label>(page, "player/nearby");
        credit = view<brls::Label>(page, "player/credit");
        for (auto pair :
             {std::pair{"previous", "previous"}, {"toggle", "toggle"}, {"next", "next"}, {"stop", "stop"}}) {
            std::string id = pair.first, op = pair.second;
            bind("player/" + id, [this, op] { command({{"op", op}}); });
        }
        bind("player/rewind", [this] {
            command({{"op", "seek"}, {"seconds", std::max(0.0, state.value("position", 0.0) - 15)}});
        });
        bind("player/forward",
             [this] { command({{"op", "seek"}, {"seconds", state.value("position", 0.0) + 15}}); });
        bind("player/quieter",
             [this] { command({{"op", "volume"}, {"value", state.value("volume", 70.0) - 5}}); });
        bind("player/louder",
             [this] { command({{"op", "volume"}, {"value", state.value("volume", 70.0) + 5}}); });
        for (auto pair : {std::pair{brls::BUTTON_LB, "previous"}, {brls::BUTTON_RB, "next"}}) {
            std::string op = pair.second;
            page->registerAction(op, pair.first, [this, op](brls::View *) {
                command({{"op", op}});
                return true;
            });
        }
        lyricId.clear();
        lines = Json::array();
        focus("player/toggle");
        renderState();
    }
    void updatePlaybackState(Json snapshot) {
        state = std::move(snapshot);
        renderState();
    }
    void renderState() {
        notice->setText(state.contains("error") && !state["error"].get<std::string>().empty()
                            ? state["error"].get<std::string>()
                            : state.value("status", std::string("Connecting…")));
        if (!now || !track) return;
        auto song = state.value("track", Json::object());
        track->setText(song.value("title", std::string("Nothing playing")));
        artist->setText(song.value("artist", std::string("Choose a song from Search or your library.")));
        album->setText(song.value("album", std::string("Ready when you are.")));
        double position = state.value("position", 0.0), duration = state.value("duration", 0.0);
        progress->setText(timeText(position) + " / " + timeText(duration));
        volume->setText("Volume " + std::to_string(int(state.value("volume", 70.0))) + "%");
        fill->setWidthPercentage(duration > 0 ? float(std::clamp(position / duration, 0.0, 1.0) * 100) : 0);
        auto id = song.value("id", std::string{});
        view<TransportIcon>(page, "player/toggle/icon")->setPlaying(
            !id.empty() && !state.value("paused", false) && state.value("status", std::string{}) != "stopped");
        auto artwork = song.value("artwork_path", std::string{});
        auto path = artwork.rfind("/v1/artwork/", 0) == 0 ? artwork + "?size=256" : std::string{};
        if (path != playerArtworkPath) {
            playerArtworkPath = path;
            auto* image = view<brls::Image>(page, "player/artwork");
            image->clear();
            artworkViews.clear();
            artworkQueue.clear();
            if (!path.empty()) {
                artworkViews[path].push_back(image);
                artworkQueue.push_back(path);
            }
        }
        if (id != lyricId) {
            lyricId = id;
            lines = Json::array();
            nearby->setText("");
            view<brls::Label>(page, "player/previous-line")->setText("");
            credit->setText("syncedlyrics");
            lyric->setText(id.empty() ? "Lyrics appear here." : "Loading lyrics…");
            if (!id.empty()) net.submit({"lyrics", "/v1/lyrics/" + encode(id), {}, generation});
        }
        if (!lines.empty()) {
            long ms = long(position * 1000);
            int active = -1;
            for (size_t i = 0; i < lines.size(); ++i) {
                if (lines[i].value("time_ms", 0L) <= ms)
                    active = i;
                else
                    break;
            }
            lyric->setText(active >= 0 ? lines[active].value("text", std::string{}) : "♪");
            view<brls::Label>(page, "player/previous-line")->setText(
                active > 0 ? lines[active-1].value("text", std::string{}) : "");
            nearby->setText(active + 1 < int(lines.size()) ? lines[active + 1].value("text", std::string{})
                                                           : "");
        }
    }
    // Focus only after Yoga has measured a newly inflated page. This lets the
    // ScrollingFrame reveal a control below the fold instead of using zero bounds.
    void settleFocus() {
        if (pendingFocus) {
            auto *target = pendingFocus;
            pendingFocus = nullptr;
            brls::Application::giveFocus(target);
            if (pendingScroll) {
                scroll->setContentOffsetY(*pendingScroll, false);
                pendingScroll.reset();
            }
        }
    }
    void tick() {
        settleFocus();
        if (navigation) {
            auto action = std::move(navigation);
            navigation = {};
            action();
        }
        for (auto &done : net.drain()) {
            auto &job = done.job;
            auto &result = done.result;
            if (job.type == "state") polling = false;
            if ((job.type == "auth" || job.type == "login") && job.generation == authGeneration)
                authPending = false;
            if (job.generation != generation) {
                if (job.type == "qr_start" && result.contains("code"))
                    net.submit({"qr_cancel",
                                "/auth/apple/qr/cancel",
                                {{"code", result["code"]}},
                                generation,
                                job.credentials});
                continue;
            }
            if (job.type == "logout" || job.type == "qr_cancel") continue;
            if (result.value("http_status", 0) == 401 && job.type != "login") {
                config.session.clear();
                password.clear();
                try {
                    Config::clear();
                } catch (...) {
                }
                net.configure(config);
                loginScreen("Your login expired or was revoked. Sign in again, or browse as a guest.");
                continue;
            }
            if (job.type == "artwork") {
                artworkPending = false;
                if (result.contains("bytes") && result["bytes"].is_binary()) {
                    auto &bytes = result["bytes"].get_binary();
                    auto cached = std::make_shared<std::vector<uint8_t>>(bytes.begin(), bytes.end());
                    auto found = artworkViews.find(job.path);
                    if (found != artworkViews.end())
                        for (auto *image : found->second)
                            image->setImageFromMem(cached->data(), cached->size());
                    // Keep only the current full-size cover texture. The shared
                    // 64-entry compressed cache is reserved for small row art.
                    if (job.path.find("?size=64") == std::string::npos) continue;
                    auto key = config.backend + "\n" + config.session + "\n" + job.path;
                    artworkCache[key] = std::move(cached);
                    artworkLRU.push_back(key);
                    while (artworkLRU.size() > 64) {
                        artworkCache.erase(artworkLRU.front());
                        artworkLRU.pop_front();
                    }
                }
                continue;
            }
            if (job.type == "settings" || job.type == "qr_start" || job.type == "qr_status") {
                if (job.type == "qr_status") qrPending = false;
                if (result.contains("error")) {
                    if (job.type == "qr_status" && result.value("http_status", 0) == 429) continue;
                    endQR();
                    showSettings(currentUser, result["error"]);
                } else
                    try {
                        if (job.type == "settings")
                            showSettings(result.at("user"));
                        else if (job.type == "qr_start")
                            showQR(result);
                        else if (result.value("status", std::string{}) == "linked") {
                            qrActive = qrPending = false;
                            qrCode.clear();
                            showSettings(result.at("user"), "Apple Music connected. Your library is ready.");
                        }
                    } catch (const std::exception &e) {
                        endQR();
                        showSettings(currentUser, e.what());
                    }
                continue;
            }
            if (job.type == "login" || job.type == "auth") {
                authPending = false;
                if (result.contains("error")) {
                    if (job.type == "auth" && !checking) {
                        notice->setText(result["error"]);
                        lastAuth = Clock::now();
                    } else
                        loginScreen(result["error"]);
                } else if (job.type == "login" || checking)
                    authenticated(result, job.type == "login");
                continue;
            }
            if (job.type == "state") {
                updatePlaybackState(result);
            } else if (job.type == "command") {
                if (result.contains("error")) notice->setText(result["error"]);
            } else if (result.contains("error")) {
                if (job.type == "lyrics" && lyric)
                    lyric->setText(result["error"]);
                else
                    message(
                        "Could not load music", result["error"],
                        [this] { browse(currentPath, kind, pageTitle, false); }, "collection-error");
            } else if (job.type == "page")
                showPage(result);
            else if (job.type == "lyrics" && lyric && job.path == "/v1/lyrics/" + encode(lyricId)) {
                lines = result.value("lines", Json::array());
                credit->setText(result.value("provider", std::string("syncedlyrics")) + "  " +
                                result.value("copyright", std::string{}));
                if (lines.empty()) lyric->setText("No synchronized lyrics available");
                renderState();
            }
        }
        if (!artworkPending && !artworkQueue.empty()) {
            auto path = artworkQueue.front();
            if (net.submit({"artwork", path, {}, generation})) {
                artworkPending = true;
                artworkQueue.pop_front();
            }
        }
        auto current = Clock::now();
        if (qrActive && !qrCode.empty()) {
            if (current >= qrUntil) {
                endQR();
                showSettings(currentUser, "QR code expired. Request another to continue.");
            } else {
                if (qrTimer)
                    qrTimer->setText("Expires in " +
                                     timeText(std::chrono::duration<double>(qrUntil - current).count()));
                if (!qrPending && current - lastQR > std::chrono::seconds(3)) {
                    lastQR = current;
                    qrPending =
                        net.submit({"qr_status", "/auth/apple/qr/status", {{"code", qrCode}}, generation});
                }
            }
        }
        if (loggedIn && !authPending && current - lastAuth > std::chrono::seconds(30)) {
            lastAuth = current;
            authGeneration = generation;
            authPending = net.submit({"auth", "/auth/me", {}, generation});
        }
        if (loggedIn && !inSettings && !polling && current - lastPoll > std::chrono::milliseconds(500)) {
            lastPoll = current;
            polling = net.submit({"state", "", {{"op", "state"}}, generation});
        }
    }
};
int main() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    if (!brls::Application::init()) return 1;
    brls::Application::createWindow("SwitchMusic");
    brls::Application::getPlatform()->setThemeVariant(brls::ThemeVariant::DARK);
    brls::Application::setGlobalQuit(true);
    try {
        mkdir("sdmc:/config", 0777);
        mkdir("sdmc:/config/switchmusic", 0777);
        Network network(Config{});
        Library library(network);
        while (brls::Application::mainLoop())
            library.tick();
    } catch (const std::exception &e) {
        auto *content = brls::View::createFromXMLResource("switchmusic/message.xml");
        dynamic_cast<brls::Label *>(content->getView("page/title"))->setText("SwitchMusic could not start");
        dynamic_cast<brls::Label *>(content->getView("page/subtitle"))->setText(e.what());
        auto *frame = new brls::AppletFrame(content);
        frame->setTitle("SwitchMusic setup");
        brls::Application::pushActivity(new brls::Activity(frame));
        while (brls::Application::mainLoop()) {
        }
    }
    curl_global_cleanup();
    return 0;
}
