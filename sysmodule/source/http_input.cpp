#include <switchmusic/audio.hpp>
#include <curl/curl.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <cstdio>
#include <limits>
#include <cctype>
#include <chrono>
#include <thread>
extern "C" {
#include <libavformat/avio.h>
#include <libavutil/error.h>
}
namespace smusic {
namespace {
constexpr size_t BlockSize = 256 * 1024;
class ConnectionFailure : public std::runtime_error {
public: using std::runtime_error::runtime_error;
};
bool safePath(const std::string &path) {
    return path.rfind("/v1/", 0) == 0 && path.find_first_of("\r\n#") == std::string::npos &&
           path.substr(0, path.find('?')).find("..") == std::string::npos;
}
} // namespace
struct HttpInput::Impl {
    Config config;
    Cancel cancel;
    CURL *curl = nullptr;
    CURLM *multi = nullptr;
    curl_slist *requestHeaders = nullptr;
    bool active = false, forceFresh = false;
    int64_t streamNext = -1;
    std::array<char, CURL_ERROR_SIZE> transportDetail{};
    std::array<uint8_t, BlockSize> block{};
    std::string path, etag, replyEtag;
    std::string *textSink = nullptr;
    uint8_t *output = nullptr;
    size_t capacity = 0, written = 0;
    int64_t position = 0, total = -1, blockStart = -1, replyStart = -1, replyEnd = -1, replyTotal = -1;
    int64_t requested = 0, received = 0;
    size_t blockLength = 0;
    long responseStatus = 0;
    bool rangeRequest = false, windowComplete = false, allocationFailed = false, invalidRange = false;
    Impl(Config c, Cancel fn) : config(std::move(c)), cancel(std::move(fn)) {
        config.validate();
        curl = curl_easy_init();
        multi = curl_multi_init();
        if (!curl || !multi) {
            if (curl)
                curl_easy_cleanup(curl);
            if (multi)
                curl_multi_cleanup(multi);
            throw std::runtime_error("HTTP allocation failed");
        }
    }
    void stopTransfer() {
        if (active) {
            curl_multi_remove_handle(multi, curl);
            active = false;
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, nullptr);
        curl_slist_free_all(requestHeaders);
        requestHeaders = nullptr;
        streamNext = -1;
    }
    ~Impl() {
        stopTransfer();
        curl_multi_cleanup(multi);
        curl_easy_cleanup(curl);
    }
    static size_t collect(char *p, size_t a, size_t b, void *data) {
        auto &self = *static_cast<Impl *>(data);
        if (a && b > SIZE_MAX / a)
            return 0;
        auto n = a * b;
        try {
            // HTTP errors are reported as status codes, not as body-size failures.
            if (self.responseStatus != 200 && self.responseStatus != 206)
                return n;
            if (self.textSink) {
                self.textSink->append(p, n);
                return n;
            }
            int64_t origin = self.responseStatus == 206 ? self.replyStart : 0;
            if (origin < 0 || origin > self.requested) {
                self.invalidRange = true;
                return 0;
            }
            int64_t skip = self.requested - origin;
            size_t begin = self.received < skip ? size_t(std::min<int64_t>(n, skip - self.received)) : 0;
            if (int64_t(n) > INT64_MAX - self.received)
                return 0;
            size_t count = n - begin;
            if (count > self.capacity - self.written) {
                // libcurl keeps this unconsumed chunk while paused. The next
                // read resumes the same HTTP stream with a fresh small window.
                self.windowComplete = true;
                return CURL_WRITEFUNC_PAUSE;
            }
            self.received += n;
            if (count) {
                std::memcpy(self.output + self.written, p + begin, count);
                self.written += count;
            }
            if (self.written == self.capacity)
                self.windowComplete = true;
            return n;
        } catch (const std::bad_alloc &) {
            self.allocationFailed = true;
            return 0;
        } catch (...) {
            return 0;
        }
    }
    static size_t header(char *p, size_t a, size_t b, void *data) {
        auto &self = *static_cast<Impl *>(data);
        size_t n = a * b;
        try {
            std::string line(p, n), lower = line;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (lower.rfind("http/", 0) == 0) {
                self.replyStart = self.replyEnd = self.replyTotal = -1;
                self.replyEtag.clear();
                auto space = lower.find(' ');
                if (space != std::string::npos)
                    self.responseStatus = std::strtol(lower.c_str() + space + 1, nullptr, 10);
            }
            if (lower.rfind("content-range:", 0) == 0) {
                long long start, end, total;
                if (sscanf(lower.c_str(), "content-range: bytes %lld-%lld/%lld", &start, &end, &total) == 3) {
                    self.replyStart = start;
                    self.replyEnd = end;
                    self.replyTotal = total;
                }
            }
            if (lower.rfind("etag:", 0) == 0) {
                auto begin = line.find_first_not_of(" \t", 5);
                auto end = line.find_last_not_of("\r\n \t");
                if (begin != std::string::npos && end >= begin)
                    self.replyEtag = line.substr(begin, end - begin + 1);
            }
            return n;
        } catch (const std::bad_alloc &) {
            self.allocationFailed = true;
            return 0;
        } catch (...) {
            return 0;
        }
    }
    static int progress(void *data, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
        auto &self = *static_cast<Impl *>(data);
        try {
            return self.cancel && self.cancel() ? 1 : 0;
        } catch (...) {
            return 1;
        }
    }
    long getOnce(const std::string &resource, uint8_t *data, size_t limit, const std::string &range = "",
             std::string *text = nullptr) {
        if (!safePath(resource))
            throw std::runtime_error("Invalid playback path");
        if (cancel && cancel())
            throw Cancelled();
        bool resume =
            active && responseStatus == 200 && streamNext == requested && resource == path && !range.empty();
        output = data;
        capacity = limit;
        written = 0;
        textSink = text;
        rangeRequest = !range.empty();
        windowComplete = allocationFailed = invalidRange = false;
        CURLcode rc = CURLE_OK;
        if (resume) {
            rc = curl_easy_pause(curl, CURLPAUSE_CONT);
        } else {
            stopTransfer();
            received = 0;
            replyStart = replyEnd = replyTotal = -1;
            replyEtag.clear();
            responseStatus = 0;
            curl_easy_reset(curl);
            if(forceFresh){curl_easy_setopt(curl,CURLOPT_FRESH_CONNECT,1L);curl_easy_setopt(curl,CURLOPT_DNS_CACHE_TIMEOUT,0L);forceFresh=false;}
            transportDetail.fill(0);
            auto url = config.backend + resource;
            curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, transportDetail.data());
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_COOKIE, ("switchmusic_session=" + config.session).c_str());
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
            // HTTP/1.1 backpressure avoids HTTP/2 per-stream buffering while paused.
            curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
            // Preparation and range-ignoring proxies may take time, especially for
            // lossless music. Detect stalled transfers, not total song duration.
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
            curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
            curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 180L);
            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, this);
            curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header);
            curl_easy_setopt(curl, CURLOPT_HEADERDATA, this);
            curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
            curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
#ifdef __SWITCH__
            curl_easy_setopt(curl, CURLOPT_CAINFO, "romfs:/certs/cacert.pem");
#endif
            curl_slist *list = nullptr;
            if (rangeRequest) {
                curl_easy_setopt(curl, CURLOPT_RANGE, range.c_str());
                if (!etag.empty())
                    list = curl_slist_append(list, ("If-Range: " + etag).c_str());
            }
            requestHeaders = list;
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, requestHeaders);
            if (rangeRequest) {
                auto result = curl_multi_add_handle(multi, curl);
                if (result != CURLM_OK)
                    throw std::runtime_error("Cannot initialize playback stream");
                active = true;
            } else
                rc = curl_easy_perform(curl);
        }
        if (rangeRequest && rc == CURLE_OK) {
            while (active && !windowComplete) {
                if (cancel && cancel())
                    throw Cancelled();
                int running = 0;
                auto result = curl_multi_perform(multi, &running);
                if (result != CURLM_OK)
                    throw std::runtime_error("Playback stream: " + std::string(curl_multi_strerror(result)));
                int left = 0;
                while (auto *message = curl_multi_info_read(multi, &left)) {
                    if (message->msg == CURLMSG_DONE) {
                        rc = message->data.result;
                        curl_multi_remove_handle(multi, curl);
                        active = false;
                    }
                }
                if (active && !windowComplete) {
                    int descriptors = 0;
                    result = curl_multi_poll(multi, nullptr, 0, 200, &descriptors);
                    if (result != CURLM_OK)
                        throw std::runtime_error("Playback stream wait failed");
                }
            }
        }
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        streamNext = rangeRequest ? requested + int64_t(written) : -1;
        if (cancel && cancel())
            throw Cancelled();
        if (status == 401 || status == 403)
            throw HttpError(status, "Backend account authorization expired or denied");
        if (status && status != 200 && status != 206)
            throw HttpError(status, "Playback HTTP " + std::to_string(status) + " at " + resource);
        if (allocationFailed)
            throw std::bad_alloc();
        if (invalidRange)
            throw std::runtime_error("Invalid playback Content-Range at " + resource);
        if (rc != CURLE_OK){
            auto message="Playback transport: " + std::string(curl_easy_strerror(rc)) +
                (transportDetail[0] ? " (" + std::string(transportDetail.data()) + ")" : "") + " at " + resource;
            if(status==0&&written==0&&(rc==CURLE_COULDNT_CONNECT||rc==CURLE_COULDNT_RESOLVE_HOST||rc==CURLE_OPERATION_TIMEDOUT))
                throw ConnectionFailure(message);
            throw std::runtime_error(message);
        }
        if (status != 200 && status != 206)
            throw std::runtime_error("Playback server returned no HTTP response at " + resource);
        return status;
    }
    long get(const std::string& resource,uint8_t* data,size_t limit,const std::string& range="",std::string* text=nullptr){
        // Retry only failures before a response/body. Never replay a partially
        // consumed stream or hide HTTP/authentication/TLS/data errors.
        for(unsigned attempt=0;;++attempt){
            try{return getOnce(resource,data,limit,range,text);}
            catch(const ConnectionFailure&){
                if(attempt==4)throw;
                stopTransfer();
                forceFresh=true;
                auto until=std::chrono::steady_clock::now()+std::chrono::milliseconds(std::min(1000u<<attempt,4000u));
                while(std::chrono::steady_clock::now()<until){
                    if(cancel&&cancel())throw Cancelled();
                    std::this_thread::sleep_for(std::chrono::milliseconds(25));
                }
            }
        }
    }
    void fetch(int64_t offset) {
        size_t wanted = total >= 0 ? size_t(std::min<int64_t>(BlockSize, total - offset)) : BlockSize;
        if (!wanted)
            throw std::runtime_error("Read beyond playback media");
        int64_t end = offset + int64_t(std::min<size_t>(wanted - 1, size_t(INT64_MAX - offset)));
        requested = offset;
        auto status = get(path, block.data(), wanted, std::to_string(offset) + "-" + std::to_string(end));
        int64_t size = -1;
        if (status == 206) {
            size = replyTotal;
            if (replyStart < 0 || replyStart > offset || replyEnd < offset || replyEnd >= size || size <= 0 ||
                !written || int64_t(written) > replyEnd - offset + 1)
                throw std::runtime_error("Invalid playback byte-range response");
        } else {
            // Sequential reads resume the paused HTTP stream. After a random
            // seek, a range-ignoring server needs its prefix streamed/discarded.
            // The M3U8 also supplies size for chunked full-file responses.
            curl_off_t length = -1;
            curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length);
            size = length >= 0 ? int64_t(length) : total;
            if (size <= 0 || offset >= size || !written)
                throw std::runtime_error("Incomplete playback stream or missing media length");
        }
        if (total >= 0 && (size != total || (!etag.empty() && replyEtag != etag)))
            throw std::runtime_error("Playback cache changed; restart this track");
        if (int64_t(written) > size - offset)
            throw std::runtime_error("Playback response exceeds media length");
        total = size;
        etag = replyEtag;
        blockStart = offset;
        blockLength = written;
    }
};
HttpInput::HttpInput(Config config, Cancel cancel) : impl(new Impl(std::move(config), std::move(cancel))) {}
HttpInput::~HttpInput() = default;
std::string HttpInput::text(const std::string &path, size_t initialCapacity) {
    std::string body;
    body.reserve(std::min<size_t>(initialCapacity, 32 * 1024));
    auto status = impl->get(path, nullptr, 0, "", &body);
    if (status != 200)
        throw std::runtime_error("Invalid playback metadata response");
    return body;
}
void HttpInput::open(const std::string &path, int64_t expectedSize) {
    impl->path = path;
    impl->position = 0;
    impl->total = expectedSize;
    impl->etag.clear();
    impl->blockStart = -1;
    impl->fetch(0);
}
int HttpInput::read(uint8_t *output, int count) {
    if (impl->cancel && impl->cancel())
        throw Cancelled();
    if (count <= 0)
        return AVERROR(EINVAL);
    if (impl->position >= impl->total)
        return AVERROR_EOF;
    if (impl->position < impl->blockStart || impl->position >= impl->blockStart + int64_t(impl->blockLength))
        impl->fetch(impl->position);
    auto offset = size_t(impl->position - impl->blockStart);
    auto n = std::min(size_t(count), impl->blockLength - offset);
    memcpy(output, impl->block.data() + offset, n);
    impl->position += n;
    return int(n);
}
int64_t HttpInput::seek(int64_t offset, int whence) {
    if (whence & AVSEEK_SIZE)
        return impl->total;
    whence &= ~AVSEEK_FORCE;
    int64_t base = whence == SEEK_SET   ? 0
                   : whence == SEEK_CUR ? impl->position
                   : whence == SEEK_END ? impl->total
                                        : -1;
    if (base < 0 || offset < -base || offset > impl->total - base)
        return AVERROR(EINVAL);
    return impl->position = base + offset;
}
} // namespace smusic
