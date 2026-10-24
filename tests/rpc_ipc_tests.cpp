/*
 * End-to-end task 1.2 checks against a real openshow-worker process:
 * the JSON-RPC handshake (including refusing commands before hello),
 * a layer command that observably changes the composed output state,
 * structured errors, and both ways the worker must exit — a served
 * "shutdown" and the frontend simply disconnecting.
 *
 * Needs a display (the worker opens its output window), so it belongs to
 * the desktop preset.
 */

#include <boost/json.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <thread>
#include <vector>

#include <poll.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "openshow/protocol/preview_wire.h"
#include "openshow/protocol/protocol.h"
#include "openshow/scene/demo.h"

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

std::string socketPath(const char* tag) {
    return "/tmp/openshow-rpc-" + std::to_string(getpid()) + "-" + tag + ".sock";
}

std::string gWorkerBinary = "./openshow-worker";

pid_t spawnWorker(const std::string& path) {
    const pid_t pid = fork();
    if (pid == 0) {
        execl(gWorkerBinary.c_str(), "openshow-worker", "--socket", path.c_str(),
              static_cast<char*>(nullptr));
        _exit(127);
    }
    return pid;
}

int connectRetry(const std::string& path) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            return -1;
        }
        sockaddr_un address = {};
        address.sun_family = AF_UNIX;
        std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
            return fd;
        }
        close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return -1;
}

proto::Decoder gDecoder;

// preview.ready notifications can interleave with any reply once previews
// are running; they queue here so the preview scenario still finds them.
std::deque<boost::json::object> gReadyQueue;

void routeNotification(const boost::json::value& message) {
    if (!message.is_object()) {
        return;
    }
    const boost::json::object& object = message.as_object();
    const auto method = object.find("method");
    if (method != object.end() && method->value().is_string() &&
        method->value().as_string() == proto::kMethodPreviewReady &&
        object.contains("params") && object.at("params").is_object()) {
        gReadyQueue.emplace_back(object.at("params").as_object());
    }
}

bool messageHasId(const boost::json::value& message, std::int64_t id) {
    const auto found = message.as_object().find("id");
    if (found == message.as_object().end()) {
        return false;
    }
    if (found->value().is_int64()) {
        return found->value().as_int64() == id;
    }
    return found->value().is_uint64() &&
           found->value().as_uint64() == static_cast<std::uint64_t>(id);
}

// Sends one framed request and waits (bounded) for the reply carrying its
// id; notifications that arrive on the way are routed, not mistaken for an
// answer.
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
            char buffer[4096];
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
            if (message.as_object().contains("method")) {
                routeNotification(message);
            } else if (messageHasId(message, id)) {
                *reply = std::move(message);
                return true;
            }
        }
    }
    return false;
}

bool sendNotification(int fd, const char* method, boost::json::object params) {
    std::string wire;
    proto::appendFrame(proto::makeNotification(method, std::move(params)), &wire);
    return ::send(fd, wire.data(), wire.size(), MSG_NOSIGNAL) >= 0;
}

bool isErrorCode(const boost::json::value& reply, proto::ErrorCode code) {
    return reply.is_object() && reply.as_object().contains("error") &&
           reply.as_object().at("error").as_object().at("code").as_int64() ==
               static_cast<int>(code);
}

bool fieldEquals(const boost::json::value& object, const char* field, const char* expected) {
    if (!object.is_object()) {
        return false;
    }
    const auto found = object.as_object().find(field);
    return found != object.as_object().end() && found->value().is_string() &&
           found->value().as_string() == expected;
}

std::uint64_t numberValue(const boost::json::value& value) {
    return value.is_uint64() ? value.as_uint64()
                             : static_cast<std::uint64_t>(value.as_int64());
}

// Polls output.status until it reports the given monitor (or timeout).
bool waitForMonitor(int fd, int wanted, std::uint64_t atLeastRevision, int seconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    std::int64_t id = 100;
    while (std::chrono::steady_clock::now() < deadline) {
        boost::json::value reply;
        if (call(fd, ++id, "output.status", {}, &reply)) {
            const boost::json::value& status = reply.at("result");
            if (status.as_object().at("monitor").as_int64() == wanted &&
                numberValue(status.as_object().at("revision")) >= atLeastRevision) {
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

// Reads the scene sub-object out of an output.status result.
bool sceneField(const boost::json::value& status, const char* field,
                std::uint64_t* out) {
    if (!status.is_object()) {
        return false;
    }
    const auto scene = status.as_object().find("scene");
    if (scene == status.as_object().end() || !scene->value().is_object()) {
        return false;
    }
    const auto found = scene->value().as_object().find(field);
    if (found == scene->value().as_object().end()) {
        return false;
    }
    *out = numberValue(found->value());
    return true;
}

// Polls output.status until the scene store reports the wanted active
// revision at or past atLeastRevision (or timeout).
bool waitForSceneActive(int fd, std::uint64_t wanted, std::uint64_t atLeastRevision,
                        int seconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    std::int64_t id = 400;
    while (std::chrono::steady_clock::now() < deadline) {
        boost::json::value reply;
        if (call(fd, ++id, "output.status", {}, &reply)) {
            const boost::json::value& status = reply.at("result");
            std::uint64_t active = 0;
            if (sceneField(status, "active", &active) && active == wanted &&
                numberValue(status.as_object().at("revision")) >= atLeastRevision) {
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

// Waits (bounded) for the next preview.ready notification, whether it
// already arrived interleaved with some earlier reply or comes in now.
bool nextPreviewReady(int fd, boost::json::object* ready, int seconds) {
    if (!gReadyQueue.empty()) {
        *ready = std::move(gReadyQueue.front());
        gReadyQueue.pop_front();
        return true;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    for (;;) {
        const auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::milliseconds(0)) {
            return false;
        }
        pollfd pending{fd, POLLIN, 0};
        const int readyFd =
            ::poll(&pending, 1,
                   static_cast<int>(
                       std::chrono::duration_cast<std::chrono::milliseconds>(remaining)
                           .count()));
        if (readyFd < 0 && errno == EINTR) {
            continue;
        }
        if (readyFd > 0) {
            char buffer[65536];
            const ssize_t count = ::recv(fd, buffer, sizeof(buffer), 0);
            if (count <= 0) {
                return false;
            }
            gDecoder.feed(std::string_view(buffer, static_cast<std::size_t>(count)));
            std::string error;
            boost::json::value message;
            while (gDecoder.next(&message, &error)) {
                routeNotification(message);
            }
            if (!gReadyQueue.empty()) {
                *ready = std::move(gReadyQueue.front());
                gReadyQueue.pop_front();
                return true;
            }
        }
    }
}

// Throws away queued notifications (and any still buffered on the socket):
// after a stop they belong to a stream that no longer exists.
void drainReadyNotifications(int fd) {
    gReadyQueue.clear();
    for (;;) {
        pollfd pending{fd, POLLIN, 0};
        if (::poll(&pending, 1, 0) <= 0) {
            break;
        }
        char buffer[65536];
        const ssize_t count = ::recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);
        if (count <= 0) {
            break;
        }
        gDecoder.feed(std::string_view(buffer, static_cast<std::size_t>(count)));
        std::string error;
        boost::json::value message;
        while (gDecoder.next(&message, &error)) {
            routeNotification(message);
        }
    }
    gReadyQueue.clear();
}

// Reads the counters this scenario cares about out of one output.status.
bool readPreviewCounts(int fd, std::int64_t id, std::uint64_t* frames,
                       std::uint64_t* published, std::uint64_t* dropped) {
    boost::json::value reply;
    if (!call(fd, id, "output.status", {}, &reply)) {
        return false;
    }
    const boost::json::object& status = reply.at("result").as_object();
    *frames = numberValue(status.at("frames"));
    const boost::json::object& preview = status.at("preview").as_object();
    *published = numberValue(preview.at("published"));
    *dropped = numberValue(preview.at("dropped"));
    return true;
}

// Waits for one preview.ready, copies its payload under the seqlock and
// releases the slot; true only when the sequence says nothing moved during
// the copy.
bool copyOneFrame(int fd, const std::uint8_t* segment, std::size_t bytesPerSlot,
                  std::uint64_t width, std::uint64_t height,
                  std::vector<std::uint8_t>* pixels) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    boost::json::object ready;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!nextPreviewReady(fd, &ready, 3)) {
            return false;
        }
        const std::uint64_t slot = numberValue(ready.at("slot"));
        const std::uint64_t seq = numberValue(ready.at("seq"));
        const std::uint64_t rowBytes = numberValue(ready.at("rowBytes"));
        bool intact = false;
        if (slot < proto::kPreviewSlotCount && rowBytes == width * 4) {
            const std::uint8_t* slotBase =
                segment + proto::previewSlotOffset(static_cast<std::uint32_t>(slot),
                                                   bytesPerSlot);
            const auto* header =
                reinterpret_cast<const proto::PreviewSlotHeader*>(slotBase);
            const std::uint64_t seqBefore =
                std::atomic_ref<const std::uint64_t>(header->seq).load();
            if (seqBefore == seq && (seqBefore & 1) == 0) {
                pixels->resize(static_cast<std::size_t>(rowBytes) * height);
                std::memcpy(pixels->data(), slotBase + sizeof(proto::PreviewSlotHeader),
                            pixels->size());
                intact = std::atomic_ref<const std::uint64_t>(header->seq).load() ==
                         seqBefore;
            }
        }
        sendNotification(fd, proto::kMethodPreviewReleased,
                         boost::json::object{{"slot", slot}, {"seq", seq}});
        if (intact) {
            return true;
        }
    }
    return false;
}

// Waits (bounded) for waitpid to report an exit; returns the exit code or -1.
int waitExit(pid_t pid, int seconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        int status = 0;
        const pid_t done = ::waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return -1;
}

void killWorker(pid_t pid) {
    ::kill(pid, SIGKILL);
    int status = 0;
    ::waitpid(pid, &status, 0);
}

// Scenario A: full session — gated commands, hello, live change, shutdown.
void testServedSession() {
    const std::string path = socketPath("serve");
    const pid_t pid = spawnWorker(path);
    if (!check(pid > 0, "worker spawned")) {
        return;
    }
    const int fd = connectRetry(path);
    if (!check(fd >= 0, "connected to the worker socket")) {
        killWorker(pid);
        return;
    }

    // Commands before the handshake are refused, not served.
    boost::json::value reply;
    check(call(fd, 1, "output.status", {}, &reply) &&
              isErrorCode(reply, proto::kNotHandshaked),
          "output.status before hello is refused with kNotHandshaked");

    // Handshake.
    if (check(call(fd, 2, "hello", proto::helloParams("ipc-test", 0, 1), &reply) &&
                  fieldEquals(reply.at("result"), "application", "openshow-worker"),
              "hello succeeds after connecting")) {
        const boost::json::value& result = reply.at("result");
        check(result.as_object().at("protocol").as_object().at("major").is_number(),
              "hello negotiates a protocol version");
        check(result.as_object().at("capabilities").as_array().size() == 5,
              "hello lists worker capabilities");
    }

    // Baseline status.
    if (check(call(fd, 3, "output.status", {}, &reply) &&
                  reply.at("result").is_object(),
              "output.status answers after hello")) {
        check(reply.at("result").as_object().at("layers").as_object().at("images")
                  .as_bool(),
              "images layer starts enabled");
    }

    // The live command: turn the images layer off and watch the render
    // thread apply it (status revision catches up with the ack).
    std::uint64_t ackedRevision = 0;
    if (check(call(fd, 4, "layer.setEnabled",
                   boost::json::object{{"layer", "images"}, {"enabled", false}}, &reply) &&
                  reply.as_object().contains("result"),
              "layer.setEnabled images=false is acknowledged")) {
        ackedRevision = reply.at("result").as_object().at("revision").is_uint64()
                            ? reply.at("result").as_object().at("revision").as_uint64()
                            : static_cast<std::uint64_t>(
                                  reply.at("result").as_object().at("revision").as_int64());
        bool applied = false;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            if (call(fd, 5, "output.status", {}, &reply)) {
                const boost::json::value& status = reply.at("result");
                const std::uint64_t revision =
                    status.as_object().at("revision").is_uint64()
                        ? status.as_object().at("revision").as_uint64()
                        : static_cast<std::uint64_t>(
                              status.as_object().at("revision").as_int64());
                if (!status.as_object().at("layers").as_object().at("images").as_bool() &&
                    revision >= ackedRevision) {
                    applied = true;
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        check(applied, "the command changed the composed output state on the render thread");
    }

    // Unknown layer names and methods answer with structured errors.
    check(call(fd, 6, "layer.setEnabled",
               boost::json::object{{"layer", "holograms"}, {"enabled", true}}, &reply) &&
              isErrorCode(reply, proto::kInvalidParams),
          "unknown layer name is a structured invalid-params error");
    check(call(fd, 7, "dance.for.me", {}, &reply) &&
              isErrorCode(reply, proto::kMethodNotFound),
          "unknown method is a structured method-not-found error");

    // Served shutdown: the worker exits cleanly by itself.
    if (check(call(fd, 8, "shutdown", {}, &reply) &&
                  reply.at("result").as_object().at("ok").as_bool(),
              "shutdown is acknowledged")) {
        close(fd);
        check(waitExit(pid, 5) == 0, "worker exits with status 0 after shutdown");
    } else {
        killWorker(pid);
    }
}

// Scenario B: no shutdown served — the frontend just goes away.
void testDisconnectStopsWorker() {
    const std::string path = socketPath("gone");
    const pid_t pid = spawnWorker(path);
    if (!check(pid > 0, "worker spawned (disconnect scenario)")) {
        return;
    }
    const int fd = connectRetry(path);
    if (!check(fd >= 0, "connected to the worker socket")) {
        killWorker(pid);
        return;
    }
    boost::json::value reply;
    check(call(fd, 1, "hello", proto::helloParams("ipc-test", 0, 1), &reply),
          "hello succeeded before disconnect");
    close(fd);  // the frontend dies mid-session
    check(waitExit(pid, 5) == 0, "worker exits with status 0 when its frontend disconnects");
}

// Scenario C (task 1.3): destination control over the wire. Enumerate the
// panels, refuse bad selections, move the output onto the first panel and
// back to windowed, waiting for the status to confirm each arrival.
void testDisplaySelection() {
    const std::string path = socketPath("displays");
    const pid_t pid = spawnWorker(path);
    if (!check(pid > 0, "worker spawned (display scenario)")) {
        return;
    }
    const int fd = connectRetry(path);
    if (!check(fd >= 0, "connected to the worker socket")) {
        killWorker(pid);
        return;
    }
    boost::json::value reply;
    if (!check(call(fd, 1, "hello", proto::helloParams("ipc-test", 0, 1), &reply),
               "hello succeeded (display scenario)")) {
        killWorker(pid);
        return;
    }

    if (check(call(fd, 2, "output.displays", {}, &reply) && reply.at("result").is_array(),
              "output.displays answers with an array")) {
        const boost::json::array& displays = reply.at("result").as_array();
        check(!displays.empty(), "at least one display is enumerated");
        if (!displays.empty()) {
            const boost::json::object& first = displays[0].as_object();
            check(numberValue(first.at("index")) == 0 && first.at("name").is_string() &&
                      numberValue(first.at("width")) > 0,
                  "display entries carry index, name and size");
        }

        check(call(fd, 3, "output.select", boost::json::object{{"monitor", 99}}, &reply) &&
                  isErrorCode(reply, proto::kInvalidParams),
              "a nonexistent monitor is refused");
        check(call(fd, 4, "output.select", {}, &reply) &&
                  isErrorCode(reply, proto::kInvalidParams),
              "output.select without a monitor is refused");

        if (!displays.empty() &&
            check(call(fd, 5, "output.select", boost::json::object{{"monitor", 0}}, &reply) &&
                      reply.as_object().contains("result"),
                  "output.select monitor 0 is acknowledged")) {
            check(waitForMonitor(fd, 0, numberValue(reply.at("result").as_object().at("revision")),
                                 6),
                  "status reports the output arrived on monitor 0");
        }
        if (check(call(fd, 6, "output.select", boost::json::object{{"monitor", -1}}, &reply) &&
                      reply.as_object().contains("result"),
                  "output.select windowed is acknowledged")) {
            check(waitForMonitor(fd, -1,
                                 numberValue(reply.at("result").as_object().at("revision")), 6),
                  "status reports the output returned to windowed");
        }
    }

    call(fd, 7, "shutdown", {}, &reply);
    close(fd);
    check(waitExit(pid, 5) == 0, "display-scenario worker exits cleanly");
}

// Scenario D (task 1.4): the shared-memory preview as the frontend sees it.
// Start a stream, wait for a first ready, map the promised segment, copy
// frames under the slot seqlock and release them, then hold all three slots
// to prove the producer drops instead of blocking the audience output,
// release to see the stream resume, stop (segment unlinked), and start a
// second stream to prove streams are replaceable.
void testPreviewStream() {
    const std::string path = socketPath("preview");
    const pid_t pid = spawnWorker(path);
    if (!check(pid > 0, "worker spawned (preview scenario)")) {
        return;
    }
    const int fd = connectRetry(path);
    if (!check(fd >= 0, "connected to the worker socket")) {
        killWorker(pid);
        return;
    }
    boost::json::value reply;
    if (!check(call(fd, 1, "hello", proto::helloParams("ipc-test", 0, 1), &reply),
               "hello succeeded (preview scenario)")) {
        killWorker(pid);
        return;
    }

    // Explicit geometry: this also proves numeric params are honoured —
    // Boost.JSON stores small integers as int64, so a uint64-only reader
    // silently ignores every one of them (seen 2026-10-23).
    const std::uint64_t width = 320;
    const std::uint64_t height = 240;
    std::string name;
    std::size_t bytesPerSlot = 0;
    const bool started = call(fd, 2, "preview.start",
                              boost::json::object{{"width", width}, {"height", height},
                                                  {"fps", 15}},
                              &reply) &&
                         reply.as_object().contains("result");
    if (!check(started, "preview.start with an explicit size is acknowledged")) {
        killWorker(pid);
        return;
    }
    {
        const boost::json::object& result = reply.at("result").as_object();
        name = std::string(result.at("name").as_string());
        bytesPerSlot = static_cast<std::size_t>(numberValue(result.at("bytesPerSlot")));
        check(!name.empty() && name.front() == '/', "preview.start names a shm segment");
        check(numberValue(result.at("slotCount")) == proto::kPreviewSlotCount,
              "the start result reports three slots");
        check(bytesPerSlot == proto::previewSlotStrideBytes(width, height),
              "bytesPerSlot matches the requested geometry");
        check(numberValue(result.at("width")) == width &&
                  numberValue(result.at("height")) == height,
              "the start result echoes the requested geometry");
        check(fieldEquals(result, "format", "RGBA8888"), "the wire format is RGBA8888");
    }

    // The segment exists once the render thread has acted; a first ready
    // proves it did, which is exactly when the frontend maps.
    boost::json::object ready;
    if (!check(nextPreviewReady(fd, &ready, 5), "a preview.ready notification arrives")) {
        ::close(fd);
        killWorker(pid);
        return;
    }

    const std::size_t totalBytes = proto::previewSegmentBytes(width, height);
    const int shmFd = ::shm_open(name.c_str(), O_RDONLY, 0);
    if (!check(shmFd >= 0, "the promised shm segment exists once frames flow")) {
        killWorker(pid);
        return;
    }
    struct stat info {};
    ::fstat(shmFd, &info);
    check(static_cast<std::size_t>(info.st_size) == totalBytes,
          "the segment is exactly the promised size");
    const auto* segment = static_cast<const proto::PreviewSegmentHeader*>(
        ::mmap(nullptr, totalBytes, PROT_READ, MAP_SHARED, shmFd, 0));
    if (!check(segment != MAP_FAILED, "the consumer maps the segment read-only")) {
        ::close(shmFd);
        killWorker(pid);
        return;
    }
    check(segment->magic == proto::kPreviewMagic &&
              segment->layoutVersion == proto::kPreviewLayoutVersion &&
              segment->slotCount == proto::kPreviewSlotCount &&
              segment->bytesPerSlot == bytesPerSlot && segment->width == width &&
              segment->height == height &&
              segment->format == proto::kPreviewFourccRGBA8,
          "the segment header matches the start result");
    // That first ready was the "the segment exists" signal and its frame
    // was not copied, so release the slot it came on: an unreleased slot
    // stays owned forever by design (the smoke script learned this first).
    sendNotification(fd, proto::kMethodPreviewReleased,
                     boost::json::object{{"slot", numberValue(ready.at("slot"))},
                                         {"seq", numberValue(ready.at("seq"))}});

    // Copy two frames that survive the seqlock check; the first one is kept
    // for a content comparison after a layer toggle. Publishing is
    // content-driven since task 1.5e — a static slide yields exactly one
    // frame — so every copy is preceded by a layer toggle, which doubles
    // as proof that a command wakes the event-driven loop.
    std::uint64_t rpcId = 3;
    const auto toggleLayer = [&](const char* layerName, bool on) {
        return call(fd, static_cast<std::int64_t>(rpcId++), "layer.setEnabled",
                    boost::json::object{{"layer", layerName}, {"enabled", on}}, &reply) &&
               reply.as_object().contains("result");
    };
    std::vector<std::uint8_t> frameWithImages;
    const bool wokeOnce = toggleLayer("clock", false);
    const bool gotFirst =
        wokeOnce &&
        copyOneFrame(fd, reinterpret_cast<const std::uint8_t*>(segment), bytesPerSlot,
                     width, height, &frameWithImages);
    check(gotFirst, "a frame copied intact after a command woke the loop");
    std::vector<std::uint8_t> frameSecond;
    check(toggleLayer("clock", true) &&
              copyOneFrame(fd, reinterpret_cast<const std::uint8_t*>(segment), bytesPerSlot,
                           width, height, &frameSecond),
          "a second frame copied intact through the slot seqlock");
    bool frameHadContent = false;
    for (std::size_t i = 3; gotFirst && i < frameWithImages.size(); i += 4) {
        if (frameWithImages[i] == 0xFF &&
            (frameWithImages[i - 3] | frameWithImages[i - 2] | frameWithImages[i - 1]) !=
                0) {
            frameHadContent = true;
            break;
        }
    }
    check(frameHadContent, "the copied frame is not a blank buffer");

    // The preview is not a snapshot: toggling the images layer must change
    // what the slots carry. Many pixels have to differ because the photo
    // covers a fifth of the canvas, while the ticking clock alone repaints
    // a thin text strip.
    if (gotFirst) {
        const bool imagesOffAcked = toggleLayer("images", false);
        if (check(imagesOffAcked,
                  "turning the images layer off is acknowledged (preview scenario)")) {
            const std::uint64_t ackedRevision =
                numberValue(reply.at("result").as_object().at("revision"));
            bool applied = false;
            const auto applyDeadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!applied && std::chrono::steady_clock::now() < applyDeadline) {
                boost::json::value statusReply;
                if (call(fd, static_cast<std::int64_t>(rpcId++), "output.status", {},
                         &statusReply)) {
                    const boost::json::object& status = statusReply.at("result").as_object();
                    applied = !status.at("layers").as_object().at("images").as_bool() &&
                              numberValue(status.at("revision")) >= ackedRevision;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            check(applied, "the render thread applied the images-layer change");
            std::vector<std::uint8_t> frameWithoutImages;
            if (check(copyOneFrame(fd, reinterpret_cast<const std::uint8_t*>(segment),
                                   bytesPerSlot, width, height, &frameWithoutImages),
                      "a frame copied after the layer toggle")) {
                std::size_t differing = 0;
                for (std::size_t i = 0; i + 2 < frameWithoutImages.size(); i += 4) {
                    if (frameWithImages[i] != frameWithoutImages[i] ||
                        frameWithImages[i + 1] != frameWithoutImages[i + 1] ||
                        frameWithImages[i + 2] != frameWithoutImages[i + 2]) {
                        ++differing;
                    }
                }
                check(differing * 100 > frameWithoutImages.size() / 4 * 5,
                      "preview content followed the layer toggle");
            }
        }
    }

    // Stalled consumer: hold three slots and never release them.
    struct Held {
        std::uint64_t slot;
        std::uint64_t seq;
    };
    std::vector<Held> held;
    std::uint64_t seenSlots = 0;
    const auto fillDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (held.size() < proto::kPreviewSlotCount &&
           std::chrono::steady_clock::now() < fillDeadline) {
        // Content-driven publishing: each slot needs a fresh change to
        // arrive. Quiet spells are the new normal, so a missed ready is a
        // reason to toggle again, not to give up.
        toggleLayer("clock", (held.size() & 1u) == 1u);
        if (!nextPreviewReady(fd, &ready, 2)) {
            continue;
        }
        const std::uint64_t slot = numberValue(ready.at("slot"));
        if (slot >= proto::kPreviewSlotCount) {
            continue;
        }
        held.push_back({slot, numberValue(ready.at("seq"))});
        seenSlots |= 1u << slot;
    }
    check(held.size() == proto::kPreviewSlotCount && seenSlots == 0b111,
          "all three slots take turns carrying frames");

    std::uint64_t frames0 = 0;
    std::uint64_t published0 = 0;
    std::uint64_t dropped0 = 0;
    std::uint64_t frames1 = 0;
    std::uint64_t published1 = 0;
    std::uint64_t dropped1 = 0;
    const bool counts0 = readPreviewCounts(fd, static_cast<std::int64_t>(rpcId++), &frames0,
                                           &published0, &dropped0);
    // Keep changing content while every slot is held: the audience output
    // must keep presenting and the preview must count drops instead of
    // blocking or overwriting an owned slot.
    const auto stallEnd = std::chrono::steady_clock::now() + std::chrono::milliseconds(1200);
    for (int i = 0; std::chrono::steady_clock::now() < stallEnd; ++i) {
        toggleLayer("clock", (i & 1) == 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    const bool counts1 = readPreviewCounts(fd, static_cast<std::int64_t>(rpcId++), &frames1,
                                           &published1, &dropped1);
    if (check(counts0 && counts1, "status answers while every slot is held")) {
        check(frames1 > frames0,
              "the audience output keeps presenting while the preview consumer stalls");
        check(dropped1 > dropped0, "the stalled preview ticks count as drops");
        check(published1 == published0,
              "no slot is overwritten while its last frame is still owned");
    }

    // Release: the same slots carry frames again immediately.
    for (const Held& frame : held) {
        sendNotification(fd, proto::kMethodPreviewReleased,
                         boost::json::object{{"slot", frame.slot}, {"seq", frame.seq}});
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    std::uint64_t frames2 = 0;
    std::uint64_t published2 = 0;
    std::uint64_t dropped2 = 0;
    if (check(readPreviewCounts(fd, static_cast<std::int64_t>(rpcId++), &frames2, &published2,
                                &dropped2),
              "status answers again after the releases")) {
        check(published2 > published1, "publishing resumes as soon as slots are released");
    }

    // Stop: the stream retires and unlinks its segment.
    if (check(call(fd, 6, "preview.stop", {}, &reply) &&
                  reply.at("result").as_object().at("ok").as_bool(),
              "preview.stop is acknowledged")) {
        bool stopped = false;
        const auto stopDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!stopped && std::chrono::steady_clock::now() < stopDeadline) {
            boost::json::value statusReply;
            if (call(fd, 7, "output.status", {}, &statusReply)) {
                stopped = !statusReply.at("result").as_object().at("preview").as_object()
                               .at("running")
                               .as_bool();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        check(stopped, "status reports the preview stream stopped");
    }
    ::munmap(const_cast<proto::PreviewSegmentHeader*>(segment), totalBytes);
    ::close(shmFd);
    const int relink = ::shm_open(name.c_str(), O_RDONLY, 0);
    check(relink < 0, "the stopped stream unlinked its segment");
    if (relink >= 0) {
        ::close(relink);
    }

    // A second stream under a fresh name proves the seam is repeatable.
    // Anything still queued belongs to the dead segment: throw it away.
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    drainReadyNotifications(fd);
    if (check(call(fd, 8, "preview.start", {}, &reply) &&
                  reply.as_object().contains("result"),
              "preview.start again is acknowledged")) {
        const std::string secondName =
            std::string(reply.at("result").as_object().at("name").as_string());
        check(secondName != name, "a replaced stream gets its own segment name");
        // Defaults: 640x360 as documented.
        check(numberValue(reply.at("result").as_object().at("width")) == 640,
              "preview.start defaults to the documented width");
        check(nextPreviewReady(fd, &ready, 5), "the second stream produces frames too");
    }

    call(fd, 9, "shutdown", {}, &reply);
    close(fd);
    check(waitExit(pid, 5) == 0, "preview-scenario worker exits cleanly");
}

}  // namespace

// Scenario E (task 2.4b): the compiled-scene store over the wire. Prepare
// two scenes (the frontend's compiler is the producer), refuse a malformed
// one, present each and watch the applied revision settle, and refuse a
// present of an unknown revision.
void testSceneStore() {
    const std::string path = socketPath("scenes");
    const pid_t pid = spawnWorker(path);
    if (!check(pid > 0, "worker spawned (scene scenario)")) {
        return;
    }
    const int fd = connectRetry(path);
    if (!check(fd >= 0, "connected to the worker socket")) {
        killWorker(pid);
        return;
    }
    boost::json::value reply;
    if (!check(call(fd, 1, "hello", proto::helloParams("ipc-test", 0, 2), &reply),
               "hello succeeded (scene scenario)")) {
        killWorker(pid);
        return;
    }

    // A compiled scene as the frontend's scene::toJson would emit it.
    const auto compiledScene = [](const char* line) {
        return boost::json::object{
            {"scene", 1},
            {"showId", "show1"},
            {"slideId", "slide2"},
            {"backgroundColor", 0x202040},
            {"background",
             boost::json::object{{"image", false}, {"assetId", ""}, {"assetPath", ""}}},
            {"items",
             boost::json::array{boost::json::object{{"lines", boost::json::array{line}},
                                                    {"color", 0xffffff},
                                                    {"fontFamily", "DejaVu Serif"},
                                                    {"fontSize", 64.0},
                                                    {"x", 160.0},
                                                    {"y", 100.0},
                                                    {"width", 1600.0},
                                                    {"height", 880.0},
                                                    {"align", "center"},
                                                    {"lineSpacing", 1.25}}}}};
    };

    // Two malformed payloads are refused with the wire validator's reasons.
    {
        boost::json::object broken = compiledScene("x");
        broken["backgroundColor"] = 0x1000000;  // beyond 24 bits
        check(call(fd, 2, "scene.prepare", broken, &reply) &&
                  isErrorCode(reply, proto::kInvalidParams),
              "a scene with an out-of-range colour is refused by scene.prepare");
    }
    {
        boost::json::object broken = compiledScene("x");
        broken["scene"] = 2;  // unsupported scene format
        check(call(fd, 3, "scene.prepare", broken, &reply) &&
                  isErrorCode(reply, proto::kInvalidParams),
              "a future scene format is refused by scene.prepare");
    }

    // Presenting before anything is prepared is refused.
    check(call(fd, 4, "scene.present", boost::json::object{{"revision", 1}}, &reply) &&
              isErrorCode(reply, proto::kInvalidParams),
          "presenting an unprepared revision is refused");

    // The positive payloads are the ones the frontend really sends: demo
    // fixture slides put through scene::compileDemoSlide + toJson — byte
    // for byte what WorkerController::presentDemoSlide puts on the socket.
    const openshow::domain::Documents demo = openshow::scene::demoDocuments();
    const auto fixturePayload = [&demo](std::size_t index) {
        openshow::scene::Scene scene;
        std::string error;
        if (!openshow::scene::compileDemoSlide(demo, index, &scene, &error)) {
            check(false, "the demo fixture slide compiles for the wire");
            return boost::json::object{};
        }
        boost::json::value json = openshow::scene::toJson(scene);
        return json.as_object();
    };

    // Prepare two valid scenes; each reports a distinct store revision.
    std::uint64_t first = 0;
    if (!check(call(fd, 5, "scene.prepare", fixturePayload(0), &reply) &&
                   reply.as_object().contains("result"),
               "the frontend's compiled title slide is accepted by scene.prepare")) {
        killWorker(pid);
        close(fd);
        waitExit(pid, 5);
        return;
    }
    first = numberValue(reply.at("result").as_object().at("revision"));
    check(call(fd, 6, "scene.prepare", fixturePayload(1), &reply) &&
              reply.as_object().contains("result"),
              "the frontend's compiled stanza slide is accepted too");
    const std::uint64_t second = numberValue(reply.at("result").as_object().at("revision"));
    check(first != second && first != 0 && second != 0,
          "each prepared scene gets its own non-zero revision");

    // Status reflects the store size.
    if (check(call(fd, 7, "output.status", {}, &reply) && reply.at("result").is_object(),
              "output.status answers during the scene scenario")) {
        std::uint64_t prepared = 0;
        check(sceneField(reply.at("result"), "prepared", &prepared) && prepared >= 2,
              "status counts the prepared scenes");
    }

    // Present the second scene, then the first; each applied revision
    // must settle to the value the command named.
    if (check(call(fd, 8, "scene.present", boost::json::object{{"revision", second}}, &reply) &&
                  reply.as_object().contains("result"),
              "presenting the second scene is acknowledged")) {
        check(waitForSceneActive(fd, second,
                                 numberValue(reply.at("result").as_object().at("revision")), 6),
              "status reports the second scene applied");
    }
    if (check(call(fd, 9, "scene.present", boost::json::object{{"revision", first}}, &reply) &&
                  reply.as_object().contains("result"),
              "presenting the first scene is acknowledged")) {
        check(waitForSceneActive(fd, first,
                                 numberValue(reply.at("result").as_object().at("revision")), 6),
              "status reports the first scene applied");
    }

    check(call(fd, 10, "scene.present", boost::json::object{{"revision", 99999}}, &reply) &&
              isErrorCode(reply, proto::kInvalidParams),
          "presenting a never-prepared revision is refused");

    // Task 3.2: scene.present's optional "transition". Cut and fade are
    // accepted and land like any present; anything else is refused before
    // the session is touched, and the scene on air does not move.
    if (check(call(fd, 11, "scene.present",
                   boost::json::object{{"revision", second}, {"transition", "cut"}}, &reply) &&
                  reply.as_object().contains("result"),
              "presenting with transition=cut is acknowledged")) {
        check(waitForSceneActive(fd, second,
                                 numberValue(reply.at("result").as_object().at("revision")), 6),
              "status reports the cut scene applied");
    }
    if (check(call(fd, 12, "scene.present",
                   boost::json::object{{"revision", first}, {"transition", "fade"}}, &reply) &&
                  reply.as_object().contains("result"),
              "presenting with transition=fade is acknowledged")) {
        check(waitForSceneActive(fd, first,
                                 numberValue(reply.at("result").as_object().at("revision")), 6),
              "status reports the faded scene applied");
    }
    check(call(fd, 13, "scene.present",
               boost::json::object{{"revision", second}, {"transition", "spin"}}, &reply) &&
              isErrorCode(reply, proto::kInvalidParams),
          "an unknown transition name is refused");
    check(call(fd, 14, "scene.present",
               boost::json::object{{"revision", second}, {"transition", true}}, &reply) &&
              isErrorCode(reply, proto::kInvalidParams),
          "a non-string transition is refused");

    call(fd, 15, "shutdown", {}, &reply);
    close(fd);
    check(waitExit(pid, 5) == 0, "scene-scenario worker exits cleanly");
}

int main(int argc, char** argv) {
    if (argc > 1) {
        gWorkerBinary = argv[1];
    }
    testServedSession();
    testDisconnectStopsWorker();
    testDisplaySelection();
    testPreviewStream();
    testSceneStore();

    if (gFailures != 0) {
        std::fprintf(stderr, "%d IPC check(s) failed\n", gFailures);
        return 1;
    }
    std::printf("ok: worker RPC session, live command, destination control, "
                "preview stream and both exits\n");
    return 0;
}
