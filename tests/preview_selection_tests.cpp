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
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
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
    std::uint64_t width = 0;     // notification field
    std::uint64_t height = 0;    // notification field
    std::uint64_t rowBytes = 0;  // notification field
    std::uint64_t headTarget = 0;  // read out of the shm slot header
    std::uint64_t headRequest = 0;
    std::uint64_t headWidth = 0;   // ditto
    std::uint64_t headHeight = 0;
    std::uint64_t headRowBytes = 0;
    bool geometryAgrees = false;  // header and notification say one size
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

    // A frame's own size, never the stream's (task 6.16b): the copy below is
    // bounded by the notification's numbers, so it must ALSO be bounded by
    // the slot they live in, and the header is the producer's account of the
    // same bytes. All three have to agree before a pixel is trusted.
    const bool extentSane = rowBytes == width * 4 && width > 0 && height > 0 &&
                            rowBytes * height <=
                                gConsumer.slotStride - sizeof(proto::PreviewSlotHeader);
    if (gConsumer.map != nullptr && slot < gConsumer.slotCount && extentSane) {
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
            const std::uint64_t headWidth = header->width;
            const std::uint64_t headHeight = header->height;
            const std::uint64_t headRowBytes = header->rowBytes;
            const std::uint64_t after =
                std::atomic_ref<const std::uint64_t>(header->seq).load();
            if (after == before) {  // the producer never touched it mid-copy
                delivery.headTarget = headTarget;
                delivery.headRequest = headRequest;
                delivery.headWidth = headWidth;
                delivery.headHeight = headHeight;
                delivery.headRowBytes = headRowBytes;
                delivery.width = width;
                delivery.height = height;
                delivery.rowBytes = rowBytes;
                delivery.geometryAgrees = headWidth == width && headHeight == height &&
                                         headRowBytes == rowBytes;
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

// status.preview.thumbnail within it (task 6.16b).
boost::json::object thumbnailStatus(int fd, std::int64_t& nextId) {
    const boost::json::object preview = previewStatus(fd, nextId);
    const auto found = preview.find("thumbnail");
    return found == preview.end() || !found->value().is_object() ? boost::json::object{}
                                                                : found->value().as_object();
}

// status.preview.thumbnail.images: the list's OWN decoded-picture pool, whose
// budget differs from status.scene.images' by design.
boost::json::object thumbnailImages(int fd, std::int64_t& nextId) {
    const boost::json::object block = thumbnailStatus(fd, nextId);
    const auto found = block.find("images");
    return found == block.end() || !found->value().is_object() ? boost::json::object{}
                                                              : found->value().as_object();
}

std::vector<Delivery> thumbnailDeliveries() {
    std::vector<Delivery> out;
    for (const auto& delivery : deliveries()) {
        if (delivery.target == proto::kPreviewTargetThumbnail) {
            out.push_back(delivery);
        }
    }
    return out;
}

std::size_t thumbnailDeliveriesFor(std::uint32_t request) {
    std::size_t count = 0;
    for (const auto& delivery : thumbnailDeliveries()) {
        if (delivery.request == request) {
            ++count;
        }
    }
    return count;
}

// One ask, and everything the answer can say: a refusal, or the id each
// accepted slide's frames will carry, or the per-entry refusals of a batch
// that was mostly fine.
struct ThumbnailAck {
    bool ok = false;
    std::string error;
    std::vector<std::pair<std::string, std::uint32_t>> accepted;
    std::vector<std::pair<std::string, std::string>> refused;
};

ThumbnailAck askThumbnails(int fd, std::int64_t& nextId, const boost::json::array& scenes,
                           std::uint32_t width, std::uint32_t height, std::uint32_t request) {
    ThumbnailAck ack;
    boost::json::value reply;
    if (!call(fd, nextId++, "preview.thumbnails",
              boost::json::object{{"scenes", scenes},
                                  {"width", width},
                                  {"height", height},
                                  {"request", request}},
              &reply)) {
        ack.error = "the call did not complete";
        return ack;
    }
    const auto error = reply.as_object().find("error");
    if (error != reply.as_object().end()) {
        const boost::json::value& given = error->value();
        if (given.is_object()) {
            const auto message = given.as_object().find("message");
            ack.error = message != given.as_object().end() && message->value().is_string()
                            ? std::string(message->value().as_string())
                            : "a refusal with no message";
        } else {
            ack.error = "a refusal that is not an object";
        }
        return ack;
    }
    const auto result = reply.as_object().find("result");
    if (result == reply.as_object().end() || !result->value().is_object()) {
        ack.error = "an accepted ask answered with no result";
        return ack;
    }
    const boost::json::object& body = result->value().as_object();
    const auto frames = body.find("frames");
    if (frames != body.end() && frames->value().is_array()) {
        for (const boost::json::value& item : frames->value().as_array()) {
            if (!item.is_object()) {
                continue;
            }
            const boost::json::object& entry = item.as_object();
            const auto key = entry.find("key");
            const auto id = entry.find("id");
            if (key != entry.end() && key->value().is_string() && id != entry.end()) {
                ack.accepted.emplace_back(std::string(key->value().as_string()),
                                          static_cast<std::uint32_t>(numberValue(id->value())));
            }
        }
    }
    const auto refusals = body.find("refused");
    if (refusals != body.end() && refusals->value().is_array()) {
        for (const boost::json::value& item : refusals->value().as_array()) {
            if (!item.is_object()) {
                continue;
            }
            const boost::json::object& entry = item.as_object();
            const auto key = entry.find("key");
            const auto why = entry.find("why");
            ack.refused.emplace_back(
                key != entry.end() && key->value().is_string()
                    ? std::string(key->value().as_string())
                    : std::string(),
                why != entry.end() && why->value().is_string()
                    ? std::string(why->value().as_string())
                    : std::string());
        }
    }
    ack.ok = true;
    return ack;
}

std::uint32_t acceptedId(const ThumbnailAck& ack, const char* key) {
    for (const std::pair<std::string, std::uint32_t>& entry : ack.accepted) {
        if (entry.first == key) {
            return entry.second;
        }
    }
    return 0;
}

bool askNames(const ThumbnailAck& ack, const char* key) {
    for (const std::pair<std::string, std::uint32_t>& entry : ack.accepted) {
        if (entry.first == key) {
            return true;
        }
    }
    return false;
}

bool refusalNames(const ThumbnailAck& ack, const char* key) {
    for (const std::pair<std::string, std::string>& entry : ack.refused) {
        if (entry.first == key) {
            return true;
        }
    }
    return false;
}

// The newest row of the list that answers `request` and carries slide `id`.
bool waitForThumbnail(int fd, std::uint32_t request, std::uint32_t id, int milliseconds,
                      Delivery* out) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < until) {
        for (const auto& delivery : thumbnailDeliveries()) {
            if (delivery.request == request && delivery.revision == id) {
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

// A solid PNG at an arbitrary size (ffmpeg, as in the live media fixtures).
// Task 6.16b's decode pump needs a REAL file: a slide with no background
// picture is settled without a codec ever being opened, which is precisely the
// case that proves nothing about decoding one.
bool writePng(const std::string& path, const char* colour, int width, int height) {
    const std::string command =
        "ffmpeg -v error -y -f lavfi -i color=c=" + std::string(colour) + ":s=" +
        std::to_string(width) + "x" + std::to_string(height) + ":r=1 -frames:v 1 '" + path +
        "' >/dev/null 2>&1";
    return std::system(command.c_str()) == 0 && ::access(path.c_str(), R_OK) == 0;
}

// A slide whose background is a PICTURE over a colour fill. The two colours are
// chosen to be far apart on purpose: a thumbnail that arrives in the FILL's
// colour is one whose background was never decoded, and the difference between
// "the worker decoded it" and "the worker drew what it always draws" is the
// entire claim of the list's decode path.
boost::json::object stillScene(const char* slideId, const std::string& path,
                               std::uint32_t fill) {
    return boost::json::object{
        {"scene", 3},
        {"showId", "select16"},
        {"slideId", slideId},
        {"backgroundColor", fill},
        {"background", boost::json::object{{"image", true},
                                           {"fit", "cover"},
                                           {"assetId", std::string("still-") + slideId},
                                           {"assetPath", path}}},
        {"items", boost::json::array{}},
    };
}

// One entry of a slide-list batch: the slide's own identity, and the compiled
// scene the worker should picture it with.
boost::json::object listEntry(const char* key, const boost::json::object& scene) {
    return boost::json::object{{"key", key}, {"scene", scene}};
}

// A solid-colour scene with one word.
boost::json::object solidScene(const char* slideId, std::uint32_t color, const char* word) {
    return boost::json::object{
        {"scene", 3},
        {"showId", "select16"},
        {"slideId", slideId},
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

// --- the slide list's rows (task 6.16b, review R16) ----------------------
//
// The second half of R16: the slide list drew its own picture of a slide, in
// the frontend, by its own rules, while the worker drew the audience something
// else. A row that disagrees with the panel is worse than an empty row, so the
// list asks the WORKER for its own composition of a BAND of slides and receives
// it as frames of a third target on the stream it already had.
//
// The three things that make that safe are the three things checked here:
//
//   - identity: a row arrives with the id the ack gave ITS slide, on a target
//     that says "thumbnail", at the size the list asked for, and shows that
//     slide and not the one on air. Without this the feature is a slideshow of
//     whoever happened to be drawn last.
//   - cancellation: a batch REPLACES the one before it, so scrolling is not a
//     queue. A replaced generation stops being owed, and an empty batch is a
//     stand-down that still needs a generation of its own.
//   - isolation: the list draws with a compositor of its own, decodes into a
//     pool of its own, and leaves the audience's scene, presented-frame
//     counter, prepared store and decoded-picture budget exactly where they
//     were. That is the difference between a preview and a second output.
void testSlideList(std::int64_t& nextId, int fd, std::uint64_t blueRevision) {
    // ------------------------------------------------------------------
    // 1. The block exists at zero, with a picture pool of its own.
    // ------------------------------------------------------------------
    const boost::json::object idle = thumbnailStatus(fd, nextId);
    if (!check(!idle.empty(), "output.status.preview reports `thumbnail`")) {
        return;
    }
    check(fieldValue(idle, "active") == 0 && fieldValue(idle, "queued") == 0 &&
              fieldValue(idle, "published") == 0 && fieldValue(idle, "dropped") == 0,
          "and it says the slide list has never been asked for");
    check(idle.contains("images") && idle.at("images").is_object(),
          "and it reports the list\'s decoded pictures as its OWN counters");

    // ------------------------------------------------------------------
    // 2. The refusals, every one of them before anything is owed.
    // ------------------------------------------------------------------
    // Refused on their NUMBERS, before a scene is compiled or a slot claimed:
    // this is a command an operator\'s scroll wheel fires, so the cost of a bad
    // ask has to be a comparison. The generation does not advance on a refusal,
    // so all three below share request 1 — which is itself the check that a
    // refusal reserves nothing.
    const boost::json::array oneRow{listEntry("solo", solidScene("L0", kGreen, "ONE"))};
    {
        const ThumbnailAck tooBig = askThumbnails(fd, nextId, oneRow, 512, 512, 1);
        check(!tooBig.ok, "a 512x512 row is refused on this 640x360 stream");
        check(tooBig.error.find("921664") != std::string::npos,
              ("and the refusal names the slot it could not fit: " + tooBig.error).c_str());

        const ThumbnailAck tooSmall = askThumbnails(fd, nextId, oneRow, 16, 16, 1);
        check(!tooSmall.ok, "a 16x16 row is refused as well");
        check(tooSmall.error.find("32..512") != std::string::npos,
              ("and the refusal names the box it was judged against: " + tooSmall.error).c_str());

        boost::json::array crowded;
        for (int i = 0; i < 33; ++i) {
            crowded.push_back(listEntry("row", solidScene("L", kRed, "WORD")));
        }
        const ThumbnailAck tooMany = askThumbnails(fd, nextId, crowded, 160, 90, 1);
        check(!tooMany.ok, "thirty-three scenes are refused");
        check(tooMany.error.find("32") != std::string::npos &&
                  tooMany.error.find("33") != std::string::npos,
              ("and the refusal says 32 against the 33 that were asked: " + tooMany.error).c_str());

        const boost::json::object nothing = thumbnailStatus(fd, nextId);
        check(fieldValue(nothing, "queued") == 0 && fieldValue(nothing, "active") == 0,
              "and three refusals reserved nothing at all");
    }

    // ------------------------------------------------------------------
    // 3. A band, with one bad row in it.
    // ------------------------------------------------------------------
    const boost::json::array band{
        listEntry("row-red", solidScene("L1", kRed, "ONE")),
        listEntry("row-broken", boost::json::object{{"scene", 99}}),
        listEntry("row-blue", solidScene("L2", kBlue, "TWO")),
        listEntry("row-green", solidScene("L3", kGreen, "THREE"))};

    // What the show looks like BEFORE, so "unaffected" is a comparison and not
    // an assumption. Pictures, prepared store, presented frames, active scene.
    const boost::json::object before = status(fd, nextId);
    const boost::json::object beforeScene =
        before.contains("scene") && before.at("scene").is_object()
            ? before.at("scene").as_object()
            : boost::json::object{};
    const boost::json::object beforeImages =
        beforeScene.contains("images") && beforeScene.at("images").is_object()
            ? beforeScene.at("images").as_object()
            : boost::json::object{};
    const std::uint64_t showPixelsBefore = fieldValue(beforeImages, "decodedBytes");
    const std::uint64_t preparedBefore = fieldValue(beforeScene, "prepared");
    const std::uint64_t presentedBefore = fieldValue(before, "frames");

    const ThumbnailAck ack = askThumbnails(fd, nextId, band, 160, 90, 1);
    if (!check(ack.ok, ("a band of slides is accepted: " + ack.error).c_str())) {
        return;
    }
    check(ack.accepted.size() == 3,
          "three of its four rows are accepted, and the malformed one costs only itself");
    check(askNames(ack, "row-red") && askNames(ack, "row-blue") && askNames(ack, "row-green"),
          "every well-formed row keeps its own identity in the ack");
    check(refusalNames(ack, "row-broken"), "and the broken one is refused by name, not silently");
    std::uint32_t redId = 0;
    std::uint32_t blueId = 0;
    std::uint32_t greenId = 0;
    {
        std::uint32_t seen[3] = {acceptedId(ack, "row-red"), acceptedId(ack, "row-blue"),
                                 acceptedId(ack, "row-green")};
        redId = seen[0];
        blueId = seen[1];
        greenId = seen[2];
        check(seen[0] != 0 && seen[1] != 0 && seen[2] != 0,
              "no row is handed the id zero, which means \"no slide\"");
        check(seen[0] != seen[1] && seen[1] != seen[2] && seen[0] != seen[2],
              "and the three rows have three different ids: one picture cannot answer for all "
              "of them");
    }

    const boost::json::object queued = thumbnailStatus(fd, nextId);
    check(fieldValue(queued, "active") == 1, "status says the list is owed a band");
    check(fieldValue(queued, "request") == 1 && fieldValue(queued, "width") == 160 &&
              fieldValue(queued, "height") == 90,
          "status names the accepted generation and the size it will be drawn at");

    // ------------------------------------------------------------------
    // 4. The rows arrive, and each one is its own slide.
    // ------------------------------------------------------------------
    struct Row {
        const char* key;
        std::uint32_t id;
    };
    int rowsSeen = 0;
    for (const Row row : {Row{"red", redId}, Row{"blue", blueId}, Row{"green", greenId}}) {
        Delivery frame;
        const std::string label = std::string("the ") + row.key + " row";
        if (!check(waitForThumbnail(fd, 1, row.id, 3000, &frame),
                   (label + " reaches the consumer").c_str())) {
            continue;
        }
        ++rowsSeen;
        check(frame.target == proto::kPreviewTargetThumbnail &&
                  frame.headTarget == proto::kPreviewTargetThumbnail,
              (label + " names itself a thumbnail in the notification AND in the slot header")
                  .c_str());
        check(frame.request == 1, (label + " names the batch generation it answers").c_str());
        check(frame.width == 160 && frame.height == 90 && frame.rowBytes == 160 * 4,
              (label + " arrives at the asked size, with rows packed").c_str());
        check(frame.geometryAgrees,
              (label + ": the header\'s own size agrees with the notification that carried it, "
                      "so a consumer bounded by either copies the same bytes").c_str());
    }
    if (rowsSeen == 3) {
        // The identity claim, which is the whole feature: three rows of one
        // band, three different slides. A target that drew the same picture
        // thirty times would pass every counter above.
        Delivery red, blue, green;
        waitForThumbnail(fd, 1, redId, 500, &red);
        waitForThumbnail(fd, 1, blueId, 500, &blue);
        waitForThumbnail(fd, 1, greenId, 500, &green);
        check(red.red > red.green + 80.0 && red.red > red.blue + 80.0,
              "the first row is the RED slide");
        check(blue.blue > blue.red + 80.0 && blue.blue > blue.green + 80.0,
              "the second is the BLUE slide, not the first row painted again");
        check(green.green > green.red + 80.0 && green.green > green.blue + 80.0,
              "and the third is the GREEN one: a row is the slide the ack named");
    }

    // ------------------------------------------------------------------
    // 5. The audience, the show, and the two pools.
    // ------------------------------------------------------------------
    pump(fd, 400);
    const boost::json::object after = status(fd, nextId);
    const boost::json::object afterScene =
        after.contains("scene") && after.at("scene").is_object() ? after.at("scene").as_object()
                                                                : boost::json::object{};
    const boost::json::object afterImages =
        afterScene.contains("images") && afterScene.at("images").is_object()
            ? afterScene.at("images").as_object()
            : boost::json::object{};
    check(fieldValue(afterScene, "active") == blueRevision,
          "the audience still has the scene it had: a list of previews is not a present");
    check(fieldValue(afterScene, "prepared") == preparedBefore,
          "the prepared store is unmoved: a thumbnail scene never becomes content somebody "
          "can put on air");
    check(fieldValue(afterImages, "decodedBytes") == showPixelsBefore,
          "the SHOW\'s decoded-picture budget is exactly where it was: nothing a row needed is "
          "charged to the panel");
    check(fieldValue(after, "frames") == presentedBefore,
          "and the audience\'s presented-frame counter is flat across a whole band");
    {
        // The tag has to be honest in both directions: no row may arrive
        // wearing the audience\'s target, because a frontend routes on it.
        int impostors = 0;
        for (const auto& delivery : deliveries()) {
            if (delivery.target == proto::kPreviewTargetAudience &&
                (delivery.width != 640 || delivery.height != 360)) {
                ++impostors;
            }
        }
        check(impostors == 0, "no row ever arrived tagged for the audience");
    }

    // ------------------------------------------------------------------
    // 6. A background that has to be DECODED for a row.
    // ------------------------------------------------------------------
    // The rows above are flat colour: their slides have no asset, so the pump
    // settles them without opening a codec and the decode path has proved
    // nothing. A real document\'s slides have backdrops, and the claim about
    // them is twofold — the row shows the BACKDROP (so the command thread\'s
    // scaled decode actually fed the list\'s cache), and it costs the list tens
    // of kilobytes rather than the 5.8 MB the file decodes to at full size
    // (R12\'s finding, re-fought at thumbnail scale).
    const std::string dir = "/tmp/opencode/preview-thumbnail-fixtures-" +
                            std::to_string(static_cast<long>(::getpid()));
    ::mkdir(dir.c_str(), 0755);
    const std::string backdrop = dir + "/backdrop.png";
    if (!check(writePng(backdrop, "0xF0E030", 1600, 900),
               "a 1600x900 PNG backdrop is staged for a row")) {
        return;
    }
    const ThumbnailAck stills =
        askThumbnails(fd, nextId,
                      boost::json::array{listEntry("row-still", stillScene("L4", backdrop, kRed))},
                      160, 90, 2);
    check(stills.ok && stills.accepted.size() == 1, "a backdrop slide is accepted as a row");
    Delivery row;
    if (check(waitForThumbnail(fd, 2, acceptedId(stills, "row-still"), 4000, &row),
              "the backdrop row arrives")) {
        // Yellow picture over a red fill. If the cache had missed, the draw
        // would show the fill: the green in this frame can only have come
        // from the decoded PNG.
        const std::string seen = "r" + std::to_string(static_cast<int>(row.red)) + " g" +
                                 std::to_string(static_cast<int>(row.green)) + " b" +
                                 std::to_string(static_cast<int>(row.blue));
        check(row.green > 150.0 && row.blue < 120.0,
              ("the row shows its BACKDROP, not the colour behind it: the scaled decode fed "
               "the list's own cache, measured " + seen)
                  .c_str());
    }
    const boost::json::object listImages = thumbnailImages(fd, nextId);
    const std::uint64_t rowBytes = fieldValue(listImages, "decodedBytes");
    check(rowBytes > 0, "and the list\'s pool says it is holding a picture");
    check(rowBytes <= 64u * 1024u,
          "that picture costs the list tens of kilobytes: 160x90 is 57,600 bytes, where the "
          "same file at full size is 5,760,000");
    check(fieldValue(listImages, "budget") == 12u * 1024u * 1024u,
          "the list\'s ceiling is its own 12 MiB, not the show\'s 192 MiB — the wire says so");
    const boost::json::object showAgain = status(fd, nextId);
    const boost::json::object showScene = showAgain.at("scene").as_object();
    check(fieldValue(showScene.at("images").as_object(), "decodedBytes") == showPixelsBefore,
          "and the show has still not decoded a byte of it");

    // Asking for the same row again must not grow that pool: the cache is
    // keyed on the file, and a scroll back over a document is the normal case.
    const ThumbnailAck again =
        askThumbnails(fd, nextId,
                      boost::json::array{listEntry("row-still", stillScene("L4", backdrop, kRed))},
                      160, 90, 3);
    check(again.ok, "the same backdrop slide can be asked for again");
    pump(fd, 400);
    const boost::json::object poolAgain = thumbnailImages(fd, nextId);
    check(fieldValue(poolAgain, "decodedBytes") == rowBytes &&
              fieldValue(poolAgain, "entries") == fieldValue(listImages, "entries"),
          "and re-asking a slide costs no further picture memory: a list plateaus, it does not "
          "remember every scroll");

    // ------------------------------------------------------------------
    // 7. Replacement: a newer band cancels the older one\'s debt.
    // ------------------------------------------------------------------
    boost::json::array wide;
    for (int i = 0; i < 20; ++i) {
        wide.push_back(listEntry(("wide-" + std::to_string(i)).c_str(),
                                 solidScene(("W" + std::to_string(i)).c_str(), kBlue, "SCROLL")));
    }
    const ThumbnailAck scrolled = askThumbnails(fd, nextId, wide, 160, 90, 4);
    check(scrolled.ok && scrolled.accepted.size() == 20, "a wide band of twenty rows is accepted");
    // ... and immediately replaced, which is what an operator\'s flick does.
    const ThumbnailAck narrow = askThumbnails(fd, nextId, oneRow, 160, 90, 5);
    check(narrow.ok && narrow.accepted.size() == 1, "and a single row replaces it wholesale");
    pump(fd, 700);
    const boost::json::object settled = thumbnailStatus(fd, nextId);
    check(fieldValue(settled, "request") == 5, "status names the newest batch as the debt");
    check(fieldValue(settled, "queued") <= 1,
          "and owes at most the rows of THAT batch: twenty slides the operator has scrolled past "
          "are not still queued");
    const std::size_t wideFrames = thumbnailDeliveriesFor(4);
    pump(fd, 400);
    check(thumbnailDeliveriesFor(4) == wideFrames,
          "no further frame carries the replaced generation: cancellation is not just a status "
          "field");
    check(waitForThumbnail(fd, 5, acceptedId(narrow, "solo"), 2000, &row),
          "the row that survived the scroll arrives");

    // ------------------------------------------------------------------
    // 8. Standing down, and the generation rule.
    // ------------------------------------------------------------------
    const ThumbnailAck down = askThumbnails(fd, nextId, boost::json::array{}, 160, 90, 6);
    check(down.ok && down.accepted.empty(), "an empty batch stands the list down");
    pump(fd, 400);
    const boost::json::object stopped = thumbnailStatus(fd, nextId);
    check(fieldValue(stopped, "active") == 0 && fieldValue(stopped, "queued") == 0,
          "status says nothing is owed to the list any more");
    const std::size_t atRest = thumbnailDeliveries().size();
    pump(fd, 500);
    check(thumbnailDeliveries().size() == atRest, "and no further row arrives");

    // A stand-down is a CHANGE, so it consumed generation 6: replaying it is
    // the same bug as replaying an ask — a retry after a dropped reply would
    // resurrect a list the operator has closed.
    const ThumbnailAck replay = askThumbnails(fd, nextId, oneRow, 160, 90, 6);
    check(!replay.ok, "a replayed generation is refused");
    check(replay.error.find("does not follow") != std::string::npos,
          ("and the refusal names both numbers: " + replay.error).c_str());

    // A key too long to be an identity is refused by the handler and ECHOED
    // CLIPPED: R11\'s rule about putting a client\'s own text back on the wire.
    const std::string longKey(200, 'k');
    const ThumbnailAck shouting =
        askThumbnails(fd, nextId,
                      boost::json::array{listEntry(longKey.c_str(),
                                                   solidScene("L5", kRed, "TOO LONG"))},
                      160, 90, 7);
    check(shouting.ok && shouting.accepted.empty() && shouting.refused.size() == 1,
          "a 200-character key is refused, and the batch it travelled in is otherwise fine");
    check(!shouting.refused.empty() && shouting.refused[0].first.size() < longKey.size(),
          "and the echoed key is clipped rather than sent back whole");

    // ------------------------------------------------------------------
    // 9. A stream too small for a batch that stands.
    // ------------------------------------------------------------------
    //
    // A slot's payload area was reserved for the STREAM'S OWN frame size, so a
    // row accepted on a 640x360 stream — 480x480 fills one to the byte — cannot
    // exist on a 320x180 one. Changing the preview size while the slide list is
    // open is an ordinary thing for an operator to do, and the alternative to
    // standing the batch down is not a wrong picture: it is 921,600 bytes of
    // pixels written into a slot reserved for 230,400, which is the worker's
    // memory. Two places check the extent for exactly this reason (the start
    // path and the draw path), and what is under test here is what that pair
    // guarantees rather than either check alone: after the restart the list is
    // stood down and says so, no row of the old band was drawn, and a fresh ask
    // at an extent the new stream carries is accepted from generation 1 — which
    // is the recovery the client makes when it mirrors `active` as false.
    {
        boost::json::array tall;
        for (int i = 0; i < 20; ++i) {
            tall.push_back(listEntry(("tall-" + std::to_string(i)).c_str(),
                                     solidScene(("T" + std::to_string(i)).c_str(), kGreen, "BIG")));
        }
        const ThumbnailAck big = askThumbnails(fd, nextId, tall, 480, 480, 8);
        check(big.ok && big.accepted.size() == 20,
              "a 480x480 band fits a 640x360 stream's slots exactly, and is owed");
        check(fieldValue(thumbnailStatus(fd, nextId), "active") == 1,
              "the list is registered before the restart");

        boost::json::value reply;
        if (!check(call(fd, nextId++, "preview.start",
                        boost::json::object{{"width", 320}, {"height", 180}, {"fps", 60}},
                        &reply) &&
                       reply.as_object().contains("result"),
                   "preview.start replaced the stream while the list owed twenty rows")) {
            std::filesystem::remove_all(dir);
            return;
        }
        // The consumer follows the stream: the mapping it holds is of a segment
        // this worker no longer writes, and its slots are not these slots.
        const boost::json::object& result = reply.at("result").as_object();
        if (gConsumer.map != nullptr) {
            ::munmap(gConsumer.map, static_cast<std::size_t>(gConsumer.slotStride) *
                                        static_cast<std::size_t>(gConsumer.slotCount) + 64);
            gConsumer.map = nullptr;
        }
        if (gConsumer.fd >= 0) {
            ::close(gConsumer.fd);
            gConsumer.fd = -1;
        }
        {
            std::lock_guard<std::mutex> lock(gConsumer.mutex);
            gConsumer.delivered.clear();
        }
        gConsumer.name = std::string(result.at("name").as_string());
        gConsumer.slotCount = result.at("slotCount").to_number<std::uint64_t>();
        gConsumer.slotStride = result.at("bytesPerSlot").to_number<std::uint64_t>();

        pump(fd, 700);  // the new stream's own mirror frame, plus any row that
                        // should NOT have been drawn into it
        const boost::json::object after = thumbnailStatus(fd, nextId);
        check(fieldValue(after, "active") == 0,
              "a batch the new stream cannot hold is stood down, not dropped frame by frame");
        check(fieldValue(after, "queued") == 0 && fieldValue(after, "request") == 0,
              "and the stand-down is honest about itself: no debt, no reserved generation");
        check(thumbnailDeliveries().empty(),
              "and no row of the old band was painted into a slot too small for it");

        const ThumbnailAck smaller =
            askThumbnails(fd, nextId,
                          boost::json::array{listEntry("small", solidScene("T9", kBlue, "NEW"))},
                          64, 36, 1);
        check(smaller.ok && smaller.accepted.size() == 1,
              "a fresh ask at an extent the new stream carries is accepted, counting from 1");
        Delivery shrunk;
        if (check(waitForThumbnail(fd, 1, acceptedId(smaller, "small"), 2000, &shrunk),
                  "and that row arrives on the new stream")) {
            check(shrunk.width == 64 && shrunk.height == 36 && shrunk.rowBytes == 64 * 4,
                  "at the size it was asked for, in the notification and in the slot header");
        }
    }

    // ------------------------------------------------------------------
    // 10. A row that needs TWO pictures: a backdrop AND a foreground
    //     still (task 6.17b).
    // ------------------------------------------------------------------
    // The pump's unit of work is a PATH and its unit of debt is a ROW: a
    // slide with two pictures owes ONE frame, and that frame is owed only
    // when both paths have settled. Marking a job drawn-ready on its first
    // decode is the bug this section exists to catch, and it is invisible in
    // every fixture above, where a row names at most one file.
    //
    // It runs last because the extent below differs from the 64x36 ask that
    // ended section 9, and a changed extent builds the list a fresh
    // compositor: the pool here is the row's own, which is what makes its
    // entry count a claim about this batch.
    const std::string foreground = dir + "/plate.png";
    if (!check(writePng(foreground, "0x2040F0", 800, 450),
               "a blue foreground PNG is staged for a two-picture row")) {
        return;
    }
    boost::json::object both = stillScene("L6", backdrop, kRed);
    both["items"].as_array().push_back(boost::json::object{
        {"kind", "image"},
        {"assetId", "fg-asset"},
        {"assetPath", foreground},
        {"x", 0.0},
        {"y", 0.0},
        {"width", 960.0},
        {"height", 1080.0},
    });
    const ThumbnailAck pair =
        askThumbnails(fd, nextId, boost::json::array{listEntry("row-both", both)}, 160, 90, 2);
    check(pair.ok && pair.accepted.size() == 1,
          "the two-picture slide is accepted as one row on the new stream");
    Delivery pairRow;
    if (check(waitForThumbnail(fd, 2, acceptedId(pair, "row-both"), 4000, &pairRow),
              "and its row arrives - one frame for two decodes")) {
        // Left half the blue picture, right half the yellow backdrop, over a
        // red fill neither half leaves visible. A row drawn the moment its
        // FIRST path settled would be pure backdrop yellow: blue 48, red 240.
        const std::string seen = "r" + std::to_string(static_cast<int>(pairRow.red)) + " g" +
                                 std::to_string(static_cast<int>(pairRow.green)) + " b" +
                                 std::to_string(static_cast<int>(pairRow.blue));
        std::printf("measured: a row of two pictures, mean channels %s\n", seen.c_str());
        check(pairRow.blue > 100.0 && pairRow.red < 200.0,
              ("the row shows BOTH pictures, the foreground over half of it, measured " + seen)
                  .c_str());
    }
    {
        const boost::json::object pool = thumbnailImages(fd, nextId);
        check(fieldValue(pool, "entries") == 2,
              "the list's pool holds exactly two pictures: the backdrop and this row's own");
        check(fieldValue(pool, "decodedBytes") > 0 &&
                  fieldValue(pool, "decodedBytes") <= 2u * 57600u,
              "both accounted at thumbnail scale, neither at the file's own size");
    }
    // Asking again at the SAME extent keeps the list's compositor, so both
    // paths are answered from its cache: the row arrives with no further
    // picture memory spent. This is the free-settle leg of the pump, which a
    // single-path row can only ever half-prove.
    const ThumbnailAck pairAgain =
        askThumbnails(fd, nextId, boost::json::array{listEntry("row-both", both)}, 160, 90, 3);
    check(pairAgain.ok && pairAgain.accepted.size() == 1, "the two-picture row can be asked again");
    Delivery againRow;
    if (check(waitForThumbnail(fd, 3, acceptedId(pairAgain, "row-both"), 4000, &againRow),
              "and it arrives again, settled from the cache")) {
        const boost::json::object pool = thumbnailImages(fd, nextId);
        check(fieldValue(pool, "entries") == 2,
              "and the pool holds the same two pictures: a scroll does not re-decode a slide");
    }

    std::filesystem::remove_all(dir);
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
// --- every box in the off-air frame (task 6.17a2, review R17) ------------
//
// The canvas edits a STACK of boxes now, and the picture under its handles
// is this scene. One claim, measured on real pixels: a selected scene with
// two boxes draws the SECOND box's words into its off-air frame. A compiler
// or renderer that dropped a trailing item - or a selection path that
// reconstructed a single-box scene from the slide's first box, the bug the
// stacked authoring is replacing - would answer with the base box alone.
// The metric is luminance ink on black, and the two frames differ in
// exactly one thing: the stacked scene has one more box. This suite is the
// display-labelled one, running where fonts exist, because the words are
// what is being weighed.
void testStackedBoxesOffAir(std::int64_t& nextId, int fd) {
    boost::json::object stacked = solidScene("STACKED16", 0x000000u, "EVERY BOX");
    boost::json::object strip;
    strip["kind"] = "text";
    strip["lines"] = boost::json::array{"ALSO ON AIR"};
    strip["color"] = 0xFFFFFFu;
    strip["fontFamily"] = "";
    strip["fontSize"] = 90.0;
    strip["x"] = 960.0;
    strip["y"] = 700.0;
    strip["width"] = 800.0;
    strip["height"] = 280.0;
    strip["align"] = "center";
    strip["lineSpacing"] = 1.25;
    stacked["items"].as_array().push_back(strip);
    const std::uint64_t leanRevision = prepareScene(fd, nextId,
                                                    solidScene("STACKED16", 0x000000u,
                                                               "EVERY BOX"));
    const std::uint64_t stackedRevision = prepareScene(fd, nextId, stacked);
    if (!check(leanRevision != 0 && stackedRevision != 0,
               "both the one-box and the two-box scene prepare")) {
        return;
    }

    // Requests continue the session's numbering, whatever the earlier
    // scenarios spent - the worker's gate is monotonic, not zero-based.
    const boost::json::object settled = selectionStatus(fd, nextId);
    std::uint32_t request = static_cast<std::uint32_t>(fieldValue(settled, "request")) + 1;

    const std::string leanRefusal = selectScene(fd, nextId, leanRevision, request);
    check(leanRefusal.empty(), ("the one-box scene is accepted: " + leanRefusal).c_str());
    Delivery lean;
    if (!check(waitForSelection(fd, request, 3000, &lean), "its off-air frame arrives")) {
        return;
    }
    ++request;
    const std::string stackedRefusal = selectScene(fd, nextId, stackedRevision, request);
    check(stackedRefusal.empty(),
          ("the stacked scene is accepted: " + stackedRefusal).c_str());
    Delivery full;
    if (!check(waitForSelection(fd, request, 3000, &full), "and so does the stacked frame")) {
        return;
    }
    check(full.target == proto::kPreviewTargetSelection && full.request == request,
          "the stacked frame is the selection's answer to this ask");

    const double leanInk = (lean.red + lean.green + lean.blue) / 3.0;
    const double fullInk = (full.red + full.green + full.blue) / 3.0;
    std::printf("measured: off-air frame mean luminance one box %.2f, stacked %.2f\n",
                leanInk, fullInk);
    // Two words at 90-120px on black put several points of mean luminance on
    // the frame; the strip alone is worth far more than the 0.5 floor, and
    // a dropped second box cannot reach it.
    check(fullInk > leanInk + 0.5,
          ("the SECOND box's words are in the off-air picture (mean " +
           std::to_string(leanInk) + " -> " + std::to_string(fullInk) + ")").c_str());
}

// --- a foreground picture on an off-air slide -------------------------
//
// Task 6.17b's other half of the same promise: the panel's draw is the
// audience's composition, so a STACKED PICTURE has to reach it too. Two
// frames are measured against each other, both black except for what the
// worker decoded: the slide with a yellow plate over its left half, and the
// same slide with the item removed. A prepare that never decoded the item, or
// a draw that never looked it up, leaves the two frames identical - and the
// measurement says so in numbers.
void testForegroundImageOffAir(std::int64_t& nextId, int fd) {
    const std::string dir = "/tmp/opencode/preview-foreground-fixtures-" +
                            std::to_string(static_cast<long>(::getpid()));
    ::mkdir(dir.c_str(), 0755);
    const std::string plate = dir + "/plate.png";
    if (!check(writePng(plate, "0xF0E030", 800, 450),
               "a yellow foreground PNG is staged for the off-air draw")) {
        return;
    }
    const auto pictureScene = [&plate](bool withPicture) {
        boost::json::array items;
        if (withPicture) {
            items.push_back(boost::json::object{{"kind", "image"},
                                                {"assetId", "fg-asset"},
                                                {"assetPath", plate},
                                                {"x", 0.0},
                                                {"y", 0.0},
                                                {"width", 960.0},
                                                {"height", 1080.0}});
        }
        return boost::json::object{
            {"scene", 3},
            {"showId", "select16"},
            {"slideId", withPicture ? "FG1" : "FG0"},
            {"backgroundColor", 0x000000u},
            {"background",
             boost::json::object{{"image", false}, {"fit", "cover"}, {"assetId", ""}, {"assetPath", ""}}},
            {"items", std::move(items)},
        };
    };
    // The show's own picture accounting: a prepared scene's FOREGROUND
    // picture has to enter the pin union, or the housekeeping pass can
    // evict it while the scene is still in the store — the panel would then
    // draw a slide with a hole in it that a re-present would not fix.
    // Measured as a DELTA across one prepare because the prepared store
    // holds everything earlier scenarios made: the claim is about THIS
    // picture, and a delta is the only number that says so.
    const auto showImages = [fd, &nextId]() {
        const boost::json::object state = status(fd, nextId);
        if (!state.contains("scene") || !state.at("scene").is_object()) {
            return boost::json::object{};
        }
        const boost::json::object& sceneState = state.at("scene").as_object();
        if (!sceneState.contains("images") || !sceneState.at("images").is_object()) {
            return boost::json::object{};
        }
        return sceneState.at("images").as_object();
    };
    const double pinsBeforePlate = fieldValue(showImages(), "pins");
    const std::uint64_t bare = prepareScene(fd, nextId, pictureScene(false));
    const std::uint64_t plated = prepareScene(fd, nextId, pictureScene(true));
    const double pinsAfterPlate = fieldValue(showImages(), "pins");
    std::printf("measured: prepared-store pins around a foreground prepare — %.0f -> %.0f\n",
                pinsBeforePlate, pinsAfterPlate);
    check(pinsAfterPlate == pinsBeforePlate + 1.0,
          "the stack's picture is pinned with its scene: one held slide, one reference "
          "(a union that only walked backgrounds reads 0 here)");
    if (!check(bare != 0 && plated != 0, "the bare and plated off-air slides prepare")) {
        std::filesystem::remove_all(dir);
        return;
    }
    // Requests continue the session's numbering, whatever earlier scenarios
    // spent: the worker's gate is monotonic, not zero-based.
    const boost::json::object settled = selectionStatus(fd, nextId);
    std::uint32_t request = static_cast<std::uint32_t>(fieldValue(settled, "request")) + 1;

    const std::string bareAsk = selectScene(fd, nextId, bare, request);
    check(bareAsk.empty(), ("the bare slide is selected: " + bareAsk).c_str());
    Delivery black;
    if (!check(waitForSelection(fd, request, 3000, &black), "and its frame arrives")) {
        std::filesystem::remove_all(dir);
        return;
    }
    ++request;
    const std::string platedAsk = selectScene(fd, nextId, plated, request);
    check(platedAsk.empty(), ("the plated slide is selected: " + platedAsk).c_str());
    Delivery yellow;
    if (!check(waitForSelection(fd, request, 3000, &yellow),
               "and the frame with the foreground picture arrives")) {
        std::filesystem::remove_all(dir);
        return;
    }
    check(yellow.target == proto::kPreviewTargetSelection && yellow.request == request,
          "the plated frame answers this ask, on the selection target");
    const double blackInk = (black.red + black.green + black.blue) / 3.0;
    const double yellowInk = (yellow.red + yellow.green + yellow.blue) / 3.0;
    std::printf("measured: off-air foreground picture — no item mean %.2f, with item %.2f\n",
                blackInk, yellowInk);
    // A yellow plate over half a black canvas is worth ~120 of mean red. The
    // floor is generous by a factor of twenty; the point is the direction and
    // the gap, not the exact colour the codec round-trips.
    check(yellowInk > blackInk + 20.0,
          ("the foreground picture is IN the off-air frame (mean " +
           std::to_string(blackInk) + " -> " + std::to_string(yellowInk) + ")").c_str());
    check(yellow.red > yellow.blue + 40.0,
          "and it is the picture's yellow, not a decode that came out grey");
    std::filesystem::remove_all(dir);
}

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
        // And the list's version of the same negotiation: for a worker-drawn
        // slide list the capability IS the whole agreement, so a real worker
        // is pinned naming it here rather than only the scripted peer S15 runs
        // against (task 6.16b). A frontend that guessed instead would show an
        // operator rows of nothing on a worker that cannot draw them.
        const bool hasThumbnails = std::any_of(caps.begin(), caps.end(),
                                               [](const boost::json::value& value) {
                                                   return value.is_string() &&
                                                          value.as_string() ==
                                                              "preview.thumbnail.v1";
                                               });
        check(hasThumbnails, "hello advertises preview.thumbnail.v1");
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
    testSlideList(nextId, fd, blueRevision);
    testStackedBoxesOffAir(nextId, fd);
    testForegroundImageOffAir(nextId, fd);
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
