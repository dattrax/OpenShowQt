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
#include <cmath>
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

// For claims that carry measured numbers.
bool check(bool condition, const std::string& what) {
    return check(condition, what.c_str());
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

// Mean of one delivered frame's RGB channels.
struct Sample {
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
};

// Task 6.11b (review R13) needs the RAMP, not the newest frame: a fade is
// wrong in the middle and right at both ends, so the mid stages have to be
// caught while they fly past. Set around the present that starts a fade, and
// every frame delivered in that window is kept.
bool gRecording = false;
std::vector<Sample> gSamples;

Sample meanOf(const std::vector<std::uint8_t>& rgba) {
    Sample sample;
    const std::size_t pixels = rgba.size() / 4;
    if (pixels == 0) {
        return sample;
    }
    for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
        sample.red += rgba[pixel * 4];
        sample.green += rgba[pixel * 4 + 1];
        sample.blue += rgba[pixel * 4 + 2];
    }
    sample.red /= static_cast<double>(pixels);
    sample.green /= static_cast<double>(pixels);
    sample.blue /= static_cast<double>(pixels);
    return sample;
}

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
                if (gRecording) {
                    gSamples.push_back(meanOf(gPreview.latest));
                }
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
    const Sample sample = meanOf(gPreview.latest);
    return {sample.red, sample.green};
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
                                                {"fontSize", 120.0},
                                                {"x", 160.0},
                                                {"y", 100.0},
                                                {"width", 1600.0},
                                                {"height", 880.0},
                                                {"align", "center"},
                                                {"lineSpacing", 1.25}}}},
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

/* --- task 6.11b (review R13): a picture in the fade, on the GPU ----------- */

// R13 lived in the middle of a live fade over a picture: the shipped
// background transition attenuated the incoming scene's colour FILL and its
// picture separately, so the fill showed through an opaque still while the
// dissolve ran. Both endpoints of that fade are correct and a solid-colour
// scene has no picture to leak through, which is precisely why the existing
// scenarios missed it. This one watches the frames the GPU delivered.
bool shell(const std::string& command) {
    return std::system(command.c_str()) == 0;
}

// Solid stills are ffmpeg's job: this binary speaks the wire only — it does not
// link the render stack — and the worker is already proven to decode what the
// CLI writes (background_media_tests builds its fixtures the same way).
bool writeSolidPng(const std::string& path, const char* rgbHex) {
    return shell("ffmpeg -v error -y -f lavfi -i color=c=" + std::string(rgbHex) +
                 ":s=64x64:r=1 -frames:v 1 '" + path + "' >/dev/null 2>&1") &&
           ::access(path.c_str(), R_OK) == 0;
}

// A background that is a PICTURE over a colour fill, with no words: the mean of
// a delivered frame is then the background and nothing else.
boost::json::object stillScene(const char* slideId, const std::string& path,
                               std::uint32_t fillColor) {
    return boost::json::object{
        {"scene", 1},
        {"showId", "live-fade"},
        {"slideId", slideId},
        {"backgroundColor", fillColor},
        {"background", boost::json::object{{"image", true},
                                           {"assetId", std::string("still-") + slideId},
                                           {"assetPath", path}}},
        {"items", boost::json::array{}},
    };
}

// The same background with no picture at all: what the colour fill alone looks
// like as delivered, which is the size of the leak the replaced composite
// produced. Measured, not assumed.
boost::json::object fillScene(std::uint32_t fillColor) {
    return boost::json::object{
        {"scene", 1},
        {"showId", "live-fade"},
        {"slideId", "PIC-FILL"},
        {"backgroundColor", fillColor},
        {"background", boost::json::object{{"image", false}, {"assetId", ""}, {"assetPath", ""}}},
        {"items", boost::json::array{}},
    };
}

// Where a delivered frame sits along `from` -> `to`, read off the two channels
// the stills disagree about.
double along(const Sample& from, const Sample& to, const Sample& frame) {
    const auto channel = [](double start, double end, double now) {
        const double span = end - start;
        return std::fabs(span) < 8.0 ? 0.5 : (now - start) / span;
    };
    return 0.5 * (channel(from.red, to.red, frame.red) + channel(from.blue, to.blue, frame.blue));
}

void exercisePictureFade(int fd, std::int64_t& nextId) {
    if (!shell("ffmpeg -version >/dev/null 2>&1")) {
        std::printf("   skipped the picture-fade scenario (no ffmpeg)\n");
        return;
    }
    const std::string tag = std::to_string(getpid());
    const std::string redStill = "/tmp/openshow-fade-red-" + tag + ".png";
    const std::string blueStill = "/tmp/openshow-fade-blue-" + tag + ".png";
    if (!check(writeSolidPng(redStill, "0xE03030"), "the red still was written") ||
        !check(writeSolidPng(blueStill, "0x3030E0"), "the blue still was written")) {
        return;
    }
    // The two stills carry the same 48 of green; the fill carries 200. Green is
    // therefore the channel that can only come from the picture — a delivered
    // frame with more of it than the endpoints have has the incoming scene's
    // fill in it. And because the two endpoints agree, the correct mid-fade
    // answer is the steady green at EVERY stage, whatever blend curve or
    // compositing space the GPU uses. That is what makes this check safe to
    // make against live output.
    const std::uint32_t loudFill = 0x28C828u;
    if (presentAndWait(fd, nextId, stillScene("PIC-A", redStill, loudFill)) == 0) {
        check(false, "the red-still scene is presented");
        return;
    }
    pump(fd, 1300);  // the media fade runs 800 ms; this is the settled endpoint
    const Sample from = meanOf(gPreview.latest);
    check(from.red > 150.0 && from.green < 100.0 && from.blue < 100.0,
          "the settled frame is the red still, not the fill under it (r=" +
              std::to_string(static_cast<int>(from.red)) + " g=" +
              std::to_string(static_cast<int>(from.green)) + " b=" +
              std::to_string(static_cast<int>(from.blue)) + ")");

    gSamples.clear();
    gRecording = true;
    const std::uint64_t blue =
        presentAndWait(fd, nextId, stillScene("PIC-B", blueStill, loudFill));
    check(blue != 0, "the blue-still scene is presented");
    pump(fd, 1300);
    gRecording = false;
    const Sample to = meanOf(gPreview.latest);
    check(to.blue > 150.0 && to.red < 100.0,
          "the fade lands on the blue still (r=" + std::to_string(static_cast<int>(to.red)) +
              " b=" + std::to_string(static_cast<int>(to.blue)) + ")");
    check(std::fabs(from.green - to.green) <= 12.0,
          "both endpoints carry the same green, so the mid-fade answer is known in advance (" +
              std::to_string(static_cast<int>(from.green)) + " vs " +
              std::to_string(static_cast<int>(to.green)) + ")");

    // What the fill itself looks like as delivered, measured rather than
    // assumed: the size of the leak the old composite produced is the distance
    // between this and the picture, scaled by stage*(1 - stage).
    if (presentAndWait(fd, nextId, fillScene(loudFill)) == 0) {
        check(false, "the bare-fill scene is presented");
    }
    pump(fd, 700);
    const Sample fill = meanOf(gPreview.latest);
    check(fill.green > 150.0,
          "the fill really is loud in the channel the pictures are quiet in (g=" +
              std::to_string(static_cast<int>(fill.green)) + ")");

    int midFrames = 0;
    double worstLift = 0.0;
    double worstDefectWouldBe = 0.0;
    std::string worst = "none";
    for (const Sample& frame : gSamples) {
        const double stage = along(from, to, frame);
        if (stage < 0.1 || stage > 0.9) {
            continue;  // not the fade's middle; the two composites agree there
        }
        ++midFrames;
        const double expectedGreen = from.green + (to.green - from.green) * stage;
        const double lift = frame.green - expectedGreen;
        if (std::fabs(lift) > std::fabs(worstLift)) {
            worstLift = lift;
            worstDefectWouldBe = stage * (1.0 - stage) * (fill.green - from.green);
            worst = "stage " + std::to_string(stage) + ", g=" +
                    std::to_string(static_cast<int>(frame.green)) + " want " +
                    std::to_string(static_cast<int>(expectedGreen));
        }
    }
    std::printf("measured: %zu frames across the live picture fade, %d mid-way; worst green "
                "lift %.1f (%s), where the two-draw fade would have measured +%.1f\n",
                gSamples.size(), midFrames, worstLift, worst.c_str(), worstDefectWouldBe);
    check(midFrames >= 1,
          "the preview stream carried this fade through its middle (two endpoints alone would "
          "make the leak check vacuous)");
    check(std::fabs(worstLift) <= 12.0,
          "no delivered frame of the live fade shows the incoming scene's fill through an "
          "opaque picture (worst lift " + std::to_string(worstLift) + ", " + worst + ")");
    check(worstDefectWouldBe > 12.0,
          "and the tolerance is tighter than the defect: the composite this replaced would "
          "have measured +" + std::to_string(worstDefectWouldBe) + " there");

    ::unlink(redStill.c_str());
    ::unlink(blueStill.c_str());
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

    // Task 6.11b (review R13): the same fade with a PICTURE in it, measured
    // against the frames the GPU delivered mid-transition.
    exercisePictureFade(fd, nextId);

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
