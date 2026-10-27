/*
 * Honest output status (task 6.4, review R4): the applied fields of
 * output.status — revision, scene.active, scene.pending, the preview's
 * revision tag — must describe the newest SUCCESSFULLY PRESENTED frame,
 * never the moment a command was noticed.
 *
 * The two lies R4 named are injected, not hoped for:
 *
 *   S1 present failure. OPENSHOW_FAULT_SCENE_PRESENT_FAILS=2 turns the
 *   presents carrying a new scene into refused submits (each followed by
 *   a hold so the state is observable). While the retries run, status
 *   must show the command QUEUED (pending names it, active does not) and
 *   must count the failures; nothing may advertise the scene live before
 *   it has actually presented, and the confirmation must arrive with
 *   pending settled. The preview's first red frame carries the revision
 *   the frame really drew.
 *
 *   S2 command during apply. OPENSHOW_FAULT_SCENE_APPLY_MS=400 parks the
 *   render thread inside scene B's apply — transport opens really take
 *   hundreds of ms — so scene C is accepted mid-apply. B's commit must
 *   credit only B (active=B, revision=revB, pending=C), and no sample
 *   may ever pair B with C's revision; the preview frame published for
 *   B's present is tagged revB, which is the revision its pixels carry
 *   (the old code tagged it revC). C then lands normally, transitioning
 *   distinct from confirmed.
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
#include <map>
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
std::string gSocket = "/tmp/openshow-status.sock";

pid_t spawnWorker(const std::map<std::string, std::string>& env) {
    const pid_t pid = fork();
    if (pid == 0) {
        for (const auto& entry : env) {
            ::setenv(entry.first.c_str(), entry.second.c_str(), 1);
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

struct Preview {
    std::string name;
    std::uint64_t slotCount = 0;
    std::uint64_t slotStride = 0;
    std::uint8_t* map = nullptr;
    int fd = -1;
} gPreview;

// One delivered preview frame as the consumer saw it: the revision tag
// the worker attached, the mean channels of the pixels underneath it,
// and when it arrived relative to the scenario epoch.
struct PreviewFrame {
    long arrivedMs = 0;
    std::uint64_t tag = 0;
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
};

std::vector<PreviewFrame> gFrames;
bool gCollect = false;
std::chrono::steady_clock::time_point gEpoch;

long sinceEpoch() {
    return static_cast<long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - gEpoch)
            .count());
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
            if (after == before && gCollect) {
                double red = 0.0;
                double green = 0.0;
                double blue = 0.0;
                const std::size_t pixels = copy.size() / 4;
                for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
                    red += copy[pixel * 4];
                    green += copy[pixel * 4 + 1];
                    blue += copy[pixel * 4 + 2];
                }
                PreviewFrame frame;
                frame.arrivedMs = sinceEpoch();
                frame.tag = number("revision");
                frame.red = red / static_cast<double>(pixels ? pixels : 1);
                frame.green = green / static_cast<double>(pixels ? pixels : 1);
                frame.blue = blue / static_cast<double>(pixels ? pixels : 1);
                gFrames.push_back(frame);
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

// One output.status reading, flattened.
struct Sample {
    long t = 0;
    std::uint64_t revision = 0;
    std::uint64_t presentFailures = 0;
    std::uint64_t active = 0;
    std::uint64_t pending = 0;
    bool transitioning = false;
    bool ok = false;
};

Sample sample(int fd, std::int64_t& nextId) {
    Sample out;
    boost::json::value reply;
    if (!call(fd, nextId++, "output.status", {}, &reply) ||
        !reply.as_object().contains("result")) {
        return out;
    }
    const boost::json::object& result = reply.at("result").as_object();
    out.t = sinceEpoch();
    out.revision = numberValue(result.at("revision"));
    out.presentFailures = numberValue(result.at("presentFailures"));
    const boost::json::object& scene = result.at("scene").as_object();
    out.active = numberValue(scene.at("active"));
    out.pending = numberValue(scene.at("pending"));
    out.transitioning = scene.at("transitioning").as_bool();
    out.ok = true;
    return out;
}

// A solid-colour scene with one word — scenes differ only in hue and
// word, so mean channel brightness is a robust content claim.
boost::json::object solidScene(const char* slideId, std::uint32_t color,
                               const char* word) {
    return boost::json::object{
        {"scene", 2},
        {"showId", "output-status"},
        {"slideId", slideId},
        {"backgroundColor", color},
        {"background",
         boost::json::object{{"image", false}, {"assetId", ""}, {"assetPath", ""}}},
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

std::uint64_t prepare(int fd, std::int64_t& nextId, const boost::json::object& scene) {
    boost::json::value reply;
    if (!call(fd, nextId++, "scene.prepare", scene, &reply) ||
        !reply.as_object().contains("result")) {
        return 0;
    }
    return numberValue(reply.at("result").as_object().at("revision"));
}

std::uint64_t present(int fd, std::int64_t& nextId, std::uint64_t storeRevision) {
    boost::json::value reply;
    if (!call(fd, nextId++, "scene.present",
              boost::json::object{{"revision", storeRevision}}, &reply) ||
        !reply.as_object().contains("result")) {
        return 0;
    }
    return numberValue(reply.at("result").as_object().at("revision"));
}

// Presents an already-prepared scene and waits for the confirmed state:
// active names it, nothing is queued, the revision has caught up.
bool presentAndWait(int fd, std::int64_t& nextId, std::uint64_t storeRevision,
                    std::uint64_t* ackOut) {
    const std::uint64_t ack = present(fd, nextId, storeRevision);
    if (ackOut != nullptr) {
        *ackOut = ack;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline) {
        const Sample s = sample(fd, nextId);
        if (s.ok && s.active == storeRevision && s.pending == 0 &&
            s.revision >= ack) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

void handshake(int fd, std::int64_t& nextId) {
    boost::json::value reply;
    check(call(fd, nextId++, "hello", proto::helloParams("status-test", 0, 1), &reply) &&
              reply.as_object().contains("result"),
          "handshake completed");
    if (check(call(fd, nextId++, "preview.start",
                   boost::json::object{{"width", 320}, {"height", 180}, {"fps", 30}},
                   &reply) &&
                  reply.as_object().contains("result"),
              "preview.start succeeded")) {
        const boost::json::object& result = reply.at("result").as_object();
        gPreview.name = std::string(result.at("name").as_string());
        gPreview.slotCount = result.at("slotCount").to_number<std::uint64_t>();
        gPreview.slotStride = result.at("bytesPerSlot").to_number<std::uint64_t>();
    }
}

std::int64_t gNextId = 1;

void shutdownWorker(int fd, pid_t worker) {
    boost::json::value reply;
    call(fd, gNextId++, "shutdown", {}, &reply);
    int status = 0;
    waitpid(worker, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "the worker exited cleanly");
    close(fd);
    ::unlink(gSocket.c_str());
    if (gPreview.map != nullptr) {
        ::munmap(gPreview.map, gPreview.slotStride * gPreview.slotCount + 64);
        ::close(gPreview.fd);
        gPreview = Preview{};
    }
}

// ---------------------------------------------------------------------
// S1: presents carrying a new scene fail twice before one succeeds.
// ---------------------------------------------------------------------
void scenarioPresentFailure() {
    gSocket = "/tmp/openshow-status-s1-" + std::to_string(getpid()) + ".sock";
    ::unlink(gSocket.c_str());
    const pid_t worker = spawnWorker({{"OPENSHOW_FAULT_SCENE_PRESENT_FAILS", "2"},
                                      {"OPENSHOW_FAULT_HOLD_MS", "250"}});
    const int fd = connectRetry();
    if (!check(fd >= 0, "S1: connected to a live worker")) {
        kill(worker, SIGKILL);
        waitpid(worker, nullptr, 0);
        return;
    }
    gNextId = 1;
    handshake(fd, gNextId);

    const std::uint64_t red = prepare(fd, gNextId, solidScene("A", 0xFF0000u, "ALPHA"));
    check(red != 0, "S1: the red scene is prepared");

    const Sample before = sample(fd, gNextId);
    check(before.ok && before.presentFailures == 0,
          "S1: no present failures recorded before the injection");

    gEpoch = std::chrono::steady_clock::now();
    gFrames.clear();
    gCollect = true;
    const std::uint64_t ack = present(fd, gNextId, red);
    check(ack != 0, "S1: the present command is acknowledged with a revision");

    // Poll through the whole retry episode, sampling every status beat.
    std::vector<Sample> seen;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    bool confirmed = false;
    while (std::chrono::steady_clock::now() < deadline) {
        seen.push_back(sample(fd, gNextId));
        const Sample& s = seen.back();
        if (s.ok && s.active == red && s.pending == 0 && s.revision >= ack &&
            s.presentFailures >= 2) {
            confirmed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    gCollect = false;

    check(confirmed, "S1: the scene is confirmed after the failed presents settle");
    const std::uint64_t failures = seen.empty() ? 0 : seen.back().presentFailures;
    check(failures == 2, "S1: both refused submits are honestly counted");

    // The lie R4 named: nothing may advertise the scene before a frame
    // carried it. Every sample naming it must post-date both failures.
    bool earlyConfirmation = false;
    bool honestQueue = false;
    for (const auto& s : seen) {
        if (s.ok && s.active == red && s.presentFailures < 2) {
            earlyConfirmation = true;
        }
        if (s.ok && s.active != red && s.pending == red) {
            honestQueue = true;
        }
    }
    check(!earlyConfirmation,
          "S1: no status sample calls the scene active before it presented");
    check(honestQueue, "S1: the failed episode shows the command queued, not on air");

    // The first preview frame carrying the new content is tagged with the
    // revision of the frame that actually drew it.
    bool checkedRed = false;
    for (const auto& frame : gFrames) {
        if (frame.red > 150.0 && frame.green < 100.0) {
            check(frame.tag == ack,
                  "S1: the first red preview frame is tagged the presented revision");
            checkedRed = true;
            break;
        }
    }
    check(checkedRed, "S1: the preview stream reached the red scene");

    shutdownWorker(fd, worker);
}

// ---------------------------------------------------------------------
// S2: scene C is accepted while scene B is mid-apply.
// ---------------------------------------------------------------------
void scenarioCommandDuringApply() {
    gSocket = "/tmp/openshow-status-s2-" + std::to_string(getpid()) + ".sock";
    ::unlink(gSocket.c_str());
    const pid_t worker = spawnWorker({{"OPENSHOW_FAULT_SCENE_APPLY_MS", "400"}});
    const int fd = connectRetry();
    if (!check(fd >= 0, "S2: connected to a live worker")) {
        kill(worker, SIGKILL);
        waitpid(worker, nullptr, 0);
        return;
    }
    gNextId = 1;
    handshake(fd, gNextId);

    const std::uint64_t red = prepare(fd, gNextId, solidScene("A", 0xFF0000u, "ALPHA"));
    const std::uint64_t blue = prepare(fd, gNextId, solidScene("B", 0x0000FFu, "BETA"));
    const std::uint64_t green = prepare(fd, gNextId, solidScene("C", 0x00FF00u, "GAMMA"));
    check(red != 0 && blue != 0 && green != 0 && blue != green,
          "S2: the three scenes are prepared");
    check(presentAndWait(fd, gNextId, red, nullptr),
          "S2: the red scene is confirmed on air");

    gEpoch = std::chrono::steady_clock::now();
    gFrames.clear();
    gCollect = true;
    const std::uint64_t ackB = present(fd, gNextId, blue);
    const long ackBMs = sinceEpoch();
    // Parked inside B's apply (400 ms): C lands mid-flight.
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    const std::uint64_t ackC = present(fd, gNextId, green);
    check(ackB != 0 && ackC != 0 && ackC > ackB,
          "S2: both presents are acknowledged, C after B");

    std::vector<Sample> seen;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    bool confirmed = false;
    while (std::chrono::steady_clock::now() < deadline) {
        seen.push_back(sample(fd, gNextId));
        const Sample& s = seen.back();
        if (s.ok && s.active == green && s.pending == 0 && !s.transitioning &&
            s.revision >= ackC) {
            confirmed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    check(confirmed, "S2: scene C ends confirmed — active, nothing queued, settled");

    bool bWithCreditForC = false;   // the R4 lie: B credited with C's command
    bool cWithoutCredit = false;    // C on air without its revision reached
    bool queuedBehind = false;      // B on air while C is still queued
    bool transitioningB = false;    // B committed mid-crossfade
    for (const auto& s : seen) {
        if (!s.ok) {
            continue;
        }
        if (s.active == blue && s.revision >= ackC) {
            bWithCreditForC = true;
        }
        if (s.active == green && s.revision < ackC) {
            cWithoutCredit = true;
        }
        if (s.active == blue && s.pending == green) {
            queuedBehind = true;
        }
        if (s.active == blue && s.transitioning) {
            transitioningB = true;
        }
    }
    check(!bWithCreditForC,
          "S2: no sample credits scene B with a revision past C's command");
    check(!cWithoutCredit,
          "S2: scene C is never active before its command's revision");
    check(queuedBehind, "S2: B is on air with C honestly queued behind it");
    check(transitioningB, "S2: B's commit marks the switch as not yet settled");

    // The preview frame published for B's present (the first one after
    // B was acknowledged) carries B's revision — the revision its pixels
    // hold. The old code tagged it with the latest desired revision.
    bool sawFrameForB = false;
    for (const auto& frame : gFrames) {
        if (frame.arrivedMs >= ackBMs) {
            sawFrameForB = true;
            check(frame.tag == ackB,
                  "S2: the first preview frame after B's ack is tagged B's revision");
            break;
        }
    }
    check(sawFrameForB, "S2: the preview stream kept moving across the episode");

    // The settled picture is really C, and the newest frame says so.
    if (check(!gFrames.empty(), "S2: preview frames were delivered")) {
        const PreviewFrame& last = gFrames.back();
        check(last.green > 150.0 && last.red < 100.0,
              "S2: the settled preview is the green scene C");
        check(last.tag == ackC, "S2: the settled preview frame is tagged C's revision");
    }

    shutdownWorker(fd, worker);
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc > 1) {
        gWorkerBinary = argv[1];
    }
    gDecoder.reset();
    scenarioPresentFailure();
    gDecoder.reset();
    scenarioCommandDuringApply();

    if (gFailures > 0) {
        return 1;
    }
    std::printf("output_status_tests: status reports what presented, not what was asked\n");
    return 0;
}
