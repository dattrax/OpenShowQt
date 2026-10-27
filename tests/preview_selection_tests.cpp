/*
 * Task 6.16a (review R16): the editor's picture, drawn by the worker, off
 * the air.
 *
 * The review found a second text engine. The edit canvas laid its slide out
 * with QML (`fontSizeMode: Text.Fit` over a Rectangle), so what an operator
 * composed was a Qt guess at what Skia would do — different wrapping, a
 * different fit, a different fallback — and the only real render was the one
 * the AUDIENCE was already being shown. Looking at a slide you had not put
 * on air was not a thing the worker could do.
 *
 * So the preview stream grew a second target. `preview.select {revision,
 * request}` names a scene in the prepared store and the render thread draws
 * it into the same transport, tagged with whom it was drawn for and which
 * request it answers; the audience's mirror keeps riding alongside. Four
 * promises, one scenario group each below:
 *
 *   - the off-air render is REAL and OFF-AIR: the pixels are the scene the
 *     operator asked for (not a black frame, not the audience's), and while
 *     they flow the audience's presented frame counter is FLAT, its scene
 *     active is unchanged, and its own frames keep their own colour;
 *   - one accepted request yields ONE frame: a burst of asks produces a
 *     bounded, strictly increasing run of generations and then stops —
 *     a selection is not a second stream to tick forever;
 *   - a stale frame cannot be delivered, and a stale REQUEST cannot be
 *     accepted: a replayed generation is refused naming both numbers, a
 *     superseded draw is destroyed before it reaches the wire (its counter
 *     is `stale`, and the frame that lands answers the NEWER ask), and a
 *     scene the store has forgotten is refused naming the revision;
 *   - the audience is unaffected: a crossfade under way completes, shows
 *     its mid blend and lands on the new scene while the editor hammers.
 *
 * The stale case is armed on purpose with OPENSHOW_FAULT_PREVIEW_MS, which
 * sleeps inside the measured region of a preview draw: ask, then re-ask
 * while the first draw is still asleep. Without the knob the two commands
 * would land in the same render pass and the superseded frame would never
 * have begun, which is the version of the story that teaches nothing.
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
    "/tmp/openshow-select-" + std::to_string(getpid()) + ".sock";

// The armed cost of one preview draw, and how long to wait between the two
// asks so the first is definitely mid-flight when the second arrives.
constexpr int kFaultPreviewMs = 250;
constexpr int kAskLeadMs = 80;

// Scene colours: far apart in one channel each, so "which scene are these
// pixels" is a mean, not a comparison against a reference image.
constexpr std::uint32_t kRed = 0xFF0000u;
constexpr std::uint32_t kGreen = 0x00FF00u;
constexpr std::uint32_t kBlue = 0x0000FFu;

pid_t spawnWorker(bool armFault) {
    const pid_t pid = fork();
    if (pid == 0) {
        if (armFault) {
            ::setenv("OPENSHOW_FAULT_PREVIEW_MS", std::to_string(kFaultPreviewMs).c_str(),
                     1);
        }
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

// One frame this consumer copied, with the metadata the worker published
// beside it and the colour the pixels actually are.
struct Delivery {
    std::uint64_t slot = 0;
    std::uint64_t seq = 0;
    std::uint64_t revision = 0;
    std::uint64_t target = 0;    // notification field
    std::uint64_t request = 0;   // notification field
    std::uint64_t headTarget = 0;  // read out of the shm slot header
    std::uint64_t headRequest = 0;
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    std::string stream;
};

// A consumer that releases every slot as soon as it has copied it: the only
// things allowed to hold this stream back are the policies under test.
struct Consumer {
    std::string name;
    std::uint64_t slotCount = 0;
    std::uint64_t slotStride = 0;
    std::uint8_t* map = nullptr;
    int fd = -1;
    std::mutex mutex;
    std::vector<Delivery> delivered;
} gConsumer;

void sendRaw(int fd, const boost::json::value& message) {
    std::string wire;
    proto::appendFrame(message, &wire);
    ::send(fd, wire.data(), wire.size(), MSG_NOSIGNAL);
}

void mapSegment() {
    for (int attempt = 0; attempt < 40 && gConsumer.map == nullptr; ++attempt) {
        const int shm = ::shm_open(gConsumer.name.c_str(), O_RDONLY, 0);
        if (shm < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        const std::size_t total = gConsumer.slotStride * gConsumer.slotCount + 64;
        void* map = ::mmap(nullptr, total, PROT_READ, MAP_SHARED, shm, 0);
        if (map == MAP_FAILED) {
            ::close(shm);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        const auto* header = static_cast<const proto::PreviewSegmentHeader*>(map);
        if (header->magic != proto::kPreviewMagic ||
            header->bytesPerSlot != gConsumer.slotStride) {
            ::munmap(map, total);
            ::close(shm);
            return;
        }
        gConsumer.map = static_cast<std::uint8_t*>(map);
        gConsumer.fd = shm;
    }
}

void handlePreviewReady(int fd, const boost::json::object& params) {
    if (gConsumer.map == nullptr && !gConsumer.name.empty()) {
        mapSegment();
    }
    auto number = [&params](const char* key) -> std::uint64_t {
        const auto entry = params.find(key);
        return entry == params.end() ? 0 : entry->value().to_number<std::uint64_t>();
    };
    const std::uint64_t slot = number("slot");
    const std::uint64_t seq = number("seq");
    const auto stream = params.find("stream");
    if (stream == params.end() || !stream->value().is_string() ||
        std::string_view(stream->value().as_string()) != gConsumer.name) {
        return;  // a frame of somebody else's stream is not ours to copy
    }
    const std::uint64_t width = number("width");
    const std::uint64_t height = number("height");
    const std::uint64_t rowBytes = number("rowBytes");

    Delivery delivery;
    delivery.slot = slot;
    delivery.seq = seq;
    delivery.revision = number("revision");
    delivery.target = number("target");
    delivery.request = number("request");
    delivery.stream = gConsumer.name;

    if (gConsumer.map != nullptr && slot < gConsumer.slotCount && rowBytes == width * 4 &&
        width > 0 && height > 0) {
        const std::uint8_t* base =
            gConsumer.map + proto::previewSlotOffset(static_cast<uint32_t>(slot),
                                                     gConsumer.slotStride);
        const auto* header = reinterpret_cast<const proto::PreviewSlotHeader*>(base);
        const std::uint64_t before =
            std::atomic_ref<const std::uint64_t>(header->seq).load();
        if (before == seq && (before & 1) == 0) {
            std::vector<std::uint8_t> copy(base + 64, base + 64 + rowBytes * height);
            const std::uint64_t headTarget = header->target;
            const std::uint64_t headRequest = header->request;
            const std::uint64_t after =
                std::atomic_ref<const std::uint64_t>(header->seq).load();
            if (after == before) {  // the producer never touched it mid-copy
                delivery.headTarget = headTarget;
                delivery.headRequest = headRequest;
                double red = 0.0;
                double green = 0.0;
                double blue = 0.0;
                const std::size_t pixels = copy.size() / 4;
                for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
                    red += copy[pixel * 4];
                    green += copy[pixel * 4 + 1];
                    blue += copy[pixel * 4 + 2];
                }
                const double count = static_cast<double>(pixels ? pixels : 1);
                delivery.red = red / count;
                delivery.green = green / count;
                delivery.blue = blue / count;
                std::lock_guard<std::mutex> lock(gConsumer.mutex);
                gConsumer.delivered.push_back(std::move(delivery));
            }
        }
    }
    sendRaw(fd, proto::makeNotification(
                   proto::kMethodPreviewReleased,
                   boost::json::object{{"slot", static_cast<std::int64_t>(slot)},
                                       {"seq", static_cast<std::int64_t>(seq)},
                                       {"stream", gConsumer.name}}));
}

proto::Decoder gDecoder;

// One request/reply, consuming the preview traffic that interleaves — the
// stream has to keep draining while the fixture is asking questions.
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

void pump(int fd, int milliseconds) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < until) {
        pollfd pending{fd, POLLIN, 0};
        if (::poll(&pending, 1, 50) > 0) {
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

std::uint64_t numberValue(const boost::json::value& value) {
    // Status fields come in three shapes: counters (uint64), flags (bool),
    // and times (double). Reading a bool as an int64 is a throw, so every
    // shape is asked about — `active` is a flag, and a fixture that crashed
    // on it would test nothing at all.
    if (value.is_bool()) {
        return value.as_bool() ? 1u : 0u;
    }
    if (value.is_uint64()) {
        return value.as_uint64();
    }
    if (value.is_int64()) {
        return static_cast<std::uint64_t>(value.as_int64());
    }
    if (value.is_double()) {
        return static_cast<std::uint64_t>(value.as_double());
    }
    return 0;
}

boost::json::object status(int fd, std::int64_t& nextId) {
    boost::json::value reply;
    if (!call(fd, nextId++, "output.status", {}, &reply) ||
        !reply.as_object().contains("result")) {
        return {};
    }
    return reply.at("result").as_object();
}

std::uint64_t fieldValue(const boost::json::object& object, const char* key) {
    const auto found = object.find(key);
    return found == object.end() ? 0 : numberValue(found->value());
}

// status.preview, and status.preview.selection within it.
boost::json::object previewStatus(int fd, std::int64_t& nextId) {
    const boost::json::object result = status(fd, nextId);
    const auto found = result.find("preview");
    return found == result.end() || !found->value().is_object() ? boost::json::object{}
                                                                : found->value().as_object();
}

boost::json::object selectionStatus(int fd, std::int64_t& nextId) {
    const boost::json::object preview = previewStatus(fd, nextId);
    const auto found = preview.find("selection");
    return found == preview.end() || !found->value().is_object() ? boost::json::object{}
                                                                 : found->value().as_object();
}

std::vector<Delivery> deliveries() {
    std::lock_guard<std::mutex> lock(gConsumer.mutex);
    return gConsumer.delivered;
}

std::vector<Delivery> selectionDeliveries() {
    std::vector<Delivery> out;
    for (const auto& delivery : deliveries()) {
        if (delivery.target == proto::kPreviewTargetSelection) {
            out.push_back(delivery);
        }
    }
    return out;
}

// The newest frame answering `request`, if one has arrived.
bool waitForSelection(int fd, std::uint32_t request, int milliseconds,
                      Delivery* out) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < until) {
        for (const auto& delivery : selectionDeliveries()) {
            if (delivery.request == request) {
                if (out != nullptr) {
                    *out = delivery;
                }
                return true;
            }
        }
        pump(fd, 40);
    }
    return false;
}

// A solid-colour scene with one word.
boost::json::object solidScene(const char* slideId, std::uint32_t color, const char* word) {
    return boost::json::object{
        {"scene", 1},
        {"showId", "select16"},
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

std::uint64_t prepareScene(int fd, std::int64_t& nextId, const boost::json::object& scene) {
    boost::json::value reply;
    if (!call(fd, nextId++, "scene.prepare", scene, &reply) ||
        !reply.as_object().contains("result")) {
        return 0;
    }
    return numberValue(reply.at("result").as_object().at("revision"));
}

// Prepares and presents, and waits for the audience to have it.
std::uint64_t presentAndWait(int fd, std::int64_t& nextId, const boost::json::object& scene,
                             const char* transition = nullptr) {
    const std::uint64_t revision = prepareScene(fd, nextId, scene);
    if (revision == 0) {
        return 0;
    }
    boost::json::object params{{"revision", revision}};
    if (transition != nullptr) {
        params["transition"] = transition;
    }
    boost::json::value reply;
    if (!call(fd, nextId++, "scene.present", params, &reply) ||
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

// preview.select, as the frontend sends it. Returns the error text (empty
// when the ask was accepted).
std::string selectScene(int fd, std::int64_t& nextId, std::uint64_t revision,
                        std::uint32_t request) {
    boost::json::value reply;
    if (!call(fd, nextId++, proto::kMethodPreviewSelect,
              boost::json::object{{"revision", revision},
                                  {"request", static_cast<std::uint32_t>(request)}},
              &reply)) {
        return "<no reply>";
    }
    if (reply.as_object().contains("error")) {
        return std::string(reply.at("error").at("message").as_string());
    }
    return {};
}

std::size_t publishedCount(int fd, std::int64_t& nextId) {
    return static_cast<std::size_t>(fieldValue(selectionStatus(fd, nextId), "published"));
}

// The AUDIENCE's own published count: how many frames of the show reached the
// shared stream. Its gate is "a presented frame has not been mirrored yet",
// so this counter is a direct reading of how much the audience owes — which
// is exactly what drawing an edit must leave alone.
std::size_t mirrorCount(int fd, std::int64_t& nextId) {
    return static_cast<std::size_t>(fieldValue(previewStatus(fd, nextId), "published"));
}

// --- the ask, answered -------------------------------------------------
//
// The slide the audience is NOT shown, rendered by the worker: the pixels
// must be the asked-for scene (so the render is real), must say so in the
// metadata (so the client can route it), and must not have touched the
// audience on the way out (so "off the air" means something).
void testOffAirRender(std::int64_t& nextId, int fd, std::uint64_t redRevision,
                      std::uint64_t blueRevision) {
    const boost::json::object before = status(fd, nextId);
    const std::uint64_t presentedBefore = fieldValue(before, "frames");

    const std::string refusal = selectScene(fd, nextId, blueRevision, 1);
    check(refusal.empty(), ("preview.select accepts a prepared scene: " + refusal).c_str());

    Delivery frame;
    const bool arrived = waitForSelection(fd, 1, 3000, &frame);
    check(arrived, "the off-air render reaches the consumer");
    if (!arrived) {
        return;
    }
    check(frame.target == proto::kPreviewTargetSelection,
          "the frame names itself as the selection target");
    check(frame.request == 1, "the frame names the request it answers");
    check(frame.revision == (blueRevision & 0xFFFFFFFFu),
          "the frame carries the SELECTED scene's revision, not the presented one");
    // The header and the notification are written under the same seqlock, so
    // a consumer may route on either; disagreement would be a frame that
    // tells the client one thing and carries another.
    check(frame.headTarget == frame.target && frame.headRequest == frame.request,
          "the slot header and the notification agree on target and request");

    // The scene, not a black frame and not the audience's: the blue scene
    // fills the target, so blue dominates by a mile.
    check(frame.blue > frame.red + 80.0 && frame.blue > frame.green + 80.0,
          "the pixels ARE the selected scene (blue), not black and not the audience's red");

    // And the audience. The panel is still showing the red scene, its
    // presented-frame counter is FLAT across everything above and the
    // window below, and the mirror frames that arrived in the same window
    // are still red.
    pump(fd, 500);
    const boost::json::object after = status(fd, nextId);
    const bool audienceScene =
        after.contains("scene") && after.at("scene").is_object() &&
        fieldValue(after.at("scene").as_object(), "active") == redRevision;
    check(audienceScene,
          "the audience's active scene is untouched by looking at an edit");
    const std::uint64_t presentedAfter = fieldValue(after, "frames");
    check(presentedAfter == presentedBefore,
          "the audience's presented-frame counter is flat: an edit draw is not a present");

    const boost::json::object selection = selectionStatus(fd, nextId);
    check(fieldValue(selection, "active") == 1, "status reports the selection is registered");
    check(fieldValue(selection, "revision") == blueRevision,
          "status names the revision the selection answers");
    check(fieldValue(selection, "request") == 1, "status names the accepted request");
    check(fieldValue(selection, "published") >= 1,
          "status counts the frame that reached the socket");

    // Both targets on one stream: the mirror keeps arriving (so the edit did
    // not take the transport over) and keeps showing the audience's own
    // scene (so the two never share pixels).
    int mirrors = 0;
    int redMirrors = 0;
    int blueMirrors = 0;
    for (const auto& delivery : deliveries()) {
        if (delivery.target != proto::kPreviewTargetAudience) {
            continue;
        }
        ++mirrors;
        if (delivery.red > delivery.blue + 80.0 && delivery.red > delivery.green + 80.0) {
            ++redMirrors;
        }
        if (delivery.blue > delivery.red + 80.0) {
            ++blueMirrors;
        }
    }
    check(mirrors >= 1, "the audience mirror kept arriving on the shared stream");
    check(redMirrors >= 1, "and the mirror shows the audience's own scene");
    check(blueMirrors == 0,
          "no edit frame leaked into a frame tagged for the audience");

    // An off-air draw must also leave the audience's DEBT alone. The mirror's
    // gate is "the last presented frame has not reached the stream", so a
    // selection that advanced the frame counter without presenting would
    // leave the stream chasing a frame that was never presented: the editor
    // asked for a picture of their own slide and the audience pays for a
    // second mirror of the same one. Both sides are read at rest, so this is
    // not a cadence question — it is whether a frame appears that nothing
    // presented.
    pump(fd, 400);
    const std::size_t mirrorsAtRest = mirrorCount(fd, nextId);
    const std::string again = selectScene(fd, nextId, blueRevision, 2);
    check(again.empty(), ("a second ask of the same scene is accepted: " + again).c_str());
    Delivery second;
    if (check(waitForSelection(fd, 2, 3000, &second), "and the second ask is answered")) {
        pump(fd, 600);
        check(mirrorCount(fd, nextId) == mirrorsAtRest,
              "an off-air draw creates no mirror debt - the audience owed nothing more "
              "because an edit was drawn");
        check(second.request == 2 && second.target == proto::kPreviewTargetSelection,
              "the second frame answers the second ask, and only that");
    }
}

// --- one ask, one frame ------------------------------------------------
//
// The ask is a request, not a subscription: the stream must not turn into a
// second thing ticking at 60 fps under an editor that has stopped typing. A
// burst of asks produces a bounded, strictly increasing run of generations,
// the newest ask is the one that lands, and the published counter stops.
void testOneFramePerRequest(std::int64_t& nextId, int fd, std::uint64_t greenRevision,
                            std::uint64_t blueRevision) {
    const std::size_t before = publishedCount(fd, nextId);

    constexpr int kBurst = 6;
    for (int i = 0; i < kBurst; ++i) {
        const std::uint64_t revision = (i % 2 == 0) ? greenRevision : blueRevision;
        const std::string refusal = selectScene(fd, nextId, revision, 10 + i);
        check(refusal.empty(), "every ask in the burst is accepted");
    }
    // Let the render thread work through them and then be idle.
    pump(fd, 1500);
    const std::size_t afterBurst = publishedCount(fd, nextId);

    std::vector<std::uint32_t> requests;
    for (const auto& delivery : selectionDeliveries()) {
        if (delivery.request >= 10 && delivery.request < 10 + kBurst) {
            requests.push_back(static_cast<std::uint32_t>(delivery.request));
        }
    }
    check(!requests.empty(), "the burst produced selection frames");
    // Strictly increasing: one frame per accepted request, no duplicates,
    // and no resurrected older generation (the sequence the frontend sees is
    // the sequence it asked in).
    bool increasing = true;
    for (std::size_t i = 1; i < requests.size(); ++i) {
        if (requests[i] <= requests[i - 1]) {
            increasing = false;
        }
    }
    check(increasing, "the generations the consumer receives strictly increase");
    check(requests.size() <= kBurst,
          "a burst of six asks yields at most six frames (one per request, not a stream)");
    check(requests.back() == 10 + kBurst - 1,
          "the newest ask is the one that lands: the operator sees what they last picked");
    check(afterBurst - before <= static_cast<std::size_t>(kBurst),
          "the published counter moved by the burst, not by a subscription");

    // Now idle: nothing asked, nothing more published.
    const std::size_t settled = publishedCount(fd, nextId);
    pump(fd, 900);
    check(publishedCount(fd, nextId) == settled,
          "an answered ask stops being owed: the selection does not keep ticking");
}

// --- what gets refused -------------------------------------------------
//
// Every refusal has to name something the frontend can act on, and none of
// them may leave the accepted generation moved: a refusal that consumed the
// counter would strand the client, whose next ask is by definition higher.
void testRefusals(std::int64_t& nextId, int fd, std::uint64_t greenRevision) {
    const boost::json::object settled = selectionStatus(fd, nextId);
    const std::uint64_t accepted = fieldValue(settled, "request");

    // A revision the bounded store does not hold (or no longer holds).
    const std::string unknown = selectScene(fd, nextId, 999999, accepted + 1);
    check(!unknown.empty(), "an unknown revision is refused");
    check(unknown.find("999999") != std::string::npos,
          ("the refusal names the revision: " + unknown).c_str());
    check(unknown.find("store") != std::string::npos,
          ("the refusal says where: the prepared store: " + unknown).c_str());

    // A replayed generation — the exact shape of a queued or retried ask.
    const std::string replay = selectScene(fd, nextId, greenRevision, 1);
    check(!replay.empty(), "a replayed request generation is refused");
    check(replay.find("1") != std::string::npos &&
              replay.find(std::to_string(accepted)) != std::string::npos,
          ("the refusal names both generations: " + replay).c_str());

    // And the same generation twice: not accepted the first time round, so
    // still not accepted the second (the counter moved for nobody).
    const std::string replayAgain = selectScene(fd, nextId, greenRevision, 1);
    check(!replayAgain.empty(), "the refused generation stays refused");

    const boost::json::object still = selectionStatus(fd, nextId);
    check(fieldValue(still, "request") == accepted,
          "refusals leave the accepted generation exactly where it was");
    check(fieldValue(still, "active") == fieldValue(settled, "active"),
          "and leave the registered selection alone");

    // With no stream there is nowhere for the pixels to go. That refusal is
    // checked in the stand-down test, which stops the stream and asks again.
}

// --- the audience keeps its show ---------------------------------------
//
// R13's rule, re-proved with a second caller on the stream: the audience's
// crossfade completes, actually shows its mid blend, and lands on the new
// scene while the editor hammers away. The fade's clock is the audience's,
// so drawing an edit alongside it can neither strand it nor run it ahead.
void testAudienceUnaffected(std::int64_t& nextId, int fd, std::uint64_t redRevision,
                            std::uint64_t greenRevision, std::uint64_t blueRevision) {
    check(redRevision != 0, "the red scene was presented earlier in this fixture");
    boost::json::value present;
    check(call(fd, nextId++, "scene.present",
               boost::json::object{{"revision", redRevision}, {"transition", "cut"}}, &present),
          "back to the red scene, on a cut");
    pump(fd, 700);  // let it settle, so the fade below starts clean

    // The fade itself: red -> blue, the worker's default 800 ms.
    const auto fadeStart = std::chrono::steady_clock::now();
    check(call(fd, nextId++, "scene.present",
               boost::json::object{{"revision", blueRevision}, {"transition", "fade"}},
               &present),
          "the fade to the blue scene is issued");

    // While it runs, the editor keeps asking. One ask per poll: this is the
    // load that used to be the preview's excuse to compose its own moment.
    std::uint32_t request = 100;
    int mixed = 0;
    int edits = 0;
    std::uint64_t active = 0;
    bool transitioning = true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        const boost::json::object result = status(fd, nextId);
        const auto sceneField = result.find("scene");
        if (sceneField != result.end() && sceneField->value().is_object()) {
            const boost::json::object& scene = sceneField->value().as_object();
            active = fieldValue(scene, "active");
            transitioning = fieldValue(scene, "transitioning") != 0;
        }
        selectScene(fd, nextId, (request % 2 == 0) ? greenRevision : redRevision, request);
        ++request;
        ++edits;
        pump(fd, 40);
        for (const auto& delivery : deliveries()) {
            // A genuine blend of the audience's two scenes: red fading to
            // blue, so both are lit and neither dominates by 80.
            if (delivery.target == proto::kPreviewTargetAudience && delivery.red > 60.0 &&
                delivery.blue > 60.0 && std::abs(delivery.red - delivery.blue) < 90.0 &&
                delivery.green < 60.0) {
                ++mixed;
            }
        }
        if (!transitioning && active == blueRevision && mixed > 0) {
            break;
        }
    }

    check(edits >= 5, "the editor was actually asking during the fade");
    check(active == blueRevision,
          "the fade lands on the new scene with an editor drawing alongside it");
    check(!transitioning, "and reports itself finished, not stranded mid-blend");
    check(mixed > 0, "and the audience's own mirror still shows a mid blend");
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - fadeStart).count();
    check(seconds >= 0.3,
          "the fade was not skipped to its end by the extra target (it took real time)");
}

// --- stand-down, and no stream -----------------------------------------
//
// Revision 0 takes the target off: the stream goes back to mirroring the
// audience alone, and an ask with no stream at all is refused for the reason
// the operator can act on.
void testStandDown(std::int64_t& nextId, int fd, std::uint64_t greenRevision) {
    const boost::json::object settled = selectionStatus(fd, nextId);
    const std::uint32_t next = static_cast<std::uint32_t>(fieldValue(settled, "request")) + 1;
    const std::size_t before = publishedCount(fd, nextId);

    const std::string refusal = selectScene(fd, nextId, 0, next);
    check(refusal.empty(), "revision 0 stands the selection down");
    pump(fd, 400);
    const boost::json::object selection = selectionStatus(fd, nextId);
    check(fieldValue(selection, "active") == 0, "status says no selection is registered");
    const std::size_t after = publishedCount(fd, nextId);
    check(after == before, "and no further selection frames arrive");

    boost::json::value stopped;
    if (check(call(fd, nextId++, "preview.stop", boost::json::object{}, &stopped),
              "preview.stop succeeds")) {
        const std::string noStream = selectScene(fd, nextId, greenRevision, next + 1);
        check(!noStream.empty(), "with no stream an ask is refused");
        check(noStream.find("stream") != std::string::npos,
              ("and says there is no stream: " + noStream).c_str());
    }
}

// --- the superseded draw (armed) ---------------------------------------
//
// The race the generation gate exists for: a draw that has already spent its
// time when the operator picks something else. With the fault knob the first
// draw is verifiably still asleep when the second ask lands, so the frame it
// would have published is destroyed rather than delivered, and the ask that
// arrives second is the one the wire carries.
void testSupersededDrawIsDestroyed(std::int64_t& nextId, int fd, std::uint64_t greenRevision,
                                   std::uint64_t blueRevision) {
    const std::string first = selectScene(fd, nextId, greenRevision, 1);
    check(first.empty(), ("the first ask is accepted: " + first).c_str());
    std::this_thread::sleep_for(std::chrono::milliseconds(kAskLeadMs));  // inside the draw
    const std::string second = selectScene(fd, nextId, blueRevision, 2);
    check(second.empty(),
          ("and the ask that supersedes it is accepted too: " + second).c_str());

    Delivery frame;
    // The budget is armed too: a 250 ms draw stands the next attempt down
    // for seconds, so this window is generous on purpose. What it must not
    // turn into is a second frame of the SUPERSEDED ask, however long the
    // consumer waits for the one it is owed.
    const bool newerArrived = waitForSelection(fd, 2, 9000, &frame);
    check(newerArrived, "the newer ask is the frame that reaches the consumer");

    bool supersededReached = false;
    for (const auto& delivery : selectionDeliveries()) {
        if (delivery.request == 1 && delivery.target == proto::kPreviewTargetSelection) {
            supersededReached = true;
        }
    }
    check(!supersededReached,
          "the superseded draw never reached the wire (stale imagery is destroyed, not "
          "downgraded)");
    if (newerArrived) {
        check(frame.blue > frame.red + 80.0 && frame.blue > frame.green + 80.0,
              "and it is the scene the operator last picked");
    }
    const boost::json::object selection = selectionStatus(fd, nextId);
    check(fieldValue(selection, "stale") >= 1,
          "the destroyed draw is counted, not silently lost");
    check(fieldValue(selection, "request") == 2, "the accepted generation is the newer ask");
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc > 1) {
        gWorkerBinary = argv[1];
    }

    // ------------------------------------------------------------------
    // Phase one: the ordinary worker.
    // ------------------------------------------------------------------
    ::unlink(gSocket.c_str());
    pid_t worker = spawnWorker(false);
    int fd = connectRetry();
    if (!check(fd >= 0, "connected to a worker")) {
        kill(worker, SIGKILL);
        waitpid(worker, nullptr, 0);
        return 1;
    }
    std::int64_t nextId = 1;
    boost::json::value reply;
    check(call(fd, nextId++, "hello", proto::helloParams("select16-test", 0, 1), &reply) &&
              reply.as_object().contains("result"),
          "handshake completed");
    {  // The capability is what tells a frontend the worker can draw an edit.
        const auto& caps = reply.at("result").as_object().at("capabilities").as_array();
        const bool hasSelect = std::any_of(caps.begin(), caps.end(),
                                           [](const boost::json::value& value) {
                                               return value.is_string() &&
                                                      value.as_string() == "preview.select.v1";
                                           });
        check(hasSelect, "hello advertises preview.select.v1");
    }
    {  // The block exists before anything has been asked for.
        const boost::json::object preview = previewStatus(fd, nextId);
        check(preview.contains("selection"), "output.status.preview reports `selection`");
        const boost::json::object selection = selectionStatus(fd, nextId);
        check(fieldValue(selection, "active") == 0, "nothing is selected yet");
        check(fieldValue(selection, "published") == 0, "no selection frame has been published");
        check(selection.contains("stale"), "status.preview.selection reports `stale`");
        check(selection.contains("deferred"), "status.preview.selection reports `deferred`");
    }
    {  // With no stream the answer is a refusal, not a queued surprise.
        const std::string noStream = selectScene(fd, nextId, 1, 1);
        check(!noStream.empty(), "preview.select before preview.start is refused");
        check(noStream.find("stream") != std::string::npos,
              "and the refusal names the missing stream");
    }

    if (check(call(fd, nextId++, "preview.start",
                   boost::json::object{{"width", 640}, {"height", 360}, {"fps", 60}}, &reply) &&
                  reply.as_object().contains("result"),
              "preview.start succeeded")) {
        const boost::json::object& result = reply.at("result").as_object();
        gConsumer.name = std::string(result.at("name").as_string());
        gConsumer.slotCount = result.at("slotCount").to_number<std::uint64_t>();
        gConsumer.slotStride = result.at("bytesPerSlot").to_number<std::uint64_t>();
    }

    const std::uint64_t redRevision = presentAndWait(fd, nextId, solidScene("A", kRed, "RED"));
    const std::uint64_t greenRevision = prepareScene(fd, nextId, solidScene("B", kGreen, "GREEN"));
    const std::uint64_t blueRevision = prepareScene(fd, nextId, solidScene("C", kBlue, "BLUE"));
    check(redRevision != 0 && greenRevision != 0 && blueRevision != 0,
          "one scene on air and two prepared off it");
    pump(fd, 700);  // the mirror's own frame

    testOffAirRender(nextId, fd, redRevision, blueRevision);
    testOneFramePerRequest(nextId, fd, greenRevision, blueRevision);
    testRefusals(nextId, fd, greenRevision);
    testAudienceUnaffected(nextId, fd, redRevision, greenRevision, blueRevision);
    testStandDown(nextId, fd, greenRevision);

    close(fd);
    kill(worker, SIGKILL);
    waitpid(worker, nullptr, 0);
    if (gConsumer.map != nullptr) {
        ::munmap(gConsumer.map, gConsumer.slotStride * gConsumer.slotCount + 64);
        gConsumer.map = nullptr;
    }
    if (gConsumer.fd >= 0) {
        ::close(gConsumer.fd);
        gConsumer.fd = -1;
    }
    gConsumer.name.clear();
    gConsumer.delivered.clear();

    // ------------------------------------------------------------------
    // Phase two: the same worker with an expensive draw armed, which is how
    // the superseded frame is made to exist.
    // ------------------------------------------------------------------
    ::unlink(gSocket.c_str());
    worker = spawnWorker(true);
    fd = connectRetry();
    if (!check(fd >= 0, "connected to a worker with an expensive preview draw armed")) {
        kill(worker, SIGKILL);
        waitpid(worker, nullptr, 0);
        return gFailures == 0 ? 1 : 0;
    }
    nextId = 1;
    if (!call(fd, nextId++, "hello", proto::helloParams("select16-test", 0, 1), &reply) ||
        !reply.as_object().contains("result")) {
        check(false, "handshake with the armed worker");
    } else if (check(call(fd, nextId++, "preview.start",
                          boost::json::object{{"width", 320}, {"height", 180}, {"fps", 60}},
                          &reply) &&
                         reply.as_object().contains("result"),
                     "preview.start succeeded on the armed worker")) {
        const boost::json::object& result = reply.at("result").as_object();
        gConsumer.name = std::string(result.at("name").as_string());
        gConsumer.slotCount = result.at("slotCount").to_number<std::uint64_t>();
        gConsumer.slotStride = result.at("bytesPerSlot").to_number<std::uint64_t>();
    }
    const std::uint64_t armedGreen = prepareScene(fd, nextId, solidScene("A", kGreen, "GREEN"));
    const std::uint64_t armedBlue = prepareScene(fd, nextId, solidScene("B", kBlue, "BLUE"));
    check(armedGreen != 0 && armedBlue != 0, "two scenes prepared on the armed worker");
    testSupersededDrawIsDestroyed(nextId, fd, armedGreen, armedBlue);

    close(fd);
    kill(worker, SIGKILL);
    waitpid(worker, nullptr, 0);
    ::unlink(gSocket.c_str());

    if (gFailures != 0) {
        std::fprintf(stderr, "preview_selection_tests: %d failure(s)\n", gFailures);
        return 1;
    }
    std::printf("ok: an off-air edit is drawn by the worker into the same stream, one frame "
                "per accepted request, and a superseded one is destroyed rather than shown\n");
    return 0;
}
