#pragma once
#include <switchmusic/artwork.hpp>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
namespace smusic {
// One current cover; HTTP work is independent of decoder/audout and IPC dispatch.
class ArtworkWorker {
    std::mutex mutex;
    std::condition_variable wake;
    std::atomic<unsigned> generation{0};
    bool stopped = false, pending = false;
    Config config;
    std::string track, path;
    std::shared_ptr<const std::vector<uint8_t>> image;
    std::thread worker;
    void run() {
        for (;;) {
            Config active;
            std::string id, url;
            unsigned epoch;
            {
                std::unique_lock guard(mutex);
                wake.wait(guard, [&] { return stopped || pending; });
                if (stopped)
                    return;
                active = config;
                id = track;
                url = path;
                epoch = generation.load();
                pending = false;
            }
            try {
                auto pixels = apiBytes(active, url + "?size=256&format=rgba4444", CoverBytes,
                                       [&, epoch] { return generation.load() != epoch; });
                if (pixels.size() != CoverBytes)
                    continue;
                auto result = std::make_shared<const std::vector<uint8_t>>(std::move(pixels));
                std::lock_guard guard(mutex);
                if (generation.load() == epoch)
                    image = std::move(result);
            } catch (...) { /* Missing artwork must never stop audio playback. */
            }
        }
    }

  public:
    ArtworkWorker() = default;
    ~ArtworkWorker() {
        {
            std::lock_guard guard(mutex);
            stopped = true;
            ++generation;
        }
        wake.notify_all();
        if (worker.joinable())
            worker.join();
    }
    void clear() {
        std::lock_guard guard(mutex);
        ++generation;
        pending = false;
        track.clear();
        image.reset();
    }
    void request(const Config &value, const Json &song) {
        auto source = song.value("artwork_path", std::string{});
        auto id = song.value("id", std::string{});
        std::lock_guard guard(mutex);
        ++generation;
        image.reset();
        track = id;
        config = value;
        path = source;
        pending = source.rfind("/v1/artwork/", 0) == 0;
        if (pending && !worker.joinable()) {
            try {
                worker = std::thread([this] { run(); });
            } catch (...) {
                pending = false;
            } // Artwork must not prevent audio on a tight heap.
        }
        if (pending)
            wake.notify_one();
    }
    Json chunk(const Json &request) {
        auto id = request.value("track_id", std::string{});
        std::shared_ptr<const std::vector<uint8_t>> pixels;
        {
            std::lock_guard guard(mutex);
            if (id != track || !image)
                return {{"track_id", id}, {"ready", false}};
            pixels = image;
        }
        if (!request.contains("offset") || !request["offset"].is_number_unsigned())
            return error("Invalid artwork offset");
        return artworkChunk(id, *pixels, request["offset"].get<size_t>());
    }
};
} // namespace smusic
