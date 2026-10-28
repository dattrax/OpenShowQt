/*
 * Live preview ownership over the socket (task 6.5, review R5): ready
 * names its stream, a release must name it back, and slots held by a
 * consumer stay held against every kind of bogus release — including a
 * delayed release of a RETIRED stream whose slot/seq numbers happen to
 * match the live stream's exactly (R5's runtime probe: a new stream's
 * slot 0 opens at sequence 2, and an old stream's delayed
 * (slot 0, sequence 2) used to free it instantly).
 *
 * The consumer here deliberately never releases while it holds: the
 * three slots fill, then every further publish attempt must DROP, and
 * status.preview.published becomes the ownership meter — anything that
 * frees a slot without the exact (stream, slot, seq) proof shows up as
 * a published frame that cannot exist. Notifications are recorded as
 * delivered (ungated) so the replacement-phase checks are not vacuous;
 * releases are issued only from the ledger of the named stream they
 * belong to.
 *
 * Needs a display (the worker opens its output window): desktop preset,
 * label display.
 */

#include <boost/json.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

namespace {

namespace proto = openshow::protocol;

int gFailures = 0;

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++gFailures;
    }
    return condition;
}

std::string gWorkerBinary = "./openshow-worker";
std::string gSocket = "/tmp/openshow-owned.sock";

pid_t spawnWorker() {
    const pid_t pid = fork();
    if (pid == 0) {
        execl(gWorkerBinary.c_str(), "openshow-worker", "--socket", gSocket.c_str(),
              "--monitor", "-1", static_cast<char*>(nullptr));
        _exit(127);
    }
    return pid;
}

int connectRetry() {
    for (int attempt = 0; attempt < 100; ++attempt) {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            return -1;
        }
        sockaddr_un address = {};
        address.sun_family = AF_UNIX;
        std::strncpy(address.sun_path, gSocket.c_str(), sizeof(address.sun_path) - 1);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
            return fd;
        }
        close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return -1;
}

// One delivered preview.ready, recorded exactly as it arrived.
struct Ready {
    std::uint64_t slot = 0;
    std::uint64_t seq = 0;
    std::string stream;
};

std::vector<Ready> gDelivered;  // every ready ever delivered, ungated
proto::Decoder gDecoder;
std::int64_t gNextId = 1;

std::uint64_t numberValue(const boost::json::value& value) {
    return value.is_uint64() ? value.as_uint64()
                             : static_cast<std::uint64_t>(value.as_int64());
}

void sendRaw(int fd, const boost::json::value& message) {
    std::string wire;
    proto::appendFrame(message, &wire);
    ::send(fd, wire.data(), wire.size(), MSG_NOSIGNAL);
}

void handlePreviewReady(const boost::json::object& params) {
    auto number = [&params](const char* key) -> std::uint64_t {
        const auto entry = params.find(key);
        return entry == params.end() ? 0 : entry->value().to_number<std::uint64_t>();
    };
    Ready ready;
    ready.slot = number("slot");
    ready.seq = number("seq");
    const auto stream = params.find("stream");
    if (stream != params.end() && stream->value().is_string()) {
        ready.stream = std::string(stream->value().as_string());
    }
    gDelivered.push_back(std::move(ready));
}

// The production consumer's rule (as WorkerController now applies it):
// a notification naming a stream other than the one this client owns
// describes a segment it does not have — it is not part of the ledger
// and nothing is ever released under it.
std::vector<Ready> ledgerFor(const std::string& stream) {
    std::vector<Ready> owned;
    for (const auto& ready : gDelivered) {
        if (ready.stream == stream) {
            owned.push_back(ready);
        }
    }
    return owned;
}

// Pump the socket for a while so notifications queued by the worker
// reach the decoder (requests flush them; this consumes replies too).
void pump(int fd, int milliseconds) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                              std::chrono::duration<double>(milliseconds / 1000.0));
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd pending{fd, POLLIN, 0};
        const int ready = ::poll(&pending, 1, 50);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready > 0) {
            char buffer[65536];
            const ssize_t count = ::recv(fd, buffer, sizeof(buffer), 0);
            if (count <= 0) {
                return;
            }
            gDecoder.feed(std::string_view(buffer, static_cast<std::size_t>(count)));
        }
        std::string error;
        boost::json::value message;
        while (gDecoder.next(&message, &error)) {
            if (!message.is_object()) {
                continue;
            }
            const auto method = message.as_object().find("method");
            if (method != message.as_object().end()) {
                if (method->value().is_string() &&
                    std::string_view(method->value().as_string()) ==
                        proto::kMethodPreviewReady &&
                    message.as_object().contains("params") &&
                    message.at("params").is_object()) {
                    handlePreviewReady(message.at("params").as_object());
                }
                continue;  // replies to nothing we are waiting for
            }
            // Replies to in-flight requests are consumed here; call()
            // below waits for its own id and ignores these.
        }
    }
}

bool call(int fd, std::int64_t id, const char* method, boost::json::object params,
          boost::json::value* reply) {
    std::string wire;
    proto::appendFrame(proto::makeRequest(id, method, std::move(params)), &wire);
    if (::send(fd, wire.data(), wire.size(), MSG_NOSIGNAL) < 0) {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd pending{fd, POLLIN, 0};
        const int ready = ::poll(&pending, 1, 100);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready > 0) {
            char buffer[65536];
            const ssize_t count = ::recv(fd, buffer, sizeof(buffer), 0);
            if (count <= 0) {
                return false;
            }
            gDecoder.feed(std::string_view(buffer, static_cast<std::size_t>(count)));
        }
        std::string error;
        boost::json::value message;
        while (gDecoder.next(&message, &error)) {
            if (!message.is_object()) {
                continue;
            }
            const auto methodField = message.as_object().find("method");
            if (methodField != message.as_object().end()) {
                if (methodField->value().is_string() &&
                    std::string_view(methodField->value().as_string()) ==
                        proto::kMethodPreviewReady &&
                    message.as_object().contains("params") &&
                    message.at("params").is_object()) {
                    handlePreviewReady(message.at("params").as_object());
                }
                continue;
            }
            const auto found = message.as_object().find("id");
            if (found != message.as_object().end() && found->value().is_int64() &&
                found->value().as_int64() == id) {
                *reply = std::move(message);
                return true;
            }
        }
    }
    return false;
}

void sendRelease(int fd, std::uint64_t slot, std::uint64_t seq,
                 const std::string& stream) {
    boost::json::object params{{"slot", static_cast<std::int64_t>(slot)},
                               {"seq", static_cast<std::int64_t>(seq)}};
    if (!stream.empty()) {
        params["stream"] = stream;
    }
    sendRaw(fd, proto::makeNotification(proto::kMethodPreviewReleased, params));
}

struct PreviewState {
    bool running = false;
    std::string name;
    std::uint64_t published = 0;
    std::uint64_t dropped = 0;
    bool ok = false;
};

PreviewState previewState(int fd) {
    PreviewState out;
    boost::json::value reply;
    if (!call(fd, gNextId++, "output.status", {}, &reply) ||
        !reply.as_object().contains("result")) {
        return out;
    }
    const boost::json::object& preview =
        reply.at("result").as_object().at("preview").as_object();
    out.running = preview.at("running").as_bool();
    if (preview.at("name").is_string()) {
        out.name = std::string(preview.at("name").as_string());
    }
    out.published = numberValue(preview.at("published"));
    out.dropped = numberValue(preview.at("dropped"));
    out.ok = true;
    return out;
}

// Every layer toggle is one command revision, one presented frame and
// one preview publish attempt — the pump this fixture measures against.
void toggleLayers(int fd, int rounds) {
    for (int round = 0; round < rounds; ++round) {
        boost::json::value reply;
        call(fd, gNextId++, "layer.setEnabled",
             boost::json::object{{"layer", "background"},
                                 {"enabled", round % 2 == 0}},
             &reply);
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }
}

boost::json::object solidScene(std::uint32_t color, const char* word) {
    return boost::json::object{
        {"scene", 3},
        {"showId", "preview-ownership"},
        {"slideId", word},
        {"backgroundColor", color},
        {"background",
         boost::json::object{{"image", false}, {"fit", "cover"}, {"assetId", ""}, {"assetPath", ""}}},
        {"items",
         boost::json::array{boost::json::object{{"kind", "text"}, {"lines", boost::json::array{word}},
                                                {"color", 0xFFFFFFu},
                                                {"fontFamily", ""},
                                                {"fontSize", 120.0},
                                                {"x", 160.0},
                                                {"y", 100.0},
                                                {"width", 1600.0},
                                                {"height", 880.0},
                                                {"align", "center"},
                                                {"lineSpacing", 1.25}}}},
    };
}

// Presents a slide: one command revision, one presented frame, one
// publish attempt (plus the run loop's own frames).
bool presentScene(int fd, const boost::json::object& scene) {
    boost::json::value reply;
    if (!call(fd, gNextId++, "scene.prepare", scene, &reply) ||
        !reply.as_object().contains("result")) {
        return false;
    }
    const std::uint64_t store = numberValue(reply.at("result").as_object().at("revision"));
    return call(fd, gNextId++, "scene.present", boost::json::object{{"revision", store}},
                &reply) &&
           reply.as_object().contains("result");
}

// Waits until the render thread has actually built the stream named in
// status. Sound for a FRESH preview (startPreview zeroes running);
// after a swap the flag stays true across the rebuild, so the swap is
// confirmed there by the new stream's first published frame instead.
bool waitBuilt(int fd, const std::string& name) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (std::chrono::steady_clock::now() < deadline) {
        const PreviewState state = previewState(fd);
        if (state.ok && state.running && state.name == name) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    return false;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc > 1) {
        gWorkerBinary = argv[1];
    }
    ::unlink(gSocket.c_str());
    const pid_t worker = spawnWorker();
    const int fd = connectRetry();
    if (!check(fd >= 0, "connected to a live worker")) {
        kill(worker, SIGKILL);
        waitpid(worker, nullptr, 0);
        return 1;
    }
    boost::json::value reply;
    check(call(fd, gNextId++, "hello", proto::helloParams("ownership-test", 0, 1), &reply) &&
              reply.as_object().contains("result"),
          "handshake completed");
    {
        const boost::json::object& result = reply.at("result").as_object();
        check(result.at("protocol").as_object().at("minor").to_number<std::uint64_t>() ==
                  proto::kProtocolMinor,
              "hello reports the minor-4 protocol");
        const auto& caps = result.at("capabilities").as_array();
        check(std::any_of(caps.begin(), caps.end(), [](const boost::json::value& v) {
                  return v.is_string() && v.as_string() == "preview.stream-id.v1";
              }),
              "hello advertises preview.stream-id.v1");
    }

    // --- stream 1: fill three slots and hold every one of them ---
    check(call(fd, gNextId++, "preview.start",
               boost::json::object{{"width", 320}, {"height", 180}, {"fps", 30}}, &reply) &&
              reply.as_object().contains("result"),
          "preview.start succeeded");
    const std::string stream1 =
        std::string(reply.at("result").as_object().at("name").as_string());
    check(waitBuilt(fd, stream1), "the render thread brought stream 1 up");

    presentScene(fd, solidScene(0xFF0000u, "ALPHA"));
    toggleLayers(fd, 6);  // enough publish attempts to fill three slots
    pump(fd, 300);        // let the last notifications land
    const std::vector<Ready> held1 = ledgerFor(stream1);
    check(held1.size() >= 3, "the three slots filled with held frames");
    PreviewState filled = previewState(fd);
    check(filled.ok && filled.published == 3,
          "published stops at three: exactly the three slots, no more");
    bool allNamed = !held1.empty();
    for (const auto& ready : held1) {
        if (ready.stream != stream1) {
            allNamed = false;
        }
    }
    check(allNamed, "every ready names the live stream");

    // With all slots owned, further presents DROP — that is the meter.
    toggleLayers(fd, 8);
    PreviewState afterHolds = previewState(fd);
    check(afterHolds.published == 3, "held slots publish nothing further");
    check(afterHolds.dropped > filled.dropped,
          "presents against owned slots drop, honestly counted");

    // --- bogus releases free nothing ---
    // Wrong sequence (odd and future) with the right name, right
    // sequence with the WRONG name, and no name at all (the pre-6.5
    // wire, which must not keep working).
    for (const auto& ready : held1) {
        sendRelease(fd, ready.slot, ready.seq | 1, stream1);            // odd
        sendRelease(fd, ready.slot, ready.seq + 100, stream1);          // future
        sendRelease(fd, ready.slot, ready.seq, "/openshow-not-mine");   // wrong stream
        sendRelease(fd, ready.slot, ready.seq, "");                     // unnamed legacy
    }
    toggleLayers(fd, 8);
    PreviewState afterBogus = previewState(fd);
    check(afterBogus.published == 3,
          "odd, future, foreign-stream and unnamed releases free nothing");

    // --- the exact (stream, slot, seq) proof works, one slot at a time ---
    const Ready freed = held1.front();
    const std::size_t deliveredBefore = gDelivered.size();
    sendRelease(fd, freed.slot, freed.seq, stream1);
    toggleLayers(fd, 4);
    PreviewState afterExact = previewState(fd);
    check(afterExact.published == 4,
          "the exact release returned exactly one slot to the producer");
    if (check(gDelivered.size() == deliveredBefore + 1,
              "exactly one new ready arrived after the release")) {
        check(gDelivered.back().slot == freed.slot,
              "the new frame lands on the slot the release freed");
        check(gDelivered.back().stream == stream1, "and names the live stream");
    }

    // --- replacement with frames outstanding (R5's delayed-release case) ---
    // Everything stream1 ever delivered stays held while a second stream
    // starts. stream1's release numbers then arrive late — including
    // (0, 2), (1, 2), (2, 2): the EXACT triples the new stream publishes
    // with. None of them may free a live slot.
    check(call(fd, gNextId++, "preview.start",
               boost::json::object{{"width", 320}, {"height", 180}, {"fps", 30}}, &reply) &&
              reply.as_object().contains("result"),
          "the preview stream can be replaced");
    const std::string stream2 =
        std::string(reply.at("result").as_object().at("name").as_string());
    check(stream2 != stream1, "the replacement names a new segment");

    // The new stream's first published frame proves the render thread's
    // swap is done: status counters were zeroed at preview.start, so
    // published >= 1 is stream2's own bookkeeping. The reply carrying it
    // is the swap confirmation — the worker appends socket bytes in
    // causal order, so every notification it dequeued BEFORE the swap
    // (legitimately naming stream1 while it was still live) was already
    // delivered ahead of this reply. Anything arriving after this mark
    // was dequeued post-swap and must name stream2.
    const auto confirmDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    PreviewState n2Filled = previewState(fd);
    while ((!n2Filled.ok || n2Filled.published < 1) &&
           std::chrono::steady_clock::now() < confirmDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        n2Filled = previewState(fd);
    }
    check(n2Filled.ok && n2Filled.published >= 1,
          "the new stream publishes its first frame once the swap is applied");
    const std::size_t mark = gDelivered.size();

    // Fill the new stream's remaining slots (all held — nothing is
    // released for it yet).
    while (n2Filled.published < 3 && std::chrono::steady_clock::now() < confirmDeadline) {
        toggleLayers(fd, 3);
        pump(fd, 150);
        n2Filled = previewState(fd);
    }
    pump(fd, 200);  // settle the last notifications
    check(n2Filled.published == 3, "the new stream filled three slots and stopped there");

    // Delayed releases from the retired stream, numbers and all:
    // stream1's own ledger triples plus the exact opening triples of
    // the new stream. Every one must free nothing.
    for (const auto& ready : held1) {
        sendRelease(fd, ready.slot, ready.seq, stream1);
    }
    sendRelease(fd, 0, 2, stream1);
    sendRelease(fd, 1, 2, stream1);
    sendRelease(fd, 2, 2, stream1);
    toggleLayers(fd, 8);
    PreviewState afterDelayed = previewState(fd);
    check(afterDelayed.published == 3,
          "delayed releases of the old stream freed none of the new stream's slots");
    check(afterDelayed.dropped > n2Filled.dropped, "and presents keep dropping");

    // Past the mark, every notification the worker delivered was
    // dequeued after the swap — none may name the retired stream.
    bool staleReadySeen = false;
    for (std::size_t i = mark; i < gDelivered.size(); ++i) {
        if (gDelivered[i].stream != stream2) {
            staleReadySeen = true;
        }
    }
    check(!staleReadySeen, "no ready names the retired stream after the swap is confirmed");
    check(n2Filled.name == stream2, "status still names the live stream");

    // Positive control: the new stream's OWN exact releases flow it again.
    const std::vector<Ready> held2 = ledgerFor(stream2);
    check(held2.size() >= 3, "the new stream delivered its frames");
    for (const auto& ready : held2) {
        sendRelease(fd, ready.slot, ready.seq, stream2);
    }
    toggleLayers(fd, 4);
    PreviewState afterN2 = previewState(fd);
    check(afterN2.published > 3, "the new stream's exact releases return its slots");

    call(fd, gNextId++, "preview.stop", {}, &reply);
    call(fd, gNextId++, "shutdown", {}, &reply);
    int status = 0;
    waitpid(worker, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "the worker exited cleanly");
    close(fd);
    ::unlink(gSocket.c_str());

    if (gFailures > 0) {
        return 1;
    }
    std::printf("preview_ownership_tests: releases prove ownership or free nothing\n");
    return 0;
}
