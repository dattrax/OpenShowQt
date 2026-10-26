/*
 * Task 6.11a (review R13): what the preview is allowed to cost the audience.
 *
 * The preview is composed on the render thread, so every millisecond it
 * spends is a millisecond the audience's next frame waits. That is fine
 * while a preview frame costs a fraction of a frame period, and it is a
 * stall the moment the target gets expensive — a large preview on a weak
 * GPU, a readback that takes tens of milliseconds. The rule 6.11a installs
 * is a share of the audience's frame period (25% of 1/60 s ≈ 4.17 ms): a
 * preview frame that costs more than that stands its own next attempt down
 * until the average is back inside the share, instead of taking its cost
 * out of every audience frame.
 *
 * This fixture arms that case on purpose: `OPENSHOW_FAULT_PREVIEW_MS=40`
 * sleeps inside the measured region of every preview publish (40 ms, ~10x
 * the share), the preview is asked for the fastest cadence the worker
 * accepts (60 fps), and the consumer releases every slot immediately so
 * nothing else can throttle it. Then:
 *
 *   - the budget policy engages — status.preview.deferred climbs, and
 *     status.preview.worstMs reports the honest 40 ms, so the deferral is
 *     visibly caused by a real cost rather than a stalled consumer;
 *   - the preview's RATE respects the share: about one publish per
 *     (cost × 3) ms of show, and never one per audience frame — without
 *     the deferral the same window would deliver roughly twice as many;
 *   - the audience pays for it anyway: the fade still animates frame after
 *     frame, a genuine mid-blend still reaches the operator, and the fade
 *     still lands on the new scene.
 *
 * The counts are the whole point, so they are stated as absolutes against
 * the fixture's own window, never against the constants in the code: a
 * budget raised to 100 ms would make "deferred == 0" the pass condition for
 * a preview that never defers, which is how a timing test survives having
 * no timing policy at all.
 *
 * Needs a display (the worker opens its output window): desktop preset,
 * label display.
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
#include <mutex>
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
    "/tmp/openshow-budget-" + std::to_string(getpid()) + ".sock";

// The armed cost of one preview publish, and the window the fixture watches
// while the audience's fade animates. 40 ms is deliberately about ten times
// the 4.17 ms the audience lends the preview, and 700 ms is most of the
// 800 ms media fade: enough publishes to make the rate a measurement rather
// than an anecdote.
constexpr int kFaultPreviewMs = 40;
constexpr int kFadeWindowMs = 700;

// The rate the deferral owes: each publish costs kFaultPreviewMs and then
// stands down for cost x (period/budget - 1) = 3 x cost, so consecutive
// publishes are at least 4 x cost = 160 ms apart. The floors and ceilings
// below keep a wide margin for scheduling while still refusing the
// undeferring behaviour (~one publish per 60 ms audience frame here).
constexpr int kMinPublishesInWindow = 2;
constexpr int kMaxPublishesInWindow = 8;
constexpr int kMinGapBetweenPublishesMs = 100;
constexpr int kMinDeferredInWindow = 3;

// The media crossfade's duration (FreeShow's 800 ms default), and how far a
// preview frame may read ahead of the audience's own clock. The slack is not
// generous by accident: fadeStart is the moment the CLIENT learned the new
// scene was active, which is after the render pass that applied it and stamped
// the fade's origin, so a correct preview is always behind (measured: never
// ahead). 0.2 is 160 ms of slack for the status round trip and the fixture's
// own 50 ms poll, and it is still three times below the +0.63 a preview that
// composes its own later moment reads.
constexpr double kMediaFadeSeconds = 0.8;
constexpr double kMaxPreviewLead = 0.2;

pid_t spawnWorker() {
    const pid_t pid = fork();
    if (pid == 0) {
        ::setenv("OPENSHOW_FAULT_PREVIEW_MS", std::to_string(kFaultPreviewMs).c_str(), 1);
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

// One frame the operator actually received, and when. The green channel is
// the fade's position: the two scenes are 0xFF0000 and 0x00FF00, so a media
// blend b reads mean green ≈ 255 × b.
struct Delivery {
    std::chrono::steady_clock::time_point when;
    double green = 0.0;
};

// A consumer that releases every slot the moment it has copied it: the only
// thing allowed to throttle this preview is the timing policy under test.
struct Preview {
    std::string name;
    std::uint64_t slotCount = 0;
    std::uint64_t slotStride = 0;
    std::uint8_t* map = nullptr;
    int fd = -1;
    std::mutex mutex;
    int delivered = 0;
    std::vector<Delivery> deliveries;
    // Newest frame's mean red and green, and how many deliveries read as a
    // genuine mix of the two scenes rather than either end.
    double meanRed = 0.0;
    double meanGreen = 0.0;
    int mixed = 0;
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
            gPreview.map + proto::previewSlotOffset(static_cast<uint32_t>(slot),
                                                    gPreview.slotStride);
        const auto* header = reinterpret_cast<const proto::PreviewSlotHeader*>(base);
        const std::uint64_t before =
            std::atomic_ref<const std::uint64_t>(header->seq).load();
        if (before == seq && (before & 1) == 0) {
            std::vector<std::uint8_t> copy(base + 64, base + 64 + rowBytes * height);
            const std::uint64_t after =
                std::atomic_ref<const std::uint64_t>(header->seq).load();
            if (after == before) {
                double red = 0.0;
                double green = 0.0;
                const std::size_t pixels = copy.size() / 4;
                for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
                    red += copy[pixel * 4];
                    green += copy[pixel * 4 + 1];
                }
                red /= static_cast<double>(pixels ? pixels : 1);
                green /= static_cast<double>(pixels ? pixels : 1);
                std::lock_guard<std::mutex> lock(gPreview.mutex);
                gPreview.meanRed = red;
                gPreview.meanGreen = green;
                // A frame that is neither scene: the crossfade mid blend.
                if (red > 50.0 && red < 210.0 && green > 50.0 && green < 210.0) {
                    ++gPreview.mixed;
                }
                ++gPreview.delivered;
                gPreview.deliveries.push_back({std::chrono::steady_clock::now(), green});
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

// The status object of the live worker, or an empty object on failure.
boost::json::object status(int fd, std::int64_t& nextId) {
    boost::json::value reply;
    if (!call(fd, nextId++, "output.status", {}, &reply) ||
        !reply.as_object().contains("result")) {
        return {};
    }
    return reply.at("result").as_object();
}

const boost::json::object* previewStatus(int fd, std::int64_t& nextId) {
    static boost::json::object holder;
    const boost::json::object result = status(fd, nextId);
    const auto found = result.find("preview");
    holder = found == result.end() || !found->value().is_object()
                 ? boost::json::object{}
                 : found->value().as_object();
    return &holder;
}

std::uint64_t fieldValue(const boost::json::object& object, const char* key) {
    const auto found = object.find(key);
    return found == object.end() ? 0 : numberValue(found->value());
}

double doubleField(const boost::json::object& object, const char* key) {
    const auto found = object.find(key);
    if (found == object.end()) {
        return -1.0;
    }
    return found->value().is_double() ? found->value().as_double()
                                      : static_cast<double>(numberValue(found->value()));
}

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

// A solid-colour scene with one word.
boost::json::object solidScene(const char* slideId, std::uint32_t color, const char* word) {
    return boost::json::object{
        {"scene", 1},
        {"showId", "budget"},
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
        const boost::json::object result = status(fd, nextId);
        const auto sceneField = result.find("scene");
        if (sceneField != result.end() && sceneField->value().is_object() &&
            fieldValue(sceneField->value().as_object(), "active") == revision) {
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
    if (!check(fd >= 0, "connected to a worker with an expensive preview armed")) {
        kill(worker, SIGKILL);
        waitpid(worker, nullptr, 0);
        return 1;
    }
    std::int64_t nextId = 1;
    boost::json::value reply;
    check(call(fd, nextId++, "hello", proto::helloParams("budget-test", 0, 1), &reply) &&
              reply.as_object().contains("result"),
          "handshake completed");

    // The two fields 6.11a added are on the wire before anything has run.
    {
        const boost::json::object preview = *previewStatus(fd, nextId);
        check(preview.contains("deferred"),
              "output.status.preview reports `deferred`");
        check(preview.contains("worstMs"), "output.status.preview reports `worstMs`");
        check(fieldValue(preview, "deferred") == 0, "nothing has deferred yet");
    }

    if (check(call(fd,
                   nextId++,
                   "preview.start",
                   boost::json::object{{"width", 640}, {"height", 360}, {"fps", 60}},
                   &reply) &&
                  reply.as_object().contains("result"),
              "preview.start succeeded at 640x360 @ 60 fps")) {
        const boost::json::object& result = reply.at("result").as_object();
        gPreview.name = std::string(result.at("name").as_string());
        gPreview.slotCount = result.at("slotCount").to_number<std::uint64_t>();
        gPreview.slotStride = result.at("bytesPerSlot").to_number<std::uint64_t>();
    }

    const std::uint64_t red = presentAndWait(fd, nextId, solidScene("A", 0xFF0000u, "ALPHA"));
    check(red != 0, "the red scene is presented");
    pump(fd, 900);
    {
        std::lock_guard<std::mutex> lock(gPreview.mutex);
        check(gPreview.meanRed > 150.0 && gPreview.meanGreen < 100.0,
              "the settled preview is red before the fade");
    }
    // One expensive publish is enough to show up honestly in the meter.
    {
        const boost::json::object preview = *previewStatus(fd, nextId);
        check(fieldValue(preview, "published") >= 1, "the preview published the change");
        check(doubleField(preview, "worstMs") >= static_cast<double>(kFaultPreviewMs),
              "worstMs reports the 40 ms the armed preview really cost the render thread");
    }

    // Fade (the default transition) to green and watch the audience's own
    // window: the fade must animate, and the preview must rate-limit itself.
    const boost::json::object before = status(fd, nextId);
    const boost::json::object beforePreview =
        before.contains("preview") ? before.at("preview").as_object()
                                   : boost::json::object{};
    const std::uint64_t baselineFrames = fieldValue(before, "frames");
    const std::uint64_t baselinePublished = fieldValue(beforePreview, "published");
    const std::uint64_t baselineDeferred = fieldValue(beforePreview, "deferred");
    const std::size_t deliveriesBefore = [&] {
        std::lock_guard<std::mutex> lock(gPreview.mutex);
        return gPreview.deliveries.size();
    }();

    const std::uint64_t green =
        presentAndWait(fd, nextId, solidScene("B", 0x00FF00u, "BETA"));
    check(green != 0 && green != red, "the green scene is presented");
    // The fade's origin is at or BEFORE this instant: the apply and the fade's
    // first capture happen in the same render pass, and output.status goes out
    // after that pass. So everything the audience shows from here on is bounded
    // by (now - fadeStart) / 800 ms, which is what the preview's own blends are
    // judged against below.
    const auto fadeStart = std::chrono::steady_clock::now();
    std::uint64_t peakGain = 0;
    const auto fadeWindow =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kFadeWindowMs);
    while (std::chrono::steady_clock::now() < fadeWindow) {
        pump(fd, 100);
        peakGain = std::max(peakGain, fieldValue(status(fd, nextId), "frames") - baselineFrames);
    }
    check(peakGain >= 4,
          "the audience's fade still animates frame after frame under an expensive preview");

    const boost::json::object duringStatus = status(fd, nextId);
    const boost::json::object duringPreview = duringStatus.contains("preview")
                                                   ? duringStatus.at("preview").as_object()
                                                   : boost::json::object{};
    const std::uint64_t framesDuring = fieldValue(duringStatus, "frames") - baselineFrames;
    const std::uint64_t publishedDuring =
        fieldValue(duringPreview, "published") - baselinePublished;
    const std::uint64_t deferredDuring = fieldValue(duringPreview, "deferred") - baselineDeferred;
    const std::vector<Delivery> deliveries = [&] {
        std::lock_guard<std::mutex> lock(gPreview.mutex);
        return std::vector<Delivery>(gPreview.deliveries.begin() +
                                         static_cast<std::ptrdiff_t>(deliveriesBefore),
                                     gPreview.deliveries.end());
    }();

    // The budget. 40 ms of preview against a 4.17 ms share means the policy
    // owes the audience three frames stood down for every one it takes, so
    // the window allows a handful of publishes — and would reject the ~12 an
    // undeferring preview fits into the same 700 ms.
    check(publishedDuring >= kMinPublishesInWindow,
          "the preview is not starved: it still reaches the operator while the show animates");
    check(publishedDuring <= kMaxPublishesInWindow,
          "the preview stays inside its share of the audience's frame — it does not take "
          "40 ms out of every frame");
    check(deferredDuring >= kMinDeferredInWindow,
          "the stood-down attempts are counted, so the deferral is visible on the wire");
    // Deferral must not become a re-ask loop: one audience frame can stand the
    // preview down at most once, and a preview-only wake is bounded by the
    // defer horizon itself. Measured 44 stand-downs against 49 audience frames
    // here; before the horizon was fed into the loop's wake-up the same window
    // counted 58,035 — the loop asking for a frame it had just refused some
    // twenty thousand times a second, which is the busy loop 1.5e exists to
    // prevent, paid for with the audience's CPU.
    //
    // The ceiling is stated as arithmetic, not a magic pad: attempts are one
    // per audience frame, plus one per pass the defer horizon itself wakes the
    // loop for (one per 4x cost, so window/(4*cost)), plus the loop's 500 ms
    // housekeeping pass, plus one for the status read bracketing a slightly
    // different instant. Here that is 49 + 4 + 2 + 1 = 56 against a measured
    // 44, and the same window without the horizon fix counted 58,035, so the
    // bound is three orders of magnitude away from what it refuses.
    check(deferredDuring <= framesDuring + kFadeWindowMs / (4 * kFaultPreviewMs) + 3,
          "the preview stands down at most once per audience frame — refusing a frame does "
          "not turn the render loop into a spin");
    check(doubleField(duringPreview, "worstMs") >= static_cast<double>(kFaultPreviewMs),
          "and the meter says what one preview frame cost");

    // The same rule, seen as a gap instead of a count: two consecutive
    // deliveries may not be closer than the defer window they owe (4x the
    // cost, here 160 ms; 100 ms is the floor with room for scheduling). An
    // undeferring preview delivers on every audience frame, ~60 ms apart.
    int gaps = 0;
    int narrowestMs = 1 << 20;
    for (std::size_t i = 1; i < deliveries.size(); ++i) {
        const int ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deliveries[i].when -
                                                                  deliveries[i - 1].when)
                .count());
        narrowestMs = std::min(narrowestMs, ms);
        ++gaps;
    }
    check(gaps >= 2, "the window delivered often enough for the gap to mean something");
    check(narrowestMs >= kMinGapBetweenPublishesMs,
          "consecutive preview frames are at least the defer window apart — the expensive "
          "preview defers its own next attempt");

    // The rule R13 is about, in the one form the wire can show: THE PREVIEW MAY
    // LAG THE AUDIENCE, IT MAY NEVER LEAD IT. Mean green is the media blend the
    // operator is being shown (the scenes are pure red and pure green), so a
    // frame that reads further along the fade than the audience's own clock at
    // its arrival was composed on some other clock — which is precisely what a
    // preview that captures a moment of its own does. Measured lead with the
    // shipped code is always negative (the preview is behind); a preview that
    // captures "as of now + 0.5 s" reads +0.63, three times the slack below.
    double worstLead = -1.0;
    for (const Delivery& delivery : deliveries) {
        const double audienceSeconds =
            std::chrono::duration<double>(delivery.when - fadeStart).count();
        const double owedBlend = audienceSeconds / kMediaFadeSeconds;
        worstLead = std::max(worstLead, delivery.green / 255.0 - owedBlend);
    }
    check(worstLead <= kMaxPreviewLead,
          "no preview frame showed a blend from the audience's future — the preview draws "
          "the frame the panel drew, not one of its own");

    // What the audience got anyway: a real crossfade, mid blend included,
    // landing on the new scene.
    pump(fd, 900);
    {
        std::lock_guard<std::mutex> lock(gPreview.mutex);
        check(gPreview.mixed >= 1,
              "a genuine mid-blend frame still reached the operator under the budget");
        check(gPreview.meanGreen > 150.0 && gPreview.meanRed < 100.0,
              "the fade still lands on the new (green) scene");
    }
    const std::uint64_t settledA = fieldValue(status(fd, nextId), "frames");
    pump(fd, 400);
    check(fieldValue(status(fd, nextId), "frames") == settledA,
          "the loop goes idle again once the fade is complete — no busy preview either");

    call(fd, nextId++, "shutdown", {}, &reply);
    int status_code = 0;
    waitpid(worker, &status_code, 0);
    check(WIFEXITED(status_code) && WEXITSTATUS(status_code) == 0,
          "the worker exited cleanly");

    close(fd);
    ::unlink(gSocket.c_str());
    if (gPreview.map != nullptr) {
        ::munmap(gPreview.map, gPreview.slotStride * gPreview.slotCount + 64);
        ::close(gPreview.fd);
    }
    if (gFailures > 0) {
        std::fprintf(stderr, "preview_budget_tests: %d failure(s)\n", gFailures);
        return 1;
    }
    std::fprintf(stderr,
                 "measured: published=%llu deferred=%llu worstMs=%.2f deliveries=%zu "
                 "narrowestGapMs=%d audienceFrames=%llu worstLead=%+.3f\n",
                 static_cast<unsigned long long>(publishedDuring),
                 static_cast<unsigned long long>(deferredDuring),
                 doubleField(duringPreview, "worstMs"),
                 deliveries.size(),
                 narrowestMs,
                 static_cast<unsigned long long>(framesDuring),
                 worstLead);
    std::printf("ok: an expensive preview stays inside the audience's timing budget\n");
    return 0;
}
