/*
 * Task 6.8a acceptance against a real openshow-worker (review R11): what the
 * control transport does with input that is too big, the wrong shape, or
 * outside the range an argument can name. One rule runs through every check: a
 * message with a problem is a REFUSAL that keeps the session, because a worker
 * that hangs up on the operator's paste of large text costs the whole rig for a
 * typing mistake. Only a stream that lost framing — a frame over the byte
 * budget, a payload nested past the parse ceiling — ends the connection, and
 * even then the worker exits clean rather than crashing. Pinned here rather
 * than at library level because it is the routing between decoder and session:
 * an oversized scene refuses AND the next command works; an integer is range
 * checked before it is narrowed, so 2^32 is not monitor 0; and a message with
 * no id is neither answered nor executed (task 6.3a's lost update). Needs a
 * display, so it carries the `display` label. The worker stops when its
 * frontend goes, so every scenario that ends a stream owns its own worker.
 */

#include <boost/json.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "openshow/protocol/preview_wire.h"
#include "openshow/protocol/protocol.h"
#include "openshow/scene/demo.h"
#include "openshow/scene/scene.h"

namespace {

namespace proto = openshow::protocol;
namespace scene = openshow::scene;

int gFailures = 0;

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++gFailures;
    }
    return condition;
}

std::string gWorkerBinary = "./openshow-worker";

std::string socketPath(const char* tag) {
    return "/tmp/openshow-68a-" + std::to_string(getpid()) + "-" + tag + ".sock";
}

// --- one supervised session -----------------------------------------------

struct Session {
    pid_t pid = -1;
    int fd = -1;
    proto::Decoder decoder;
};

// Reads everything the worker sends within `ms`. An empty answer is a real
// observation here: it is what a notification is supposed to produce.
std::vector<boost::json::value> drainFor(Session& session, int ms) {
    std::vector<boost::json::value> arrived;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd pending{session.fd, POLLIN, 0};
        const int ready = ::poll(&pending, 1, 20);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (ready == 0) {
            continue;
        }
        char buffer[8192];
        const ssize_t count = ::recv(session.fd, buffer, sizeof(buffer), 0);
        if (count <= 0) {
            break;  // the stream ended; the caller decides what that means
        }
        session.decoder.feed(std::string_view(buffer, static_cast<std::size_t>(count)));
        boost::json::value message;
        std::string error;
        while (session.decoder.next(&message, &error)) {
            arrived.push_back(std::move(message));
        }
    }
    return arrived;
}

// Sends one framed request and returns the reply carrying its id. Returns the
// moment that reply arrives rather than waiting out a window: this fixture makes
// dozens of calls, and a fixed settle time per call would turn a two-second
// scenario into a minute of sitting there.
bool call(Session& session, std::int64_t id, const char* method,
           boost::json::object params, boost::json::value* reply) {
    std::string wire;
    if (!proto::appendFrame(proto::makeRequest(id, method, std::move(params)), &wire) ||
        ::send(session.fd, wire.data(), wire.size(), MSG_NOSIGNAL) < 0) {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd pending{session.fd, POLLIN, 0};
        const int ready = ::poll(&pending, 1, 20);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (ready == 0) {
            continue;
        }
        char buffer[8192];
        const ssize_t count = ::recv(session.fd, buffer, sizeof(buffer), 0);
        if (count <= 0) {
            return false;  // the stream ended without answering: that is a failure
        }
        session.decoder.feed(std::string_view(buffer, static_cast<std::size_t>(count)));
        boost::json::value message;
        std::string error;
        while (session.decoder.next(&message, &error)) {
            if (!message.is_object()) {
                continue;
            }
            const auto idField = message.as_object().find("id");
            const bool mine =
                idField != message.as_object().end() &&
                ((idField->value().is_int64() && idField->value().as_int64() == id) ||
                 (idField->value().is_uint64() &&
                  idField->value().as_uint64() == static_cast<std::uint64_t>(id)));
            if (mine && !message.as_object().contains("method")) {
                *reply = message;
                return true;
            }
        }
    }
    return false;
}

// Bytes exactly as given: the hostile cases cannot go through appendFrame,
// which is itself one of the things under test.
bool sendRaw(Session& session, const std::string& bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t written = ::send(session.fd, bytes.data() + offset,
                                       bytes.size() - offset, MSG_NOSIGNAL);
        if (written <= 0) {
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

std::string frame(std::string payload, std::uint32_t claimedLength = 0) {
    const std::uint32_t length =
        claimedLength != 0 ? claimedLength : static_cast<std::uint32_t>(payload.size());
    std::string wire;
    wire.push_back(static_cast<char>(length & 0xFF));
    wire.push_back(static_cast<char>((length >> 8) & 0xFF));
    wire.push_back(static_cast<char>((length >> 16) & 0xFF));
    wire.push_back(static_cast<char>((length >> 24) & 0xFF));
    wire.append(payload);
    return wire;
}

// Spawns the worker, connects, and handshakes. Every scenario starts from a
// session this binary believes is live.
bool openSession(Session& session, const char* tag) {
    const std::string path = socketPath(tag);
    const pid_t pid = fork();
    if (pid == 0) {
        execl(gWorkerBinary.c_str(), "openshow-worker", "--socket", path.c_str(),
              static_cast<char*>(nullptr));
        _exit(127);
    }
    session.pid = pid;
    if (pid <= 0) {
        return false;
    }
    for (int attempt = 0; attempt < 100 && session.fd < 0; ++attempt) {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            return false;
        }
        sockaddr_un address = {};
        address.sun_family = AF_UNIX;
        std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
            session.fd = fd;
            break;
        }
        close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (session.fd < 0) {
        return false;
    }
    boost::json::value reply;
    if (!call(session, 1, "hello", proto::helloParams("limits-test", 0, 1), &reply)) {
        return false;
    }
    return reply.as_object().contains("result");
}

// Hangs up and reaps. A worker whose frontend was dropped still has to exit
// with a status: dropping a hostile stream must not cost the operator a crash.
void closeSession(Session& session, bool expectCleanExit = false) {
    if (session.fd >= 0) {
        ::close(session.fd);
        session.fd = -1;
    }
    if (session.pid <= 0) {
        return;
    }
    int status = 0;
    bool reaped = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        const pid_t result = waitpid(session.pid, &status, WNOHANG);
        if (result == session.pid) {
            reaped = true;
            break;
        }
        if (result < 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    if (!reaped) {
        ::kill(session.pid, SIGKILL);
        waitpid(session.pid, &status, 0);
        check(!expectCleanExit, "a worker whose stream was refused still exits");
    } else if (expectCleanExit) {
        check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "and it exits with status 0 rather than on a signal");
    }
    session.pid = -1;
}

// --- small readers over replies -------------------------------------------

std::string errorText(const boost::json::value& reply) {
    if (!reply.is_object()) {
        return "";
    }
    const auto error = reply.as_object().find("error");
    if (error == reply.as_object().end() || !error->value().is_object()) {
        return "";
    }
    const auto message = error->value().as_object().find("message");
    return message == error->value().as_object().end() || !message->value().is_string()
               ? std::string()
               : std::string(message->value().as_string());
}

bool isRefusal(const boost::json::value& reply) {
    if (!reply.is_object()) {
        return false;
    }
    const auto error = reply.as_object().find("error");
    return error != reply.as_object().end() && error->value().is_object();
}

std::int64_t errorCode(const boost::json::value& reply) {
    if (!isRefusal(reply)) {
        return 0;
    }
    const auto code = reply.as_object().at("error").as_object().find("code");
    return code == reply.as_object().at("error").as_object().end() ||
                   !code->value().is_int64()
               ? 0
               : code->value().as_int64();
}

std::int64_t replyId(const boost::json::value& reply) {
    if (!reply.is_object()) {
        return -1;
    }
    const auto found = reply.as_object().find("id");
    if (found == reply.as_object().end()) {
        return -1;
    }
    if (found->value().is_int64()) {
        return found->value().as_int64();
    }
    return found->value().is_uint64()
               ? static_cast<std::int64_t>(found->value().as_uint64())
               : -1;
}

std::int64_t numberAt(const boost::json::value& object, const char* key) {
    if (!object.is_object()) {
        return -1;
    }
    const auto found = object.as_object().find(key);
    if (found == object.as_object().end()) {
        return -1;
    }
    if (found->value().is_uint64()) {
        return static_cast<std::int64_t>(found->value().as_uint64());
    }
    return found->value().is_int64() ? found->value().as_int64() : -1;
}

boost::json::value statusOf(Session& session, std::int64_t& id) {
    boost::json::value reply;
    if (!call(session, ++id, "output.status", {}, &reply) ||
        !reply.as_object().contains("result")) {
        return nullptr;
    }
    return reply.at("result");
}

std::int64_t activeRevision(const boost::json::value& status) {
    if (!status.is_object()) {
        return -1;
    }
    const auto sceneState = status.as_object().find("scene");
    if (sceneState == status.as_object().end() || !sceneState->value().is_object()) {
        return -1;
    }
    return numberAt(sceneState->value(), "active");
}

// Polls status until the composed output reports `revision` on air. An acked
// scene.present means the store names the revision; the audience moves when a
// composed frame takes it, so a check about WHERE the audience is has to wait
// for that frame rather than read the reply.
bool waitActive(Session& session, std::int64_t& id, std::int64_t revision) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        if (activeRevision(statusOf(session, id)) == revision) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return activeRevision(statusOf(session, id)) == revision;
}

// A refusal that says WHY. `expectIn` is the phrase naming the rule the message
// broke; a bare code proves less, and a disconnect proves the bug rather than
// the fix.
bool refusedWith(Session& session, std::int64_t& id, const char* method,
                 boost::json::object params, const std::string& expectIn, const char* what) {
    boost::json::value reply;
    const bool answered = call(session, ++id, method, std::move(params), &reply);
    const std::string text = errorText(reply);
    const bool ok = answered && isRefusal(reply) &&
                    errorCode(reply) == static_cast<std::int64_t>(proto::kInvalidParams) &&
                    text.find(expectIn) != std::string::npos;
    if (!ok) {
        std::fprintf(stderr, "  '%s' answered: %s\n", method,
                     answered ? (text.empty() ? "not a refusal" : text.c_str())
                              : "nothing at all");
    }
    return check(ok, what);
}

// The session still works. This is the half of R11 a refusal alone cannot
// prove: a worker that answers an error and then hangs up passes it by accident.
bool stillUsable(Session& session, std::int64_t& id, const char* what) {
    boost::json::value reply;
    return check(call(session, ++id, "ping", {}, &reply) &&
                     reply.as_object().contains("result"),
                 what);
}

// A scene to break: the compiled demo title, the same shape the UI prepares.
boost::json::object baseScene() {
    const auto documents = scene::demoDocuments();
    scene::Scene compiled;
    std::string error;
    if (!scene::compileDemoSlide(documents, 0, &compiled, &error)) {
        std::fprintf(stderr, "  demo scene did not compile: %s\n", error.c_str());
        return {};
    }
    return scene::toJson(compiled).as_object();
}

// A copy of `sceneJson` whose items carry exactly these lines, expressed as one
// entry per line holding its length in characters. Lengths are per LINE rather
// than per item on purpose: a helper that split a big number into legal lines
// would quietly never test the line budget, and the live fixture has to be able
// to break each of the three text budgets separately.
boost::json::object itemsOf(const boost::json::object& sceneJson,
                            const std::vector<std::vector<std::size_t>>& lineChars) {
    boost::json::object tampered = sceneJson;
    const boost::json::object templateItem =
        sceneJson.at("items").as_array().at(0).as_object();
    boost::json::array items;
    for (const auto& itemLines : lineChars) {
        boost::json::object item = templateItem;
        boost::json::array lines;
        for (const std::size_t chars : itemLines) {
            lines.emplace_back(std::string(chars, 'v'));
        }
        item["lines"] = std::move(lines);
        items.push_back(std::move(item));
    }
    tampered["items"] = std::move(items);
    return tampered;
}

// --- scenarios ------------------------------------------------------------

// Oversized authored content and media-byte references: refused, explained,
// and the session carries on as if nothing happened.
void testOversizedContent() {
    Session session;
    if (!check(openSession(session, "oversized"), "worker up and handshaked")) {
        return;
    }
    std::int64_t id = 10;
    const boost::json::object plain = baseScene();

    // Every text budget, one message each, from one byte past the boundary. All
    // of them are frames the transport is glad to carry — what refuses them is
    // the application's own budget, which is R11's point: the review's repro is
    // an operator pasting something long, not someone attacking the framing.
    const std::size_t linesPerItem = scene::kMaxItemTextChars / scene::kMaxLineChars;
    struct Oversize {
        const char* what;
        std::vector<std::vector<std::size_t>> lineChars;
        const char* expectIn;
    };
    for (const Oversize& oversize :
         {Oversize{"one line past the line budget", {{scene::kMaxLineChars + 1}},
                   "characters is over the limit of"},
          Oversize{"one line past the line-count budget",
                   std::vector<std::vector<std::size_t>>(
                       1, std::vector<std::size_t>(scene::kMaxItemLines + 1, 4)),
                   "lines are over the limit"},
          Oversize{"one line past a layer's own text budget",
                   std::vector<std::vector<std::size_t>>(
                       1, std::vector<std::size_t>(linesPerItem + 1, scene::kMaxLineChars)),
                   "item's text exceeds"},
          Oversize{"one layer past the layer-count budget",
                   std::vector<std::vector<std::size_t>>(scene::kMaxSceneItems + 1,
                                                         std::vector<std::size_t>{4}),
                   "items is over the limit"},
          Oversize{"one layer past the whole-slide text budget",
                   std::vector<std::vector<std::size_t>>(
                       scene::kMaxSceneTextChars / scene::kMaxItemTextChars + 1,
                       std::vector<std::size_t>(linesPerItem, scene::kMaxLineChars)),
                   "slide's text exceeds"}}) {
        const boost::json::object tooBig = itemsOf(plain, oversize.lineChars);
        std::string wire;
        if (!check(proto::appendFrame(proto::makeRequest(1, "scene.prepare", tooBig),
                                      &wire) &&
                       wire.size() < proto::kMaxMessageBytes,
                   "each oversized scene is still a frame the transport would carry")) {
            break;
        }
        refusedWith(session, id, "scene.prepare", tooBig, oversize.expectIn, oversize.what);
        stillUsable(session, id, "and the worker is still serving commands after it");
    }

    // A data: URI is a media-byte payload wearing an asset path, and a network
    // reference is a fetch the worker has no contract for. R11 asks for both to
    // be refused as inputs, with local paths kept, so both sides are pinned.
    const std::string base64 = "iVBORw0KGgoAAAANSUhEUg" + std::string(200000, 'A');
    for (const std::string& reference : {std::string("data:image/png;base64,") + base64,
                                         std::string("http://host.example/backdrop.png"),
                                         std::string("DATA:image/png;base64,AAAA")}) {
        boost::json::object smuggled = plain;
        smuggled["background"].as_object()["image"] = true;
        smuggled["background"].as_object()["assetPath"] = reference;
        refusedWith(session, id, "scene.prepare", smuggled, "local files only",
                    "a media reference that is not a local file is refused");
        stillUsable(session, id, "and a refused reference does not end the session");
    }

    // The length budget still bounds what IS a local path: 400 KB of filename
    // is not a reference the renderer can open either, and it is refused by the
    // number rather than by the scheme rule — which is the ordering scene.cpp
    // chose on purpose (an unparseable URI gets the reason that fits it).
    {
        boost::json::object longPath = plain;
        longPath["background"].as_object()["image"] = true;
        longPath["background"].as_object()["assetPath"] =
            "/shows/" + std::string(scene::kMaxPathChars, 'p') + ".png";
        refusedWith(session, id, "scene.prepare", longPath, "over the limit of",
                    "an absurdly long local path is refused by its length");
    }

    // The supported case: a local path, with a space in it, is content.
    boost::json::object local = plain;
    local["background"].as_object()["image"] = false;
    local["background"].as_object()["assetPath"] = "/shows/service A/backdrop.jpg";
    boost::json::value reply;
    check(call(session, ++id, "scene.prepare", local, &reply) &&
              reply.as_object().contains("result"),
          "a legitimate local asset path is still accepted");

    // An unknown key is a field the operator set that would never appear on
    // air, so it is refused by name rather than ignored.
    boost::json::object typo = plain;
    typo["backgroundColour"] = 0x000000;
    refusedWith(session, id, "scene.prepare", typo, "has no field",
                "a key the scene format does not define is refused by name");
    boost::json::object nestedTypo = plain;
    nestedTypo["items"].as_array()[0].as_object()["size"] = 40.0;
    refusedWith(session, id, "scene.prepare", nestedTypo, "has no field 'size'",
                "and so is a near-miss key inside a text item");

    // And the shape the UI really sends still works, so none of the above is a
    // refusal that also broke the supported path.
    check(call(session, ++id, "scene.prepare", plain, &reply) &&
              reply.as_object().contains("result"),
          "the unmodified compiled scene still prepares");
    check(call(session, ++id, "shutdown", {}, &reply) &&
              reply.at("result").as_object().at("ok").as_bool(),
          "shutdown served");
    closeSession(session);
}

// Integer arguments are range-checked where they arrive. Narrowing first is how
// 4294967296 becomes monitor 0 — a real destination — so the audience's output
// moves on an argument nobody sent.
void testIntegerRanges() {
    Session session;
    if (!check(openSession(session, "ranges"), "worker up for the range checks")) {
        return;
    }
    std::int64_t id = 20;
    const boost::json::value before = statusOf(session, id);
    const std::int64_t monitorBefore = numberAt(before, "monitor");
    check(monitorBefore != -1, "status names where the output is before the attempts");

    for (const boost::json::value bogus : {boost::json::value(static_cast<std::uint64_t>(
                                               4294967296ULL)),
                                           boost::json::value(static_cast<std::int64_t>(
                                               2147483648LL)),
                                           boost::json::value(static_cast<std::int64_t>(-2)),
                                           boost::json::value("0"),
                                           boost::json::value(1.5)}) {
        refusedWith(session, id, "output.select", boost::json::object{{"monitor", bogus}},
                    "output.select requires",
                    "a monitor index outside what an index can name is refused");
    }
    check(numberAt(statusOf(session, id), "monitor") == monitorBefore,
          "and none of those arguments moved the audience's output");
    stillUsable(session, id, "and the session is alive after the refusals");

    // Preview geometry: absent means the default, present means a REAL value.
    // Defaulting a "width" of "wide" starts a stream with geometry the client
    // neither asked for nor can detect.
    for (const char* key : {"width", "height", "fps"}) {
        refusedWith(session, id, "preview.start", boost::json::object{{key, "wide"}},
                    std::string("preview.start \"") + key,
                    "a non-numeric preview argument is refused by name");
        refusedWith(session, id, "preview.start",
                    boost::json::object{{key, static_cast<std::uint64_t>(4294967296ULL)}},
                    std::string("preview.start \"") + key,
                    "a preview argument past 32 bits is refused, not truncated");
    }
    const boost::json::value previewStatus = statusOf(session, id);
    check(previewStatus.is_object() &&
              !previewStatus.as_object().at("preview").as_object().at("running").as_bool(),
          "and no preview was started by the arguments that were refused");

    // A legal-looking preview release with a slot that only fits in 33 bits:
    // nothing is running, so the only honest outcome is that no slot is freed
    // and the transport is still there. (The width rule itself is pinned in
    // preview_stream_tests and in RpcServer's jsonUint32; here it must not
    // arrive as slot 1.)
    boost::json::value released;
    check(call(session, ++id, proto::kMethodPreviewReleased,
               boost::json::object{{"stream", "openshow-preview-none"},
                                   {"slot", static_cast<std::uint64_t>(4294967297ULL)},
                                   {"seq", 1}},
               &released) &&
              released.as_object().contains("result"),
          "a preview.released with a 33-bit slot is answered, and releases nothing");
    stillUsable(session, id, "and a wide slot index left the transport usable");

    boost::json::value reply;
    check(call(session, ++id, "shutdown", {}, &reply), "shutdown served");
    closeSession(session);
}

// The envelope: an id that cannot be answered is refused under 0, and a
// notification is neither answered nor executed.
void testIdsAndNotifications() {
    Session session;
    if (!check(openSession(session, "envelope"), "worker up for the envelope checks")) {
        return;
    }
    std::int64_t id = 30;

    // A string id: answered under 0, JSON-RPC's "the id could not be
    // determined". Answering under a narrowed guess would hand the reply to a
    // different request — task 6.3a correlates by id.
    struct BadId {
        const char* frame;
        const char* expectIn;
    };
    for (const BadId bad : {BadId{R"({"jsonrpc":"2.0","id":"7","method":"ping"})", "\"7\""},
                            BadId{R"({"jsonrpc":"2.0","id":true,"method":"ping"})", "boolean"},
                            BadId{R"({"jsonrpc":"2.0","id":null,"method":"ping"})", "null"},
                            BadId{R"({"jsonrpc":"2.0","id":7.5,"method":"ping"})", "integer"},
                            BadId{R"({"jsonrpc":"2.0","id":18446744073709551615,"method":"ping"})",
                                  "signed 64-bit"}}) {
        if (!check(sendRaw(session, frame(bad.frame)), "the bad-id frame was written")) {
            closeSession(session);
            return;
        }
        bool underZero = false;
        bool namedTheProblem = false;
        for (const auto& answer : drainFor(session, 1000)) {
            if (isRefusal(answer) && replyId(answer) == 0) {
                underZero = true;
                namedTheProblem = errorText(answer).find(bad.expectIn) != std::string::npos;
            }
        }
        check(underZero, "an unanswerable id is refused under id 0, not a narrowed guess");
        check(namedTheProblem, "and the refusal says what was wrong with that id");
    }
    stillUsable(session, id, "and the session survives five unanswerable ids");

    // A bare response, sent as if it were a request: refused under its own id,
    // which the client that sent it can correlate.
    if (check(sendRaw(session, frame(R"({"jsonrpc":"2.0","id":777,"result":{}})")),
              "the bare-response frame was written")) {
        bool underItsOwnId = false;
        for (const auto& answer : drainFor(session, 1000)) {
            if (isRefusal(answer) && replyId(answer) == 777) {
                underItsOwnId = true;
            }
        }
        check(underItsOwnId, "a bare response is refused under the id it carried");
    }

    // Routing indexes on the method name, and the refusal quotes it — clipped,
    // so the answer cannot be as large as the frame it is complaining about.
    check(sendRaw(session, frame(R"({"jsonrpc":"2.0","id":888,"method":")" +
                                 std::string(2000, 'm') + R"("})")),
          "the 2000-byte method frame was written");
    bool namedTheLength = false;
    for (const auto& answer : drainFor(session, 1000)) {
        const std::string text = errorText(answer);
        if (isRefusal(answer) && text.find("longer than") != std::string::npos &&
            text.size() < 300) {
            namedTheLength = true;
        }
    }
    check(namedTheLength,
          "an over-long method is refused by the rule, in a reply far smaller than the frame");
    check(sendRaw(session, frame(R"({"jsonrpc":"2.0","id":889,"method":""})")),
          "the empty-method frame was written");
    bool emptyRefused = false;
    for (const auto& answer : drainFor(session, 1000)) {
        if (isRefusal(answer) &&
            errorText(answer).find("must not be empty") != std::string::npos) {
            emptyRefused = true;
        }
    }
    check(emptyRefused, "an empty method is refused");
    stillUsable(session, id, "and the session survives the method-name checks");

    // A command with no id: nothing answers it and — the part that matters —
    // nothing executes. Two scenes, one presented, then an attempt to move the
    // audience with a message that can never be acknowledged.
    boost::json::value reply;
    std::int64_t first = 0;
    std::int64_t second = 0;
    if (call(session, ++id, "scene.prepare", baseScene(), &reply) &&
        reply.as_object().contains("result")) {
        first = numberAt(reply.at("result"), "revision");
    }
    boost::json::object other = baseScene();
    other["slideId"] = "the-other-slide";
    if (call(session, ++id, "scene.prepare", other, &reply) &&
        reply.as_object().contains("result")) {
        second = numberAt(reply.at("result"), "revision");
    }
    if (!check(first != 0 && second != 0 && first != second,
               "two scenes are prepared to move the audience between")) {
        closeSession(session);
        return;
    }
    check(call(session, ++id, "scene.present", boost::json::object{{"revision", second}},
               &reply) &&
              reply.as_object().contains("result"),
          "the second scene is presented as a request");
    if (!check(waitActive(session, id, second),
               "and the status confirms which scene is on air")) {
        std::fprintf(stderr, "  asked for revision %lld, status reports active %lld\n",
                     static_cast<long long>(second),
                     static_cast<long long>(activeRevision(statusOf(session, id))));
    }

    const std::string commandWithoutId =
        std::string(R"({"jsonrpc":"2.0","method":"scene.present","params":{)") +
        R"("revision":)" + std::to_string(first) + "}}";
    check(sendRaw(session, frame(commandWithoutId)),
          "a scene.present sent with no id is written");
    check(drainFor(session, 1200).empty(),
          "and NOTHING answers a command sent without an id");
    check(activeRevision(statusOf(session, id)) == second,
          "and it was not executed either: an unacknowledgeable command must not move the "
          "audience");

    // The one method this protocol documents as fire-and-forget: no reply, and
    // the session carries on, because per-frame releases depend on it.
    check(sendRaw(session,
                  frame(std::string(R"({"jsonrpc":"2.0","method":")") +
                        proto::kMethodPreviewReleased +
                        R"(","params":{"stream":"openshow-preview-none","slot":0,"seq":1}})")),
          "a preview.released notification is written");
    check(drainFor(session, 600).empty(),
          "preview.released is answered by nothing, exactly as its contract says");
    stillUsable(session, id, "and the real per-frame notification path still works");

    check(call(session, ++id, "shutdown", {}, &reply), "shutdown served");
    closeSession(session);
}

// A stream that has lost framing is fatal to the STREAM, and the worker exits
// clean rather than crashing. Each case owns its worker, because this worker
// has one frontend and stops when it goes.
void testStreamEndsWhenFramingIsLost(const char* tag, const std::string& payload,
                                     std::uint32_t claimedLength, const char* what) {
    Session session;
    if (!check(openSession(session, tag), "worker up for a stream violation")) {
        return;
    }
    check(sendRaw(session, frame(payload, claimedLength)),
          "the violating frame was written");
    bool ended = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!ended && std::chrono::steady_clock::now() < deadline) {
        pollfd pending{session.fd, POLLIN, 0};
        const int ready = ::poll(&pending, 1, 100);
        if (ready < 0) {
            break;
        }
        if (ready == 0) {
            continue;
        }
        char buffer[64];
        const ssize_t count = ::recv(session.fd, buffer, sizeof(buffer), 0);
        ended = count == 0 || count < 0;  // hang-up or reset
    }
    check(ended, what);
    closeSession(session, /*expectCleanExit=*/true);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1) {
        gWorkerBinary = argv[1];
    }
    testOversizedContent();
    testIntegerRanges();
    testIdsAndNotifications();
    // A length beyond the budget is refused from the prefix alone, which is the
    // point of reading the length before the payload.
    testStreamEndsWhenFramingIsLost("frame", R"({"jsonrpc":"2.0")",
                                    proto::kMaxMessageBytes + 1,
                                    "a frame over the byte budget ends the stream");
    testStreamEndsWhenFramingIsLost(
        "deep", std::string(proto::kMaxParseDepth + 2, '[') +
                    std::string(proto::kMaxParseDepth + 2, ']'),
        0, "a frame nested past the parse ceiling ends the stream");

    if (gFailures != 0) {
        std::fprintf(stderr, "%d RPC limit check(s) failed\n", gFailures);
        return 1;
    }
    std::printf("ok: refused input keeps the session, and lost framing ends only the stream\n");
    return 0;
}
