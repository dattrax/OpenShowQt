/*
 * Production compositor over the real socket session (task 6.1a, review
 * R1: "production output still uses the development scene").
 *
 * A --socket worker is what the audience sees, so this test reads the
 * live preview stream of a fresh worker and asserts the production
 * composition rules at the real output/preview boundary:
 *
 *   1. A fresh worker starts uniformly black — every pixel, the
 *      footer/clock band included. Before 6.1a this frame carried the
 *      1.x demo picture and the diagnostics footer.
 *   2. output.status reports the production layer defaults: the scene
 *      tabs on, aux and the clock/diagnostics footer off.
 *   3. A presented scene composes into the stream — colour and words —
 *      and the footer band stays pure scene colour: no device/frame
 *      diagnostics, no clock. Output and preview share the one
 *      production compositor (fScene draws both targets with the same
 *      applied mask and the same empty status line), so the scene the
 *      preview carries IS the scene the audience panel carries.
 *   4. Clearing everything — a scene with no words and a black
 *      background — lands uniformly black again, footer band included.
 *
 * Needs a display (the worker opens its output window): desktop preset,
 * label display. Modelled on live_fade_tests; Qt-free.
 */

#include <boost/json.hpp>

#include <algorithm>
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
    "/tmp/openshow-production-" + std::to_string(getpid()) + ".sock";

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
    int framesSeen = 0;
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
    // Consumer-side gate (task 6.5, review R5): a ready naming a stream
    // other than the one this client started describes a segment it does
    // not have — copy nothing and release nothing under it.
    const auto stream = params.find("stream");
    if (stream == params.end() || !stream->value().is_string() ||
        std::string_view(stream->value().as_string()) != gPreview.name) {
        return;
    }
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
                                       {"seq", static_cast<std::int64_t>(seq)},
                                       {"stream", gPreview.name}}));
}

proto::Decoder gDecoder;

// Drain pending socket traffic, capturing any preview.ready frames.
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

bool layerState(int fd, std::int64_t& nextId, const char* layer, bool want,
                const char* what) {
    boost::json::value reply;
    if (!call(fd, nextId++, "output.status", {}, &reply) ||
        !reply.as_object().contains("result")) {
        check(false, what);
        return false;
    }
    const auto* entry = reply.at("result").as_object().find("layers");
    const bool ok = entry != reply.at("result").as_object().end() &&
                    entry->value().is_object() &&
                    entry->value().as_object().contains(layer) &&
                    entry->value().as_object().at(layer).as_bool() == want;
    check(ok, what);
    return ok;
}

// --- pixel helpers over the newest captured preview frame ---

bool pixelRgb(std::uint32_t x, std::uint32_t y, int* rgb) {
    if (gPreview.latest.empty() || x >= gPreview.width || y >= gPreview.height) {
        return false;
    }
    const std::size_t i = (static_cast<std::size_t>(y) * gPreview.width + x) * 4;
    rgb[0] = gPreview.latest[i];
    rgb[1] = gPreview.latest[i + 1];
    rgb[2] = gPreview.latest[i + 2];
    return true;
}

// The footer band (authored y 985..1040, the diagnostics/clock strip the
// review found on the audience output) scaled to the preview geometry.
struct Band {
    std::uint32_t y0, y1, x0, x1;
};

Band footerBand() {
    const double scale = static_cast<double>(gPreview.width) / 1920.0;
    return Band{static_cast<std::uint32_t>(985 * scale),
                static_cast<std::uint32_t>(1040 * scale),
                static_cast<std::uint32_t>(130 * scale),
                static_cast<std::uint32_t>(1500 * scale)};
}

bool frameAllBlack() {
    if (gPreview.latest.empty()) {
        return false;
    }
    for (std::size_t pixel = 0; pixel < gPreview.latest.size() / 4; ++pixel) {
        const std::size_t i = pixel * 4;
        if (gPreview.latest[i] != 0 || gPreview.latest[i + 1] != 0 ||
            gPreview.latest[i + 2] != 0 || gPreview.latest[i + 3] != 255) {
            return false;
        }
    }
    return true;
}

int blackPixelsInBand(const Band& band) {
    int count = 0;
    for (std::uint32_t y = band.y0; y < band.y1; ++y) {
        for (std::uint32_t x = band.x0; x < band.x1; ++x) {
            int rgb[3];
            if (pixelRgb(x, y, rgb) && rgb[0] == 0 && rgb[1] == 0 && rgb[2] == 0) {
                ++count;
            }
        }
    }
    return count;
}

int sceneRedPixelsInBand(const Band& band) {
    int count = 0;
    for (std::uint32_t y = band.y0; y < band.y1; ++y) {
        for (std::uint32_t x = band.x0; x < band.x1; ++x) {
            int rgb[3];
            if (pixelRgb(x, y, rgb) && rgb[0] >= 250 && rgb[1] <= 6 && rgb[2] <= 6) {
                ++count;
            }
        }
    }
    return count;
}

// --- scene payloads ---

boost::json::object scenePayload(const char* slideId, std::uint32_t color,
                                 bool withWords) {
    boost::json::array items;
    if (withWords) {
        items.push_back(boost::json::object{{"lines", boost::json::array{"PROD"}},
                                            {"color", 0xFFFFFFu},
                                            {"fontFamily", ""},
                                            {"fontSize", 120.0},
                                            {"x", 160.0},
                                            {"y", 100.0},
                                            {"width", 1600.0},
                                            {"height", 880.0},
                                            {"align", "center"},
                                            {"lineSpacing", 1.25}});
    }
    return boost::json::object{
        {"scene", 1},
        {"showId", "production-output"},
        {"slideId", slideId},
        {"backgroundColor", color},
        {"background",
         boost::json::object{{"image", false}, {"assetId", ""}, {"assetPath", ""}}},
        {"items", std::move(items)},
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
    check(call(fd, nextId++, "hello", proto::helloParams("production-test", 0, 1), &reply) &&
              reply.as_object().contains("result"),
          "handshake completed");

    // 2. Production layer defaults, straight off the real RPC surface.
    layerState(fd, nextId, "background", true, "background layer starts enabled");
    layerState(fd, nextId, "words", true, "words layer starts enabled");
    layerState(fd, nextId, "aux", false, "aux layer starts disabled");
    layerState(fd, nextId, "clock", false, "clock/diagnostics layer starts disabled");

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

    // 1. The fresh worker's first frames are uniformly black, the
    //    footer/clock band included: no demo picture, no diagnostics.
    const auto firstDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (gPreview.latest.empty() && std::chrono::steady_clock::now() < firstDeadline) {
        pump(fd, 200);
    }
    if (check(!gPreview.latest.empty(), "the preview stream delivered the start-up frame")) {
        check(frameAllBlack(), "the start-up frame is uniformly black incl. the footer");
        const Band band = footerBand();
        const int bandPixels = static_cast<int>((band.y1 - band.y0) * (band.x1 - band.x0));
        check(blackPixelsInBand(band) == bandPixels,
              "the footer band of the start-up frame is black pixel-for-pixel");
    }

    // 3. A presented scene composes into the same stream — colour and
    //    words — and nothing else does.
    const std::uint64_t red =
        presentAndWait(fd, nextId, scenePayload("A", 0xFF0000u, true));
    check(red != 0, "the red scene is presented");
    pump(fd, 1400);  // past the 800 ms media + 500 ms text fade, settled
    {
        int rgb[3];
        bool ink = false;
        for (std::uint32_t y = 0; y < gPreview.height && !ink; y += 2) {
            for (std::uint32_t x = 0; x < gPreview.width && !ink; x += 2) {
                if (pixelRgb(x, y, rgb) && rgb[0] > 220 && rgb[1] > 220 && rgb[2] > 220) {
                    ink = true;  // white words on red
                }
            }
        }
        check(pixelRgb(gPreview.width / 2, 8, rgb) && rgb[0] >= 250 && rgb[1] <= 6 &&
                  rgb[2] <= 6,
              "the settled frame is the scene's red");
        check(ink, "the settled frame carries the scene's words");
        const Band band = footerBand();
        const int bandPixels = static_cast<int>((band.y1 - band.y0) * (band.x1 - band.x0));
        check(sceneRedPixelsInBand(band) == bandPixels,
              "the footer band shows only scene colour: no diagnostics, no clock");
    }

    // 4. Clear-all: a scene with no words and a black background lands
    //    uniformly black again — footer band included.
    const std::uint64_t cleared =
        presentAndWait(fd, nextId, scenePayload("CLEAR", 0x000000u, false));
    check(cleared != 0 && cleared != red, "the cleared scene is presented");
    pump(fd, 1400);
    check(frameAllBlack(), "clear-all is uniformly black incl. the footer");
    check(gPreview.delivered >= 3, "the preview stream carried startup, scene and clear");

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
    std::printf("production_output_tests: socket output starts black and stays production\n");
    return 0;
}
