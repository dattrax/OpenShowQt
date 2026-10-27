/*
 * Task 5.5 acceptance: preload, continuous backgrounds, visible errors — plus
 * the typed media report (6.7c) and the seek semantics (6.7d), which are the
 * same session seen from the socket.
 *
 * A real openshow-worker --socket session (the task's transport is only
 * alive in socket mode) proves the acceptance sentences:
 *
 *  - "Lyrics preserve playback": with a video background on air, the
 *    media position climbs THROUGH a lyric change over the same asset
 *    (the loop-continuous position reported in output.status.media — a
 *    transport that reopened would restart near zero), frames keep
 *    rolling at video pace, and toggling the background layer away hides
 *    the picture without stopping the play.
 *
 *  - "Failures are visible": a missing and a corrupt video each produce
 *    a media.error notification whose text matches the sticky
 *    output.status.media.error, the worker keeps serving, and presenting
 *    a good background clears the error and resumes playing.
 *
 *  - Preload (5.5c): a big still decodes during scene.prepare (the RPC
 *    thread pays it; the re-prepare is a cache hit), and presenting a
 *    preloaded slide reaches the air within 100 ms at p95.
 *
 *  - "A seek means what the audience gets" (6.7d, review R19): a scheduled
 *    burst, a backward seek after the clip looped, and a seek issued while the
 *    transport is held each present the media position at the right clock
 *    deadline — and a jump the audio refuses is reported, not quietly kept.
 *
 * Needs a display (the worker opens its output window) and the media
 * fixtures; the big-JPEG preload scenario additionally needs the ffmpeg
 * CLI and is skipped without it. Excluded from the headless preset.
 */

#include <boost/json.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "openshow/protocol/protocol.h"
#include "openshow/scene/scene.h"

namespace {

namespace proto = openshow::protocol;
namespace scene = openshow::scene;

int gFailures = 0;

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++gFailures;
    }
    return condition;
}

std::string gWorkerBinary = "./openshow-worker";

std::string socketPath(const char* tag) {
    return "/tmp/openshow-bg-" + std::to_string(getpid()) + "-" + tag + ".sock";
}

pid_t spawnWorker(const std::string& path, const std::vector<std::string>& extraEnv = {}) {
    const pid_t pid = fork();
    if (pid == 0) {
        // Fault knobs (OPENSHOW_FAULT_*) are set in the child only: the
        // test process itself must stay un-faulted, and a scenario that
        // needs a broken component owns a worker where nothing else does.
        for (const std::string& entry : extraEnv) {
            const std::size_t equals = entry.find('=');
            if (equals != std::string::npos) {
                ::setenv(entry.substr(0, equals).c_str(), entry.substr(equals + 1).c_str(), 1);
            }
        }
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

// media.error notifications interleave with any reply; they queue here.
std::deque<boost::json::object> gMediaErrors;

void routeNotification(const boost::json::value& message) {
    if (!message.is_object()) {
        return;
    }
    const boost::json::object& object = message.as_object();
    const auto method = object.find("method");
    if (method != object.end() && method->value().is_string() &&
        method->value().as_string() == proto::kMethodMediaError &&
        object.contains("params") && object.at("params").is_object()) {
        gMediaErrors.emplace_back(object.at("params").as_object());
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

std::int64_t gNextId = 1;

bool call(int fd, const char* method, boost::json::object params,
          boost::json::value* reply) {
    const std::int64_t id = gNextId++;
    std::string wire;
    proto::appendFrame(proto::makeRequest(id, method, std::move(params)), &wire);
    if (::send(fd, wire.data(), wire.size(), MSG_NOSIGNAL) < 0) {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd pfd{fd, POLLIN, 0};
        const int ready = ::poll(&pfd, 1, 50);
        if (ready < 0 && errno != EINTR) {
            return false;
        }
        if (ready > 0) {
            char buffer[64 * 1024];
            const ssize_t got = ::recv(fd, buffer, sizeof(buffer), 0);
            if (got <= 0) {
                return false;
            }
            gDecoder.feed(std::string_view(buffer, static_cast<size_t>(got)));
        }
        boost::json::value message;
        std::string decodeError;
        while (gDecoder.next(&message, &decodeError)) {
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
        if (gDecoder.failed()) {
            return false;
        }
    }
    return false;
}

boost::json::object status(int fd) {
    boost::json::value reply;
    if (!call(fd, "output.status", {}, &reply) || !reply.as_object().contains("result")) {
        return {};
    }
    return reply.at("result").as_object();
}

// The media object of one status snapshot.
boost::json::object mediaOf(const boost::json::object& result) {
    if (!result.contains("media") || !result.at("media").is_object()) {
        return {};
    }
    return result.at("media").as_object();
}

// The decoded-still cache object of one status snapshot (task 6.10).
boost::json::object imagesOf(const boost::json::object& result) {
    if (!result.contains("scene") || !result.at("scene").is_object()) {
        return {};
    }
    const boost::json::object& scene = result.at("scene").as_object();
    if (!scene.contains("images") || !scene.at("images").is_object()) {
        return {};
    }
    return scene.at("images").as_object();
}

double numberOf(const boost::json::object& object, const char* key) {
    const auto entry = object.find(key);
    if (entry == object.end()) {
        return -999.0;
    }
    return entry->value().to_number<double>();
}

std::string textOf(const boost::json::object& object, const char* key) {
    const auto entry = object.find(key);
    if (entry == object.end() || !entry->value().is_string()) {
        return "";
    }
    return std::string(entry->value().as_string());
}

// One scene: a lyric line over the given background. The scene lib builds
// the payload, so the wire shape cannot drift from the codec. The assetId
// is deliberately stable per path: real services reference ONE model asset
// from every slide that shares a background, and the transport's
// continuity identity (asset id, path, kind) relies on that — a test that
// invented fresh ids per slide would (correctly) force a reopen.
boost::json::object lyricScene(const std::string& line, const std::string& slideId,
                               const std::string& backgroundPath, bool video,
                               const std::string& assetId = "") {
    scene::Scene sceneValue;
    sceneValue.showId = "bg-acceptance";
    sceneValue.slideId = slideId;
    sceneValue.background.color = 0x102030;
    if (!backgroundPath.empty()) {
        sceneValue.background.hasImage = true;
        sceneValue.background.isVideo = video;
        sceneValue.background.assetId =
            assetId.empty() ? "asset-" + slideId : assetId;
        sceneValue.background.assetPath = backgroundPath;
    }
    scene::SceneItem item;
    item.lines = {line};
    item.color = 0xFFFFFF;
    item.fontFamily = "DejaVu Sans";
    item.fontSize = 72.0;
    item.x = 160.0;
    item.y = 440.0;
    item.width = 1600.0;
    item.height = 200.0;
    sceneValue.items.push_back(item);
    return scene::toJson(sceneValue).as_object();
}

// prepare + present; returns the store revision presented (0 on failure).
std::uint64_t preparePresent(int fd, const boost::json::object& sceneJson,
                             const char* transition = "cut") {
    boost::json::value reply;
    if (!call(fd, "scene.prepare", sceneJson, &reply) ||
        !reply.as_object().contains("result")) {
        std::fprintf(stderr, "FAIL: scene.prepare rejected\n");
        ++gFailures;
        return 0;
    }
    const std::uint64_t revision =
        reply.at("result").as_object().at("revision").to_number<std::uint64_t>();
    boost::json::value presentReply;
    if (!call(fd, "scene.present",
              boost::json::object{{"revision", revision},
                                  {"transition", std::string(transition)}},
              &presentReply) ||
        !presentReply.as_object().contains("result")) {
        std::fprintf(stderr, "FAIL: scene.present rejected\n");
        ++gFailures;
        return 0;
    }
    return revision;
}

// Polls status (bounded) until scene.active == revision, and reports how
// long the wait took from just before the first poll.
bool waitActive(int fd, std::uint64_t revision, double* secondsOut) {
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        const boost::json::object result = status(fd);
        if (result.contains("scene") && result.at("scene").is_object() &&
            result.at("scene").as_object().at("active").to_number<std::uint64_t>() ==
                revision) {
            if (secondsOut != nullptr) {
                *secondsOut =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
                        .count();
            }
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

void sleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

bool fileExists(const std::string& path) {
    struct stat info {};
    return ::stat(path.c_str(), &info) == 0;
}

void exerciseBackgroundMedia(int fd, const std::string& videoA,
                             const std::string& videoB, const std::string& missing,
                             const std::string& corrupt, const std::string& bigStill) {
    // --- video background comes on air ---
    const std::uint64_t scene1 =
        preparePresent(fd, lyricScene("Verse one", "v1", videoA, true, "video-a"));
    double onAirSeconds = 0.0;
    check(scene1 != 0 && waitActive(fd, scene1, &onAirSeconds),
          "the video-background scene goes on air");

    // Wait for the transport's head frame and a little playback.
    double positionFirst = -1.0;
    const auto positionDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (std::chrono::steady_clock::now() < positionDeadline && positionFirst <= 0.0) {
        positionFirst = numberOf(mediaOf(status(fd)), "position");
        sleepMs(50);
    }
    check(positionFirst > 0.0, "media.position reports playback under way");

    const boost::json::object before = status(fd);
    check(mediaOf(before).contains("playing") && mediaOf(before).at("playing").as_bool(),
          "status.media.playing is true for the video background");
    check(textOf(mediaOf(before), "error").empty(),
          "a healthy video background reports no error");

    // Let the transport run further so a reopened transport could not
    // plausibly reach the same position across the lyric change.
    sleepMs(1500);
    const boost::json::object preChange = status(fd);
    const double positionBefore = numberOf(mediaOf(preChange), "position");
    const double framesBefore = numberOf(preChange, "frames");

    // --- the acceptance shot: a lyric change over the SAME background ---
    const std::uint64_t scene2 =
        preparePresent(fd, lyricScene("Verse two", "v2", videoA, true, "video-a"));
    check(scene2 != 0 && waitActive(fd, scene2, nullptr),
          "the lyric change goes on air");
    sleepMs(400);  // a few video-paced frames through the change
    const boost::json::object postChange = status(fd);
    const double positionAfter = numberOf(mediaOf(postChange), "position");
    const double framesAfter = numberOf(postChange, "frames");
    check(mediaOf(postChange).at("playing").as_bool(),
          "still playing through the lyric change");
    check(textOf(mediaOf(postChange), "error").empty(),
          "the lyric change raises no error");
    // The continuity proof: the loop-continuous position climbed THROUGH
    // the change (a reopened transport would sit near its head).
    check(positionAfter >= positionBefore + 0.3,
          "media position climbs through the lyric change (background kept rolling)");
    check(framesAfter > framesBefore, "video-paced presents continue through the change");

    // --- the background layer off hides the picture, the play continues ---
    boost::json::value layerReply;
    if (check(call(fd, "layer.setEnabled",
                   boost::json::object{{"layer", "background"}, {"enabled", false}},
                   &layerReply) &&
                  layerReply.as_object().contains("result"),
              "the background layer can be turned off")) {
        sleepMs(600);
        const boost::json::object hidden = status(fd);
        check(mediaOf(hidden).at("playing").as_bool(),
              "hiding the background layer does not stop the playback");
        check(numberOf(mediaOf(hidden), "position") > positionAfter,
              "the media position keeps advancing with the background hidden");
        const boost::json::object layers = hidden.at("layers").as_object();
        check(layers.contains("background") && !layers.at("background").as_bool(),
              "status reports the background layer off");
        call(fd, "layer.setEnabled",
             boost::json::object{{"layer", "background"}, {"enabled", true}}, &layerReply);
    }

    // --- swap to a different video: a fresh transport takes over ---
    const std::uint64_t scene3 =
        preparePresent(fd, lyricScene("Different stage", "v3", videoB, true));
    check(scene3 != 0 && waitActive(fd, scene3, nullptr),
          "the swapped-video scene goes on air");
    double swapped = -1.0;
    const auto swapDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (std::chrono::steady_clock::now() < swapDeadline &&
           (swapped <= 0.0 || textOf(mediaOf(status(fd)), "path") != videoB)) {
        const boost::json::object media = mediaOf(status(fd));
        swapped = textOf(media, "path") == videoB ? numberOf(media, "position") : -1.0;
        sleepMs(50);
    }
    check(swapped > 0.0, "the swapped background plays its own media position");

    // --- task 6.6b (review R6): a swap burst racing live submits ---
    // Teardown now quiesces the producer (requestStop + join) BEFORE the
    // seam's presenter, clock and pending slot are edited, so a swap is
    // safe at any point of the old transport's submit cadence. The burst
    // makes "any point" the norm: eight A/B alternations fired with no
    // settling waits between them. The transport must still settle on the
    // final identity — playing, its own path, position advancing — with no
    // media error and a responsive worker throughout (a torn-down submitter
    // calling into a dead presenter does not survive this under ASAN, and
    // a seam left stale never settles).
    gMediaErrors.clear();
    std::uint64_t burstFinal = 0;
    std::string burstClip;
    for (int round = 0; round < 8; ++round) {
        burstClip = (round % 2) ? videoA : videoB;
        burstFinal = preparePresent(fd, lyricScene("Burst", "burst" + std::to_string(round),
                                                   burstClip, true));
        // Short enough never to settle, long enough that the previous
        // round's transport really opened and is submitting when this
        // round's swap tears it down — a young producer mid-cadence is
        // exactly the window R6 names.
        sleepMs(120);
    }
    check(burstFinal != 0 && waitActive(fd, burstFinal, nullptr),
          "the transport settles after the swap burst");
    double burstPosition = -1.0;
    const auto burstDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (std::chrono::steady_clock::now() < burstDeadline &&
           (burstPosition <= 0.0 || textOf(mediaOf(status(fd)), "path") != burstClip)) {
        const boost::json::object media = mediaOf(status(fd));
        burstPosition =
            textOf(media, "path") == burstClip ? numberOf(media, "position") : -1.0;
        sleepMs(50);
    }
    const boost::json::object burstMedia = mediaOf(status(fd));
    check(burstPosition > 0.0 && textOf(burstMedia, "path") == burstClip &&
              burstMedia.at("playing").as_bool(),
          "after the burst the transport plays the final identity cleanly");
    check(gMediaErrors.empty(), "the swap burst raises no media errors");
    const double burstAdvance = numberOf(burstMedia, "position");
    sleepMs(400);
    check(numberOf(mediaOf(status(fd)), "position") > burstAdvance,
          "the settled transport keeps advancing its position");

    // --- a missing file fails visibly ---
    gMediaErrors.clear();
    const std::uint64_t sceneBroken =
        preparePresent(fd, lyricScene("Broken", "vb", missing, true));
    check(sceneBroken != 0 && waitActive(fd, sceneBroken, nullptr),
          "even the broken scene changes the slide (only the media fails)");
    sleepMs(300);  // let the notification ride one flush tick
    const boost::json::object brokenStatus = mediaOf(status(fd));
    check(!gMediaErrors.empty(), "a missing background video pushes media.error");
    if (!gMediaErrors.empty()) {
        check(textOf(gMediaErrors.front(), "path") == missing,
              "the media.error notification names the missing asset");
        check(!textOf(gMediaErrors.front(), "message").empty(),
              "the media.error notification carries a message");
    }
    check(!textOf(brokenStatus, "error").empty(),
          "status.media.error is sticky after the failure");
    check(!brokenStatus.at("playing").as_bool(),
          "a failed background is not reported as playing");

    // --- a corrupt file fails visibly too (different path, new message) ---
    gMediaErrors.clear();
    const std::uint64_t sceneCorrupt =
        preparePresent(fd, lyricScene("Corrupt", "vc", corrupt, true));
    check(sceneCorrupt != 0 && waitActive(fd, sceneCorrupt, nullptr),
          "the corrupt-background scene still changes the slide");
    sleepMs(300);
    check(!gMediaErrors.empty(), "a corrupt background video pushes media.error");
    check(!textOf(mediaOf(status(fd)), "error").empty(),
          "status.media.error is set for the corrupt asset");

    // --- recovery: a good background clears the error ---
    const std::uint64_t sceneBack =
        preparePresent(fd, lyricScene("Recovered", "v4", videoA, true));
    check(sceneBack != 0 && waitActive(fd, sceneBack, nullptr),
          "the good scene comes back on air");
    // A successful reopen clears the sticky error and plays again. The
    // position contract (mediaPlaying): -1.0 until the transport presents
    // its head, so the wait requires a real position, not merely the
    // playing flag — task 6.4 made this visible: honest confirmation now
    // lands within milliseconds of the reopen, where the early status lie
    // used to start the polling a whole transport-open earlier.
    double recoveredPosition = -1.0;
    const auto recoverDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (std::chrono::steady_clock::now() < recoverDeadline) {
        const boost::json::object media = mediaOf(status(fd));
        const double position = numberOf(media, "position");
        if (media.contains("playing") && media.at("playing").as_bool() &&
            textOf(media, "error").empty() && position >= 0.0) {
            recoveredPosition = position;
            break;
        }
        sleepMs(50);
    }
    check(recoveredPosition >= 0.0,
          "a successful reopen clears the sticky error and plays again");

    // --- preload: the big still decodes during prepare; presenting a
    // prepared slide is prompt (the 100 ms p95 target) ---
    if (!bigStill.empty()) {
        const auto prepStart = std::chrono::steady_clock::now();
        boost::json::value prepReply;
        const bool prepared = call(fd, "scene.prepare",
                                   lyricScene("Huge still", "vp", bigStill, false),
                                   &prepReply) &&
                              prepReply.as_object().contains("result");
        const double prepSeconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - prepStart)
                .count();
        check(prepared, "the still-background scene prepares");
        // Re-prepare the same path: the image cache answers instantly.
        const auto secondStart = std::chrono::steady_clock::now();
        boost::json::value secondReply;
        const bool preparedAgain = call(fd, "scene.prepare",
                                        lyricScene("Huge still cached", "vp2", bigStill,
                                                   false),
                                        &secondReply) &&
                                   secondReply.as_object().contains("result");
        const double secondSeconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - secondStart)
                .count();
        check(preparedAgain && secondSeconds < prepSeconds,
              "the second prepare of the same image is a cache hit");

        // p95 of present -> on air over repeated prepared slides.
        std::vector<double> latencies;
        for (int attempt = 0; attempt < 15; ++attempt) {
            const std::uint64_t revision = preparePresent(
                fd, lyricScene("Preloaded " + std::to_string(attempt),
                               "pl" + std::to_string(attempt), bigStill, false));
            double seconds = 0.0;
            if (revision == 0 || !waitActive(fd, revision, &seconds)) {
                // Name the stall: what does the worker think is on air?
                const boost::json::object stuck = status(fd);
                std::string active = "none";
                std::string pending = "none";
                std::string prepared = "none";
                if (stuck.contains("scene") && stuck.at("scene").is_object()) {
                    const boost::json::object sceneState = stuck.at("scene").as_object();
                    active = std::to_string(
                        sceneState.at("active").to_number<std::uint64_t>());
                    pending = std::to_string(
                        sceneState.at("pending").to_number<std::uint64_t>());
                    prepared = std::to_string(
                        sceneState.at("prepared").to_number<std::uint64_t>());
                }
                std::fprintf(stderr,
                             "diag: attempt %d waited for revision %llu; status now "
                             "active=%s pending=%s prepared=%s frames=%s\n",
                             attempt, static_cast<unsigned long long>(revision),
                             active.c_str(), pending.c_str(), prepared.c_str(),
                             textOf(stuck, "device").empty()
                                 ? "none"
                                 : std::to_string(
                                       numberOf(stuck, "frames"))
                                       .c_str());  // frames always accompany device
                check(false, "a preloaded slide failed to reach the air");
                break;
            }
            latencies.push_back(seconds);
        }
        if (latencies.size() == 15) {
            std::sort(latencies.begin(), latencies.end());
            const double p95 = latencies[13];  // 14th of 15
            std::fprintf(stderr,
                         "preload present->air p95 %.1f ms (max %.1f ms)\n", p95 * 1000.0,
                         latencies.back() * 1000.0);
            check(p95 < 0.100, "preloaded slide changes reach the air within 100 ms p95");
        }
    }
}

// Runs a command quietly; true on success.
bool generate(const std::string& command) {
    return std::system(command.c_str()) == 0;
}

// --- task 6.7c (review R18): the typed media report -------------------------
//
// The scenarios below are the acceptance sentences of 6.7c, each one on the
// worker's own report rather than on its log:
//
//   * an undecodable still names itself, and the SAME slide recovers when
//     the operator puts the file back (no new asset, no new notification);
//   * a background that dies while it plays stops being reported as
//     playing, within the heartbeat, and pushes the failure;
//   * a clip that carries audio never reports the "none" lane, and its
//     error matches whatever lane its machine really has;
//   * a worker whose audio start is refused plays the picture and says
//     "degraded" — and a silent clip clears that report (recovery is typed).

// Polls status until the media object satisfies predicate (bounded).
bool waitMedia(int fd, const std::function<bool(const boost::json::object&)>& predicate,
               int timeoutMs) {
    for (int waited = 0; waited < timeoutMs; waited += 50) {
        const boost::json::object media = mediaOf(status(fd));
        if (!media.empty() && predicate(media)) {
            return true;
        }
        sleepMs(50);
    }
    return false;
}

// The still that cannot be decoded, then repaired under the same slide.
void exerciseImageRepair(int fd, const std::string& missingStill,
                         const std::string& stillFixture) {
    gMediaErrors.clear();
    const std::uint64_t broken =
        preparePresent(fd, lyricScene("Broken still", "t1", missingStill, false));
    check(broken != 0 && waitActive(fd, broken, nullptr),
          "the undecodable-still scene goes on air (the slide works, the picture does not)");
    sleepMs(300);  // let the notification ride one flush tick
    check(!gMediaErrors.empty(),
          "an undecodable still pushes media.error instead of a silent colour fill");
    if (!gMediaErrors.empty()) {
        check(textOf(gMediaErrors.front(), "path") == missingStill,
              "the notification names the still it could not decode");
        check(textOf(gMediaErrors.front(), "message").find("image") != std::string::npos,
              "the message names the class of asset that failed");
    }
    const boost::json::object brokenMedia = mediaOf(status(fd));
    check(!textOf(brokenMedia, "error").empty(), "status.media.error holds the still's failure");
    check(textOf(brokenMedia, "path") == missingStill,
          "and the report stays paired with the asset the audience is looking at");
    check(!brokenMedia.contains("playing") || !brokenMedia.at("playing").as_bool(),
          "a still is never reported as playing");
    check(textOf(brokenMedia, "audio") == "none", "a still has no audio lane to report");

    // The operator restores the file. A prepare is an operator action, so a
    // cached failure must not be reused: the picture has to come back and
    // its error has to clear — while NOTHING new is pushed for the good
    // news (recovery is a status change, not an event).
    if (!generate("cp '" + stillFixture + "' '" + missingStill + "'")) {
        check(false, "the repaired still could be staged (fixture failure)");
        return;
    }
    gMediaErrors.clear();
    const std::uint64_t repaired =
        preparePresent(fd, lyricScene("Broken still", "t1", missingStill, false));
    check(repaired != 0 && waitActive(fd, repaired, nullptr),
          "the same slide re-presents after the file returns");
    const bool cleared = waitMedia(
        fd, [](const boost::json::object& media) { return textOf(media, "error").empty(); }, 3000);
    check(cleared, "the repaired still clears the sticky error on the SAME asset");
    check(gMediaErrors.empty(), "recovering the still pushes nothing new");
    const boost::json::object repairedMedia = mediaOf(status(fd));
    check(textOf(repairedMedia, "path") == missingStill,
          "the report still names the asset now on air — with no error attached to it");
}

// A clip whose file is destroyed while it plays.
// --- task 6.10 (review R12): the decoded-still cache through the wire ---
//
// scene_image_cache_tests pins the cache's accounting rules directly; this
// scenario proves the same properties through the LIVE worker: the counters
// ride output.status, a churn of DISTINCT 4K stills plateaus instead of
// growing, the store's oldest prepared picture survives that churn (the
// pin, not the LRU), and a same-path repair recovers even after the
// failure placeholder has been through the churn too.
//
// The arithmetic is written in absolute units (task 6.8b's law, and the
// review's own number): a 4K still decodes to 3840x2160x4 = 33,177,600
// bytes, the budget is 192 MiB = 201,326,592, and the scene store holds 16
// revisions. So 40 distinct 4K paths ASK 1.24 GiB of a cache that can hold
// at most 16 of them — and only ~6 of those 16 could survive the budget by
// recency alone, which is what makes the pin check below non-vacuous.
void exerciseImageCacheBudgets(int fd, const std::string& dir) {
    constexpr std::uint64_t kStillBytes = 3840ULL * 2160ULL * 4ULL;  // 31.6 MiB decoded
    constexpr int kRounds = 40;
    constexpr int kStore = 16;  // OutputSession::kMaxPreparedScenes

    // One 4K JPEG, copied to every churn path: the cache is keyed by PATH,
    // the scenario's subject is distinct paths, and 40 encodes would only
    // spend the fixture's time.
    const std::string still = dir + "/cache-4k.jpg";
    if (!generate("ffmpeg -v error -y -f lavfi -i testsrc=size=3840x2160:rate=1 "
                  "-frames:v 1 -q:v 8 '" +
                  still + "' >/dev/null 2>&1") ||
        !fileExists(still)) {
        std::printf("   skipped the cache-churn scenario (no 4K still)\n");
        return;
    }

    {  // The counters exist on the wire at all (the review's diagnostics half).
        const boost::json::object reported = imagesOf(status(fd));
        check(reported.contains("decodedBytes") && reported.contains("entries") &&
                  reported.contains("pins") && reported.contains("evictions"),
              "output.status.scene.images reports decodedBytes, entries, pins and evictions");
    }

    const auto churnTo = [&dir, &still, fd](int index) -> std::uint64_t {
        const std::string path = dir + "/churn-" + std::to_string(index) + ".jpg";
        std::error_code ec;
        std::filesystem::copy_file(
            still, path, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            std::fprintf(stderr, "FAIL: could not stage churn still %s\n", path.c_str());
            ++gFailures;
            return 0;
        }
        return preparePresent(fd, lyricScene("Churn " + std::to_string(index),
                                             "cache" + std::to_string(index), path, false));
    };

    std::vector<std::uint64_t> revisions;
    for (int i = 0; i < kRounds; ++i) {
        const std::uint64_t revision = churnTo(i);
        if (revision == 0) {
            return;  // preparePresent counted the failure
        }
        revisions.push_back(revision);
    }

    const boost::json::object after = imagesOf(status(fd));
    const double held = numberOf(after, "decodedBytes");
    const double asked = static_cast<double>(kRounds) * static_cast<double>(kStillBytes);
    std::printf("   cache: %d distinct 4K stills (%.0f MiB asked) -> %.0f MiB decoded, "
                "%.0f entries, %.0f pins, %.0f evictions\n",
                kRounds, asked / 1048576.0, held / 1048576.0, numberOf(after, "entries"),
                numberOf(after, "pins"), numberOf(after, "evictions"));
    check(held > 0.0 && held < asked / 2.0,
          "40 distinct 4K stills (1.24 GiB of decoded pixels asked) do NOT all stay "
          "resident — the growth the review measured is bounded");
    check(held <= static_cast<double>(kStore + 1) * static_cast<double>(kStillBytes),
          "what is held is bounded by the scene store's revisions plus the one path "
          "mid-load: the excess over the budget is pinned CONTENT, reported here rather "
          "than paid for with a colour fill");
    check(numberOf(after, "entries") <= kStore + 2,
          "the churned-away paths leave the map, they do not linger as weight");
    check(numberOf(after, "evictions") >= kRounds - kStore - 1,
          "the store's castoffs really were evicted, and the counter says so");
    check(numberOf(after, "pins") >= kStore - 1 && numberOf(after, "pins") <= kStore + 1,
          "the pins are the store's held scenes: sixteen distinct stills prepared, sixteen "
          "references — a session that pinned only the current picture (or nothing) reads "
          "single digits here");

    // The store keeps the NEWEST 16 revisions; the 192 MiB budget fits only
    // six 4K stills. So every held scene older than the newest ~6 survived
    // because the store PINS it. Present the OLDEST held one: if the pins
    // failed, its picture would be gone and the apply path would report a
    // media.error for a file that exists and decodes — the R18 report makes
    // a colour fill visible, which is exactly what makes this a check.
    const std::uint64_t oldestHeld = revisions[kRounds - kStore];
    gMediaErrors.clear();
    boost::json::value presentReply;
    const bool presented =
        call(fd, "scene.present",
             boost::json::object{{"revision", oldestHeld},
                                 {"transition", std::string("cut")}},
             &presentReply) &&
        presentReply.as_object().contains("result") && waitActive(fd, oldestHeld, nullptr);
    check(presented, "the oldest still-held scene presents after the churn");
    sleepMs(300);  // let a broken-still report ride one flush tick
    check(gMediaErrors.empty(),
          "the held scene's still came from the cache, not a colour fill: a media.error "
          "here would mean eviction took a prepared scene's picture despite its pin");
    check(textOf(mediaOf(status(fd)), "error").empty(),
          "and the media status is clean for the surviving picture");

    // --- the same-path repair, with the failure placeholder churned out ---
    // (exerciseImageRepair proves the immediate retry; this proves the
    // cache forgot the failure and the operator's fix still lands.)
    const std::string repair = dir + "/repair.jpg";
    ::unlink(repair.c_str());
    gMediaErrors.clear();
    const std::uint64_t broken =
        preparePresent(fd, lyricScene("Broken then fixed", "repair-a", repair, false));
    check(broken != 0 && waitActive(fd, broken, nullptr),
          "the scene whose still is missing still goes on air");
    sleepMs(300);
    check(!gMediaErrors.empty(),
          "the missing still reports media.error (the R18 surface, through this cache)");
    for (int i = kRounds; i < kRounds + 12; ++i) {
        if (churnTo(i) == 0) {
            return;
        }
    }
    std::error_code repairEc;
    std::filesystem::copy_file(still, repair, std::filesystem::copy_options::overwrite_existing,
                               repairEc);
    if (repairEc) {
        check(false, "the fixture could not restore the repaired still");
    } else {
        gMediaErrors.clear();
        const std::uint64_t fixed =
            preparePresent(fd, lyricScene("Broken then fixed", "repair-b", repair, false));
        check(fixed != 0 && waitActive(fd, fixed, nullptr), "the repaired scene presents");
        sleepMs(300);
        check(gMediaErrors.empty(),
              "a repaired file at the same path recovers after its failure placeholder "
              "went through the churn: the failure was a retryable placeholder, not a "
              "permanent verdict for that path");
        check(textOf(mediaOf(status(fd)), "error").empty(),
              "the media status is clean once the picture is back");
    }

    for (int i = 0; i < kRounds + 12; ++i) {
        ::unlink((dir + "/churn-" + std::to_string(i) + ".jpg").c_str());
    }
    ::unlink(repair.c_str());
    ::unlink(still.c_str());
}

void exerciseMidPlayFailure(int fd, const std::string& healthyClip) {
    // A private copy: this scenario destroys its file, and the fixture
    // clips are needed whole by every other scenario.
    const std::string victim = "background-media-fixtures/victim.mp4";
    if (!generate("cp '" + healthyClip + "' '" + victim + "'")) {
        check(false, "the victim clip could be staged (fixture failure)");
        return;
    }
    const std::uint64_t scene = preparePresent(fd, lyricScene("Doomed", "t2", victim, true));
    check(scene != 0 && waitActive(fd, scene, nullptr), "the doomed background goes on air");
    const bool rolling = waitMedia(
        fd,
        [](const boost::json::object& media) {
            return media.contains("playing") && media.at("playing").as_bool() &&
                   numberOf(media, "position") > 0.0;
        },
        6000);
    check(rolling, "the doomed background plays first (the failure has to be mid-play)");

    gMediaErrors.clear();
    check(::truncate(victim.c_str(), 0) == 0, "the file is destroyed while the transport runs");
    // The heartbeat is what reports this: the event-driven loop presents
    // nothing once the frames stop, so a status refreshed only by frames
    // would go on claiming playback forever.
    const bool reported = waitMedia(
        fd,
        [](const boost::json::object& media) {
            return !media.at("playing").as_bool() && !textOf(media, "error").empty();
        },
        4000);
    const boost::json::object dead = mediaOf(status(fd));
    check(reported, "a background whose file dies mid-play stops being reported as playing");
    check(!gMediaErrors.empty(), "and pushes media.error while it happens");
    if (!gMediaErrors.empty()) {
        check(textOf(gMediaErrors.front(), "path") == victim,
              "the notification names the file that broke");
    }
    check(textOf(dead, "path") == victim,
          "the sticky report still names the asset that was on air");
    // The refresh runs every heartbeat and every frame while this fault
    // stands. It reports ONCE (the isNew guard): a socket that re-announced
    // the same death 120 times a minute would be noise, not a signal.
    sleepMs(1200);
    check(gMediaErrors.size() == 1,
          "the standing fault is pushed once, not once per heartbeat");

    // Presenting a healthy background is the recovery.
    const std::uint64_t healed = preparePresent(fd, lyricScene("Healed", "t3", healthyClip, true));
    check(healed != 0 && waitActive(fd, healed, nullptr), "the healthy background takes the air");
    const bool recovered = waitMedia(
        fd,
        [](const boost::json::object& media) {
            return textOf(media, "error").empty() && media.at("playing").as_bool() &&
                   numberOf(media, "position") > 0.0;
        },
        6000);
    check(recovered, "a healthy background clears the mid-play failure and plays");
    ::unlink(victim.c_str());
}

// The lane of a clip that HAS audio. Which lane a machine reports depends
// on its audio server, so the assertion is the one R18 needs everywhere:
// never "none", and the error must match the lane.
void exerciseAudioLane(int fd, const std::string& audioClip) {
    const std::uint64_t scene = preparePresent(fd, lyricScene("Audio", "t4", audioClip, true));
    check(scene != 0 && waitActive(fd, scene, nullptr), "the audio-bearing background goes on air");
    // The head frame first (playing is a frame-time claim, 6.7c), then the
    // lane beside it — the pair is the report an operator reads.
    const bool rolling = waitMedia(
        fd,
        [](const boost::json::object& media) {
            return media.at("playing").as_bool() && numberOf(media, "position") > 0.0;
        },
        6000);
    check(rolling, "the audio-bearing background plays");
    const boost::json::object media = mediaOf(status(fd));
    const std::string lane = textOf(media, "audio");
    const std::string error = textOf(media, "error");
    const bool faultLane = lane == "degraded" || lane == "no-output";
    check(lane != "none", "a clip that carries audio never reports the none lane (R18)");
    std::printf("   audio lane for a clip with sound: %s (%s)\n", lane.c_str(),
                error.empty() ? "no error" : error.c_str());
    check(lane == "live" || lane == "paused" || lane == "silent" || faultLane,
          "the reported lane is one of the protocol's six");
    if (faultLane) {
        check(!error.empty() && error.find("audio") != std::string::npos,
              "a fault lane carries a message naming the audio");
    } else {
        check(error.empty(), "a healthy lane reports no error");
    }
}

// Recovery is typed, and the per-frame refresh is what makes that matter:
// a healthy background on air reconciles its own audio lane every frame,
// and that must not erase the report of an asset that is still broken —
// here a prepared-but-unchanged slide whose still cannot be decoded.
void exerciseFaultsDoNotCrossAssets(int fd, const std::string& healthyClip,
                                    const std::string& missingStill) {
    // Put a healthy video on air and let it settle: its refresh is running
    // continuously by the time the queued fault appears.
    const std::uint64_t onAir = preparePresent(fd, lyricScene("Healthy", "x1", healthyClip, true));
    check(onAir != 0 && waitActive(fd, onAir, nullptr), "the healthy background goes on air");
    check(waitMedia(
              fd,
              [](const boost::json::object& media) {
                  return media.at("playing").as_bool() && numberOf(media, "position") > 0.0;
              },
              6000),
          "the healthy background plays");

    // Prepare (do NOT present) a slide whose still cannot be decoded: the
    // preload reports it while the healthy video is still what the audience
    // sees.
    gMediaErrors.clear();
    boost::json::value reply;
    check(call(fd, "scene.prepare", lyricScene("Queued broken", "x2", missingStill, false),
               &reply) &&
              reply.as_object().contains("result"),
          "the scene with the undecodable still is accepted into the store");
    sleepMs(300);
    const boost::json::object afterQueue = mediaOf(status(fd));
    check(!textOf(afterQueue, "error").empty(),
          "a prepared slide's broken still is reported before it is presented");
    check(textOf(afterQueue, "error").find("image") != std::string::npos,
          "the standing report is the IMAGE fault");
    check(afterQueue.at("playing").as_bool(),
          "and the healthy background keeps playing while it stands");

    // Frames and heartbeats keep reconciling the on-air transport. None of
    // them may clear a fault they do not describe.
    sleepMs(1200);
    const boost::json::object still = mediaOf(status(fd));
    check(textOf(still, "error").find("image") != std::string::npos,
          "the on-air transport's healthy lane does NOT clear another asset's fault (R18)");
    check(textOf(still, "path") == missingStill, "and the report keeps naming the broken asset");
    gMediaErrors.clear();
}

// A worker whose audio mixer refuses to start (the 6.7a fault knob): the
// picture must roll on the monotonic clock, the lane must say so, and a
// silent clip must clear the report because it lost nothing.
void exerciseAudioStartFailure(const std::string& audioClip, const std::string& silentClip) {
    const std::string socket = socketPath("audiofault");
    ::unlink(socket.c_str());
    const pid_t worker = spawnWorker(socket, {"OPENSHOW_FAULT_AUDIO_MIXER_FAIL=1"});
    if (worker < 0) {
        check(false, "the audio-fault worker could be spawned");
        return;
    }
    const int fd = connectRetry(socket);
    if (fd < 0) {
        check(false, "the audio-fault worker never answered its socket");
        kill(worker, SIGTERM);
        return;
    }
    boost::json::value reply;
    check(call(fd, "hello",
               proto::helloParams("openshow-background-media", proto::kProtocolMajor,
                                  proto::kProtocolMinor),
               &reply) &&
              reply.as_object().contains("result"),
          "handshake with the audio-fault worker");

    gMediaErrors.clear();
    const std::uint64_t scene = preparePresent(fd, lyricScene("No mixer", "f1", audioClip, true));
    check(scene != 0 && waitActive(fd, scene, nullptr),
          "the video plays on even though its audio could not start");
    // Frames first: "playing" is the frame-time claim it now is, and a
    // muted transport still has to earn it by submitting.
    const bool rolling = waitMedia(
        fd,
        [](const boost::json::object& media) {
            return media.at("playing").as_bool() && numberOf(media, "position") > 0.0;
        },
        6000);
    check(rolling, "the muted picture still plays (the audio's failure is not the video's)");
    const bool degraded = waitMedia(
        fd, [](const boost::json::object& media) { return textOf(media, "audio") == "degraded"; },
        4000);
    const boost::json::object media = mediaOf(status(fd));
    check(degraded, "a refused audio start reports the degraded lane");
    check(!textOf(media, "error").empty() &&
              textOf(media, "error").find("audio") != std::string::npos,
          "and carries a message naming what the audience is missing");
    check(!gMediaErrors.empty(), "the degradation is pushed, not only pollable");

    // A silent clip loses nothing, so its report is clean: recovery is
    // typed to the class that actually recovered.
    const std::uint64_t silent = preparePresent(fd, lyricScene("Silent", "f2", silentClip, true));
    check(silent != 0 && waitActive(fd, silent, nullptr), "the silent background takes the air");
    const bool clean = waitMedia(
        fd,
        [](const boost::json::object& media) {
            return textOf(media, "error").empty() && textOf(media, "audio") == "none";
        },
        4000);
    check(clean, "a clip with no audio clears the audio fault (it has nothing to be missing)");

    boost::json::value shutdownReply;
    check(call(fd, "shutdown", {}, &shutdownReply), "the audio-fault worker answers shutdown");
    close(fd);
    int waitStatus = 0;
    waitpid(worker, &waitStatus, 0);
    check(WIFEXITED(waitStatus) && WEXITSTATUS(waitStatus) == 0,
          "the audio-fault worker exits cleanly");
    ::unlink(socket.c_str());
}

// --- task 6.7d (review R19): seeks through the whole session ----------------
//
// R19 asks for the hard seek cases "through the actual session rather than
// only a collecting SubmitFn": a burst whose newest request must not be
// swallowed, a backward seek after the clip has looped, and a seek issued
// while the transport is held — plus a jump the media REFUSES, which has to be
// reported instead of quietly kept. Transport control has no RPC yet (that is
// 6.15a's row), so the scenarios arm a schedule on the worker
// (OPENSHOW_TEST_TRANSPORT_OPS) which the render loop applies through the same
// seam requestMediaSeek/requestMediaPause/requestMediaPlay will use, and read
// back what the audience would see: output.status.media plus the pushed
// media.error notifications.

// A worker that owns its environment — the fault knobs and the transport
// schedule are read at spawn — handshaken and ready to take scenes.
int spawnScopedWorker(const char* tag, const std::vector<std::string>& extraEnv,
                      std::string* socketOut, pid_t* pidOut) {
    const std::string socket = socketPath(tag);
    ::unlink(socket.c_str());
    const pid_t worker = spawnWorker(socket, extraEnv);
    if (worker < 0) {
        return -1;
    }
    const int fd = connectRetry(socket);
    if (fd < 0) {
        kill(worker, SIGTERM);
        return -1;
    }
    boost::json::value reply;
    if (!call(fd, "hello",
              proto::helloParams("openshow-background-media", proto::kProtocolMajor,
                                 proto::kProtocolMinor),
              &reply) ||
        !reply.as_object().contains("result")) {
        close(fd);
        kill(worker, SIGTERM);
        return -1;
    }
    *socketOut = socket;
    *pidOut = worker;
    return fd;
}

void retireScopedWorker(int fd, pid_t worker, const std::string& socket, const char* what) {
    boost::json::value shutdownReply;
    check(call(fd, "shutdown", {}, &shutdownReply), what);
    close(fd);
    int waitStatus = 0;
    waitpid(worker, &waitStatus, 0);
    check(WIFEXITED(waitStatus) && WEXITSTATUS(waitStatus) == 0,
          "and the same worker exits cleanly");
    ::unlink(socket.c_str());
}

// One status.media sample on the scenario's own clock.
struct MediaSample {
    double tMs = 0.0;
    double position = -1.0;
    bool playing = false;
    std::string error;
};

std::vector<MediaSample> sampleMedia(int fd, int durationMs, int stepMs = 100) {
    std::vector<MediaSample> samples;
    const auto began = std::chrono::steady_clock::now();
    for (int waited = 0; waited < durationMs; waited += stepMs) {
        const boost::json::object media = mediaOf(status(fd));
        MediaSample sample;
        sample.tMs = std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - began)
                         .count();
        sample.position = numberOf(media, "position");
        sample.playing = media.contains("playing") && media.at("playing").as_bool();
        sample.error = textOf(media, "error");
        samples.push_back(sample);
        sleepMs(stepMs);
    }
    return samples;
}

// The schedule below, in milliseconds after the transport opened. Keeping the
// numbers here means the assertions and the worker's script cannot drift.
constexpr double kForwardMs = 1200.0;
constexpr double kForwardTarget = 5.6;
constexpr double kBackwardMs = 3000.0;
constexpr double kBackwardTarget = 1.0;
constexpr double kBackwardClipLength = 6.0;  // the clip: target+clip is the bug
constexpr double kBurstMs = 4600.0;          // two requests 30 ms apart
constexpr double kBurstFirst = 3.0;
constexpr double kBurstLast = 0.5;
// The hold is wide enough to sample on purpose: 100 ms sampling needs several
// samples before the parked seek (whose scrub then moves the position) and
// after the resume, and a 100 ms window would assert on luck.
constexpr double kPauseMs = 6000.0;
constexpr double kParkedSeekMs = 7100.0;
constexpr double kParkedTarget = 4.0;
constexpr double kPlayMs = 8000.0;

// The R19 acceptance through the session: every request is honoured, the
// pixels and the reported position follow the media position (never
// target+clip), and a held transport scrubs to its seek rather than losing it.
void exerciseSeekThroughSession(const std::string& clip) {
    const int failuresBefore = gFailures;
    std::string socket;
    pid_t worker = -1;
    const std::string spec =
        "seek@" + std::to_string(static_cast<int>(kForwardMs)) + ":" + std::to_string(kForwardTarget)
        + ",seek@" + std::to_string(static_cast<int>(kBackwardMs)) + ":"
        + std::to_string(kBackwardTarget) + ",seek@" + std::to_string(static_cast<int>(kBurstMs))
        + ":" + std::to_string(kBurstFirst) + ",seek@" + std::to_string(static_cast<int>(kBurstMs + 30))
        + ":" + std::to_string(kBurstLast) + ",pause@" + std::to_string(static_cast<int>(kPauseMs))
        + ",seek@" + std::to_string(static_cast<int>(kParkedSeekMs)) + ":"
        + std::to_string(kParkedTarget) + ",play@" + std::to_string(static_cast<int>(kPlayMs));
    const int fd = spawnScopedWorker("seekops", {"OPENSHOW_TEST_TRANSPORT_OPS=" + spec}, &socket,
                                     &worker);
    if (fd < 0) {
        check(false, "the seek-schedule worker could be spawned and handed its script");
        return;
    }
    gMediaErrors.clear();
    const std::uint64_t scene = preparePresent(fd, lyricScene("Seek ops", "d1", clip, true));
    check(scene != 0 && waitActive(fd, scene, nullptr), "the seek-scenario background goes on air");
    // Sampling starts here, and the transport opened at the present, so the
    // samples share the script's timeline to within a frame.
    const std::vector<MediaSample> samples = sampleMedia(fd, 9000);

    // The schedule is self-anchoring: the forward jump to kForwardTarget is
    // unmistakable in the series, and it names the script's origin.
    size_t anchor = 0;
    bool anchored = false;
    for (size_t i = 0; i < samples.size(); ++i) {
        if (samples[i].position > kForwardTarget - 0.4) {
            anchor = i;
            anchored = true;
            break;
        }
    }
    check(anchored,
          "the scheduled forward seek presents near its target (nothing swallowed at the seam)");
    if (!anchored) {
        retireScopedWorker(fd, worker, socket, "the seek-schedule worker answers shutdown");
        return;
    }
    const double originMs = samples[anchor].tMs;  // == kForwardMs, as observed
    auto rel = [originMs](const MediaSample& sample) { return sample.tMs - originMs + kForwardMs; };

    // 1. The clip must actually loop before the backward seek means anything:
    //    the loop-continuous position climbs past the clip length.
    bool wrapped = false;
    size_t wrapAt = samples.size();
    for (size_t i = anchor + 1; i < samples.size(); ++i) {
        if (samples[i].position > kBackwardClipLength + 0.2) {
            wrapped = true;
            wrapAt = i;
            break;
        }
    }
    check(wrapped, "the transport looped, so the wrap continuity is in play before the seek back");

    // 2. The backward seek after the wrap: the position returns to the target,
    //    never to target + clip (R19's freeze: the picture held for a clip
    //    because the wrap base survived the seek).
    double minAfterBackward = 1e9;
    double maxAfterBackward = -1e9;
    size_t sawTarget = 0;
    for (size_t i = wrapAt; i < samples.size(); ++i) {
        const double at = rel(samples[i]);
        if (at < kBackwardMs + 400.0 || at > kBurstMs - 150.0) {
            continue;
        }
        minAfterBackward = std::min(minAfterBackward, samples[i].position);
        maxAfterBackward = std::max(maxAfterBackward, samples[i].position);
        if (samples[i].position >= kBackwardTarget - 0.15 &&
            samples[i].position <= kBackwardTarget + 1.2) {
            ++sawTarget;
        }
    }
    check(sawTarget > 0, "the backward seek after the wrap presents the TARGET");
    check(minAfterBackward < kBackwardTarget + 1.0 && maxAfterBackward < kBackwardClipLength,
          "and never the target plus a clip");

    // 3. The burst: two requests 30 ms apart. The newest is the audience's
    //    request; sitting on the first one (or on the pre-burst position) is
    //    the swallow R19 names.
    double minBurst = 1e9;
    double maxBurst = -1e9;
    size_t burstSamples = 0;
    for (const auto& sample : samples) {
        const double at = rel(sample);
        if (at < kBurstMs + 250.0 || at > kPauseMs - 300.0) {
            continue;
        }
        ++burstSamples;
        minBurst = std::min(minBurst, sample.position);
        maxBurst = std::max(maxBurst, sample.position);
    }
    check(burstSamples >= 3, "the burst window was sampled");
    check(maxBurst < kBurstFirst - 0.8,
          "the burst lands on its NEWEST target, not on the superseded one");
    check(minBurst < kBurstLast + 0.7, "and the newest target is really where the media is");

    // 4. The hold: playing goes false, the position stops, and the fault
    //    report stays empty — a parked transport is not a broken one. The
    //    window ends where the parked seek is issued; after it the position
    //    moves on purpose (assertion 5), so including it here would contradict
    //    the hold check below.
    bool parkedSawPlaying = false;
    double parkedMin = 1e9;
    double parkedMax = -1e9;
    size_t parkedSamples = 0;
    for (const auto& sample : samples) {
        const double at = rel(sample);
        if (at < kPauseMs + 200.0 || at > kParkedSeekMs - 200.0) {
            continue;
        }
        ++parkedSamples;
        parkedSawPlaying = parkedSawPlaying || sample.playing;
        parkedMin = std::min(parkedMin, sample.position);
        parkedMax = std::max(parkedMax, sample.position);
        if (!sample.error.empty()) {
            check(false, "a held transport reports no fault");
            break;
        }
    }
    check(parkedSamples >= 3, "the held window was sampled");
    check(!parkedSawPlaying, "a held background is not reported as playing");
    check(parkedMax - parkedMin < 0.05, "and its position is held while parked");

    // 5. The seek issued while parked SCRUBS: a held transport shows where the
    //    media is now, so the position moves to the target while playing stays
    //    false (the clock is frozen; the picture is not). It is also the
    //    resume's position — swallowed, playback would continue from the
    //    parked value (about 6.9 s here — a wrapped hold) instead of the target.
    bool parkedSeekScrubbed = false;
    double scrubMin = 1e9;
    double scrubMax = -1e9;
    size_t scrubSamples = 0;
    for (const auto& sample : samples) {
        const double at = rel(sample);
        if (at < kParkedSeekMs + 400.0 || at > kPlayMs - 200.0) {
            continue;
        }
        ++scrubSamples;
        scrubMin = std::min(scrubMin, sample.position);
        scrubMax = std::max(scrubMax, sample.position);
        if (sample.position >= kParkedTarget - 0.15 && !sample.playing) {
            parkedSeekScrubbed = true;
        }
    }
    check(scrubSamples >= 3, "the window between the parked seek and the resume was sampled");
    check(parkedSeekScrubbed,
          "a seek issued while held presents its target: the held picture scrubs");
    check(scrubMax - scrubMin < 0.5,
          "and scrubbing does not unfreeze the transport: it parks at the target");

    // 6. The resume: play continues FROM the scrubbed target.
    const MediaSample* firstAfterPlay = nullptr;
    double maxAfterPlay = -1e9;
    bool resumedPlaying = false;
    for (const auto& sample : samples) {
        if (rel(sample) < kPlayMs + 300.0) {
            continue;
        }
        if (firstAfterPlay == nullptr) {
            firstAfterPlay = &sample;
        }
        resumedPlaying = resumedPlaying || sample.playing;
        maxAfterPlay = std::max(maxAfterPlay, sample.position);
    }
    check(firstAfterPlay != nullptr && firstAfterPlay->position >= kParkedTarget - 0.1,
          "the seek issued while parked IS the position playback resumes from");
    check(resumedPlaying, "and the transport plays again after the held window");
    check(maxAfterPlay < kBackwardClipLength,
          "the resumed position stays in the clip (no target+clip on resume)");

    // Throughout: R19's seeks are refused only by the media, and this clip is
    // whole, so nothing may have been reported.
    for (const auto& sample : samples) {
        if (!sample.error.empty()) {
            check(false, "no scheduled seek on a readable clip reports a fault");
            break;
        }
    }
    check(gMediaErrors.empty(), "and the whole seek script pushes no media.error");
    std::printf("   seek samples=%zu: min after backward %.2f, burst %.2f..%.2f, parked %.2f..%.2f, "
                "scrubbed %.2f..%.2f, resumed from %.2f\n",
                samples.size(), minAfterBackward, minBurst, maxBurst, parkedMin, parkedMax, scrubMin,
                scrubMax, firstAfterPlay != nullptr ? firstAfterPlay->position : -1.0);
    if (gFailures > failuresBefore) {  // the whole series is the diagnosis, only when one failed
        std::printf("   seek series:");
        for (const auto& sample : samples) {
            std::printf(" %s%.1f", sample.playing ? "" : "P", sample.position);
        }
        std::printf("\n");
    }
    retireScopedWorker(fd, worker, socket, "the seek-schedule worker answers shutdown");
}

// A jump the audio REFUSES. Two workers run the same script: with the fault
// knob armed the session must tell the operator, and with it disarmed the same
// script must be silent — so the report is about the refusal, not about the
// code path that notices it.
void exerciseSeekRefusal(const std::string& audioClip) {
    constexpr double kRefusedMs = 2500.0;
    constexpr double kRefusedTarget = 0.5;
    const std::string spec =
        "seek@" + std::to_string(static_cast<int>(kRefusedMs)) + ":" + std::to_string(kRefusedTarget);
    for (int pass = 0; pass < 2; ++pass) {
        const bool faulted = pass == 0;
        const int failuresBefore = gFailures;
        std::vector<std::string> env = {"OPENSHOW_TEST_TRANSPORT_OPS=" + spec};
        if (faulted) {
            env.push_back("OPENSHOW_FAULT_AUDIO_SEEK_FAIL=1");
        }
        std::string socket;
        pid_t worker = -1;
        const int fd = spawnScopedWorker(faulted ? "seekrefuse" : "seekok", env, &socket, &worker);
        if (fd < 0) {
            check(false, "the seek-refusal worker could be spawned");
            return;
        }
        const std::uint64_t scene = preparePresent(fd, lyricScene("Seek refuse", "d2", audioClip, true));
        check(scene != 0 && waitActive(fd, scene, nullptr),
              faulted ? "the faulted-audio-seek background goes on air"
                      : "the clean-audio-seek background goes on air");
        const bool rolling = waitMedia(
            fd,
            [](const boost::json::object& media) {
                return media.contains("playing") && media.at("playing").as_bool() &&
                       numberOf(media, "position") > 0.0;
            },
            6000);
        check(rolling, faulted ? "the picture plays before the refused jump"
                               : "the picture plays before the clean jump");
        if (!rolling) {
            retireScopedWorker(fd, worker, socket, "the seek-refusal worker answers shutdown");
            continue;
        }
        // The lane decides what a sticky error MEANS on this machine: a
        // healthy lane leaves the sync fault as the only report on file, while
        // a machine without an audio server has its own (typed) complaint to
        // make first. The refusal assertions differ accordingly.
        const std::string laneBefore = textOf(mediaOf(status(fd)), "audio");
        const bool laneFault = laneBefore == "degraded" || laneBefore == "no-output";
        gMediaErrors.clear();
        const std::vector<MediaSample> samples = sampleMedia(fd, 6000);
        if (gFailures > failuresBefore) {  // a whole series is a diagnosis, not a report
            std::printf("   %s seek series:", faulted ? "faulted" : "clean");
            for (const auto& sample : samples) {
                std::printf(" %.1f", sample.position);
            }
            std::printf("\n");
        }

        size_t refusals = 0;
        for (const auto& notification : gMediaErrors) {
            if (textOf(notification, "message").find("refused") != std::string::npos) {
                ++refusals;
            }
        }
        bool landedOnTarget = false;
        bool everPlaying = false;
        std::string lastError;
        for (const auto& sample : samples) {
            everPlaying = everPlaying || sample.playing;
            if (!sample.error.empty()) {
                lastError = sample.error;
            }
            if (sample.tMs > kRefusedMs + 500.0 && sample.position >= kRefusedTarget - 0.15 &&
                sample.position <= kRefusedTarget + 1.5) {
                landedOnTarget = true;
            }
        }
        if (faulted) {
            check(refusals >= 1,
                  "a jump the audio refused pushes media.error naming the refusal");
            check(lastError.find("refused") != std::string::npos || laneFault,
                  "and the sticky report names the refusal (an unhealthy lane owns the text)");
            check(everPlaying,
                  "a refused jump does not stop the picture: the audience still has the video");
            check(landedOnTarget,
                  "the picture does move to the target — which is why the sync is the fault");
            // The healthy-lane reconcile runs every frame and every heartbeat.
            // A sync fault is NOT its to clear: the audio is healthy, just
            // elsewhere, so the report must outlive both refreshes.
            const size_t after = gMediaErrors.size();
            sleepMs(1500);
            const boost::json::object standing = mediaOf(status(fd));
            check(textOf(standing, "error").find("refused") != std::string::npos || laneFault,
                  "the standing sync fault survives the lane reconcile and the heartbeat");
            check(after == gMediaErrors.size(),
                  "and it is reported once, not once per reconcile");
        } else {
            check(refusals == 0, "the same script with a working seek pushes no refusal");
            check(lastError.empty(), "and status.media.error stays empty");
            check(landedOnTarget, "the clean transport's seek lands on the target through the session");
        }
        std::printf("   %s seek script: lane %s, %zu refusal notification(s), error '%s'\n",
                    faulted ? "faulted" : "clean", laneBefore.c_str(), refusals,
                    lastError.empty() ? "none" : lastError.c_str());
        retireScopedWorker(fd, worker, socket, "the seek-refusal worker answers shutdown");
    }
}

// --- task 6.15a (review R15): the transport over the socket -------------------
//
// R15: "Do not count a passing component benchmark as proof of operator
// functionality." The 6.7d scenarios above drive the transport through a
// spawn-time schedule — precise, but still not the operator's door. These
// scenarios drive the SAME session through the media.* RPCs the frontend
// now sends: validation at the router, acknowledgement in output.status,
// per-source gain/mute, the loop-off final-frame hold, and a refused seek
// reported — every one over the real worker, real socket, real transport.

// The status source entry of one identity, or null when the transport
// does not list it.
const boost::json::object* findSource(const boost::json::object& media, const char* name) {
    const auto sources = media.find("sources");
    if (sources == media.end() || !sources->value().is_array()) {
        return nullptr;
    }
    for (const boost::json::value& entry : sources->value().as_array()) {
        if (entry.is_object() && entry.as_object().contains("name") &&
            entry.as_object().at("name").is_string() &&
            entry.as_object().at("name").as_string() == name) {
            return &entry.as_object();
        }
    }
    return nullptr;
}

// Live transport control of a silent 6 s clip: commands with nothing on
// air refuse, the 6.15a status fields appear, pause holds, resume
// continues, a seek is ACKNOWLEDGED by the position status reports, and
// loop off ends the clip once — with the final frame held and no fault
// (a finished clip is not a broken one).
void exerciseRpcTransport(const std::string& videoA) {
    std::string socket;
    pid_t worker = -1;
    const int fd = spawnScopedWorker("rpctransport", {}, &socket, &worker);
    if (fd < 0) {
        check(false, "the rpc-transport worker could be spawned and handshaken");
        return;
    }
    boost::json::value reply;

    // Before any transport exists, every command is refused naming the
    // reason — an RPC that evaporates is R19's swallowed op again, one
    // layer up.
    check(call(fd, proto::kMethodMediaPause, {}, &reply) && reply.as_object().contains("error") &&
              textOf(reply.at("error").as_object(), "message").find("no background media") !=
                  std::string::npos,
          "pause with nothing on air is refused by name, not swallowed");
    check(call(fd, proto::kMethodMediaSeek, boost::json::object{{"position", "now"}}, &reply) &&
              reply.as_object().contains("error") &&
              textOf(reply.at("error").as_object(), "message").find("position") !=
                  std::string::npos,
          "a non-numeric seek position is a parameter error");
    check(call(fd, proto::kMethodMediaSeek, boost::json::object{{"position", -2.0}}, &reply) &&
              reply.as_object().contains("error") &&
              textOf(reply.at("error").as_object(), "message").find("position") !=
                  std::string::npos,
          "and so is a negative one — the router does not clamp nonsense into 0");
    check(call(fd, proto::kMethodMediaLoop, boost::json::object{}, &reply) &&
              reply.as_object().contains("error"),
          "media.loop without its boolean is a parameter error");
    // Valid params, no transport: each command still refuses. Every gate
    // gets its own witness so a dead refusal cannot hide behind a sibling
    // command's check.
    check(call(fd, proto::kMethodMediaSeek, boost::json::object{{"position", 3.0}}, &reply) &&
              reply.as_object().contains("error"),
          "a well-formed seek with nothing on air is refused, not parked for later");
    check(call(fd, proto::kMethodMediaLoop, boost::json::object{{"loop", false}}, &reply) &&
              reply.as_object().contains("error"),
          "a well-formed loop toggle with nothing on air is refused");
    check(call(fd, proto::kMethodMediaGain, boost::json::object{{"source", "video"}, {"gain", 0.5}},
               &reply) &&
              reply.as_object().contains("error") &&
              textOf(reply.at("error").as_object(), "message").find("no background media") !=
                  std::string::npos,
          "gain names its reason when no transport could hold it");
    {  // The enablement signal itself: path would say nothing here (it is
        // empty until the first bring-up), live is the honest no.
        const boost::json::object idle = mediaOf(status(fd));
        const auto live = idle.find("live");
        check(live != idle.end() && live->value().is_bool() && !live->value().as_bool(),
              "before any transport, status says the media is not live");
    }

    const std::uint64_t scene = preparePresent(fd, lyricScene("RPC transport", "t1", videoA, true));
    check(scene != 0 && waitActive(fd, scene, nullptr), "the transport-test background goes on air");
    const bool rolling = waitMedia(
        fd,
        [](const boost::json::object& media) {
            return media.contains("playing") && media.at("playing").as_bool() &&
                   numberOf(media, "position") > 0.0;
        },
        6000);
    check(rolling, "frames roll before the commands start");

    // The acknowledgement fields exist and start honest.
    const boost::json::object up = mediaOf(status(fd));
    check(up.contains("live") && up.at("live").as_bool(), "status reports the transport live");
    check(up.contains("loop") && up.at("loop").as_bool(), "backgrounds loop by default");
    const double duration = numberOf(up, "duration");
    check(duration > 5.5 && duration < 6.5, "duration reports the 6 s clip");
    check(findSource(up, "video") == nullptr,
          "a clip whose container has no audio stream lists no source — absent, not fake-default");

    // Hold. Status confirms, and the position is parked while held.
    check(call(fd, proto::kMethodMediaPause, {}, &reply) && reply.as_object().contains("result") &&
              reply.at("result").as_object().at("pending").as_bool(),
          "the worker acknowledges the hold as pending");
    const bool held = waitMedia(
        fd,
        [](const boost::json::object& media) {
            return !media.at("playing").as_bool() && numberOf(media, "position") >= 0.0;
        },
        4000);
    check(held, "status reports the hold: paused is not playing");
    const double parkedAt = numberOf(mediaOf(status(fd)), "position");
    sleepMs(500);
    check(numberOf(mediaOf(status(fd)), "position") == parkedAt,
          "a held transport keeps exactly its position — the audience sees one frozen frame");

    // Resume continues from the parked position; a restart at zero would
    // be a re-open wearing a resume's clothes.
    check(call(fd, proto::kMethodMediaPlay, {}, &reply) && reply.as_object().contains("result"),
          "the worker acknowledges the resume");
    const bool resumed = waitMedia(
        fd,
        [parkedAt](const boost::json::object& media) {
            return media.at("playing").as_bool() &&
                   numberOf(media, "position") > parkedAt + 0.15;
        },
        3000);
    check(resumed, "play resumes from the hold, not from the top");
    const double movingAt = numberOf(mediaOf(status(fd)), "position");
    check(movingAt < parkedAt + 2.5, "and nothing teleported past the hold");

    // The acknowledged position: the target rides the reply, and the
    // position status then reports is the target — this is the
    // "acknowledged position" of the acceptance, over the real transport.
    check(call(fd, proto::kMethodMediaSeek, boost::json::object{{"position", 5.6}}, &reply) &&
              reply.as_object().contains("result") &&
              std::abs(numberOf(reply.at("result").as_object(), "target") - 5.6) < 0.01,
          "the seek reply carries the target the worker took");
    const bool landed = waitMedia(
        fd,
        [](const boost::json::object& media) {
            const double position = numberOf(media, "position");
            return position >= 5.4 && position <= 6.05;
        },
        3000);
    check(landed, "status acknowledges the forward seek at the target");
    check(call(fd, proto::kMethodMediaSeek, boost::json::object{{"position", 1.0}}, &reply) &&
              reply.as_object().contains("result"),
          "the backward seek is taken too");
    const bool landedBack = waitMedia(
        fd,
        [](const boost::json::object& media) {
            const double position = numberOf(media, "position");
            return position >= 0.6 && position <= 2.3;
        },
        3000);
    check(landedBack, "and status acknowledges the backward one — the position, not target+clip");

    // Loop off, then the end: once, with no fault, and the final frame
    // held. (The pixels side is pinned below the session by
    // playback_control_tests' loopThenOff; this is the session's promise:
    // playing:false, error:"", position resting at the end.)
    gMediaErrors.clear();
    check(call(fd, proto::kMethodMediaLoop, boost::json::object{{"loop", false}}, &reply) &&
              reply.as_object().contains("result") &&
              reply.at("result").as_object().at("loop").as_bool() == false,
          "loop off is acknowledged");
    const bool looped = waitMedia(
        fd, [](const boost::json::object& media) {
            return media.contains("loop") && !media.at("loop").as_bool();
        },
        3000);
    check(looped, "status carries the mode the EOF will honour");
    check(call(fd, proto::kMethodMediaSeek, boost::json::object{{"position", 5.2}}, &reply) &&
              reply.as_object().contains("result"),
          "seek near the end to keep the wait short");
    const bool ended = waitMedia(
        fd,
        [](const boost::json::object& media) { return !media.at("playing").as_bool(); },
        5000);
    check(ended, "with loop off the clip ENDS: playing goes false");
    const boost::json::object atEnd = mediaOf(status(fd));
    check(textOf(atEnd, "error").empty(),
          "a finished clip is not a broken one — no fault is implied by the end");
    check(numberOf(atEnd, "position") > 5.5, "the position rests at the end of the clip");
    // Frames decoded before the end keep pacing out for a breath after the
    // EOF flag flips, so the position settles on the LAST frame's pts a
    // moment after "not playing" first reads. The promise to test is the
    // hold — two status reads agree and keep agreeing — not the instant.
    bool stable = false;
    double settled = -1.0;
    for (int waited = 0; waited < 2000 && !stable; waited += 120) {
        sleepMs(120);
        const boost::json::object first = mediaOf(status(fd));
        sleepMs(140);
        const boost::json::object second = mediaOf(status(fd));
        if (!second.at("playing").as_bool() && numberOf(second, "position") >= 0.0 &&
            std::abs(numberOf(second, "position") - numberOf(first, "position")) < 0.001) {
            settled = numberOf(second, "position");
            stable = true;
        }
    }
    check(stable, "the final frame holds: status keeps reporting one settled end position");
    check(settled > 5.5, "and the settled position is the end of the clip");
    check(gMediaErrors.empty(),
          "and nothing pushed an 'ended while looping' fault — that fault is for loop ON");
    check(call(fd, "ping", {}, &reply) && reply.as_object().contains("result"),
          "the worker lives on after the held end, ready for the next command");

    // And back the other way. A background-less scene tears the transport
    // down; live must follow it into the status while the path stays sticky
    // (5.5d's pairing rule), and the commands must be refused again. This
    // is exactly why the controls enable on live and never on the path: a
    // stale path would leave a dead transport looking commandable.
    const std::uint64_t cleared = preparePresent(fd, lyricScene("Cleared", "t2", "", true));
    check(cleared != 0 && waitActive(fd, cleared, nullptr),
          "the background-less scene goes on air");
    const bool offAir = waitMedia(
        fd, [](const boost::json::object& media) {
            const auto live = media.find("live");
            return live != media.end() && live->value().is_bool() && !live->value().as_bool();
        },
        4000);
    check(offAir, "teardown takes live with it");
    const boost::json::object afterTeardown = mediaOf(status(fd));
    // (The no-asset apply clears path by 5.5d's own rule — path names what
    // the audience is looking at, and that is now nothing. The mediaStopped
    // flavor of teardown, a failed asset, keeps the path instead so the
    // sticky error stays paired with the file it describes; both are why
    // the controls enable on live and never on the path.)
    check(textOf(afterTeardown, "path").empty(),
          "with no asset on air no path is claimed");
    check(findSource(afterTeardown, "video") == nullptr,
          "and no audio source survives the transport that registered it");
    check(call(fd, proto::kMethodMediaPause, {}, &reply) && reply.as_object().contains("error"),
          "commands against the torn-down transport are refused, never queued for a successor");
    std::printf("   rpc transport: duration %.2f, parked %.2f, ended at %.2f, settled %.2f, %zu fault push(es)\n",
                duration, parkedAt, numberOf(atEnd, "position"), settled, gMediaErrors.size());
    retireScopedWorker(fd, worker, socket, "the rpc-transport worker answers shutdown");
}

// Per-source gain/mute over the socket, against a clip WITH an audio
// stream: the source appears, the mix echo acknowledges every change,
// and the sources the transport does not have (standalone — 6.15b's
// work) refuse by name.
void exerciseRpcGain(const std::string& audioClip) {
    std::string socket;
    pid_t worker = -1;
    const int fd = spawnScopedWorker("rpcgain", {}, &socket, &worker);
    if (fd < 0) {
        check(false, "the rpc-gain worker could be spawned and handshaken");
        return;
    }
    boost::json::value reply;
    const std::uint64_t scene = preparePresent(fd, lyricScene("RPC gain", "g1", audioClip, true));
    check(scene != 0 && waitActive(fd, scene, nullptr), "the audio-bearing background goes on air");
    const bool listed = waitMedia(
        fd, [](const boost::json::object& media) { return findSource(media, "video") != nullptr; },
        6000);
    check(listed, "a clip with a decodable audio stream registers its 'video' source");
    if (!listed) {
        retireScopedWorker(fd, worker, socket, "the rpc-gain worker answers shutdown");
        return;
    }
    const boost::json::object started = mediaOf(status(fd));
    const boost::json::object* source = findSource(started, "video");
    check(source != nullptr && std::abs(numberOf(*source, "gain") - 1.0) < 0.01 &&
              source->at("muted").as_bool() == false,
          "the fresh source starts at unity, unmuted");
    check(findSource(started, "standalone") == nullptr,
          "and no standalone source exists yet — that is 6.15b's track");

    // A source the transport never registered is a REFUSAL, not a
    // no-op: the AvPlayer's unregistered-identity passthrough stays
    // below the wire; over the socket nothing evaporates.
    check(call(fd, proto::kMethodMediaGain,
               boost::json::object{{"source", "standalone"}, {"gain", 0.5}},
               &reply) &&
              reply.as_object().contains("error") &&
              textOf(reply.at("error").as_object(), "message").find("standalone") !=
                  std::string::npos,
          "gain for the absent standalone source is refused naming it");
    check(call(fd, proto::kMethodMediaMute,
               boost::json::object{{"source", "midside"}, {"mute", true}},
               &reply) &&
              reply.as_object().contains("error") &&
              textOf(reply.at("error").as_object(), "message").find("unknown audio source") !=
                  std::string::npos,
          "and an unnamed source is a parameter error");
    check(call(fd, proto::kMethodMediaMute,
               boost::json::object{{"source", "standalone"}, {"mute", true}},
               &reply) &&
              reply.as_object().contains("error") &&
              textOf(reply.at("error").as_object(), "message").find("standalone") !=
                  std::string::npos,
          "the mute leg refuses the absent source too — the gate is not gain-only");
    check(call(fd, proto::kMethodMediaGain, boost::json::object{{"source", "video"}, {"gain", 1.5}},
               &reply) &&
              reply.as_object().contains("error"),
          "a gain outside [0, 1] is a parameter error, not a clamp");

    // Change what the audience hears, and hear it back from status.
    check(call(fd, proto::kMethodMediaGain,
               boost::json::object{{"source", "video"}, {"gain", 0.4}},
               &reply) &&
              reply.as_object().contains("result") &&
              reply.at("result").as_object().at("source").as_string() == "video",
          "the gain request is acknowledged for its source");
    const bool quieted = waitMedia(
        fd,
        [](const boost::json::object& media) {
            const boost::json::object* video = findSource(media, "video");
            return video != nullptr && std::abs(numberOf(*video, "gain") - 0.4) < 0.01;
        },
        4000);
    check(quieted, "the mixer echo in status acknowledges the new gain");
    check(call(fd, proto::kMethodMediaMute,
               boost::json::object{{"source", "video"}, {"mute", true}},
               &reply) &&
              reply.as_object().contains("result") &&
              reply.at("result").as_object().at("muted").as_bool() == true,
          "mute is acknowledged with the state it set");
    const bool muted = waitMedia(
        fd,
        [](const boost::json::object& media) {
            const boost::json::object* video = findSource(media, "video");
            return video != nullptr && video->at("muted").as_bool();
        },
        4000);
    check(muted, "status carries the mute");
    check(mediaOf(status(fd)).at("playing").as_bool(),
          "a muted source does not stop the picture — mute is mix, not transport");
    check(call(fd, proto::kMethodMediaMute,
               boost::json::object{{"source", "video"}, {"mute", false}},
               &reply) &&
              reply.as_object().contains("result"),
          "unmute is taken");
    const bool audible = waitMedia(
        fd,
        [](const boost::json::object& media) {
            const boost::json::object* video = findSource(media, "video");
            return video != nullptr && !video->at("muted").as_bool();
        },
        4000);
    check(audible, "and status says the source is audible again");
    retireScopedWorker(fd, worker, socket, "the rpc-gain worker answers shutdown");
}

// A refused jump through the OPERATOR's door: same fault knob as the 6.7d
// scheduled refusal, but the request arrives as media.seek over the
// socket — the ack says "taken", and the refusal comes back as the typed
// fault and the pushed notification. The clean counterpart is
// exerciseRpcTransport's acknowledged seek: same RPC, fault off, lands.
void exerciseRpcSeekRefusal(const std::string& audioClip) {
    std::string socket;
    pid_t worker = -1;
    const int fd =
        spawnScopedWorker("rpcrefuse", {"OPENSHOW_FAULT_AUDIO_SEEK_FAIL=1"}, &socket, &worker);
    if (fd < 0) {
        check(false, "the rpc-refusal worker could be spawned");
        return;
    }
    const std::uint64_t scene = preparePresent(fd, lyricScene("RPC refuse", "r1", audioClip, true));
    check(scene != 0 && waitActive(fd, scene, nullptr), "the faulted-audio background goes on air");
    const bool rolling = waitMedia(
        fd,
        [](const boost::json::object& media) {
            return media.at("playing").as_bool() && numberOf(media, "position") > 0.0;
        },
        6000);
    check(rolling, "the picture plays before the refused jump");
    gMediaErrors.clear();
    boost::json::value reply;
    check(call(fd, proto::kMethodMediaSeek, boost::json::object{{"position", 2.0}}, &reply) &&
              reply.as_object().contains("result"),
          "the router takes the seek — the transport is live; whether the media obeys is another answer");
    const bool reported = waitMedia(
        fd,
        [](const boost::json::object& media) {
            return textOf(media, "error").find("refused") != std::string::npos;
        },
        5000);
    check(reported, "the refusal the transport hit becomes the standing status error");
    std::size_t refusals = 0;
    for (const boost::json::object& notification : gMediaErrors) {
        if (textOf(notification, "message").find("refused") != std::string::npos) {
            ++refusals;
        }
    }
    check(refusals >= 1, "and is pushed as media.error naming the refusal, not only pollable");
    const boost::json::object after = mediaOf(status(fd));
    check(after.at("playing").as_bool(),
          "a refused jump does not stop the picture: the audience keeps the video");
    retireScopedWorker(fd, worker, socket, "the rpc-refusal worker answers shutdown");
}

// The standalone block of a status media object ({} when the worker predates
// it — every read below tolerates that exactly like WorkerController does).
boost::json::object standaloneOf(const boost::json::object& media) {
    const auto track = media.find("standalone");
    return track != media.end() && track->value().is_object()
               ? track->value().as_object()
               : boost::json::object{};
}

// --- task 6.15b (review R15): the standalone production track ---
//
// The independent audio transport over the operator's wire: selection with
// a synchronous probe (a bad path is refused and touches nothing), a
// lifetime no scene change can interrupt, per-source mix that spans both
// transports, and the track's own status block carrying its own story.
// Every acceptance in the ledger row is witnessed here on a real
// --socket session: audio-only, embedded+standalone mix, lyric change,
// background REPLACE (transport torn down and rebuilt around a playing
// track), background removal, mute/removal of each source leaving the
// other correct, and the loop the transport holds.
void exerciseStandaloneTrack(const std::string& bed6, const std::string& tone2,
                             const std::string& videoA, const std::string& audioClip) {
    std::string socket;
    pid_t worker = -1;
    const int fd = spawnScopedWorker("standalone", {}, &socket, &worker);
    if (fd < 0) {
        check(false, "the standalone worker could be spawned and handshaken");
        return;
    }
    boost::json::value reply;

    // Colour fill: no background transport at all. This whole scenario
    // begins with the track being the ONLY audio the audience could have.
    const std::uint64_t blank = preparePresent(fd, lyricScene("Alone", "b1", "", false));
    check(blank != 0 && waitActive(fd, blank, nullptr), "the background-less scene goes on air");

    // Wire validation first: a path is a string, and the missing file is
    // refused NOW, naming the file — with nothing on air to disturb.
    check(call(fd, proto::kMethodMediaStandalone, boost::json::object{}, &reply) &&
              reply.as_object().contains("error") &&
              textOf(reply.at("error").as_object(), "message").find("path") !=
                  std::string::npos,
          "media.standalone without a path is a parameter error");
    check(call(fd, proto::kMethodMediaStandalone, boost::json::object{{"path", 42}}, &reply) &&
              reply.as_object().contains("error"),
          "and a non-string path is too");
    const std::string missing = "/tmp/openshow-no-such-track-" + std::to_string(getpid()) + ".m4a";
    check(call(fd, proto::kMethodMediaStandalone,
               boost::json::object{{"path", missing}}, &reply) &&
              reply.as_object().contains("error") &&
              textOf(reply.at("error").as_object(), "message").find(missing) !=
                  std::string::npos &&
              // 6.15a's rule for refusals: each leg names its OWN reason, so
              // a dead leg cannot hide behind a sibling that also refuses.
              // Naming the file is not enough — this must read as the open
              // failure, not as a stream complaint.
              textOf(reply.at("error").as_object(), "message").find("no audio stream") ==
                  std::string::npos,
          "a path with nothing readable behind it is refused naming it, as an open failure");
    {  // The probe's second leg, witnessed on its own: videoA opens fine and
       // carries video only (it is the -an clip), so "no audio" is the truth
       // here, and the refusal names that file too.
        check(call(fd, proto::kMethodMediaStandalone,
                   boost::json::object{{"path", videoA}}, &reply) &&
                  reply.as_object().contains("error") &&
                  textOf(reply.at("error").as_object(), "message").find("no audio stream") !=
                      std::string::npos &&
                  textOf(reply.at("error").as_object(), "message").find(videoA) !=
                      std::string::npos,
              "a file that opens but carries no audio is refused naming that reason");
    }
    {  // and the probe refusal changed nothing: the block is still idle.
        const boost::json::object track = standaloneOf(mediaOf(status(fd)));
        const auto live = track.find("live");
        check(live != track.end() && live->value().is_bool() && !live->value().as_bool(),
              "a refused selection leaves the block idle");
    }
    check(call(fd, proto::kMethodMediaGain,
               boost::json::object{{"source", "standalone"}, {"gain", 0.5}}, &reply) &&
              reply.as_object().contains("error") &&
              textOf(reply.at("error").as_object(), "message").find("standalone") !=
                  std::string::npos,
          "gain for a track that was never selected is refused by name");

    // --- audio-only: the track IS the session's audio ---
    check(call(fd, proto::kMethodMediaStandalone,
               boost::json::object{{"path", bed6}}, &reply) &&
              reply.at("result").as_object().at("pending").as_bool(),
          "the readable track is accepted, pending");
    const bool up = waitMedia(
        fd,
        [](const boost::json::object& media) {
            const boost::json::object track = standaloneOf(media);
            const auto live = track.find("live");
            return live != track.end() && live->value().is_bool() && live->value().as_bool();
        },
        5000);
    check(up, "status brings the track live");
    {
        const boost::json::object media = mediaOf(status(fd));
        const boost::json::object track = standaloneOf(media);
        check(track.at("path").as_string() == bed6, "the block names the selected file");
        check(numberOf(track, "duration") > 5.5 && numberOf(track, "duration") < 6.5,
              "and the probe's duration");
        check(!media.at("live").as_bool(),
              "the background block stays idle — the track never impersonates one");
        const boost::json::object* row = findSource(media, "standalone");
        const boost::json::object* videoRow = findSource(media, "video");
        check(row != nullptr && videoRow == nullptr && media.at("sources").as_array().size() == 1,
              "the sources list holds the track and nothing else");
        check(row != nullptr && std::abs(row->at("gain").as_double() - 1.0) < 0.001 &&
                  !row->at("muted").as_bool(),
              "at unity, unmuted");
    }
    // The transport advances on its own, and the BACKGROUND's commands
    // still refuse: no background transport exists, and a track playing
    // does not make media.play meaningful.
    // The reads have to be SAMPLED before they can be compared: the block
    // seeds position at -1 ("the transport has not reported yet"), so an
    // unsampled -1 -> 0 step reads as motion — mutation m13 proved exactly
    // that. Wait for a real reading, then bound the step so it is a clock
    // advancing, not merely a value that changed.
    check(waitMedia(
              fd,
              [](const boost::json::object& media) {
                  return numberOf(standaloneOf(media), "position") >= 0.0;
              },
              4000),
          "the block reports a sampled position");
    const double first = numberOf(standaloneOf(mediaOf(status(fd))), "position");
    sleepMs(500);
    {
        const double later = numberOf(standaloneOf(mediaOf(status(fd))), "position");
        check(later > first + 0.2 && later < first + 2.0,
              "the track advances: its clock runs, at a clock's rate");
        check(call(fd, proto::kMethodMediaPlay, {}, &reply) &&
                  reply.as_object().contains("error") &&
                  textOf(reply.at("error").as_object(), "message").find("no background media") !=
                      std::string::npos,
              "background transport commands still refuse with only a track on");
        const double still = numberOf(standaloneOf(mediaOf(status(fd))), "position");
        check(still >= later, "the refusal disturbed the track not at all");
    }
    // Per-source mix on a session with no background transport at all:
    // "embedded+standalone" half comes next; this half proves the mixer
    // answers for the track alone.
    check(call(fd, proto::kMethodMediaGain,
               boost::json::object{{"source", "standalone"}, {"gain", 0.4}}, &reply) &&
              reply.as_object().contains("result"),
          "the track's gain is accepted without any background transport");
    check(waitMedia(
              fd,
              [](const boost::json::object& media) {
                  const boost::json::object* row = findSource(media, "standalone");
                  return row != nullptr && std::abs(row->at("gain").as_double() - 0.4) < 0.001;
              },
              3000),
          "and the mixer echo acknowledges it");
    check(call(fd, proto::kMethodMediaMute,
               boost::json::object{{"source", "standalone"}, {"mute", true}}, &reply) &&
              reply.as_object().contains("result"),
          "muting the track is accepted");
    check(waitMedia(
              fd,
              [](const boost::json::object& media) {
                  const boost::json::object* row = findSource(media, "standalone");
                  return row != nullptr && row->at("muted").as_bool();
              },
              3000),
          "and echoed");
    check(call(fd, proto::kMethodMediaMute,
               boost::json::object{{"source", "standalone"}, {"mute", false}}, &reply) &&
              reply.as_object().contains("result"),
          "unmuting too");
    check(waitMedia(
              fd,
              [](const boost::json::object& media) {
                  const boost::json::object* row = findSource(media, "standalone");
                  return row != nullptr && !row->at("muted").as_bool();
              },
              3000),
          "and that echo");

    // --- embedded + standalone: the mix, and survival of the swap ---
    const std::uint64_t withVideo =
        preparePresent(fd, lyricScene("Mix", "b2", audioClip, true, "shared-bg"));
    check(withVideo != 0 && waitActive(fd, withVideo, nullptr),
          "the embedded-audio background goes on air beside the track");
    {
        const boost::json::object media = mediaOf(status(fd));
        const boost::json::object* row = findSource(media, "standalone");
        const boost::json::object* videoRow = findSource(media, "video");
        check(media.at("live").as_bool(), "the background transport is live too");
        check(row != nullptr && videoRow != nullptr && media.at("sources").as_array().size() == 2,
              "both sources are listed at once");
        check(row != nullptr && std::abs(row->at("gain").as_double() - 0.4) < 0.001,
              "the track keeps the gain the operator set before the background existed");
    }
    // Muting one leaves the other exactly right — R15's acceptance, on
    // the worker's own echo.
    check(call(fd, proto::kMethodMediaMute,
               boost::json::object{{"source", "video"}, {"mute", true}}, &reply) &&
              reply.as_object().contains("result"),
          "the embedded source mutes");
    check(waitMedia(
              fd,
              [](const boost::json::object& media) {
                  const boost::json::object* videoRow = findSource(media, "video");
                  return videoRow != nullptr && videoRow->at("muted").as_bool();
              },
              3000),
          "and the echo shows it");
    {
        const boost::json::object media = mediaOf(status(fd));
        const boost::json::object* row = findSource(media, "standalone");
        check(row != nullptr && !row->at("muted").as_bool(),
              "the track is untouched by the video mute");
    }
    check(call(fd, proto::kMethodMediaMute,
               boost::json::object{{"source", "video"}, {"mute", false}}, &reply) &&
              reply.as_object().contains("result"),
          "unmute the embedded source");
    check(waitMedia(
              fd,
              [](const boost::json::object& media) {
                  const boost::json::object* videoRow = findSource(media, "video");
                  return videoRow != nullptr && !videoRow->at("muted").as_bool();
              },
              3000),
          "and both are clean again");
    // A distinguishing gain on the EMBEDDED source (0.6, against the
    // track's 0.4), so the checks below that remove the track can tell
    // "the other source is exactly right" from "the other source is
    // whatever the defaults were".
    check(call(fd, proto::kMethodMediaGain,
               boost::json::object{{"source", "video"}, {"gain", 0.6}}, &reply) &&
              reply.as_object().contains("result"),
          "the embedded source gets its own gain");
    check(waitMedia(
              fd,
              [](const boost::json::object& media) {
                  const boost::json::object* videoRow = findSource(media, "video");
                  return videoRow != nullptr && std::abs(videoRow->at("gain").as_double() - 0.6) < 0.001;
              },
              3000),
          "and its row answers for itself, the track's still at 0.4");

    // A lyric change over the SAME background identity: the transport
    // stays put (6.15a's rule) and the track must not notice either.
    const double beforeLyrics = numberOf(standaloneOf(mediaOf(status(fd))), "position");
    const std::uint64_t lyrics =
        preparePresent(fd, lyricScene("Again", "b2", audioClip, true, "shared-bg"));
    check(lyrics != 0 && waitActive(fd, lyrics, nullptr), "the lyric change goes on air");
    {
        const double after = numberOf(standaloneOf(mediaOf(status(fd))), "position");
        check(after + 0.05 >= beforeLyrics, "and the track did not restart");
    }

    // --- REMOVING the track while the embedded background plays on ---
    //
    // R15's acceptance read the other way round — and this is the only
    // moment the per-identity gate is observable: a background transport IS
    // live here, so a command naming the absent track must be refused BY
    // NAME rather than land on the background's mixer. The background's own
    // row keeps its distinguishing 0.6 and stays addressable throughout.
    check(call(fd, proto::kMethodMediaStandalone, boost::json::object{{"path", ""}}, &reply) &&
              reply.as_object().contains("result"),
          "the track is removed while the background plays on");
    check(waitMedia(
              fd,
              [](const boost::json::object& media) {
                  return !standaloneOf(media).at("live").as_bool() &&
                         findSource(media, "standalone") == nullptr;
              },
              4000),
          "its live flag and its row leave together");
    {
        const boost::json::object media = mediaOf(status(fd));
        const boost::json::object* videoRow = findSource(media, "video");
        check(media.at("live").as_bool() && media.at("sources").as_array().size() == 1 &&
                  videoRow != nullptr &&
                  std::abs(videoRow->at("gain").as_double() - 0.6) < 0.001 &&
                  !videoRow->at("muted").as_bool(),
              "the embedded source is exactly as the operator left it, alone");
        check(!standaloneOf(media).at("playing").as_bool() &&
                  numberOf(standaloneOf(media), "position") < 0.0,
              "and the block reports itself idle, position and all");
    }
    // THE gate: a live background transport makes no standalone source.
    check(call(fd, proto::kMethodMediaGain,
               boost::json::object{{"source", "standalone"}, {"gain", 0.9}}, &reply) &&
              reply.as_object().contains("error") &&
              textOf(reply.at("error").as_object(), "message").find("standalone") !=
                  std::string::npos,
          "a track gain with a background live but no track is refused naming the track");
    check(call(fd, proto::kMethodMediaMute,
               boost::json::object{{"source", "standalone"}, {"mute", true}}, &reply) &&
              reply.as_object().contains("error") &&
              textOf(reply.at("error").as_object(), "message").find("standalone") !=
                  std::string::npos,
          "and so is its mute — the two legs gate separately");
    {  // Neither refusal disturbed the background's mix. (The status object
       // is bound to a local first: findSource points INTO it.)
        const boost::json::object media = mediaOf(status(fd));
        const boost::json::object* videoRow = findSource(media, "video");
        check(videoRow != nullptr && std::abs(videoRow->at("gain").as_double() - 0.6) < 0.001 &&
                  !videoRow->at("muted").as_bool(),
              "the refused track commands moved the embedded source not at all");
    }
    // Re-select: a fresh transport, so its mix starts at UNITY while the
    // background keeps its 0.6 — and both rows return, the mask
    // re-registered beside a live background.
    check(call(fd, proto::kMethodMediaStandalone, boost::json::object{{"path", bed6}}, &reply) &&
              reply.as_object().contains("result"),
          "a second selection brings a fresh track up beside the background");
    check(waitMedia(
              fd,
              [](const boost::json::object& media) {
                  const boost::json::object* row = findSource(media, "standalone");
                  const boost::json::object* videoRow = findSource(media, "video");
                  return row != nullptr && videoRow != nullptr &&
                         std::abs(row->at("gain").as_double() - 1.0) < 0.001 &&
                         std::abs(videoRow->at("gain").as_double() - 0.6) < 0.001;
              },
              5000),
          "the new track starts at unity while the background keeps its value");
    // Back to the value the remaining phases read.
    check(call(fd, proto::kMethodMediaGain,
               boost::json::object{{"source", "standalone"}, {"gain", 0.4}}, &reply) &&
              reply.as_object().contains("result"),
          "the track's gain is set again");
    check(waitMedia(
              fd,
              [](const boost::json::object& media) {
                  const boost::json::object* row = findSource(media, "standalone");
                  return row != nullptr && std::abs(row->at("gain").as_double() - 0.4) < 0.001;
              },
              3000),
          "and echoed on the new mixer");

    // The hard one: REPLACE the background video with another identity.
    // The background transport is torn down and rebuilt — the track rides
    // straight through it, and because the replacement is SILENT its own
    // row disappears while the track's stays, gain and all.
    const double beforeSwap = numberOf(standaloneOf(mediaOf(status(fd))), "position");
    const std::uint64_t swapped =
        preparePresent(fd, lyricScene("Swap", "b3", videoA, true, "video-a"));
    check(swapped != 0 && waitActive(fd, swapped, nullptr),
          "the silent background goes on air (transport rebuilt around the track)");
    {
        const boost::json::object media = mediaOf(status(fd));
        const boost::json::object* row = findSource(media, "standalone");
        check(findSource(media, "video") == nullptr,
              "the silent replacement registers no video source");
        check(row != nullptr && std::abs(row->at("gain").as_double() - 0.4) < 0.001,
              "the track's row — gain and all — survived the teardown and rebuild");
        const double after = numberOf(standaloneOf(media), "position");
        check(after + 0.05 >= beforeSwap, "and the track did not restart for the swap either");
    }

    // Background away entirely: the track is alone again, still playing,
    // still the only row. Then removal, and the commands refuse again.
    const std::uint64_t cleared = preparePresent(fd, lyricScene("Cleared", "b4", "", false));
    check(cleared != 0 && waitActive(fd, cleared, nullptr), "the background goes away");
    check(waitMedia(
              fd,
              [](const boost::json::object& media) {
                  return !media.at("live").as_bool() &&
                         standaloneOf(media).at("live").as_bool();
              },
              4000),
          "the background block idles while the track stays live");
    {
        const boost::json::object media = mediaOf(status(fd));
        check(media.at("sources").as_array().size() == 1 &&
                  findSource(media, "standalone") != nullptr,
              "mediaStopped keeps the track's row with the track");
    }
    check(call(fd, proto::kMethodMediaStandalone, boost::json::object{{"path", ""}}, &reply) &&
              reply.as_object().contains("result"),
          "the empty path removes the track");
    const bool gone = waitMedia(
        fd,
        [](const boost::json::object& media) {
            const boost::json::object track = standaloneOf(media);
            return !track.at("live").as_bool() && media.at("sources").as_array().empty();
        },
        4000);
    check(gone, "removal takes the live flag and the row with it");
    check(call(fd, proto::kMethodMediaGain,
               boost::json::object{{"source", "standalone"}, {"gain", 0.5}}, &reply) &&
              reply.as_object().contains("error") &&
              textOf(reply.at("error").as_object(), "message").find("standalone") !=
                  std::string::npos,
          "and the track's commands refuse by name again — never queued for a successor");

    // --- the loop the transport holds: EOF rewinds, the bed keeps going ---
    //
    // The clock this transport paces itself by is the HEARD sink clock, and
    // it runs past the file's end whether or not the feeder re-reads it — a
    // starved mixer pads silence and the server consumes that at exactly
    // the same rate. So the wrap is NOT a drop in "position" (the first
    // version of this leg asserted that, and was wrong about the
    // observable); it is the feeder's rewind counter, which the status
    // block carries for precisely this reason: N wraps means the audience
    // is hearing pass N+1, not the silence that replaces a run-out source.
    check(call(fd, proto::kMethodMediaStandalone,
               boost::json::object{{"path", tone2}}, &reply) &&
              reply.as_object().contains("result"),
          "the two-second track is selected");
    check(waitMedia(
              fd,
              [](const boost::json::object& media) {
                  return standaloneOf(media).at("live").as_bool();
              },
              5000),
          "and live");
    // The lane needs a moment to settle: the swap seeds it, and the sink's
    // first server callback is what promotes it to "live". Only a lane the
    // server is actually draining can starve, so only there does the wrap
    // count mean anything — a machine with no audio server never consumes
    // the mixer, and its feeder parks in write retries instead of reaching
    // EOF. That control group belongs to the other scenarios.
    const bool audible = waitMedia(
        fd,
        [](const boost::json::object& media) {
            return textOf(standaloneOf(media), "audio") == "live";
        },
        4000);
    if (audible) {
        const bool wrapped = waitMedia(
            fd,
            [](const boost::json::object& media) {
                return numberOf(standaloneOf(media), "loops") >= 1.0;
            },
            6000);
        check(wrapped, "the production bed loops: the source rewound at its end");
        {  // And a wrap is not an ending: the block stays a healthy transport.
            const boost::json::object track = standaloneOf(mediaOf(status(fd)));
            check(track.at("live").as_bool() && track.at("playing").as_bool() &&
                      textOf(track, "error").empty(),
                  "and the bed is still live, playing and error-free past the rewind");
        }
    } else {
        std::printf("   skipped the loop leg (audio lane: %s)\n",
                    textOf(standaloneOf(mediaOf(status(fd))), "audio").c_str());
    }
    retireScopedWorker(fd, worker, socket, "the standalone worker answers shutdown");

    // --- the bring-up's bad news belongs to the track's own block ---
    //
    // The selection PROBE passes (the file is fine); the mixer fault knob
    // is what fails, inside the swap. The refusal the RPC got was
    // "pending" — so the failure must land in media.standalone.error
    // naming the file, with the notification pushed, and live stays a
    // false the controls can trust.
    std::string failSocket;
    pid_t failWorker = -1;
    const int fd2 = spawnScopedWorker("standalonefail", {"OPENSHOW_FAULT_AUDIO_MIXER_FAIL=1"},
                                      &failSocket, &failWorker);
    if (fd2 < 0) {
        check(false, "the standalone-fault worker could be spawned");
        return;
    }
    gMediaErrors.clear();
    check(call(fd2, proto::kMethodMediaStandalone, boost::json::object{{"path", bed6}}, &reply) &&
              reply.as_object().contains("result"),
          "the faulted worker takes the selection (the probe passed; the bring-up is what fails)");
    const bool named = waitMedia(
        fd2,
        [](const boost::json::object& media) {
            return textOf(standaloneOf(media), "error").find("standalone track") !=
                   std::string::npos;
        },
        5000);
    check(named, "the failed bring-up is the block's own sticky error, naming the track");
    check(!standaloneOf(mediaOf(status(fd2))).at("live").as_bool(),
          "and live stays false — a dead bring-up is not commandable");
    {  // bound first: findSource points into the status object
        const boost::json::object media = mediaOf(status(fd2));
        check(findSource(media, "standalone") == nullptr,
              "no row for a source that never came up");
    }
    {
        bool pushed = false;
        for (const boost::json::object& notification : gMediaErrors) {
            if (textOf(notification, "message").find("standalone track") != std::string::npos &&
                textOf(notification, "path") == bed6) {
                pushed = true;
            }
        }
        check(pushed, "and the failure is pushed as media.error naming the track's path");
    }
    check(call(fd2, "ping", {}, &reply) && reply.as_object().contains("result"),
          "the faulted worker lives on, ready for the next selection");
    retireScopedWorker(fd2, failWorker, failSocket, "the standalone-fault worker answers shutdown");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1) {
        gWorkerBinary = argv[1];
    }

    // Self-generated fixtures (the repo's media_tests recipes): silent
    // clips on purpose — a ctest scenario must never wake the room, and
    // the silent shape exercises the monotonic-clock pacing branch. The
    // audio-clock selection of the transport is proven by the live
    // --av-bench runs and manual socket sessions (PROGRESS.md).
    if (!generate("ffmpeg -version >/dev/null 2>&1")) {
        std::printf("SKIP: the ffmpeg CLI is not available\n");
        return 77;
    }
    const std::string dir = "background-media-fixtures";
    generate("mkdir -p " + dir);
    const std::string videoA = dir + "/loop-a.mp4";
    const std::string videoB = dir + "/loop-b.mp4";
    if (!generate("ffmpeg -hide_banner -loglevel error -nostdin -y "
                  "-f lavfi -i testsrc2=size=320x240:rate=25:duration=6 "
                  "-c:v libx264 -preset ultrafast -pix_fmt yuv420p -an " +
                  videoA) ||
        !generate("ffmpeg -hide_banner -loglevel error -nostdin -y "
                  "-f lavfi -i testsrc=size=640x360:rate=25:duration=8 "
                  "-c:v libx264 -preset ultrafast -pix_fmt yuv420p -an " +
                  videoB)) {
        std::printf("SKIP: the ffmpeg CLI could not generate the fixtures\n");
        return 77;
    }

    const std::string missing = "/tmp/openshow-missing-" + std::to_string(getpid()) + ".mp4";
    ::unlink(missing.c_str());
    // A corrupt "mp4": bytes that cannot be demuxed but exist.
    const std::string corrupt = "/tmp/openshow-corrupt-" + std::to_string(getpid()) + ".mp4";
    {
        FILE* out = std::fopen(corrupt.c_str(), "wb");
        if (out == nullptr) {
            std::fprintf(stderr, "could not stage the corrupt fixture — skipping\n");
            return 77;
        }
        std::fputs("this is not a video file, just noise for the demuxer", out);
        std::fclose(out);
    }

    // The big still exercises the prepare-time decode (the repo pins no
    // giant image into git). If it cannot be produced the preload
    // sub-scenario is skipped rather than failed.
    std::string bigStill = "/tmp/openshow-big-" + std::to_string(getpid()) + ".jpg";
    const std::string stillCommand =
        "ffmpeg -v error -y -f lavfi -i testsrc=size=3840x2160:rate=1 "
        "-frames:v 1 '" +
        bigStill + "' >/dev/null 2>&1";
    if (!generate(stillCommand) || !fileExists(bigStill)) {
        bigStill.clear();
    }

    const std::string socket = socketPath("media");
    ::unlink(socket.c_str());
    const pid_t worker = spawnWorker(socket);
    if (worker < 0) {
        std::fprintf(stderr, "FAIL: could not spawn the worker\n");
        return 1;
    }
    const int fd = connectRetry(socket);
    if (fd < 0) {
        std::fprintf(stderr, "FAIL: worker never answered its socket\n");
        kill(worker, SIGTERM);
        return 1;
    }

    boost::json::value reply;
    check(call(fd, "hello",
               proto::helloParams("openshow-background-media", proto::kProtocolMajor,
                                  proto::kProtocolMinor),
               &reply) &&
              reply.as_object().contains("result"),
          "handshake with the background-media worker");
    {  // The capability names the failure surface this scenario asserts.
        const boost::json::array& caps =
            reply.at("result").as_object().at("capabilities").as_array();
        const bool hasMedia = std::any_of(caps.begin(), caps.end(),
                                          [](const boost::json::value& value) {
                                              return value.is_string() &&
                                                     value.as_string() == "media.error.v1";
                                          });
        check(hasMedia, "hello advertises the media.error.v1 capability");
    }

    exerciseBackgroundMedia(fd, videoA, videoB, missing, corrupt, bigStill);

    // --- task 6.7c (review R18): the typed media report ---
    //
    // A still whose file appears later, a clip destroyed mid-play, a clip
    // with real audio, and a worker whose audio cannot start.
    const std::string missingStill = "/tmp/openshow-still-" + std::to_string(getpid()) + ".png";
    const std::string stillFixture = "/tmp/openshow-still-src-" + std::to_string(getpid()) + ".png";
    ::unlink(missingStill.c_str());
    const bool stillOk =
        generate("ffmpeg -v error -y -f lavfi -i color=c=0x336699:s=64x64:r=1 -frames:v 1 '" +
                 stillFixture + "' >/dev/null 2>&1");
    if (stillOk && fileExists(stillFixture)) {
        exerciseImageRepair(fd, missingStill, stillFixture);
    } else {
        std::printf("   skipped the still-repair scenario (no still fixture)\n");
    }

    // --- task 6.10 (review R12): the decoded-still cache, live ---
    exerciseImageCacheBudgets(fd, dir);

    // An audio-bearing clip: a silent STREAM on purpose (anullsrc). The
    // transport must have a decoder to open — that is what the lane is
    // about — while a ctest run still keeps the room quiet.
    const std::string audioClip = dir + "/with-audio.mp4";
    // Task 6.15b: audio-only fixtures for the standalone track — silent on
    // purpose like the rest (6 s bed for the lifetime phases, 2 s tone for
    // the loop-wrap witness).
    const std::string bed6 = dir + "/bed-6s.m4a";
    const std::string tone2 = dir + "/tone-2s.m4a";
    const bool audioOk =
        generate("ffmpeg -hide_banner -loglevel error -nostdin -y "
                 "-f lavfi -i testsrc2=size=320x240:rate=25:duration=4 "
                 "-f lavfi -i anullsrc=channel_layout=stereo:sample_rate=48000 "
                 "-t 4 -c:v libx264 -preset ultrafast -pix_fmt yuv420p -c:a aac " +
                 audioClip) &&
        fileExists(audioClip) &&
        generate("ffmpeg -hide_banner -loglevel error -nostdin -y "
                 "-f lavfi -i anullsrc=channel_layout=stereo:sample_rate=48000 "
                 "-t 6 -c:a aac " +
                 bed6) &&
        fileExists(bed6) &&
        generate("ffmpeg -hide_banner -loglevel error -nostdin -y "
                 "-f lavfi -i anullsrc=channel_layout=stereo:sample_rate=48000 "
                 "-t 2 -c:a aac " +
                 tone2) &&
        fileExists(tone2);
    exerciseMidPlayFailure(fd, videoA);
    if (stillOk && fileExists(stillFixture)) {
        // A path the image cache has never seen: unlinking the repaired
        // still would NOT reproduce a failure (its success is cached for
        // good, which is the point), so the queued-broken case needs its
        // own missing asset.
        const std::string queuedMissing =
            "/tmp/openshow-queued-" + std::to_string(getpid()) + ".png";
        ::unlink(queuedMissing.c_str());
        exerciseFaultsDoNotCrossAssets(fd, videoA, queuedMissing);
        ::unlink(queuedMissing.c_str());
    }
    if (audioOk) {
        exerciseAudioLane(fd, audioClip);
    } else {
        std::printf("   skipped the audio-lane scenario (no audio fixture)\n");
    }
    ::unlink(missingStill.c_str());
    ::unlink(stillFixture.c_str());

    // A healthy exit even after the injected failures.
    boost::json::value shutdownReply;
    check(call(fd, "shutdown", {}, &shutdownReply), "the worker answers shutdown");
    close(fd);
    int waitStatus = 0;
    waitpid(worker, &waitStatus, 0);
    check(WIFEXITED(waitStatus) && WEXITSTATUS(waitStatus) == 0,
          "the worker exits cleanly after the media scenarios");

    // The audio-fault worker runs alone: two outputs on one display fight
    // over the compositor, and this one needs a sink attempt of its own.
    if (audioOk) {
        exerciseAudioStartFailure(audioClip, videoA);
    }

    // --- task 6.7d (review R19): seeks through the whole session ---
    //
    // Each of these owns its worker (the transport schedule and the seek-fault
    // knob are read at spawn), so they run once the shared worker is gone.
    exerciseSeekThroughSession(videoA);
    if (audioOk) {
        exerciseSeekRefusal(audioClip);
    } else {
        std::printf("   skipped the seek-refusal scenario (no audio fixture)\n");
    }

    // --- task 6.15a (review R15): the transport over the operator's wire ---
    //
    // The 6.7d scripts proved the session's seek semantics through a
    // spawn-time schedule; these drive the same session through the
    // media.* RPCs the frontend sends — validation, acknowledged
    // position, per-source gain/mute, the loop-off end. Each owns its
    // worker for the same reason the 6.7d ones do.
    exerciseRpcTransport(videoA);
    if (audioOk) {
        exerciseRpcGain(audioClip);
        exerciseRpcSeekRefusal(audioClip);
        // --- task 6.15b (review R15): the standalone production track ---
        //
        // Its own worker pair (the fault leg needs the mixer knob), on the
        // audio fixtures above: selection, survival, the cross-transport
        // mix, the loop, and the bring-up's bad news in the track's own
        // block.
        exerciseStandaloneTrack(bed6, tone2, videoA, audioClip);
    } else {
        std::printf("   skipped the RPC gain/refusal and standalone scenarios (no audio fixture)\n");
    }

    ::unlink(corrupt.c_str());
    if (!bigStill.empty()) {
        ::unlink(bigStill.c_str());
    }
    ::unlink(socket.c_str());

    if (gFailures > 0) {
        std::fprintf(stderr, "%d background-media check(s) failed\n", gFailures);
        return 1;
    }
    std::printf("ok: background video survives lyric changes, failures are visible, "
                "prepared slides reach the air fast\n");
    return 0;
}
