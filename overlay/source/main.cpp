#define TESLA_INIT_IMPL
#define TESLA_INIT_NO_SOCKET
#include <tesla.hpp>
#include <switchmusic/client.hpp>
#include <switchmusic/artwork.hpp>
#include <chrono>
#include <cmath>
using namespace smusic;
using Clock = std::chrono::steady_clock;
namespace {
std::string timeText(double seconds) {
    int s = std::max(0, int(seconds));
    return std::to_string(s / 60) + ":" + (s % 60 < 10 ? "0" : "") + std::to_string(s % 60);
}
class PlayerPanel : public tsl::elm::Element {
    Json state = Json::object();
    const CoverReceiver &cover;
    std::function<void(Json)> command;
    unsigned selected = 1;
    std::string failure;
    static void triangle(tsl::gfx::Renderer *r, int cx, int cy, int direction, tsl::Color color) {
        for (int dy = -12; dy <= 12; ++dy) {
            int width = (12 - std::abs(dy)) * 18 / 12;
            r->drawRect(direction > 0 ? cx - 8 : cx + 8 - width, cy + dy, width + 1, 1, color);
        }
    }
    void centered(tsl::gfx::Renderer *r, std::string text, int y, unsigned size, tsl::Color color) {
        int available = getWidth() - 32;
        auto measure = [&] { return r->drawString(text, false, 0, 0, size, tsl::Color{}, 0, false).first; };
        bool trimmed = false;
        while (!text.empty() && measure() > available) {
            if (trimmed)
                text.resize(text.size() - 3);
            size_t last = text.size() - 1;
            while (last && (uint8_t(text[last]) & 0xc0) == 0x80)
                --last;
            text.resize(last);
            text += "…";
            trimmed = true;
            if (text.size() <= 3)
                break;
        }
        auto width = measure();
        r->drawString(text, false, getX() + (getWidth() - width) / 2, y, size, color);
    }
    void activate() { command({{"op", selected == 0 ? "previous" : selected == 1 ? "toggle" : "next"}}); }

  public:
    PlayerPanel(const CoverReceiver &image, std::function<void(Json)> action)
        : cover(image), command(std::move(action)) {}
    void setState(Json value) {
        state = std::move(value);
        failure.clear();
    }
    void setError(const std::string &text) { failure = text; }
    tsl::elm::Element *requestFocus(tsl::elm::Element *, tsl::FocusDirection) override { return this; }
    void layout(u16, u16, u16, u16) override { setBoundaries(getX(), getY(), getWidth(), 512); }
    void drawFocusBackground(tsl::gfx::Renderer *) override {}
    void drawHighlight(tsl::gfx::Renderer *) override {}
    bool onClick(u64 keys) override {
        if (keys & HidNpadButton_Left) {
            selected = (selected + 2) % 3;
            return true;
        }
        if (keys & HidNpadButton_Right) {
            selected = (selected + 1) % 3;
            return true;
        }
        if (keys & HidNpadButton_A) {
            activate();
            return true;
        }
        if (keys & HidNpadButton_L) {
            command({{"op", "previous"}});
            return true;
        }
        if (keys & HidNpadButton_R) {
            command({{"op", "next"}});
            return true;
        }
        if (keys & HidNpadButton_Y) {
            command({{"op", "toggle"}});
            return true;
        }
        return false;
    }
    bool onTouch(tsl::elm::TouchEvent event, s32 x, s32 y, s32, s32, s32 initialX, s32 initialY) override {
        if (event != tsl::elm::TouchEvent::Release || std::abs(x - initialX) > 16 ||
            std::abs(y - initialY) > 16)
            return false;
        int cy = getY() + 422;
        if (std::abs(y - cy) > 34)
            return false;
        for (unsigned i = 0; i < 3; ++i)
            if (std::abs(x - (getX() + getWidth() / 2 + (int(i) - 1) * 96)) <= 34) {
                selected = i;
                activate();
                return true;
            }
        return false;
    }
    void draw(tsl::gfx::Renderer *r) override {
        const tsl::Color white{15, 15, 15, 15}, muted{10, 10, 11, 15}, accent{15, 4, 6, 15},
            background{2, 2, 3, 15};
        int center = getX() + getWidth() / 2, top = getY() + 10;
        r->drawRoundedRect(center - 128, top, 256, 256, 12, background);
        if (cover.complete())
            r->drawBitmapRGBA4444(center - 128, top, CoverSize, CoverSize, cover.pixels.data());
        else {
            centered(r, "♪", top + 135, 56, muted);
            centered(r, "SwitchMusic", top + 178, 18, muted);
        }
        auto song = state.value("track", Json::object());
        centered(r, song.value("title", std::string("Nothing playing")), getY() + 298, 23, white);
        centered(r, song.value("artist", std::string("Choose music in the app")), getY() + 324, 18, muted);
        double position = state.value("position", 0.0), duration = state.value("duration", 0.0);
        int width = getWidth() - 48, left = getX() + 24;
        r->drawRoundedRect(left, getY() + 346, width, 4, 2, tsl::Color{4, 4, 5, 15});
        if (duration > 0) {
            int filled = int(width * std::clamp(position / duration, 0.0, 1.0));
            if (filled > 0)
                r->drawRoundedRect(left, getY() + 346, filled, 4, 2, accent);
        }
        r->drawString(timeText(position), false, left, getY() + 374, 16, muted);
        auto total = timeText(duration);
        auto length = r->drawString(total, false, 0, 0, 16, tsl::Color{}, 0, false).first;
        r->drawString(total, false, left + width - length, getY() + 374, 16, muted);
        int cy = getY() + 422;
        for (unsigned i = 0; i < 3; ++i) {
            int cx = center + (int(i) - 1) * 96;
            if (hasFocus() && i == selected) {
                r->drawCircle(cx, cy, 34, true, accent);
                r->drawCircle(cx, cy, 30, true, background);
            } else
                r->drawCircle(cx, cy, 30, true, background);
            if (i == 1) {
                bool paused = state.value("paused", false) || song.empty() ||
                              state.value("status", std::string{}) == "stopped";
                if (paused)
                    triangle(r, cx + 2, cy, 1, white);
                else {
                    r->drawRect(cx - 9, cy - 12, 6, 24, white);
                    r->drawRect(cx + 3, cy - 12, 6, 24, white);
                }
            } else {
                int direction = i == 0 ? -1 : 1;
                triangle(r, cx - direction * 3, cy, direction, white);
                r->drawRect(cx + direction * 12 - 1, cy - 12, 3, 24, white);
            }
        }
        centered(r, "Previous       Play / Pause       Next", getY() + 480, 14, muted);
        centered(r, failure.empty() ? state.value("status", std::string("Connecting…")) : failure,
                 getY() + 505, 14, failure.empty() ? muted : accent);
    }
};
} // namespace
class Controls : public tsl::Gui {
    PlayerPanel *panel = nullptr;
    Config config;
    bool configured = false;
    Json state = Json::object();
    CoverReceiver cover;
    std::string trackId;
    bool artworkAvailable = false, artworkFailed = false;
    Clock::time_point last{}, lastCover{};
    void command(Json request) {
        if (!configured)
            return;
        auto response = control(config, std::move(request));
        if (response.contains("error"))
            panel->setError(response["error"]);
    }
    void add(tsl::elm::List *list, const std::string &text, std::function<Json()> action) {
        auto *item = new tsl::elm::ListItem(text);
        item->setClickListener([this, action](u64 keys) {
            if (keys & HidNpadButton_A) {
                command(action());
                return true;
            }
            return false;
        });
        list->addItem(item);
    }

  public:
    tsl::elm::Element *createUI() override {
        auto *frame = new tsl::elm::OverlayFrame("SwitchMusic", "Now playing · L/R skip · Y pause");
        auto *list = new tsl::elm::List();
        panel = new PlayerPanel(cover, [this](Json request) { command(std::move(request)); });
        list->addItem(panel, 512);
        add(list, "Seek −15 seconds", [this] {
            return Json{{"op", "seek"}, {"seconds", std::max(0.0, state.value("position", 0.0) - 15)}};
        });
        add(list, "Seek +15 seconds",
            [this] { return Json{{"op", "seek"}, {"seconds", state.value("position", 0.0) + 15}}; });
        add(list, "Volume −5",
            [this] { return Json{{"op", "volume"}, {"value", state.value("volume", 70.0) - 5}}; });
        add(list, "Volume +5",
            [this] { return Json{{"op", "volume"}, {"value", state.value("volume", 70.0) + 5}}; });
        add(list, "Stop playback", [] { return Json{{"op", "stop"}}; });
        frame->setContent(list);
        return frame;
    }
    void update() override {
        auto now = Clock::now();
        if (now - last >= std::chrono::milliseconds(750)) {
            last = now;
            try {
                auto next = Config::load();
                if (next.backend != config.backend || next.session != config.session) {
                    trackId.clear();
                    cover.begin("");
                }
                config = std::move(next);
                configured = true;
                state = control(config, {{"op", "state"}});
                if (state.contains("error") && !state["error"].get<std::string>().empty()) {
                    panel->setError(state["error"]);
                    return;
                }
                panel->setState(state);
                auto song = state.value("track", Json::object());
                auto id = song.value("id", std::string{});
                if (id != trackId) {
                    trackId = id;
                    cover.begin(id);
                    artworkFailed = false;
                    artworkAvailable = !song.value("artwork_path", std::string{}).empty();
                    lastCover = {};
                }
            } catch (const std::exception &error) {
                configured = false;
                trackId.clear();
                artworkAvailable = false;
                state = Json::object();
                cover.begin("");
                panel->setState(state);
                panel->setError(error.what());
            }
        }
        if (configured && artworkAvailable && !artworkFailed && !cover.complete() &&
            now - lastCover >= std::chrono::milliseconds(20)) {
            lastCover = now;
            try {
                auto reply =
                    control(config, {{"op", "artwork"}, {"track_id", trackId}, {"offset", cover.offset()}});
                if (reply.contains("error")) {
                    artworkFailed = true;
                    return;
                }
                if (!reply.value("ready", false)) {
                    lastCover = now + std::chrono::milliseconds(230);
                    return;
                }
                cover.accept(reply);
            } catch (...) {
                artworkFailed = true;
            }
        }
    }
};
class MusicOverlay : public tsl::Overlay {
    std::unique_ptr<tsl::Gui> loadInitialGui() override { return initially<Controls>(); }
};
int main(int argc, char **argv) { return tsl::loop<MusicOverlay>(argc, argv); }
