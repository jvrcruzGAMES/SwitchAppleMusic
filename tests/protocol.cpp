#include <switchmusic/client.hpp>
#include <switchmusic/ipc.hpp>
#include <cassert>
#include <iostream>
using namespace smusic;
int main() {
    assert(encode("a/b? x") == "a%2Fb%3F%20x");
    const std::string session(43, 'a');
    Json sent = {{"op", "play"},
                 {"index", 0u},
                 {"queue", Json::array({{{"id", "i.123"}, {"type", "library-songs"}}})}};
    auto raw = encodeControlRequest(sent, session);
    auto decoded = decodeControlMessage(raw);
    assert(decoded["version"] == 3 && decoded["session"] == session && decoded["queue"] == sent["queue"]);
    int calls = 0;
    auto handler = [&](const Json &request) {
        ++calls;
        assert(request["session"] == session);
        return Json{{"accepted", true}};
    };
    assert(decodeControlMessage(dispatchControlMessage(raw, session, handler))["accepted"] == true);
    assert(calls == 1);
    for (auto invalid : {std::string{}, std::string("bad"), std::string("[]"), std::string("null"),
                         std::string(MaxControlBytes + 1, 'x')}) {
        assert(decodeControlMessage(dispatchControlMessage(invalid, session, handler)).contains("error"));
    }
    assert(calls == 1);
    for (auto version :
         {Json(2), Json(-1), Json(-4294967293LL), Json(4294967299ULL), Json("3"), Json(nullptr)}) {
        auto wrong = decoded;
        wrong["version"] = version;
        assert(
            decodeControlMessage(dispatchControlMessage(wrong.dump(), session, handler)).contains("error"));
    }
    assert(decodeControlMessage(dispatchControlMessage(raw, "revoked", handler)).contains("error"));
    assert(decodeControlMessage(dispatchControlMessage(raw, "", handler)).contains("error"));
    auto wrong = decoded;
    wrong["session"] = 42;
    assert(decodeControlMessage(dispatchControlMessage(wrong.dump(), session, handler)).contains("error"));
    assert(calls == 1);
    // Exercise both the exact accepted limit and one byte past it.
    auto sized = sent;
    sized["padding"] = "";
    auto overhead = encodeControlRequest(sized, session).size();
    sized["padding"] = std::string(MaxControlBytes - overhead, 'x');
    assert(encodeControlRequest(sized, session).size() == MaxControlBytes);
    assert(decodeControlMessage(encodeControlRequest(sized, session)).is_object());
    sized["padding"].get_ref<std::string &>().push_back('x');
    bool rejected = false;
    try {
        encodeControlRequest(sized, session);
    } catch (...) {
        rejected = true;
    }
    assert(rejected);
    assert(decodeControlMessage(encodeControlReply({{"large", std::string(MaxControlBytes, 'x')}}))
               .contains("error"));
    assert(control(Config{}, {{"op", "state"}}).contains("error"));
    std::cout << "IPC JSON roundtrip, authorization/revocation, version types, malformed objects and exact "
                 "buffer limits passed\n";
}
