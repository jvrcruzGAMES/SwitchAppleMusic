// Run the production NRO UI on desktop Borealis; no Apple account is contacted.
#define main switchmusic_nro_main
#include "../../nro/source/main.cpp"
#undef main
#include <fstream>
#include <iostream>
#include <borealis/extern/glad/glad.h>
#include <cassert>
#include <filesystem>
int main(int argc, char **argv) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    if (!brls::Application::init()) return 1;
    brls::Application::createWindow("SwitchMusic UI preview");
    brls::Application::getPlatform()->setThemeVariant(brls::ThemeVariant::DARK);
    std::filesystem::create_directories("sdmc:/config/switchmusic");
    Config{std::getenv("SWITCHMUSIC_UI_BACKEND") ? std::getenv("SWITCHMUSIC_UI_BACKEND")
                                                 : "http://127.0.0.1:9",
           "demo", std::string(43, 'a')}
        .save();
    Network network(Config{});
    Library library(network);
    Json user = {{"username", "demo"}, {"apple_linked", true}};
    library.authenticated({{"user", user}}, false);
    std::string scene = argc > 1 ? argv[1] : "home";
    if (scene == "login")
        library.loginScreen();
    else if (scene == "guest")
        library.guest();
    else if (scene == "search")
        library.searchScreen();
    else if (scene == "settings")
        library.showSettings(user);
    else if (scene == "qr") {
        Json matrix = Json::array();
        for (int y = 0; y < 41; ++y) {
            std::string row(41, '0');
            for (int x = 4; x < 37; ++x)
                if (y >= 4 && y < 37 && (x + y) % 3) row[x] = '1';
            matrix.push_back(row);
        }
        library.showQR({{"code", std::string(43, 'a')},
                        {"confirmation", "123-456"},
                        {"expires_in", 300},
                        {"matrix", matrix}});
    } else if (scene == "collection" || scene == "long" || scene == "empty") {
        library.browse("/v1/catalog/search?term=" + scene, "catalog", "Search results", false);
    } else if (scene == "player")
        library.playing();
    Json playback = {{"status", "playing"}, {"paused", false}, {"position", 15.0}, {"duration", 215.0},
                     {"volume", 70.0}, {"track", {{"id", "100"}, {"title", "Midnight Echoes"},
                     {"artist", "The Northern Lights"}, {"album", "After Hours"},
                     {"artwork_path", "/v1/artwork/songs/100"}}}};
    // Layout frames also exercise the real XML parser, view factories and focus.
    for (int i = 0; i < 90; ++i) {
        if (!brls::Application::mainLoop()) return 2;
        library.tick();
        if (scene == "player") library.updatePlaybackState(playback);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    assert(brls::Application::getCurrentFocus());
    auto *root = brls::Application::getCurrentFocus();
    while (root->hasParent())
        root = root->getParent();
    if (scene == "collection" || scene == "long") {
        auto *row = root->getView("item/0");
        assert(row);
        auto bounds = row->getFrame();
        assert(bounds.getWidth() > 600 && bounds.getHeight() >= 70);
        auto title = row->getView("row/title")->getFrame(), type = row->getView("row/type")->getFrame();
        assert(title.getMaxX() <= type.getMinX());
        auto number = row->getView("row/index")->getFrame();
        auto *image = dynamic_cast<brls::Image *>(row->getView("row/artwork"));
        assert(image && image->getTexture() > 0);
        auto cover = image->getFrame();
        assert(number.getMaxX() <= cover.getMinX() && cover.getMaxX() <= title.getMinX());
    }

    if (scene == "player") {
        auto details = root->getView("player/details")->getFrame();
        auto lyrics = root->getView("player/lyrics-panel")->getFrame();
        assert(std::abs(details.getMinY()-lyrics.getMinY()) < 1);
        assert(std::abs(details.getHeight()-lyrics.getHeight()) < 1 && lyrics.getHeight() >= 460);
        assert(lyrics.getMinX() >= details.getMaxX());
        auto* cover = dynamic_cast<brls::Image*>(root->getView("player/artwork"));
        assert(cover && cover->getTexture() > 0 && cover->getFrame().getWidth() >= 140);
        auto* icon = dynamic_cast<TransportIcon*>(root->getView("player/toggle/icon"));
        assert(icon && icon->isPlaying());
        playback["paused"] = true; library.updatePlaybackState(playback); assert(!icon->isPlaying());
        playback["paused"] = false; library.updatePlaybackState(playback); assert(icon->isPlaying());
        assert(dynamic_cast<brls::Label*>(root->getView("player/lyric"))->getFullText() == "Let the night carry us home");
        for (auto id : {"player/previous", "player/toggle", "player/next"})
            assert(dynamic_cast<brls::Button*>(root->getView(id))->getText().empty());
        assert(root->getView("player/stop")->getFrame().getMaxY() <= details.getMaxY());
        for (auto move : {std::pair{brls::BUTTON_NAV_LEFT, "player/previous"},
                          {brls::BUTTON_NAV_RIGHT, "player/toggle"},
                          {brls::BUTTON_NAV_RIGHT, "player/next"}}) {
            brls::Application::onControllerButtonPressed(move.first, false);
            assert(brls::Application::mainLoop());
            assert(brls::Application::getCurrentFocus() == root->getView(move.second));
        }
        brls::Application::giveFocus(root->getView("player/toggle"));
        std::cout << "Player artwork, full-height lyrics, icon state and controls verified" << std::endl;
    }

    if (scene == "navigation") {
        auto render = [&] {
            for (int i = 0; i < 20; ++i) {
                assert(brls::Application::mainLoop());
                library.tick();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        };
        auto focused = [&](const std::string &id) {
            auto *current = brls::Application::getCurrentFocus();
            if (!current || current != root->getView(id)) {
                std::cerr << "Expected focus " << id << ", got " << (current ? current->describe() : "none")
                          << std::endl;
                std::abort();
            }
            std::cout << "Focus verified " << id << std::endl;
        };
        auto press = [&](brls::ControllerButton button) {
            brls::Application::onControllerButtonPressed(button, false);
            library.tick();
            render();
        };
        focused("home/search");
        press(brls::BUTTON_NAV_LEFT);
        focused("nav/home");
        press(brls::BUTTON_NAV_DOWN);
        focused("nav/search");
        press(brls::BUTTON_NAV_RIGHT);
        focused("home/search");
        press(brls::BUTTON_X);
        focused("search/open");
        press(brls::BUTTON_B);
        focused("home/search");
        library.browse("/v1/catalog/search?term=preview", "catalog", "Search results", false);
        render();
        focused("item/0");
        press(brls::BUTTON_NAV_DOWN);
        focused("item/1");
        press(brls::BUTTON_NAV_DOWN);
        focused("item/2");
        press(brls::BUTTON_A);
        render();
        press(brls::BUTTON_B);
        render();
        focused("item/2");
        press(brls::BUTTON_NAV_DOWN);
        focused("item/3");
        library.playing();
        render();
        focused("player/toggle");
        press(brls::BUTTON_B);
        render();
        focused("item/3");
        auto *top = brls::Application::getCurrentFocus();
        while (top->hasParent())
            top = top->getParent();
        assert(dynamic_cast<brls::Label *>(top->getView("page/title"))->getFullText() == "Search results");
        library.guest();
        render();
        focused("home/search");
        assert(top->getView("nav/library")->getVisibility() == brls::Visibility::GONE);
        std::cout << "Controller sidebar routes, shortcuts, collection back-stack, focus restoration and "
                     "guest visibility passed"
                  << std::endl;
    }
    dynamic_cast<brls::Label *>(root->getView("shell/status"))->setText("Preview · local artwork fixture");
    for (int i = 0; i < 3; ++i)
        assert(brls::Application::mainLoop());
    if (argc > 3) {
        GLint viewport[4];
        glGetIntegerv(GL_VIEWPORT, viewport);
        int w = viewport[2], h = viewport[3];
        std::vector<unsigned char> pixels(size_t(w) * h * 3);
        glReadBuffer(GL_FRONT);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
        std::ofstream image(argv[3], std::ios::binary);
        image << "P6\n" << w << " " << h << "\n255\n";
        for (int y = h - 1; y >= 0; --y)
            image.write(reinterpret_cast<char *>(pixels.data() + size_t(y) * w * 3), w * 3);
    }
    if (scene == "player") {
        library.updatePlaybackState({{"status", "stopped"}});
        assert(!dynamic_cast<TransportIcon*>(root->getView("player/toggle/icon"))->isPlaying());
        assert(dynamic_cast<brls::Image*>(root->getView("player/artwork"))->getTexture() == 0);
        assert(dynamic_cast<brls::Label*>(root->getView("player/lyric"))->getFullText() == "Lyrics appear here.");
    }
    std::cout << "READY " << scene << std::endl;
    // Optional hold permits capturing the actual OpenGL-rendered window.
    int frames = argc > 2 ? std::stoi(argv[2]) : 10;
    for (int i = 0; i < frames; ++i)
        if (!brls::Application::mainLoop()) break;
    return 0;
}
