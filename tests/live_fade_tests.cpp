/*
 * Live fade regression (task 3.2 promise, found by the M3 checkpoint demo).
 *
 * The worker's frame loop is event-driven (task 1.5e): it sleeps unless a
 * change is owed. A crossfade is exactly such a change — but until the M3
 * demo the loop slept the moment the fade began: one frame at blend 0
 * (still the old picture on screen), status cheerfully reported the new
 * revision on air, and the audience never saw it. The pixel fixtures
 * missed this because they drive draw() directly with a clock instead of
 * the live loop.
 *
 * So this test asserts the fade against live output: the presented frame
 * counter must keep climbing across the fade and then plateau (it animates
 * and it settles; no busy loop), and the preview stream must carry the
 * change to its end — the settled frame is the new scene, red to green,
 * not the old one the fade started from.
 *
 * Needs a display (the worker opens its output window): desktop preset,
 * label display.
 */

#include <boost/json.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
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
const std::string gSocket =
    "/tmp/openshow-fade-" + std::to_string(getpid()) + ".sock";

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

struct Preview {
    std::string name;
    std::uint64_t slotCount = 0;
    std::uint64_t slotStride = 0;
    std::uint8_t* map = nullptr;
    int fd = -1;
    std::vector<std::uint8_t> latest;  // newest valid RGBA frame
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    int delivered = 0;
} gPreview;

void sendRaw(int fd, const boost::json::value& message) {
    std::string wire;
    proto::appendFrame(message, &wire);
    ::send(fd, wire.data(), wire.size(), MSG_NOSIGNAL);
}

void handlePreviewReady(int fd, const boost::json::object& params) {
    if (gPreview.map == nullptr && !gPreview.name.empty()) {
        for (int attempt = 0; attempt < 40 && gPreview.map == nullptr; ++attempt) {
            const int shm = ::shm_open(gPreview.name.c_str(), O_RDONLY, 0);
            if (shm < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            const std::size_t total = gPreview.slotStride * gPreview.slotCount + 64;
            void* map = ::mmap(nullptr, total, PROT_READ, MAP_SHARED, shm, 0);
            if (map == MAP_FAILED) {
                ::close(shm);
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            const auto* header = static_cast<const proto::PreviewSegmentHeader*>(map);
            if (header->magic != proto::kPreviewMagic ||
                header->bytesPerSlot != gPreview.slotStride) {
                ::munmap(map, total);
                ::close(shm);
                return;
            }
            gPreview.map = static_cast<std::uint8_t*>(map);
            gPreview.fd = shm;
        }
    }
    auto number = [&params](const char* key) -> std::uint64_t {
        const auto entry = params.find(key);
        return entry == params.end() ? 0 : entry->value().to_number<std::uint64_t>();
    };
    const std::uint64_t slot = number("slot");
    const std::uint64_t seq = number("seq");
    const std::uint64_t width = number("width");
    const std::uint64_t height = number("height");
    const std::uint64_t rowBytes = number("rowBytes");
    if (gPreview.map != nullptr && slot < gPreview.slotCount && rowBytes == width * 4 &&
        width > 0 && height > 0) {
        const std::uint8_t* base =
            gPreview.map + proto::previewSlotOffset(static_cast<std::uint32_t>(slot),
                                                    gPreview.slotStride);
        const auto* header = reinterpret_cast<const proto::PreviewSlotHeader*>(base);
        const std::uint64_t before =
            std::atomic_ref<const std::uint64_t>(header->seq).load();
        if (before == seq && (before & 1) == 0) {
            std::vector<std::uint8_t> copy(base + 64, base + 64 + rowBytes * height);
            const std::uint64_t after =
                std::atomic_ref<const std::uint64_t>(header->seq).load();
            if (after == before) {
                gPreview.latest = std::move(copy);
                gPreview.width = static_cast<std::uint32_t>(width);
                gPreview.height = static_cast<std::uint32_t>(height);
                ++gPreview.delivered;
            }
        }
    }
    sendRaw(fd, proto::makeNotification(
                   proto::kMethodPreviewReleased,
                   boost::json::object{{"slot", static_cast<std::int64_t>(slot)},
                                       {"seq", static_cast<std::int64_t>(seq)}}));
}

proto::Decoder gDecoder;

// One request/reply, consuming any preview traffic that interleaves.
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
                    handlePreviewReady(fd, message.at("params").as_object());
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

std::uint64_t numberValue(const boost::json::value& value) {
    return value.is_uint64() ? value.as_uint64()
                             : static_cast<std::uint64_t>(value.as_int64());
}

std::uint64_t outputFrames(int fd, std::int64_t& nextId) {
    boost::json::value reply;
    if (!call(fd, nextId++, "output.status", {}, &reply) ||
        !reply.as_object().contains("result")) {
        return 0;
    }
    return numberValue(reply.at("result").as_object().at("frames"));
}

// Keep the socket (and the preview stream) moving for a while.
void pump(int fd, int milliseconds) {
    const auto until = std::chrono::steady_clock::now() +
                       std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < until) {
        pollfd pending{fd, POLLIN, 0};
        if (::poll(&pending, 1, 100) > 0) {
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
            if (message.is_object() && message.as_object().contains("method") &&
                message.at("method").is_string() &&
                std::string_view(message.at("method").as_string()) ==
                    proto::kMethodPreviewReady &&
                message.as_object().contains("params") &&
                message.at("params").is_object()) {
                handlePreviewReady(fd, message.at("params").as_object());
            }
        }
    }
}

// Mean red and green channels of the newest preview frame.
std::pair<double, double> meanRedGreen() {
    double red = 0.0;
    double green = 0.0;
    const std::size_t pixels =
        gPreview.latest.size() / 4;
    for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
        red += gPreview.latest[pixel * 4];
        green += gPreview.latest[pixel * 4 + 1];
    }
    return {red / static_cast<double>(pixels ? pixels : 1),
            green / static_cast<double>(pixels ? pixels : 1)};
}

// A solid-colour scene with one word — the two scenes differ only in hue
// and word, so mean channel brightness is a robust content claim.
boost::json::object solidScene(const char* slideId, std::uint32_t color,
                               const char* word) {
    return boost::json::object{
        {"scene", 1},
        {"showId", "live-fade"},
        {"slideId", slideId},
        {"backgroundColor", color},
        {"background",
         boost::json::object{{"image", false}, {"assetId", ""}, {"assetPath", ""}}},
        {"items",
         boost::json::array{boost::json::object{{"lines", boost::json::array{word}},
                                                {"color", 0xFFFFFFu},
                                                {"fontFamily", ""},
                                                {"fontSize", 120.0}}}},
    };
}

std::uint64_t presentAndWait(int fd, std::int64_t& nextId, const boost::json::object& scene) {
    boost::json::value reply;
    if (!call(fd, nextId++, "scene.prepare", scene, &reply) ||
        !reply.as_object().contains("result")) {
        return 0;
    }
    const std::uint64_t revision =
        numberValue(reply.at("result").as_object().at("revision"));
    if (!call(fd, nextId++, "scene.present",
              boost::json::object{{"revision", revision}}, &reply) ||
        !reply.as_object().contains("result")) {
        return 0;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (std::chrono::steady_clock::now() < deadline) {
        boost::json::value statusReply;
        if (call(fd, nextId++, "output.status", {}, &statusReply) &&
            statusReply.at("result").as_object().contains("scene") &&
            numberValue(statusReply.at("result").as_object()
                            .at("scene")
                            .as_object()
                            .at("active")) == revision) {
            return revision;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return 0;
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
    std::int64_t nextId = 1;
    boost::json::value reply;
    check(call(fd, nextId++, "hello", proto::helloParams("fade-test", 0, 1), &reply) &&
              reply.as_object().contains("result"),
          "handshake completed");
    if (check(call(fd, nextId++, "preview.start",
                   boost::json::object{{"width", 640}, {"height", 360}, {"fps", 15}},
                   &reply) &&
                  reply.as_object().contains("result"),
              "preview.start succeeded")) {
        const boost::json::object& result = reply.at("result").as_object();
        gPreview.name = std::string(result.at("name").as_string());
        gPreview.slotCount = result.at("slotCount").to_number<std::uint64_t>();
        gPreview.slotStride = result.at("bytesPerSlot").to_number<std::uint64_t>();
    }

    // Red scene on air and settled; the preview says red.
    const std::uint64_t red = presentAndWait(fd, nextId, solidScene("A", 0xFF0000u, "ALPHA"));
    check(red != 0, "the red scene is presented");
    pump(fd, 1000);
    auto [redMean, redGreenMean] = meanRedGreen();
    check(redMean > 150.0 && redGreenMean < 100.0,
          "the settled preview is red (the first scene on screen)");

    // Fade (the default transition) to green. During the media fade the
    // event-driven loop must keep presenting frames...
    const std::uint64_t baseline = outputFrames(fd, nextId);
    const std::uint64_t green = presentAndWait(fd, nextId, solidScene("B", 0x00FF00u, "BETA"));
    check(green != 0 && green != red, "the green scene is presented");
    std::uint64_t peakGain = 0;
    const auto fadeWindow = std::chrono::steady_clock::now() + std::chrono::milliseconds(700);
    while (std::chrono::steady_clock::now() < fadeWindow) {
        pump(fd, 100);
        peakGain = std::max(peakGain, outputFrames(fd, nextId) - baseline);
    }
    check(peakGain >= 4,
          "the fade animates: the live loop presents frame after frame across it");

    // ...and settle: the counter plateaus (no busy loop once the blends are
    // done) and the settled output is the NEW scene, not the old one.
    pump(fd, 900);
    const std::uint64_t afterA = outputFrames(fd, nextId);
    pump(fd, 400);
    const std::uint64_t afterB = outputFrames(fd, nextId);
    check(afterA == afterB, "the loop goes idle again once the fade is complete");
    auto [greenMean, greenGreenMean] = meanRedGreen();
    check(greenGreenMean > 150.0 && greenMean < 100.0,
          "the settled output is the new (green) scene — the fade actually lands");
    check(gPreview.delivered >= 3, "the preview stream carried the change through");

    call(fd, nextId++, "shutdown", {}, &reply);
    int status = 0;
    waitpid(worker, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "the worker exited cleanly");

    close(fd);
    ::unlink(gSocket.c_str());
    if (gPreview.map != nullptr) {
        ::munmap(gPreview.map, gPreview.slotStride * gPreview.slotCount + 64);
        ::close(gPreview.fd);
    }
    if (gFailures > 0) {
        return 1;
    }
    std::printf("live_fade_tests: the live loop animates and lands its fades\n");
    return 0;
}
