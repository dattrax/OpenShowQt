/*
 * Task 6.20c: the production proof, one journey, one real worker.
 *
 * The review's completion rule wants "a production
 * author→preview→go→clear→restore→media-swap proof", and the fixtures that
 * came before it each prove one seam (document_present_tests the
 * authoring-and-present hand-off, production_output_tests the production
 * compositor, preview_selection_tests the bounded off-air targets). This
 * fixture is the operator's whole night in one run: a song authored through
 * the very DocumentController the panels bind, an off-air picture of the
 * NEXT slide asked for and seen while the audience still shows the last
 * one, the go, per-layer clears and the restore, and a background asset
 * swapped out from under the still-live words — every hand-off asserted
 * twice, once in the worker's own output.status and once in the pixels of
 * the live preview stream. Where a fixture before this one would accept
 * "the command was acknowledged", this one also demands to have seen it.
 *
 * Needs a display (the worker opens its output window) and ffmpeg (the
 * background stills are real PNGs, decoded by the real image path, never
 * synthesised). Desktop preset, label display. Its own output prints the
 * environment it could NOT exercise — silent journey, no PipeWire stream;
 * whatever device the session's Vulkan loader picked, RADV and llvmpipe
 * indistinguishable from in here — so the honest skips travel with the
 * evidence instead of waiting for the final report.
 */

#include <boost/json.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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

#include <QCoreApplication>
#include <QVariant>

#include "openshow/protocol/preview_wire.h"
#include "openshow/protocol/protocol.h"

#include "DocumentController.h"
#include "SceneEdit.h"

namespace {

namespace proto = openshow::protocol;
using openshow::ui::DocumentController;

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
    "/tmp/openshow-journey-" + std::to_string(getpid()) + ".sock";

pid_t spawnWorker() {
    const pid_t pid = fork();
    if (pid == 0) {
        execl(gWorkerBinary.c_str(), "openshow-worker", "--socket", gSocket.c_str(),
              static_cast<char*>(nullptr));
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

void sendRaw(int fd, const boost::json::value& message) {
    std::string wire;
    proto::appendFrame(message, &wire);
    ::send(fd, wire.data(), wire.size(), MSG_NOSIGNAL);
}

// One copied frame: the pixels of one preview.ready slot, plus the identity
// that says whose picture it is (task 6.16a: target and request ride with
// the pixels, and a frame of the wrong kind under the right handles proves
// nothing).
struct Shot {
    bool ok = false;
    std::vector<uint8_t> px;  // RGBA, width*height
    uint32_t width = 0;
    uint32_t height = 0;
    std::uint64_t revision = 0;
    std::uint64_t request = 0;
};

struct Consumer {
    std::string name;
    uint8_t* map = nullptr;
    std::uint64_t slotCount = 0;
    std::uint64_t slotStride = 0;
    // The newest frame of each target seen so far. A journey step waits on
    // one of these, and frames of the other target flow past it, released
    // and ignored — interleaving cannot spoil a wait.
    Shot audience;
    Shot selection;
    std::uint64_t audienceFrames = 0;
    std::uint64_t selectionFrames = 0;
    // The highest output revision any present() has been ACKNOWLEDGED for.
    // An audience-target frame carrying a tag above it describes a
    // presentation that has not happened — the off-air step watches this:
    // the audience mirror may only ever carry frames the operator's own
    // presents produced, and the mirror's tag is the present ack's revision
    // (the frame's output revision), never a prepared-but-unpresented one.
    std::uint64_t maxPresentedSeen = 0;
    std::uint64_t leakedToAudience = 0;
};
Consumer g;

proto::Decoder gDecoder;

void handleNotification(int fd, const boost::json::object& params) {
    auto number = [&params](const char* key) -> std::uint64_t {
        const auto entry = params.find(key);
        return entry == params.end() ? 0 : entry->value().to_number<std::uint64_t>();
    };
    const std::uint64_t slot = number("slot");
    const std::uint64_t seq = number("seq");
    // R5's consumer-side gate: a ready naming a stream other than the one
    // this client started describes a segment this process does not have.
    const auto stream = params.find("stream");
    if (stream == params.end() || !stream->value().is_string() ||
        std::string_view(stream->value().as_string()) != g.name) {
        return;
    }
    // The segment is built by the render thread AFTER preview.start's
    // acknowledgement — the name is known before the bytes are. Map on the
    // first ready that names our stream, exactly as production_output_tests
    // does; a shm_open raced against creation is what failed the first run
    // of this fixture, and waiting for the frame that proves existence is
    // the protocol's own answer.
    if (g.map == nullptr) {
        const int shm = ::shm_open(g.name.c_str(), O_RDONLY, 0);
        if (shm >= 0) {
            const std::size_t total = g.slotStride * g.slotCount + 64;
            void* map = ::mmap(nullptr, total, PROT_READ, MAP_SHARED, shm, 0);
            ::close(shm);
            if (map != MAP_FAILED) {
                const auto* header = static_cast<const proto::PreviewSegmentHeader*>(map);
                if (header->magic == proto::kPreviewMagic &&
                    header->bytesPerSlot == g.slotStride &&
                    header->slotCount == g.slotCount) {
                    g.map = static_cast<uint8_t*>(map);
                }
            }
        }
    }
    const std::uint64_t width = number("width");
    const std::uint64_t height = number("height");
    const std::uint64_t rowBytes = number("rowBytes");
    const std::uint64_t revision = number("revision");
    const std::uint64_t target = number("target");
    const std::uint64_t request = number("request");
    if (g.map != nullptr && slot < g.slotCount && rowBytes == width * 4 && width > 0 &&
        height > 0) {
        const uint8_t* base =
            g.map + proto::previewSlotOffset(static_cast<uint32_t>(slot), g.slotStride);
        const auto* header = reinterpret_cast<const proto::PreviewSlotHeader*>(base);
        const std::uint64_t before = std::atomic_ref<const std::uint64_t>(header->seq).load();
        if (before == seq && (before & 1) == 0) {
            Shot shot;
            shot.px.assign(base + 64, base + 64 + rowBytes * height);
            shot.width = static_cast<uint32_t>(width);
            shot.height = static_cast<uint32_t>(height);
            shot.revision = revision;
            shot.request = request;
            const std::uint64_t after = std::atomic_ref<const std::uint64_t>(header->seq).load();
            if (after == before) {
                shot.ok = true;
                if (target == proto::kPreviewTargetSelection) {
                    g.selection = std::move(shot);
                    ++g.selectionFrames;
                } else {
                    if (revision != 0 && revision > g.maxPresentedSeen) {
                        ++g.leakedToAudience;  // pixels of a revision never presented
                    }
                    g.audience = std::move(shot);
                    ++g.audienceFrames;
                }
            }
        }
    }
    // The slot goes back immediately, every frame, exact sequence: the
    // journey asks for more pictures than there are slots, and a consumer
    // that stops releasing sees its deliveries stop — several waits below
    // only complete because every earlier frame was given back.
    sendRaw(fd, proto::makeNotification(proto::kMethodPreviewReleased,
                                        boost::json::object{{"slot", static_cast<std::int64_t>(slot)},
                                                            {"seq", static_cast<std::int64_t>(seq)},
                                                            {"stream", g.name}}));
}

void pump(int fd, int milliseconds) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < until) {
        pollfd pending{fd, POLLIN, 0};
        const int ready = ::poll(&pending, 1, 100);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready <= 0) {
            continue;
        }
        char buffer[65536];
        const ssize_t count = ::recv(fd, buffer, sizeof(buffer), 0);
        if (count <= 0) {
            return;
        }
        gDecoder.feed(std::string_view(buffer, static_cast<std::size_t>(count)));
        std::string error;
        boost::json::value message;
        while (gDecoder.next(&message, &error)) {
            if (message.is_object() && message.as_object().contains("method") &&
                message.as_object().contains("params") && message.at("params").is_object()) {
                // The envelope carries {method, params}; the identity and
                // slot live in params, and handing the envelope to the
                // handler is how the first draft saw a stream that named
                // nothing and released nothing.
                handleNotification(fd, message.at("params").as_object());
            }
        }
    }
}

// --- request/response ----------------------------------------------------
// Replies are matched by id; notifications that arrive alongside a reply
// feed the same consumer, because preview.ready does not schedule itself
// around the operator's polling.
bool call(int fd, std::int64_t id, const char* method, boost::json::object params,
          boost::json::value* reply) {
    std::string wire;
    proto::appendFrame(proto::makeRequest(id, method, std::move(params)), &wire);
    if (::send(fd, wire.data(), wire.size(), MSG_NOSIGNAL) < 0) {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
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
            if (message.is_object() && message.as_object().contains("method") &&
                message.as_object().contains("params") && message.at("params").is_object()) {
                handleNotification(fd, message.at("params").as_object());
                continue;
            }
            if (message.is_object() && message.as_object().contains("id")) {
                const auto& idValue = message.as_object().at("id");
                if (idValue.is_int64() && idValue.as_int64() == id) {
                    *reply = std::move(message);
                    return true;
                }
                // A different call's reply (a status poll racing nothing but
                // itself cannot happen with one thread — this is a guard
                // against a mismatched id, not a queue).
            }
        }
    }
    return false;
}

std::uint64_t numberValue(const boost::json::value& value) {
    return value.is_uint64() ? value.as_uint64()
                             : static_cast<std::uint64_t>(value.as_int64());
}

// The panel's hand-off (WorkerController::presentSceneJson with the
// auto-present flag): prepare, take the stored revision, present, and wait
// for output.status to report it ON AIR — acknowledged, not requested. The
// transition is a cut: this fixture asserts pixels, and a fade would ask
// them to be two slides at once.
//
// The two revisions the protocol counts (and the fixture must not conflate):
// the PREPARED revision names the scene in the store (what scene.present is
// asked for and what status's scene.active reports), while the revision a
// preview frame of the AUDIENCE target carries is the output revision the
// presented frame bumped — the ack of scene.present names it. Off-air frames
// carry the prepared revision they rendered (task 6.16a's rule, pinned by
// preview_selection_tests); the mirror's tag belongs to the frame, not the
// ask. So the return carries both: scene is what status must confirm, tag is
// what the next audience frame must be tagged with.
struct Presented {
    std::uint64_t scene = 0;
    std::uint64_t tag = 0;
};

Presented presentJson(int fd, std::int64_t& nextId, const QString& sceneJson) {
    boost::system::error_code parseError;
    const boost::json::value parsed = boost::json::parse(sceneJson.toStdString(), parseError);
    if (parseError || !parsed.is_object()) {
        return {};
    }
    boost::json::value reply;
    if (!call(fd, nextId++, "scene.prepare", parsed.as_object(), &reply) ||
        !reply.as_object().contains("result")) {
        return {};
    }
    const std::uint64_t scene = numberValue(reply.at("result").as_object().at("revision"));
    if (!call(fd, nextId++, "scene.present",
              boost::json::object{{"revision", scene}, {"transition", "cut"}}, &reply) ||
        !reply.as_object().contains("result")) {
        return {};
    }
    const Presented presented{scene,
                             numberValue(reply.at("result").as_object().at("revision"))};
    // From the ack on, a mirror frame carrying this tag is legitimate — and
    // the ack is decoded before any frame it caused (the worker queues the
    // mirror publish behind the reply), so the leak ceiling moves at
    // exactly the moment the client learns the presentation is real.
    g.maxPresentedSeen = presented.tag;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (std::chrono::steady_clock::now() < deadline) {
        boost::json::value statusReply;
        if (call(fd, nextId++, "output.status", {}, &statusReply)) {
            const boost::json::value& status = statusReply.at("result");
            if (status.as_object().contains("scene") &&
                status.at("scene").as_object().contains("active") &&
                numberValue(status.at("scene").as_object().at("active")) == scene) {
                return presented;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return {};
}

// Present an already-prepared revision (the restore's act), same on-air wait.
Presented presentRevision(int fd, std::int64_t& nextId, std::uint64_t scene) {
    boost::json::value reply;
    if (scene == 0 ||
        !call(fd, nextId++, "scene.present",
              boost::json::object{{"revision", scene}, {"transition", "cut"}}, &reply) ||
        !reply.as_object().contains("result")) {
        return {};
    }
    const Presented presented{scene,
                             numberValue(reply.at("result").as_object().at("revision"))};
    g.maxPresentedSeen = presented.tag;  // same ack-moment ceiling as above
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (std::chrono::steady_clock::now() < deadline) {
        boost::json::value statusReply;
        if (call(fd, nextId++, "output.status", {}, &statusReply)) {
            const boost::json::value& status = statusReply.at("result");
            if (status.as_object().contains("scene") &&
                status.at("scene").as_object().contains("active") &&
                numberValue(status.at("scene").as_object().at("active")) == scene) {
                return presented;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return {};
}

// Wait for the newest frame of one target to carry one revision. Present
// tags are unique — the output revision bumps for every present — so "the
// frame on hand carries THIS tag" already means it is the frame this present
// produced. A freshness gate on delivery counts would only add a race: the
// mirror publishes as the present lands, and a status poll can receive that
// frame before the caller even starts waiting.
bool waitFor(Shot* shot, std::uint64_t revision, int fd, int seconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (shot->ok && shot->revision == revision) {
            return true;
        }
        pump(fd, 100);
    }
    return shot->ok && shot->revision == revision;
}

// --- pixel helpers ------------------------------------------------------
// Solid colour backgrounds are sampled at a corner (as a fraction of the
// frame): the words sit centred, and cover-fit of a 16:9 still into the
// 16:9 stream leaves the corner the picture's own colour.
uint32_t pixelAt(const Shot& shot, double fx, double fy) {
    const uint32_t x = static_cast<uint32_t>(shot.width * fx);
    const uint32_t y = static_cast<uint32_t>(shot.height * fy);
    const uint8_t* p = shot.px.data() + (static_cast<size_t>(y) * shot.width + x) * 4;
    return (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2];
}

bool nearColor(uint32_t rgb, uint32_t want, int tolerance = 4) {
    const int dr = int((rgb >> 16) & 0xFF) - int((want >> 16) & 0xFF);
    const int dg = int((rgb >> 8) & 0xFF) - int((want >> 8) & 0xFF);
    const int db = int(rgb & 0xFF) - int(want & 0xFF);
    return dr * dr + dg * dg + db * db <= tolerance * tolerance * 3;
}

// Any pixel bright enough to be words (white on black): the count of lit
// pixels distinguishes "the words drew" from "the frame is empty" without
// depending on font metrics. Valid over a BLACK background only — a gold
// still is itself "lit" in this sense, so the words-over-media assertions
// use whitePixels below.
int litPixels(const Shot& shot) {
    int lit = 0;
    for (size_t i = 0; i + 2 < shot.px.size(); i += 4) {
        if (shot.px[i] > 0x40 || shot.px[i + 1] > 0x40 || shot.px[i + 2] > 0x40) {
            ++lit;
        }
    }
    return lit;
}

// Words are white items (the scene model's default text colour), and white
// is the one colour the gold and blue backdrops here are not: gold fails on
// blue, blue fails on red. Counting pixels bright in ALL THREE channels
// counts glyphs and only glyphs, whatever still stands behind them.
int whitePixels(const Shot& shot) {
    int white = 0;
    for (size_t i = 0; i + 2 < shot.px.size(); i += 4) {
        if (shot.px[i] > 0x80 && shot.px[i + 1] > 0x80 && shot.px[i + 2] > 0x80) {
            ++white;
        }
    }
    return white;
}

bool uniformlyBlack(const Shot& shot) {
    return litPixels(shot) == 0;
}

// ffmpeg, as in the live media fixtures: a REAL PNG on disk, decoded by the
// worker's real image path.
bool writePng(const std::string& path, const char* colour, int width, int height) {
    const std::string command =
        "ffmpeg -v error -y -f lavfi -i color=c=" + std::string(colour) + ":s=" +
        std::to_string(width) + "x" + std::to_string(height) + ":r=1 -frames:v 1 '" + path +
        "' >/dev/null 2>&1";
    return std::system(command.c_str()) == 0 && ::access(path.c_str(), R_OK) == 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    if (argc > 1) {
        gWorkerBinary = argv[1];
    }

    // --- the environment it could NOT exercise, printed where the proof is
    std::printf("production_journey_tests: environment notes (honest skips)\n");
    std::printf("  display: %s\n",
                getenv("WAYLAND_DISPLAY") ? "Wayland session"
                                          : (getenv("DISPLAY") ? "X11 session"
                                                               : "no display env — the worker would not start"));
    const char* runtimeDir = getenv("XDG_RUNTIME_DIR");
    const bool pipewireSocket =
        runtimeDir != nullptr &&
        std::filesystem::exists(std::filesystem::path(runtimeDir) / "pipewire-0");
    std::printf("  audio: silent journey by design — every slide here is text and stills, so no PipeWire"
                " stream is opened (pipewire-0 socket %s). The contract's one-mixed-stream wording vs the"
                " two shipped transports is 6.20d's decision to record, not a claim this fixture makes.\n",
                pipewireSocket ? "present, unused" : "absent");
    std::printf("  GPU: whatever Vulkan device the session's loader picks; from in here RADV and llvmpipe"
                " are indistinguishable — the device name is in the worker log above, and the distinction"
                " is a target-machine item (M7.4), not a claim of this fixture.\n");
    std::fflush(stdout);

    const std::filesystem::path scratch = std::filesystem::temp_directory_path() /
                                          ("openshow-journey-" + std::to_string(getpid()));
    std::filesystem::remove_all(scratch);
    ::unlink(gSocket.c_str());

    const pid_t worker = spawnWorker();
    const int fd = connectRetry();
    if (!check(fd >= 0, "connected to a live production worker")) {
        kill(worker, SIGKILL);
        waitpid(worker, nullptr, 0);
        std::filesystem::remove_all(scratch);
        return 1;
    }

    std::int64_t nextId = 1;
    boost::json::value reply;
    check(call(fd, nextId++, "hello", proto::helloParams("journey-test", 0, 1), &reply) &&
              reply.as_object().contains("result"),
          "handshake completed");

    // --- the preview stream comes up first: every later claim is watched
    // through it, not just read off the status.
    if (!check(call(fd, nextId++, "preview.start",
                    boost::json::object{{"width", 640}, {"height", 360}, {"fps", 15}}, &reply) &&
                   reply.as_object().contains("result"),
               "preview.start succeeded")) {
        kill(worker, SIGKILL);
        waitpid(worker, nullptr, 0);
        std::filesystem::remove_all(scratch);
        return 1;
    }
    {
        const boost::json::object& result = reply.at("result").as_object();
        g.name = std::string(result.at("name").as_string());
        g.slotCount = result.at("slotCount").to_number<std::uint64_t>();
        g.slotStride = result.at("bytesPerSlot").to_number<std::uint64_t>();
        check(!g.name.empty() && g.slotCount == proto::kPreviewSlotCount && g.slotStride > 64,
              "preview.start named the segment, its three slots and their stride");
    }
    check(waitFor(&g.audience, 0, fd, 5),
          "the fresh worker's first preview frame arrives");
    check(g.map != nullptr, "the preview segment mapped on the first frame naming it");
    // An empty show's mirror: scene revision 0, uniformly black — the
    // production worker with nothing to say says black.
    check(g.audience.ok && g.audience.revision == 0 && uniformlyBlack(g.audience),
          "the fresh audience frame is uniformly black");

    // --- author: the panels' own controller types the service ------------
    DocumentController docs(nullptr, scratch);
    check(docs.createSong(QStringLiteral("How Great Thou Art")), "the song is created");
    check(docs.addSlide(), "the first slide is added");
    docs.setSlideText(QStringLiteral("When morning gilds the skies\nMy heart awakes to thee"));
    docs.flushText();
    check(docs.addSlide(), "the second slide is added");
    docs.setSlideText(QStringLiteral("And as thy mercies move\nMy days draw near their sun"));
    docs.flushText();
    const QVariantList slides = docs.slides();
    check(slides.size() == 2, "both slides are listed");
    const QString slide1 = slides.at(0).toMap().value(QStringLiteral("id")).toString();
    const QString slide2 = slides.at(1).toMap().value(QStringLiteral("id")).toString();

    const QString json1 = docs.sceneJsonForSlide(slide1);
    const QString json2 = docs.sceneJsonForSlide(slide2);
    check(!json1.isEmpty() && !json2.isEmpty() && json1 != json2,
          "both slides compile through the panel's own call");

    // --- off-air picture: the NEXT slide, asked before its moment --------
    // Prepare slide 2 WITHOUT presenting it, and ask the worker to picture
    // it off-air. The selection frame must carry slide 2's revision and
    // show words; the audience must never receive it — the operator's
    // rehearsal picture cannot leak to the congregation.
    std::uint64_t rev2 = 0;
    {
        boost::json::value parsed = boost::json::parse(json2.toStdString());
        boost::json::value prepReply;
        if (call(fd, nextId++, "scene.prepare", parsed.as_object(), &prepReply) &&
            prepReply.as_object().contains("result")) {
            rev2 = numberValue(prepReply.at("result").as_object().at("revision"));
        }
    }
    check(rev2 != 0, "the next slide is prepared (stored, not presented)");
    if (rev2 != 0) {
        g.maxPresentedSeen = 0;  // nothing is on air yet: rev2 arriving on the mirror IS a leak
        boost::json::value selReply;
        check(call(fd, nextId++, proto::kMethodPreviewSelect,
                   boost::json::object{{"revision", rev2}, {"request", 1u}}, &selReply) &&
                  selReply.as_object().contains("result"),
              "the off-air picture is asked for");
        bool seen = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
        while (!seen && std::chrono::steady_clock::now() < deadline) {
            pump(fd, 250);
            seen = g.selection.ok && g.selection.revision == (rev2 & 0xFFFFFFFFu) &&
                   g.selection.request == 1;
        }
        check(seen, "the preview stream carries the off-air picture of the right revision");
        check(seen && litPixels(g.selection) > 0,
              "the off-air picture has words drawn in it (lit pixels)");
        check(g.leakedToAudience == 0,
              "the audience never received the un-presented slide — off-air means off-air");
    }

    // --- go: slide 1 to the audience, through the panel's hand-off -------
    const Presented go1 = presentJson(fd, nextId, json1);
    check(go1.scene != 0 && go1.tag != 0,
          "the first slide is presented and reports itself on air");
    g.maxPresentedSeen = go1.tag;
    check(waitFor(&g.audience, go1.tag, fd, 6),
          "the mirror stream carries an audience frame tagged with the presented frame's revision");
    check(g.audience.ok && litPixels(g.audience) > 0,
          "the audience frame has the words drawn");
    check(g.audience.ok && nearColor(pixelAt(g.audience, 0.02, 0.90), 0x000000),
          "its corner is the black background — words over black until media arrives");
    // The controller's selection moves to the live slide, the way an
    // operator's does: every later act here (attach, swap) edits the slide
    // the audience is looking at, and currentSceneJson must mean THAT one.
    docs.selectSlide(0);
    check(docs.currentSlide() == 0, "the selection follows the live slide");

    // --- media under live words: import, attach, present -----------------
    const std::string goldPng = (scratch / "gold.png").string();
    const std::string bluePng = (scratch / "blue.png").string();
    const bool pngs = writePng(goldPng, "0xF0E030", 800, 450) &&
                      writePng(bluePng, "0x2040F0", 800, 450);
    if (!check(pngs, "the two background stills are written (ffmpeg)")) {
        // Without PNGs the rest of the journey cannot be proven; say so in
        // the failure line rather than skipping silently into green.
    } else {
        check(docs.importMediaFile(QString::fromStdString(goldPng)), "the gold still imports");
        const QString goldId = QString::fromStdString(
            docs.assets().at(0).toMap().value(QStringLiteral("id")).toString().toStdString());
        check(docs.setSlideBackground(goldId), "the live slide takes the gold background");
        const Presented gold = presentJson(fd, nextId, docs.currentSceneJson());
        check(gold.scene != 0, "the slide with the gold background is on air");
        g.maxPresentedSeen = gold.tag;
        check(waitFor(&g.audience, gold.tag, fd, 6),
              "a new audience frame tagged with the gold presentation's revision arrives");
        check(g.audience.ok && nearColor(pixelAt(g.audience, 0.02, 0.90), 0xF0E030),
              "the corner SAMPLES gold — the worker decoded the asset and drew it");
        check(g.audience.ok && whitePixels(g.audience) > 0,
              "and the words still draw over it");

        // --- per-layer clears against the picture -------------------------
        const QString goldJson = docs.currentSceneJson();
        const QString wordsGone = openshow::ui::withWordsCleared(goldJson);
        const Presented noWords = presentJson(fd, nextId, wordsGone);
        check(noWords.scene != 0, "clearing the words is acknowledged on air");
        g.maxPresentedSeen = noWords.tag;
        check(waitFor(&g.audience, noWords.tag, fd, 6),
              "the audience frame after the words clear arrives");
        check(g.audience.ok && nearColor(pixelAt(g.audience, 0.02, 0.90), 0xF0E030),
              "the gold rides through the words clear — layers are independent");
        check(g.audience.ok && whitePixels(g.audience) == 0,
              "no words remain drawn (not one white pixel over the gold)");

        const QString backGone = openshow::ui::withBackgroundCleared(goldJson);
        const Presented noBack = presentJson(fd, nextId, backGone);
        check(noBack.scene != 0, "clearing the background is acknowledged on air");
        g.maxPresentedSeen = noBack.tag;
        check(waitFor(&g.audience, noBack.tag, fd, 6),
              "the audience frame after the background clear arrives");
        check(g.audience.ok && nearColor(pixelAt(g.audience, 0.02, 0.90), 0x000000),
              "the background is gone (corner is black again)");
        check(g.audience.ok && litPixels(g.audience) > 0,
              "and the words rode through the background clear");

        // --- clear all, and the restore ----------------------------------
        const Presented cleared = presentJson(fd, nextId, openshow::ui::clearedSceneJson());
        check(cleared.scene != 0, "clear-all is acknowledged on air");
        g.maxPresentedSeen = cleared.tag;
        check(waitFor(&g.audience, cleared.tag, fd, 6),
              "the audience frame after clear-all arrives");
        check(g.audience.ok && uniformlyBlack(g.audience),
              "and it is uniformly black — everything is off");
        const Presented restored = presentRevision(fd, nextId, gold.scene);
        check(restored.scene == gold.scene && restored.tag > cleared.tag,
              "restore re-presents the remembered gold revision, under a fresh tag");
        g.maxPresentedSeen = restored.tag;
        check(waitFor(&g.audience, restored.tag, fd, 6),
              "after the clear sequence, restore brings gold AND words back");
        check(g.audience.ok && nearColor(pixelAt(g.audience, 0.02, 0.90), 0xF0E030) &&
                  whitePixels(g.audience) > 0,
              "the restored frame samples gold with words over it");

        // --- the swap: a different picture under the SAME live words ------
        // The swap's promise is that the words are untouched: the compiled
        // scene after re-attaching to the blue still must carry byte-for-
        // byte the same items list the gold scene carried, with only the
        // background changed — and the mirror must show it.
        QString itemsBefore;
        {
            const boost::json::value parsed = boost::json::parse(goldJson.toStdString());
            itemsBefore = QString::fromStdString(
                boost::json::serialize(parsed.as_object().at("items")));
        }
        check(docs.importMediaFile(QString::fromStdString(bluePng)), "the blue still imports");
        check(docs.assets().size() == 2, "the library now holds both stills");
        const QString blueId = QString::fromStdString(
            docs.assets().at(1).toMap().value(QStringLiteral("id")).toString().toStdString());
        check(docs.setSlideBackground(blueId), "the live slide re-attaches to the blue still");
        const QString blueJson = docs.currentSceneJson();
        QString itemsAfter;
        {
            const boost::json::value parsed = boost::json::parse(blueJson.toStdString());
            itemsAfter = QString::fromStdString(
                boost::json::serialize(parsed.as_object().at("items")));
        }
        check(itemsAfter == itemsBefore,
              "the swap touches no word: the items list is identical across the swap");
        const Presented blue = presentJson(fd, nextId, blueJson);
        check(blue.scene != 0, "the swapped-background slide is on air");
        g.maxPresentedSeen = blue.tag;
        check(waitFor(&g.audience, blue.tag, fd, 6),
              "the audience frame after the swap arrives");
        check(g.audience.ok && nearColor(pixelAt(g.audience, 0.02, 0.90), 0x2040F0),
              "the corner SAMPLES blue — the audience sees the new picture");
        check(g.audience.ok && whitePixels(g.audience) > 0,
              "and the same words ride over it");
    }

    // The stream carried every step: author to off-air to go, both clears,
    // the restore and the swap — each one released, so the waits above were
    // never starved of slots.
    check(g.audienceFrames >= 6 && g.selectionFrames >= 1,
          "the preview stream delivered every leg of the journey (and every frame was released)");

    call(fd, nextId++, "shutdown", {}, &reply);
    int status = 0;
    waitpid(worker, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "the worker exited cleanly");

    if (g.map != nullptr) {
        ::munmap(g.map, g.slotStride * g.slotCount + 64);
    }
    close(fd);
    ::unlink(gSocket.c_str());
    std::filesystem::remove_all(scratch);

    if (gFailures > 0) {
        return 1;
    }
    std::printf("production_journey_tests: author -> off-air picture -> go -> clear ->"
                " restore -> media swap, proven in status AND in the preview stream\n");
    return 0;
}
