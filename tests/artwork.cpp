#include <switchmusic/artwork.hpp>
#include <switchmusic/artwork_worker.hpp>
#include <switchmusic/ipc.hpp>
#include <curl/curl.h>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
using namespace smusic;
int main(int argc, char **argv) {
    assert(argc == 2);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    std::vector<uint8_t> pixels(CoverBytes);
    for (size_t i = 0; i < pixels.size(); ++i)
        pixels[i] = i % 251;
    CoverReceiver image;
    image.begin("123");
    while (!image.complete()) {
        auto reply = artworkChunk("123", pixels, image.offset());
        assert(reply.dump().size() < MaxControlBytes);
        image.accept(reply);
    }
    assert(image.pixels == pixels);
    image.begin("124");
    assert(!image.accept(artworkChunk("123", pixels, 0)) && image.offset() == 0);
    auto bad = artworkChunk("124", pixels, 0);
    bad["rgba4444_hex"] = "xx";
    bool denied = false;
    try {
        image.accept(bad);
    } catch (...) {
        denied = true;
    }
    assert(denied && image.offset() == 0);
    ArtworkWorker worker;
    Config config{argv[1], "admin", std::string(43, 'a')};
    auto song = [](const std::string &id) {
        return Json{{"id", id}, {"artwork_path", "/v1/artwork/songs/" + id}};
    };
    worker.request(config, song("123"));
    image.begin("123");
    for (unsigned n = 0; n < 400 && !image.complete(); ++n) {
        auto reply = worker.chunk({{"track_id", "123"}, {"offset", image.offset()}});
        image.accept(reply);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    assert(image.complete() && image.pixels == pixels);
    worker.request(config, song("124"));
    worker.request(config, song("125"));
    image.begin("125");
    assert(!worker.chunk({{"track_id", "123"}, {"offset", 0u}}).value("ready", true));
    for (unsigned n = 0; n < 400 && !image.complete(); ++n) {
        image.accept(worker.chunk({{"track_id", "125"}, {"offset", image.offset()}}));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    assert(image.complete() && image.pixels[0] == 125);
    worker.clear();
    assert(!worker.chunk({{"track_id", "125"}, {"offset", 0u}}).value("ready", true));
    std::cout << "Artwork IPC chunks, stale-track rejection, malformed pixels, background fetch and "
                 "replacement passed\n";
}
