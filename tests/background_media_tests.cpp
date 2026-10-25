/*
 * Task 5.5 acceptance: preload, continuous backgrounds, visible errors.
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
    scene::TextItem item;
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

    // A healthy exit even after the injected failures.
    boost::json::value shutdownReply;
    check(call(fd, "shutdown", {}, &shutdownReply), "the worker answers shutdown");
    close(fd);
    int waitStatus = 0;
    waitpid(worker, &waitStatus, 0);
    check(WIFEXITED(waitStatus) && WEXITSTATUS(waitStatus) == 0,
          "the worker exits cleanly after the media scenarios");

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
