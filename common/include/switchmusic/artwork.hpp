#pragma once
#include <switchmusic/client.hpp>
#include <algorithm>
#include <cstdint>
#include <vector>
namespace smusic {
inline constexpr unsigned CoverSize = 256;
inline constexpr size_t CoverBytes = CoverSize * CoverSize * 2;
inline constexpr size_t CoverChunk = 12 * 1024;
inline Json artworkChunk(const std::string &track, const std::vector<uint8_t> &pixels, size_t offset) {
    if (pixels.size() != CoverBytes || offset >= CoverBytes)
        return error("Invalid artwork range");
    size_t count = std::min(CoverChunk, CoverBytes - offset);
    std::string encoded;
    encoded.reserve(count * 2);
    constexpr char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < count; ++i) {
        auto byte = pixels[offset + i];
        encoded.push_back(hex[byte >> 4]);
        encoded.push_back(hex[byte & 15]);
    }
    return {{"track_id", track},
            {"ready", true},
            {"width", CoverSize},
            {"height", CoverSize},
            {"total_bytes", CoverBytes},
            {"offset", offset},
            {"rgba4444_hex", std::move(encoded)}};
}
class CoverReceiver {
    std::string track;
    std::vector<uint8_t> pending;

  public:
    std::vector<uint8_t> pixels;
    void begin(const std::string &id) {
        track = id;
        if (pending.capacity() < pixels.capacity())
            pending.swap(pixels);
        pending.clear();
        pixels.clear();
        if (id.empty()) {
            std::vector<uint8_t>().swap(pending);
            std::vector<uint8_t>().swap(pixels);
        }
    }
    size_t offset() const { return pending.size(); }
    bool complete() const { return pixels.size() == CoverBytes; }
    bool accept(const Json &reply) {
        if (reply.value("track_id", std::string{}) != track || !reply.value("ready", false))
            return false;
        if (reply.value("total_bytes", size_t{}) != CoverBytes || reply.value("width", 0u) != CoverSize ||
            reply.value("height", 0u) != CoverSize || reply.value("offset", size_t(-1)) != pending.size())
            throw std::runtime_error("Invalid artwork response");
        auto encoded = reply.at("rgba4444_hex").get<std::string>();
        size_t count = std::min(CoverChunk, CoverBytes - pending.size());
        if (encoded.size() != count * 2)
            throw std::runtime_error("Invalid artwork chunk size");
        auto nibble = [](char c) -> unsigned {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            throw std::runtime_error("Invalid artwork pixels");
        };
        // Validate first so a malformed response cannot leave a partial chunk.
        for (char c : encoded)
            nibble(c);
        if (pending.empty())
            pending.reserve(CoverBytes);
        for (size_t i = 0; i < count; ++i)
            pending.push_back(uint8_t((nibble(encoded[i * 2]) << 4) | nibble(encoded[i * 2 + 1])));
        if (pending.size() == CoverBytes) {
            pixels = std::move(pending);
            pending.clear();
            return true;
        }
        return false;
    }
};
} // namespace smusic
