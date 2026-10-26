/*
 * Task 6.3a + 6.3b fixtures (review R3): WorkerController correlates worker
 * replies by request id and connection generation, mirrors each prepare's own
 * payload under the revision that prepare was acknowledged with, and follows
 * the latest-go rule: the audience moves only when the newest go's prepare
 * is acknowledged. A failed or superseded go must never auto-present, and a
 * dropped connection resets every pending intent. Clears compose against the
 * pending logical presentation, the restore keeps its CONTENT (not just a
 * store revision) until confirmed on air, and a refused restore stays
 * retryable with honest indicators.
 *
 * Task 6.8a (review R11) added S8: input this build knows the worker will
 * refuse is refused in the client — oversized authored text, a data: URI as an
 * asset reference, a frame too big to build — with the reason in the status
 * line, no byte on the socket, and the session still serving the next go. The
 * scripted peer's report is what proves the "no byte" half. Because the client
 * now validates scenes before sending, every payload these fixtures hand the
 * controller is a real compiled scene rather than a sketch of one.
 *
 * Task 6.8b (the other half of R11) added S9, S10 and S11: what the client does
 * with a worker that stops being a useful peer. The peer has three modes for
 * that — "never-answer", which takes every request and answers nothing,
 * "stop-reading", which stops calling recv forever, and "stall-then-read",
 * which stops and then starts again — because the properties at stake are
 * different: a client may keep a bounded number of requests outstanding, and a
 * bounded number of BYTES unwritten, and past either it must refuse with the
 * rule named rather than grow; but a peer that comes BACK must not have been
 * thrown away for the crime of having been slow. The report counts requests by
 * method, the bytes it ever took, and whether it resumed, which is what proves a
 * refused request never reached the wire, that the backlog stopped at its bound,
 * and that a recovering worker kept its session.
 *
 * These properties need a worker that is slow, refuses, and dies on cue, so
 * the fixtures supervise a scripted stand-in: this very binary re-executes
 * itself (via OPENSHOW_WORKER_PROGRAM) as a minimal RPC peer that speaks the
 * real framing and answers exactly one scenario:
 *
 *   hold3            hold all three prepare acks until every request has
 *                    arrived, then answer in order (the burst A/B/C)
 *   slow-first       acknowledge the first prepare 400 ms late, the rest now
 *   error-first      answer the first prepare with a JSON-RPC error
 *   die-first-prepare exit(44) without answering the first prepare
 *   fail-second-full answer the SECOND prepare carrying "RETAINME" with an
 *                    error (a restore that fails must stay retryable)
 *   clean            answer everything immediately
 *   never-answer     answer the handshake, then take every request in silence
 *                    (a wedged worker: in-flight requests can only pile up if
 *                    the client lets them, and the status poll asks again only
 *                    once the previous answer has landed)
 *   stop-reading     answer the handshake, then never read another byte
 *                    (the client's own write backlog is then the only thing
 *                    left to bound, and its deadline the only thing left to
 *                    close the connection)
 *   stall-then-read  the same deafness for kDeafForMs and then reading again
 *                    (the counter-scenario: the deadline measures whether the
 *                    backlog is moving, so a worker that resumes keeps its
 *                    session and its pending conversation)
 *
 * scene.present of a revision more than kStoreWindow (16, mirroring the
 * real worker's bounded store) behind the newest handed out is refused, so
 * a restore that still depended on the store being asked cannot hide that.
 *
 * Task 6.12 (review R7) added S12: a save MOVES an asset file and tells
 * this side. The peer now keeps a per-prepare ledger of the background file
 * each payload named, which is the only way to see a rename done right: the
 * adoption must re-prepare the owed scene, feed rewritten payloads to clears
 * and restores, repeat the old name never, and move the audience not at all.
 *
 * Task 6.15a (review R15) added S13: the media transport on both sides of
 * the socket. The peer grew a scripted transport (scenario "media") whose
 * state ONLY the media.play/pause/seek/loop/gain/mute commands move, echoed
 * back through output.status.media — and a verbatim ledger of every command
 * frame it received. The fixture asserts the controller's media* properties
 * move exactly with that echo (never with the click), and the ledger names
 * the method and params that actually rode the wire, including the refused
 * standalone gain the client must send rather than second-guess. Task 6.15b
 * grew that peer further: the media.standalone selection (accept, probe
 * refusal, "" removal) and a status block of the track's own, so the mix
 * rows span two transports and each lifetime reports for itself.
 *
 * Each child writes a small report (prepares, presents, requests for methods it
 * does not model, presented revision list, every request counted by method, and
 * the bytes it ever took from the socket) to OPENSHOW_FAKE_REPORT so the parent
 * can prove WHICH requests the controller actually sent, not just the final
 * revision. No display, no GPU, no real worker.
 */

#include <boost/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <QCoreApplication>
#include <QString>

#include "openshow/protocol/protocol.h"
#include "openshow/scene/scene.h"

#include "WorkerController.h"

namespace {

namespace proto = openshow::protocol;
namespace scene = openshow::scene;
using openshow::ui::WorkerController;

// The scripted peer's store window: presents of revisions older than this
// many behind the newest handed out are refused, like the real worker's
// bounded scene store (task 6.3b's ">16 prepares" acceptance).
constexpr std::int64_t kStoreWindow = 16;

int gFailures = 0;

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++gFailures;
    }
    return condition;
}

// Pump the Qt event loop until the predicate holds or the budget runs out.
bool waitUntil(const std::function<bool()>& predicate, int budgetMs = 8000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

std::string tempPath(const char* tag) {
    return "/tmp/openshow-63a-" + std::to_string(getpid()) + "-" + tag + ".json";
}

// Read the child's report: {"prepares":N,"presents":N,"presented":[...]}.
boost::json::object readReport(const std::string& path) {
    for (int attempt = 0; attempt < 40; ++attempt) {
        if (std::FILE* file = std::fopen(path.c_str(), "rb")) {
            char buffer[4096];
            const size_t count = std::fread(buffer, 1, sizeof(buffer) - 1, file);
            std::fclose(file);
            buffer[count] = '\0';
            boost::system::error_code error;
            boost::json::value parsed = boost::json::parse(std::string(buffer), error);
            if (!error && parsed.is_object()) {
                return parsed.as_object();
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return {};
}

// --- the scripted peer (child mode) ----------------------------------------

class FakeWorker {
public:
    FakeWorker(std::string socketPath, std::string scenario, std::string reportPath)
        : fSocketPath(std::move(socketPath)), fScenario(std::move(scenario)),
          fReportPath(std::move(reportPath)) {
        // The task 6.8b fixtures watch this file WHILE the peer is running: once
        // a peer has stopped reading, the report is the only witness either side
        // has, and a drain that is not written down did not happen.
        fLiveReport = fScenario == "never-answer" || fScenario == "stop-reading" ||
                      fScenario == "stall-then-read";
    }

    int run() {
        ::unlink(fSocketPath.c_str());  // a previous child's stale listener
        const int listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (listener < 0) {
            return 1;
        }
        sockaddr_un address = {};
        address.sun_family = AF_UNIX;
        std::strncpy(address.sun_path, fSocketPath.c_str(), sizeof(address.sun_path) - 1);
        if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(listener, 1) != 0) {
            return 2;
        }
        // The controller connects within a second of spawning; blocking is
        // fine because its destructor kills this child if a fixture fails.
        const int client = ::accept(listener, nullptr, nullptr);
        if (client < 0) {
            return 3;
        }
        fClient = client;
        pump();
        writeReport();
        return 0;
    }

private:
    struct DelayedReply {
        std::chrono::steady_clock::time_point when;
        std::string wire;
    };

    void pump() {
        for (;;) {
            int timeoutMs = 100;
            const auto now = std::chrono::steady_clock::now();
            if (!fQueue.empty()) {
                const auto wait =
                    std::chrono::duration_cast<std::chrono::milliseconds>(fQueue.front().when -
                                                                          now)
                        .count();
                timeoutMs = static_cast<int>(std::max<int64_t>(0, std::min<int64_t>(wait, 100)));
            }
            pollfd pending{fClient, POLLIN, 0};
            const int ready = ::poll(&pending, 1, timeoutMs);
            if (ready < 0) {
                return;
            }
            bool readBytes = false;
            if (ready > 0 && !fStoppedReading) {
                char buffer[4096];
                const ssize_t count = ::recv(fClient, buffer, sizeof(buffer), 0);
                if (count <= 0) {
                    return;  // the controller hung up
                }
                readBytes = true;
                fBytesReceived += static_cast<std::int64_t>(count);
                fDecoder.feed(std::string_view(buffer, static_cast<std::size_t>(count)));
                boost::json::value message;
                std::string error;
                while (fDecoder.next(&message, &error)) {
                    if (message.is_object() && message.as_object().contains("method")) {
                        handle(message.as_object());
                    }
                }
            }
            // Task 6.8b: the peer that stops reading. It has answered the
            // handshake, so from here the controller is on its own with a peer
            // that will not take a byte — exactly the state the write deadline
            // exists for. The report keeps being rewritten because this peer
            // cannot NOTICE the controller giving up on it: noticing would
            // require reading, which is the thing it is refusing to do.
            // "stall-then-read" is the same deafness with an expiry, for the
            // fixture that proves a worker which resumes keeps its session.
            if (!fDeafnessStarted && fHandshaked &&
                (fScenario == "stop-reading" || fScenario == "stall-then-read")) {
                fDeafnessStarted = true;
                fStoppedReading = true;
                if (fScenario == "stall-then-read") {
                    fResumeAt = std::chrono::steady_clock::now() +
                                std::chrono::milliseconds(kDeafForMs);
                }
                writeReport();
            }
            if (fStoppedReading) {
                if (fResumeAt && std::chrono::steady_clock::now() >= *fResumeAt) {
                    fStoppedReading = false;
                    fResumed = true;
                    fPacedReads = true;
                    fResumeAt.reset();
                    writeReport();
                } else {
                    if (++fIdleTicks % 5 == 0) {
                        writeReport();
                    }
                    // Without this the poll would return instantly forever: data
                    // is always waiting, and reading it is precisely what this
                    // peer is not doing.
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    continue;
                }
            }
            if (fScenario == "never-answer" && ++fIdleTicks % 5 == 0) {
                writeReport();  // live counts, for the fixture that reads them mid-run
            }
            const auto flushBefore = std::chrono::steady_clock::now();
            for (auto entry = fQueue.begin(); entry != fQueue.end();) {
                if (entry->when <= flushBefore) {
                    send(entry->wire);
                    entry = fQueue.erase(entry);
                } else {
                    ++entry;
                }
            }
            // A resumed peer is a SLOW peer: it takes the backlog in slices with
            // a pause between them, so what the controller owes comes down over
            // seconds rather than in one go. The pace is the whole point of
            // this mode — at the moment the client's write deadline expires it
            // then holds a backlog that is neither gone nor frozen, which is the
            // case a "close unless the queue happens to be empty" rule would
            // wrongly punish and a "close anyway" rule punishes outright.
            if (fPacedReads && readBytes) {
                std::this_thread::sleep_for(std::chrono::milliseconds(kDrainPaceMs));
            }
            if (fLiveReport && ++fReportTicks % 10 == 0) {
                writeReport();  // the drain is only observable through this file
            }
        }
    }

    void send(const std::string& wire) {
        ::send(fClient, wire.data(), wire.size(), MSG_NOSIGNAL);
    }

    void sendResult(std::int64_t id, boost::json::value result) {
        std::string wire;
        proto::appendFrame(proto::makeResult(id, std::move(result)), &wire);
        send(wire);
    }

    void sendError(std::int64_t id, const std::string& text) {
        std::string wire;
        proto::appendFrame(proto::makeError(id, proto::ErrorCode::kInvalidParams, text), &wire);
        send(wire);
    }

    void handle(const boost::json::object& request) {
        const std::int64_t id = request.contains("id") && request.at("id").is_int64()
                                    ? request.at("id").as_int64()
                                    : 0;
        const std::string method(request.at("method").as_string());
        // Per-method accounting, because the task 6.8b scenarios are about which
        // requests a bounded client sent and which it refused to send: "how many
        // of THIS one arrived" is the only question that distinguishes a client
        // that stopped at its in-flight bound from one that stopped polling.
        ++fMethodCounts[method];
        if (method == "hello") {
            fHandshaked = true;
            boost::json::object result{
                {"application", "openshow-worker"},
                {"version", "fake-for-6.3a"},
                {"protocol", boost::json::object{{"major", 0}, {"minor", 1}}},
                {"capabilities", boost::json::array{}},
            };
            sendResult(id, std::move(result));
            return;
        }
        // Task 6.8b: the worker that takes your command and never answers. This
        // is not rudeness for its own sake — it is what a wedged worker looks
        // like from the client, and it is the only way to watch in-flight
        // requests pile up (or not) and to see whether the status poll keeps
        // asking a question that has already been asked.
        if (fScenario == "never-answer") {
            return;
        }
        if (method == "output.displays") {
            sendResult(id, boost::json::array{});
            return;
        }
        if (method == "output.status") {
            boost::json::object scene{
                {"active", static_cast<std::uint64_t>(fActive)},
                {"pending", 0},
                {"prepared", static_cast<std::uint64_t>(fPrepared)},
            };
            boost::json::object result{
                {"frames", static_cast<std::uint64_t>(++fFrames)},
                {"revision", static_cast<std::uint64_t>(fLastRevision)},
                {"device", "fake"},
                {"monitor", -1},
                {"layers",
                 boost::json::object{{"words", true},
                                     {"background", true},
                                     {"video", true},
                                     {"clock", false},
                                     {"aux", false}}},
                {"scene", std::move(scene)},
                {"preview",
                 boost::json::object{{"running", false},
                                     {"dropped", 0},
                                     {"error", ""}}},
            };
            // Task 6.15a (review R15): a scripted transport whose state ONLY
            // the media.* commands move. The controller reads its media
            // properties from this echo and nowhere else, so a control that
            // ran on local optimism — a Pause label flipped by the click —
            // cannot survive here.
            if (fScenario == "media") {
                // Task 6.15b: the sources array spans lifetimes, so it is
                // built from what is actually up — the track's row appears
                // with the track and carries ITS OWN mixer echo, and the
                // track reports in its own block.
                boost::json::array sources{boost::json::object{{"name", "video"},
                                                               {"gain", fMediaGain},
                                                               {"muted", fMediaMuted}}};
                if (fStandaloneLive) {
                    sources.push_back(boost::json::object{{"name", "standalone"},
                                                          {"gain", fStandaloneGain},
                                                          {"muted", fStandaloneMuted}});
                }
                result["media"] = boost::json::object{
                    {"playing", fMediaPlaying},
                    {"path", "fake-clip.mp4"},
                    {"error", ""},
                    {"audio", "live"},
                    {"position", fMediaPosition},
                    {"live", fMediaLive},
                    {"loop", fMediaLoop},
                    {"duration", fMediaDuration},
                    {"sources", std::move(sources)},
                    {"standalone", boost::json::object{{"live", fStandaloneLive},
                                                       {"path", fStandalonePath},
                                                       {"playing", fStandalonePlaying},
                                                       {"position", fStandalonePosition},
                                                       {"duration", 6.0},
                                                       {"error", fStandaloneError},
                                                       {"audio", "live"},
                                                       {"loops", fStandaloneLoops}}},
                };
            }
            sendResult(id, std::move(result));
            return;
        }
        if (method == "scene.prepare") {
            ++fPrepares;
            // Task 6.12 (review R7): WHICH background file each prepare
            // carried. The adoption fixture asserts over this list — the
            // paths are what prove the client re-prepared the moved file,
            // derived clears from it, and never sent the old name again.
            std::string assetPath;
            if (request.contains("params") && request.at("params").is_object()) {
                const auto& params = request.at("params").as_object();
                const auto background = params.find("background");
                if (background != params.end() && background->value().is_object()) {
                    const auto path = background->value().as_object().find("assetPath");
                    if (path != background->value().as_object().end() &&
                        path->value().is_string()) {
                        assetPath = std::string(path->value().as_string());
                    }
                }
            }
            fAssetPaths.push_back(std::move(assetPath));
            if (fScenario == "die-first-prepare" && fPrepares == 1) {
                writeReport();
                _exit(44);  // no answer at all: the go hangs, then dies
            }
            if (fScenario == "error-first" && fPrepares == 1) {
                sendError(id, "fake rejection");
                return;
            }
            if (boost::json::serialize(request).find("RETAINME") != std::string::npos) {
                ++fFullPrepares;
                if (fScenario == "fail-second-full" && fFullPrepares == 2) {
                    sendError(id, "fake restore rejection");
                    return;
                }
            }
            const std::int64_t revision = ++fLastRevision;
            ++fPrepared;
            std::string wire;
            proto::appendFrame(
                proto::makeResult(id, boost::json::object{{"revision",
                                                          static_cast<std::uint64_t>(revision)}}),
                &wire);
            if (fScenario == "slow-first" && fPrepares == 1) {
                // Answer B before A: A's ack arrives after B already went.
                fQueue.push_back({std::chrono::steady_clock::now() +
                                      std::chrono::milliseconds(400),
                                  std::move(wire)});
                return;
            }
            if (fScenario == "hold3") {
                fHeld.push_back(std::move(wire));
                if (fPrepares >= 3) {
                    for (const auto& frame : fHeld) {
                        send(frame);
                    }
                    fHeld.clear();
                }
                return;
            }
            send(wire);
            return;
        }
        if (method == "scene.present") {
            ++fPresents;
            if (request.contains("params") && request.at("params").is_object() &&
                request.at("params").as_object().contains("revision")) {
                const std::int64_t revision =
                    request.at("params").as_object().at("revision").to_number<std::int64_t>();
                if (fLastRevision - revision > kStoreWindow) {
                    // The real worker prunes its store: a present of a
                    // retired revision is honestly refused.
                    sendError(id, "fake store pruned");
                    return;
                }
                fActive = static_cast<int>(revision);
                fPresented.push_back(fActive);
            }
            sendResult(id, boost::json::object{{"presented", static_cast<std::uint64_t>(fActive)}});
            return;
        }
        // Task 6.15a (review R15): the scripted transport. Every command is
        // recorded verbatim — the ledger is how the parent proves WHICH
        // method-and-params frame the controller put on the wire — and moves
        // the state output.status echoes, mirroring the real worker's
        // "acknowledged via status" rule. A gain/mute for a source this
        // transport does not have is refused naming it, exactly as the real
        // router does: the controller must survive a refusal by keeping the
        // last worker-reported state, not by guessing.
        if (method == proto::kMethodMediaPlay || method == proto::kMethodMediaPause ||
            method == proto::kMethodMediaSeek || method == proto::kMethodMediaLoop ||
            method == proto::kMethodMediaGain || method == proto::kMethodMediaMute ||
            method == proto::kMethodMediaStandalone) {
            fMediaOps.push_back(boost::json::object{
                {"method", method},
                {"params", request.contains("params") ? request.at("params")
                                                      : boost::json::object{}},
            });
            writeReport();  // live, like the deaf-peer counts
            boost::json::object args;
            if (request.contains("params") && request.at("params").is_object()) {
                args = request.at("params").as_object();
            }
            if (method == proto::kMethodMediaPlay) {
                fMediaPlaying = true;
            } else if (method == proto::kMethodMediaPause) {
                fMediaPlaying = false;
            } else if (method == proto::kMethodMediaSeek) {
                if (!args.contains("position") || !args.at("position").is_number()) {
                    sendError(id, "fake: media.seek requires a numeric position");
                    return;
                }
                fMediaPosition = args.at("position").to_number<double>();
                sendResult(id, boost::json::object{{"pending", true},
                                                   {"target", fMediaPosition}});
                return;
            } else if (method == proto::kMethodMediaLoop) {
                if (!args.contains("loop") || !args.at("loop").is_bool()) {
                    sendError(id, "fake: media.loop requires a loop boolean");
                    return;
                }
                fMediaLoop = args.at("loop").as_bool();
                sendResult(id, boost::json::object{{"pending", true}, {"loop", fMediaLoop}});
                return;
            } else if (method == proto::kMethodMediaStandalone) {
                // Task 6.15b (review R15): the scripted track, mirroring the
                // real session's rules. A missing file is refused naming it
                // and changes NOTHING (the old track keeps its state); "" is
                // the removal, which drops live/playing and the row but keeps
                // the path the sticky error describes; a new path is a fresh
                // transport, so its mix starts at unity and its wrap count
                // starts at zero. The controller must show all of that from
                // this echo alone.
                if (!args.contains("path") || !args.at("path").is_string()) {
                    sendError(id, "fake: media.standalone requires a path string");
                    return;
                }
                const std::string wanted(args.at("path").as_string());
                if (wanted.find("missing") != std::string::npos) {
                    sendError(id, "fake: standalone track: '" + wanted + "' cannot be opened");
                    return;
                }
                if (wanted.empty()) {
                    fStandaloneLive = false;
                    fStandalonePlaying = false;
                    fStandalonePosition = -1.0;
                    fStandaloneLoops = 0;
                    fStandaloneGain = 1.0;
                    fStandaloneMuted = false;  // path and error stay paired
                } else {
                    fStandaloneLive = true;
                    fStandalonePath = wanted;
                    fStandalonePlaying = true;
                    fStandalonePosition = 1.5;
                    fStandaloneError.clear();
                    fStandaloneLoops = 0;
                    fStandaloneGain = 1.0;
                    fStandaloneMuted = false;
                }
                sendResult(id, boost::json::object{{"pending", true}, {"path", wanted}});
                return;
            } else if (method == proto::kMethodMediaGain || method == proto::kMethodMediaMute) {
                const bool isGain = method == proto::kMethodMediaGain;
                const std::string source =
                    args.contains("source") && args.at("source").is_string()
                        ? std::string(args.at("source").as_string())
                        : std::string();
                // Task 6.15b: "standalone" is a source once the track is up,
                // and each command moves ONLY the row it names — the track's
                // echo never edits the video row and the video's never the
                // track's. With no track, the command is refused by name,
                // exactly as the real router does.
                if (source == "standalone" && !fStandaloneLive) {
                    sendError(id, "fake: no standalone track is playing");
                    return;
                }
                if (source != "video" && source != "standalone") {
                    sendError(id, "fake: unknown audio source");
                    return;
                }
                const bool forTrack = source == "standalone";
                if (isGain) {
                    if (!args.contains("gain") || !args.at("gain").is_number()) {
                        sendError(id, "fake: media.gain requires a gain");
                        return;
                    }
                    const double gain = args.at("gain").to_number<double>();
                    if (forTrack) {
                        fStandaloneGain = gain;
                    } else {
                        fMediaGain = gain;
                    }
                } else {
                    if (!args.contains("mute") || !args.at("mute").is_bool()) {
                        sendError(id, "fake: media.mute requires a mute boolean");
                        return;
                    }
                    const bool muted = args.at("mute").as_bool();
                    if (forTrack) {
                        fStandaloneMuted = muted;
                    } else {
                        fMediaMuted = muted;
                    }
                }
                boost::json::object ack{{"pending", true}, {"source", source}};
                if (isGain) {
                    ack["gain"] = forTrack ? fStandaloneGain : fMediaGain;
                } else {
                    ack["muted"] = forTrack ? fStandaloneMuted : fMediaMuted;
                }
                sendResult(id, std::move(ack));
                return;
            }
            sendResult(id, boost::json::object{{"pending", true}});
            return;
        }
        if (method == "shutdown") {
            sendResult(id, boost::json::object{{"ok", true}});
            writeReport();
            _exit(0);
        }
        // Anything else the controller sent that this peer does not model
        // (layer.setEnabled is the one the fixtures count): counting it is what
        // lets a fixture prove a refused message never arrived, rather than
        // proving only that a status line appeared.
        ++fUnknown;
        sendError(id, "fake method not found");
    }

    void writeReport() {
        if (fReportPath.empty()) {
            return;
        }
        boost::json::array presented;
        for (const int revision : fPresented) {
            presented.push_back(revision);
        }
        boost::json::array assetPaths;
        for (const std::string& assetPath : fAssetPaths) {
            assetPaths.emplace_back(assetPath);
        }
        boost::json::object methods;
        std::int64_t total = 0;
        for (const auto& counted : fMethodCounts) {
            methods[counted.first] = counted.second;
            total += counted.second;
        }
        boost::json::object report{
            {"scenario", fScenario},
            {"prepares", fPrepares},
            {"presents", fPresents},
            {"unknown", fUnknown},
            {"presented", std::move(presented)},
            {"assetPaths", std::move(assetPaths)},
            {"methods", std::move(methods)},
            {"requests", total},
            {"bytesReceived", fBytesReceived},
            {"stoppedReading", fStoppedReading},
            {"resumed", fResumed},
            {"mediaOps", fMediaOps},  // task 6.15a: every transport command, verbatim
        };
        // Written through a temporary file and renamed, so a parent reading this
        // while the peer is mid-run sees either the old report or the new one and
        // never a torn one: a partial read costs the fixture a retry second in
        // which its own event loop is not turning, which is exactly the pause that
        // makes a stalled-peer scenario measure the fixture instead of the code.
        const std::string staging = fReportPath + ".tmp";
        if (std::FILE* file = std::fopen(staging.c_str(), "wb")) {
            const std::string text = boost::json::serialize(boost::json::value(report));
            std::fwrite(text.data(), 1, text.size(), file);
            std::fclose(file);
            std::rename(staging.c_str(), fReportPath.c_str());
        }
    }

    std::vector<std::string> fHeld;  // hold3: acks awaiting the burst
    std::vector<DelayedReply> fQueue;  // slow-first: acks awaiting their due
    std::vector<int> fPresented;       // revisions the controller went to
    std::vector<std::string> fAssetPaths;  // each prepare's background file (task 6.12)
    // Task 6.15a (review R15): the scripted transport's state and the
    // verbatim ledger of the media.* commands that moved it. The peer starts
    // as a live, playing, looping 6 s clip with one registered audio source.
    bool fMediaPlaying = true;
    bool fMediaLive = true;
    bool fMediaLoop = true;
    double fMediaPosition = 2.0;
    double fMediaDuration = 6.0;
    double fMediaGain = 1.0;
    bool fMediaMuted = false;
    // The scripted standalone track (task 6.15b). It starts absent: the
    // scenario selects it, refuses a bad path, mixes it, removes it — and
    // every one of those must reach the controller through this echo.
    bool fStandaloneLive = false;
    std::string fStandalonePath;
    bool fStandalonePlaying = false;
    double fStandalonePosition = -1.0;
    std::string fStandaloneError;
    double fStandaloneGain = 1.0;
    bool fStandaloneMuted = false;
    std::uint64_t fStandaloneLoops = 0;
    boost::json::array fMediaOps;

    std::string fSocketPath;
    std::string fScenario;
    std::string fReportPath;
    proto::Decoder fDecoder;
    int fClient = -1;
    int fFrames = 0;
    int fPrepares = 0;
    int fPresents = 0;
    int fPrepared = 0;
    int fLastRevision = 0;
    int fFullPrepares = 0;  // prepares carrying the "RETAINME" marker
    int fUnknown = 0;       // requests for methods this peer does not model
    int fActive = 0;
    // Task 6.8b: what a peer that stops being useful has to remember.
    std::map<std::string, int> fMethodCounts;  // every request, by method
    bool fHandshaked = false;
    bool fDeafnessStarted = false;  // the peer has taken the handshake and gone quiet
    bool fStoppedReading = false;  // stop-reading: recv is off from here
    bool fResumed = false;         // stall-then-read: and recv is on again
    bool fPacedReads = false;      // stall-then-read: reading, but slowly
    bool fLiveReport = false;      // the 6.8b scenarios read the file mid-run
    std::optional<std::chrono::steady_clock::time_point> fResumeAt;
    std::int64_t fBytesReceived = 0;
    int fIdleTicks = 0;   // keeps the report current while this peer is stalled
    int fReportTicks = 0;  // and while it is busy draining
    // How long "stall-then-read" goes deaf, and how slowly it reads afterwards.
    // Together these two set what the client's write deadline sees when it
    // expires: the deafness has to be long enough for the client to fill its
    // backlog budget and arm the window, and the resume has to come early enough
    // — and drain at about a megabyte per second — for several megabytes to have
    // LEFT that backlog by the 2 s verdict while some of it is still owed.
    static constexpr int kDeafForMs = 900;
    static constexpr int kDrainPaceMs = 2;  // ~4 KiB per slice: ~2 MiB/s
};

// A number from the peer's report, or -1. readReport yields an empty object
// while the child has not written its first one, so a fixture that watches the
// file mid-run has to read it the way it reads a partially filled ledger.
std::int64_t reportNumber(const boost::json::object& report, const char* key) {
    const auto entry = report.find(key);
    if (entry == report.end() || !entry->value().is_number()) {
        return -1;
    }
    return entry->value().to_number<std::int64_t>();
}

// The count the scripted peer recorded for one method. Absent from the report
// means it received none, which for these scenarios is the assertion, so a
// missing key is 0 and only a malformed report is -1.
std::int64_t methodCount(const boost::json::object& report, const char* method) {
    const auto methods = report.find("methods");
    if (methods == report.end() || !methods->value().is_object()) {
        return -1;
    }
    const auto entry = methods->value().as_object().find(method);
    return entry == methods->value().as_object().end()
               ? 0
               : entry->value().to_number<std::int64_t>();
}

// The backlog depth a status line reports, in KiB: the number immediately
// before the unit. -1 for a line that says nothing about queued bytes. This is
// how the stalled-peer fixture reads the client's own measurement of what it is
// holding, which is the only measurement that exists once the peer has stopped
// reading — and abort() discards the buffer the number came from.
qint64 queuedKiBIn(const QString& text) {
    const int at = text.indexOf(QStringLiteral(" KiB"));
    if (at < 0) {
        return -1;
    }
    int begin = at;
    while (begin > 0 && text[begin - 1].isDigit()) {
        --begin;
    }
    if (begin == at) {
        return -1;
    }
    return text.mid(begin, at - begin).toLongLong();
}

// Runs the event loop for a wall-clock window with no condition to wait for.
// The stalled-peer scenarios have no reply to watch for; what they measure is
// what did NOT arrive while the loop kept turning.
void pumpEventsFor(int windowMs) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(windowMs);
    while (std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// --- scenes for the fixtures -------------------------------------------------

// The payloads these fixtures hand the controller have to be scenes this build's
// READER accepts: WorkerController now runs openshow::scene::fromJson over a
// scene before a single byte goes to the worker (task 6.8a, review R11), so a
// payload that merely looked like a scene would be refused locally and never
// reach the scripted peer at all — every scenario below would then be watching
// nothing. They are built through the compiled-scene type rather than by hand so
// the keys the format insists on cannot drift out of date here; the fake worker
// still never validates, the controller still mirrors payloads verbatim, and
// sceneContent() still derives the content flags the assertions read.
scene::Scene authoredScene(const std::vector<std::string>& lines, unsigned color) {
    scene::Scene scene;
    scene.showId = "fixture-show";
    scene.slideId = "fixture-slide";
    scene.background.color = color;
    if (!lines.empty()) {
        scene::TextItem item;
        item.lines = lines;
        scene.items.push_back(std::move(item));
    }
    return scene;
}

std::string sceneWordsAndBackground(const char* text, unsigned color) {
    return boost::json::serialize(
        scene::toJson(authoredScene({std::string(text)}, color)));
}

std::string sceneWordsOnly(const char* text) {
    return sceneWordsAndBackground(text, 0u);
}

std::string sceneBackgroundOnly(unsigned color) {
    return boost::json::serialize(scene::toJson(authoredScene({}, color)));
}

// Content this build's reader refuses, for the fixtures that prove the refusal
// happens BEFORE the socket: one line over the line budget, and a background
// naming a media-byte payload instead of a file.
std::string oversizeLinesScene() {
    scene::Scene scene =
        authoredScene({std::string(scene::kMaxLineChars + 1, 'v')}, 0x220000u);
    return boost::json::serialize(scene::toJson(scene));
}

std::string smuggledMediaPayload() {
    scene::Scene scene = authoredScene({"text"}, 0u);
    scene.background.hasImage = true;
    scene.background.assetPath = "data:image/png;base64,iVBORw0KGgoAAAANSUhEUg";
    return boost::json::serialize(scene::toJson(scene));
}

// Task 6.12 (review R7): a background PICTURE naming a file on disk. The
// adoption fixture asserts on WHICH path each prepare carried, so the file
// name is the payload's whole point.
std::string sceneStill(const char* slideId, const std::string& assetPath) {
    scene::Scene scene = authoredScene({}, 0x102030u);
    scene.slideId = slideId;
    scene.background.hasImage = true;
    scene.background.assetId = std::string("asset-") + slideId;
    scene.background.assetPath = assetPath;
    return boost::json::serialize(scene::toJson(scene));
}

// --- the scenarios ------------------------------------------------------------

// A present that needs the acked-revision handshake: after a go settles,
// the fake's status must report the expected revision on air.
void settle(WorkerController& controller) {
    waitUntil([&controller] { return controller.scenePending() == 0; });
}

// Run the event loop through a wall-clock window — long enough for the peer
// to deliver a deliberately late reply and for the controller to consume it,
// the ack arriving inside the window is processed by the socket reader — and
// then wait for a status poll whose reply was produced after that point.
// The assertions that follow therefore read a status reflecting the
// post-reply world, never a snapshot taken while the late reply was still
// in flight.
void digestThenRefresh(WorkerController& controller, int windowMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(windowMs);
    waitUntil([&deadline] { return std::chrono::steady_clock::now() >= deadline; },
              windowMs + 500);
    const qint64 baseline = controller.frames();
    waitUntil([&controller, baseline] { return controller.frames() > baseline; }, 2000);
}

// S1 (the R3 scenario): burst A, B, C while none is acknowledged. Each
// prepare's ack must mirror its own payload, and only C — the latest go —
// may move the audience.
void scenarioBurst() {
    ::setenv("OPENSHOW_FAKE_SCENARIO", "hold3", 1);
    const std::string report = tempPath("burst");
    ::unlink(report.c_str());
    ::setenv("OPENSHOW_FAKE_REPORT", report.c_str(), 1);

    WorkerController controller;
    controller.start();
    if (!check(waitUntil([&controller] {
                   return controller.statusText().contains(QStringLiteral("connected to worker"));
               }),
               "burst: handshake with the scripted peer")) {
        controller.shutdownNow();
        return;
    }

    controller.presentSceneJson(QString::fromStdString(sceneWordsAndBackground("AAA", 0x111111)));
    controller.presentSceneJson(QString::fromStdString(sceneWordsOnly("BBB")));
    controller.presentSceneJson(QString::fromStdString(sceneBackgroundOnly(0x222222)));

    const bool settled = waitUntil([&controller] {
        return controller.sceneActive() == 3 && controller.scenePending() == 0;
    });
    check(settled, "burst: the audience ends on C's revision");
    check(controller.sceneActive() == 3, "burst: final active revision is the third prepare");
    check(controller.scenePrepared() == 3, "burst: all three prepares are stored");
    check(controller.revision() == 3, "burst: the reported revision is the newest handed out");
    // The mirror under revision 3 must be C's JSON: background only. If a
    // single pending slot had answered the burst, the flags here would be
    // A's (words + background).
    check(!controller.wordsOnAir() && controller.backgroundOnAir(),
          "burst: content flags describe C's payload mirrored under C's revision");
    settle(controller);

    controller.shutdownNow();
    const boost::json::object counts = readReport(report);
    check(counts.at("prepares").to_number<std::int64_t>() == 3, "burst: three prepares arrived");
    // Latest-go rule: exactly one present, for C.
    check(counts.at("presents").to_number<std::int64_t>() == 1,
          "burst: only the latest go presented (no stale auto-presents)");
    const boost::json::array& presented = counts.at("presented").as_array();
    check(presented.size() == 1 && presented[0].to_number<std::int64_t>() == 3,
          "burst: the single present carried C's revision");
}

// S2: an older go acknowledged AFTER a newer one already presented. The
// late ack must mirror its own payload but never regress the audience.
void scenarioSlowReply() {
    ::setenv("OPENSHOW_FAKE_SCENARIO", "slow-first", 1);
    const std::string report = tempPath("slow");
    ::unlink(report.c_str());
    ::setenv("OPENSHOW_FAKE_REPORT", report.c_str(), 1);

    WorkerController controller;
    controller.start();
    if (!check(waitUntil([&controller] {
                   return controller.statusText().contains(QStringLiteral("connected to worker"));
               }),
               "slow: handshake with the scripted peer")) {
        controller.shutdownNow();
        return;
    }

    controller.presentSceneJson(QString::fromStdString(sceneWordsAndBackground("AAA", 0x111111)));
    controller.presentSceneJson(QString::fromStdString(sceneWordsOnly("BBB")));

    // B is acknowledged immediately and goes; A lags 400 ms.
    const bool bOnAir = waitUntil([&controller] {
        return controller.sceneActive() == 2 && controller.scenePending() == 0;
    });
    check(bOnAir, "slow: the newer go presents while the older one is unacknowledged");

    // Let A's late ack land and be digested, then read a status polled
    // after it: the checks below describe the post-late-ack world.
    digestThenRefresh(controller, 700);

    check(controller.sceneActive() == 2, "slow: the late older ack did not regress the audience");
    check(controller.wordsOnAir() && !controller.backgroundOnAir(),
          "slow: content flags still describe B (the newest go of this pair)");
    check(controller.scenePrepared() == 2, "slow: the late prepare's payload was still mirrored");

    controller.shutdownNow();
    const boost::json::object counts = readReport(report);
    const boost::json::array& presented = counts.at("presented").as_array();
    check(counts.at("presents").to_number<std::int64_t>() == 1 && presented.size() == 1 &&
              presented[0].to_number<std::int64_t>() == 2,
          "slow: exactly one present, B's revision (no stale auto-present)");
}

// S3: a failed go must resolve its intent — no auto-present may leak onto
// later replies. A bare prepare then an explicit present must behave.
void scenarioErrorReply() {
    ::setenv("OPENSHOW_FAKE_SCENARIO", "error-first", 1);
    const std::string report = tempPath("error");
    ::unlink(report.c_str());
    ::setenv("OPENSHOW_FAKE_REPORT", report.c_str(), 1);

    WorkerController controller;
    controller.start();
    if (!check(waitUntil([&controller] {
                   return controller.statusText().contains(QStringLiteral("connected to worker"));
               }),
               "error: handshake with the scripted peer")) {
        controller.shutdownNow();
        return;
    }

    controller.presentSceneJson(QString::fromStdString(sceneWordsOnly("AAA")));
    const bool reported = waitUntil([&controller] {
        return controller.statusText().contains(QStringLiteral("worker error"));
    });
    check(reported, "error: the rejected go is reported honestly");
    check(controller.sceneActive() == 0, "error: a failed go presents nothing");

    // The failed go's intent must be gone: this bare prepare's ack must NOT
    // auto-present (the pre-fix single auto-present flag would).
    controller.prepareScene(QString::fromStdString(sceneWordsOnly("BBB")));
    const bool prepared = waitUntil([&controller] { return controller.scenePrepared() == 1; });
    check(prepared, "error: the bare prepare is stored");
    // The prepare's ack precedes that poll's answer in the stream, so it is
    // already digested; refresh once more and any wrongly inherited intent
    // would show as a moved audience.
    digestThenRefresh(controller, 300);
    check(controller.sceneActive() == 0,
          "error: the bare prepare did not inherit the failed go's intent");

    // A deliberate present still works.
    controller.presentScene(1);
    const bool onAir = waitUntil([&controller] {
        return controller.sceneActive() == 1 && controller.scenePending() == 0;
    });
    check(onAir, "error: an explicit present moves the audience");
    check(controller.wordsOnAir() && !controller.backgroundOnAir(),
          "error: content flags follow the explicitly presented revision");

    controller.shutdownNow();
    const boost::json::object counts = readReport(report);
    check(counts.at("presents").to_number<std::int64_t>() == 1,
          "error: the worker was asked to present exactly once");
}

// S4: the worker dies mid-flight (no acks for the burst), every intent is
// reset, and the next connection starts from honest zero state.
void scenarioDisconnectAndReconnect() {
    const std::string report = tempPath("die");
    const std::string report2 = tempPath("clean");
    ::unlink(report.c_str());
    ::unlink(report2.c_str());

    WorkerController controller;
    ::setenv("OPENSHOW_FAKE_SCENARIO", "die-first-prepare", 1);
    ::setenv("OPENSHOW_FAKE_REPORT", report.c_str(), 1);
    controller.start();
    if (!check(waitUntil([&controller] {
                   return controller.statusText().contains(QStringLiteral("connected to worker"));
               }),
               "disconnect: handshake with the scripted peer")) {
        controller.shutdownNow();
        return;
    }
    controller.presentSceneJson(QString::fromStdString(sceneWordsAndBackground("AAA", 0x111111)));
    const bool died = waitUntil([&controller] {
        return !controller.isConnected() && controller.statusText().contains(QStringLiteral("worker exited"));
    });
    check(died, "disconnect: the worker's death is reported honestly");
    check(controller.sceneActive() == 0 && controller.scenePrepared() == 0,
          "disconnect: scene tracking reset with the connection");
    check(!controller.wordsOnAir() && !controller.backgroundOnAir(),
          "disconnect: content indicators are reset, not left describing a dead worker");

    // Second connection (generation 2): a fresh child on the same socket
    // path; a new go must behave exactly as on the first connection.
    ::setenv("OPENSHOW_FAKE_SCENARIO", "clean", 1);
    ::setenv("OPENSHOW_FAKE_REPORT", report2.c_str(), 1);
    controller.start();
    const bool reconnected = waitUntil([&controller] {
        return controller.statusText().contains(QStringLiteral("connected to worker"));
    });
    if (check(reconnected, "reconnect: a second connection completes its own handshake")) {
        controller.presentSceneJson(
            QString::fromStdString(sceneWordsAndBackground("DDD", 0x330000)));
        const bool onAir = waitUntil([&controller] {
            return controller.sceneActive() == 1 && controller.scenePending() == 0;
        });
        check(onAir, "reconnect: a fresh go presents on the new connection");
        check(controller.wordsOnAir() && controller.backgroundOnAir(),
              "reconnect: content flags describe the new scene");
    }
    controller.shutdownNow();
}

// S5 (task 6.3b): two clears clicked back to back must compose — the
// background clear derives from the words-cleared scene the first clear
// staged, not from the stale confirmed snapshot that still has both. The
// restore then brings the full pre-clear scene back, and canRestore only
// goes quiet once the restored revision is confirmed on air.
void scenarioBackToBackClears() {
    ::setenv("OPENSHOW_FAKE_SCENARIO", "clean", 1);
    const std::string report = tempPath("clears");
    ::unlink(report.c_str());
    ::setenv("OPENSHOW_FAKE_REPORT", report.c_str(), 1);

    WorkerController controller;
    controller.start();
    if (!check(waitUntil([&controller] {
                   return controller.statusText().contains(QStringLiteral("connected to worker"));
               }),
               "clears: handshake with the scripted peer")) {
        controller.shutdownNow();
        return;
    }

    controller.presentSceneJson(QString::fromStdString(sceneWordsAndBackground("FULL", 0x550000)));
    const bool fullOnAir = waitUntil([&controller] {
        return controller.sceneActive() == 1 && controller.wordsOnAir() &&
               controller.backgroundOnAir();
    });
    if (!check(fullOnAir, "clears: the full scene is on air before the clears")) {
        controller.shutdownNow();
        return;
    }

    controller.clearSlide();
    controller.clearBackground();  // same tick: the peer has acknowledged neither yet

    const bool composed = waitUntil([&controller] {
        return controller.sceneActive() == 3 && controller.scenePending() == 0;
    });
    check(composed, "clears: the composed clear reaches the audience");
    digestThenRefresh(controller, 50);
    check(!controller.wordsOnAir() && !controller.backgroundOnAir(),
          "clears: back-to-back clears compose (neither layer survives)");
    check(controller.canRestore(), "clears: restore stays available after the clear sequence");

    controller.restoreOutput();
    const bool restored = waitUntil([&controller] {
        return controller.sceneActive() == 4 && controller.wordsOnAir() &&
               controller.backgroundOnAir();
    });
    check(restored, "clears: restore brings both cleared layers back");
    check(waitUntil([&controller] { return !controller.canRestore(); }, 1500),
          "clears: canRestore retires once the restore is confirmed on air");

    controller.shutdownNow();
    const boost::json::object counts = readReport(report);
    const boost::json::array& presented = counts.at("presented").as_array();
    // Full go, the composed clear (the first clear's go is superseded by
    // the second per the latest-go rule), and the restore.
    check(counts.at("presents").to_number<std::int64_t>() == 3 && presented.size() == 3 &&
              presented[0].to_number<std::int64_t>() == 1 &&
              presented[1].to_number<std::int64_t>() == 3 &&
              presented[2].to_number<std::int64_t>() == 4,
          "clears: presented full, composed clear, restored — nothing else");
}

// S6 (task 6.3b): more prepares than the worker's store window must not
// cost the restore its content. The pre-fix restore presented the pre-clear
// REVISION; once the store pruned it the audience was out of luck. The
// retained payload must re-prepare instead.
void scenarioRestoreSurvivesStoreWindow() {
    ::setenv("OPENSHOW_FAKE_SCENARIO", "clean", 1);
    const std::string report = tempPath("window");
    ::unlink(report.c_str());
    ::setenv("OPENSHOW_FAKE_REPORT", report.c_str(), 1);

    WorkerController controller;
    controller.start();
    if (!check(waitUntil([&controller] {
                   return controller.statusText().contains(QStringLiteral("connected to worker"));
               }),
               "window: handshake with the scripted peer")) {
        controller.shutdownNow();
        return;
    }

    controller.presentSceneJson(
        QString::fromStdString(sceneWordsAndBackground("KEPT", 0x660000)));
    const bool onAir = waitUntil([&controller] {
        return controller.sceneActive() == 1 && controller.wordsOnAir();
    });
    if (!check(onAir, "window: the full scene is on air")) {
        controller.shutdownNow();
        return;
    }

    controller.clearSlide();
    const bool cleared = waitUntil([&controller] {
        return controller.sceneActive() == 2 && !controller.wordsOnAir();
    });
    check(cleared, "window: the clear is on air");

    // Push the store far past its window with bare prepares (they store
    // without presenting, so the clear's pre-clear revision is what ages).
    for (int i = 0; i < 20; ++i) {
        controller.prepareScene(QString::fromStdString(sceneWordsOnly("filler")));
    }
    const bool stored = waitUntil([&controller] { return controller.scenePrepared() == 22; });
    if (!check(stored, "window: twenty more prepares filled the store")) {
        controller.shutdownNow();
        return;
    }
    check(controller.canRestore(), "window: the retained restore content is still available");

    controller.restoreOutput();
    const bool restored =
        waitUntil([&controller] {
            return controller.sceneActive() == 23 && controller.wordsOnAir() &&
                   controller.backgroundOnAir();
        },
                  10000);
    check(restored, "window: restore re-prepared the retained content past the store window");
    check(waitUntil([&controller] { return !controller.canRestore(); }, 1500),
          "window: the confirmed restore retires the snapshot");

    controller.shutdownNow();
    const boost::json::object counts = readReport(report);
    const boost::json::array& presented = counts.at("presented").as_array();
    // Full scene, the clear, and the restore of re-prepared content. The
    // restore's present names the fresh revision 23: revision 1 is far
    // outside the window and a present of it would have been refused.
    check(counts.at("presents").to_number<std::int64_t>() == 3 && presented.size() == 3 &&
              presented.back().to_number<std::int64_t>() == 23,
          "window: the restore presented the re-prepared revision, not the pruned one");
}

// S7 (task 6.3b): a restore the worker refuses must leave the retry
// available and the indicators honest — the pre-fix code consumed the
// restore slot before any acknowledgement, so one error lost restore.
void scenarioFailedRestoreRetry() {
    ::setenv("OPENSHOW_FAKE_SCENARIO", "fail-second-full", 1);
    const std::string report = tempPath("retry");
    ::unlink(report.c_str());
    ::setenv("OPENSHOW_FAKE_REPORT", report.c_str(), 1);

    WorkerController controller;
    controller.start();
    if (!check(waitUntil([&controller] {
                   return controller.statusText().contains(QStringLiteral("connected to worker"));
               }),
               "retry: handshake with the scripted peer")) {
        controller.shutdownNow();
        return;
    }

    controller.presentSceneJson(
        QString::fromStdString(sceneWordsAndBackground("RETAINME", 0x770000)));
    const bool onAir = waitUntil([&controller] {
        return controller.sceneActive() == 1 && controller.wordsOnAir() &&
               controller.backgroundOnAir();
    });
    if (!check(onAir, "retry: the full scene is on air")) {
        controller.shutdownNow();
        return;
    }

    controller.clearSlide();
    const bool cleared = waitUntil([&controller] {
        return controller.sceneActive() == 2 && !controller.wordsOnAir();
    });
    check(cleared, "retry: the words clear is on air");

    controller.restoreOutput();  // refused by the scripted peer
    const bool reported = waitUntil([&controller] {
        return controller.statusText().contains(QStringLiteral("worker error"));
    });
    check(reported, "retry: the refused restore is reported honestly");
    check(controller.canRestore(), "retry: a failed restore keeps the retry available");
    digestThenRefresh(controller, 300);
    check(controller.sceneActive() == 2 && !controller.wordsOnAir(),
          "retry: the refused restore did not move the audience");

    controller.restoreOutput();  // the retry is accepted
    const bool restored = waitUntil([&controller] {
        return controller.sceneActive() == 3 && controller.wordsOnAir() &&
               controller.backgroundOnAir();
    });
    check(restored, "retry: the retried restore brings the retained content on air");
    check(waitUntil([&controller] { return !controller.canRestore(); }, 1500),
          "retry: the confirmed restore retires the snapshot");

    controller.shutdownNow();
    const boost::json::object counts = readReport(report);
    // Three presents total: the full scene, the clear's go, and the
    // retried restore. The refused restore never presented — a fourth
    // present, or one naming a revision that never stored, would show the
    // controller lying about the rejection.
    const boost::json::array& presented = counts.at("presented").as_array();
    check(counts.at("presents").to_number<std::int64_t>() == 3 && presented.size() == 3 &&
              presented[0].to_number<std::int64_t>() == 1 &&
              presented[1].to_number<std::int64_t>() == 2 &&
              presented[2].to_number<std::int64_t>() == 3,
          "retry: the refused restore presented nothing; the retry did");
}

// S8 (task 6.8a, review R11): input this build knows the worker will refuse is
// refused HERE, in the client, and the operator is told why. The scripted peer is
// deliberately a plain, accepting one: everything these assertions measure is
// what did NOT arrive, which only an unrefusing peer can show.
void scenarioRefusedLocally() {
    ::setenv("OPENSHOW_FAKE_SCENARIO", "clean", 1);
    const std::string report = tempPath("local-refusal");
    ::unlink(report.c_str());
    ::setenv("OPENSHOW_FAKE_REPORT", report.c_str(), 1);

    WorkerController controller;
    controller.start();
    if (!check(waitUntil([&controller] {
                   return controller.statusText().contains(QStringLiteral("connected to worker"));
               }),
               "local: handshake with the scripted peer")) {
        controller.shutdownNow();
        return;
    }

    // The operator's paste: text over the line budget. It is refused before the
    // socket, so the peer must see no prepare, and the status line must name the
    // scene as the thing that was not sent — "worker error" arriving five
    // seconds later, or a dropped connection, are both the bug this replaces.
    controller.presentSceneJson(QString::fromStdString(oversizeLinesScene()));
    if (!check(waitUntil([&controller] {
                   return controller.statusText().contains(QStringLiteral("was not sent"));
               }),
               "local: oversized text is reported as not sent, in the operator's words")) {
        std::fprintf(stderr, "  status was: %s\n",
                     controller.statusText().toLocal8Bit().constData());
    }
    check(controller.scenePrepared() == 0 && controller.sceneActive() == 0,
          "local: a scene that was not sent stored nothing and moved nothing");

    // A media-byte payload in an asset reference is refused for its own reason,
    // and again without a byte on the wire.
    controller.prepareScene(QString::fromStdString(smuggledMediaPayload()));
    if (!check(waitUntil([&controller] {
                   return controller.statusText().contains(
                       QStringLiteral("the scene was not sent"));
               }) &&
                   controller.statusText().contains(QStringLiteral("local files only")),
               "local: a data: asset reference is refused by the rule it broke")) {
        std::fprintf(stderr, "  status was: %s\n",
                     controller.statusText().toLocal8Bit().constData());
    }

    // And the session is intact: the very next go is a normal one, acknowledged,
    // presented, and on air. A guard that refused by hanging up, or that consumed
    // the request id it never used, would show up here as a go that hangs.
    controller.presentSceneJson(QString::fromStdString(sceneWordsOnly("AFTER")));
    const bool onAir = waitUntil([&controller] {
        return controller.sceneActive() == 1 && controller.wordsOnAir() &&
               controller.scenePending() == 0;
    });
    check(onAir, "local: the go after a refusal still reaches the worker and the audience");

    // A refused NEW scene must not cost the operator the restore snapshot a
    // clear was keeping: the refusal changes nothing at all, so the content that
    // could put the audience back is still there. (The order matters: a client
    // that cleared the snapshot before trying to send loses the restore on a
    // refusal, which the audience would feel as "restore does nothing".)
    controller.clearSlide();
    const bool cleared = waitUntil([&controller] {
        return controller.sceneActive() == 2 && !controller.wordsOnAir();
    });
    if (!check(cleared, "local: the words clear is on air")) {
        controller.shutdownNow();
        return;
    }
    controller.presentSceneJson(QString::fromStdString(oversizeLinesScene()));
    digestThenRefresh(controller, 200);
    check(controller.sceneActive() == 2 && !controller.wordsOnAir(),
          "local: a refused present leaves the audience where the clear put it");
    check(controller.canRestore(),
          "local: a refused present keeps the restore the clear was holding");
    controller.restoreOutput();
    check(waitUntil([&controller] {
              return controller.sceneActive() == 3 && controller.wordsOnAir();
          }),
          "local: and that restore still puts the retained content on air");

    // The transport's own budget, from the sender's side: a request whose frame
    // cannot be built is refused with a status line rather than written in
    // pieces. The peer counts requests for methods it does not model, so this
    // also proves not one byte of the giant frame left the process.
    controller.setLayerEnabled(QString(3u << 20, QChar('l')), true);
    // Named by method: the status line already carries the scene refusals, and a
    // check that only looked for "was not sent" would pass on those.
    if (!check(waitUntil([&controller] {
                   return controller.statusText().contains(QStringLiteral("layer.setEnabled")) &&
                          controller.statusText().contains(QStringLiteral("was not sent"));
               }),
               "local: a request over the frame budget is refused, not written")) {
        std::fprintf(stderr, "  status was: %s\n",
                     controller.statusText().toLocal8Bit().constData());
    }

    // The counter has to be proved live as well as silent: this ordinary call
    // DOES reach the peer, so a report of "one" below means the giant one did
    // not, rather than the peer counting nothing at all.
    controller.setLayerEnabled(QStringLiteral("clock"), true);
    digestThenRefresh(controller, 300);
    controller.shutdownNow();

    const boost::json::object counts = readReport(report);
    // The accepted go, the clear's derived scene, and the restore of the retained
    // content: three prepares and three presents. The two refused scenes and the
    // unbuildable frame contributed no request of any kind — which is the whole
    // acceptance, since scenePrepared() was already 0 after the first refusals.
    const boost::json::array& presented = counts.at("presented").as_array();
    check(counts.at("prepares").to_number<std::int64_t>() == 3 &&
              counts.at("presents").to_number<std::int64_t>() == 3 && presented.size() == 3 &&
              presented[0].to_number<std::int64_t>() == 1 &&
              presented[1].to_number<std::int64_t>() == 2 &&
              presented[2].to_number<std::int64_t>() == 3,
          "local: three prepares and three presents, the refusals contributing neither");
    check(counts.at("unknown").to_number<std::int64_t>() == 1,
          "local: the over-budget frame never reached the worker (the ordinary one did)");
}

// S9 (task 6.8b, review R11): a worker that takes every command and answers
// nothing. Once the worker is the thing that has stopped, the client is the only
// party left with a bound on how deep the hole gets, and two rules decide it: a
// bounded number of requests may be outstanding, and the status refresh may not
// ask a second question while the first is still unanswered. Both are read from
// the peer's own counts, because the honest answer to "did the client stop?" is
// "count what arrived".
void scenarioUnansweredRequestsStayBounded() {
    ::setenv("OPENSHOW_FAKE_SCENARIO", "never-answer", 1);
    const std::string report = tempPath("never-answer");
    ::unlink(report.c_str());
    ::setenv("OPENSHOW_FAKE_REPORT", report.c_str(), 1);

    WorkerController controller;
    controller.start();
    if (!check(waitUntil([&controller] {
                   return controller.statusText().contains(QStringLiteral("connected to worker"));
               }),
               "unanswered: handshake with the scripted peer")) {
        controller.shutdownNow();
        return;
    }

    // Five of the refresh poll's 500 ms ticks, every one of them arriving on a
    // status request that will never be answered. Before task 6.8b each tick
    // added a request of its own; now the tick that finds one outstanding asks
    // nothing, because the answer to "what does the output look like" is already
    // on its way and a second copy only lengthens the queue in front of the
    // commands the operator actually typed.
    pumpEventsFor(2600);
    boost::json::object counts = readReport(report);
    check(methodCount(counts, "output.status") == 1,
          "unanswered: the refresh poll asked once and waited — five poll ticks passed "
          "against an unanswered request, and only one status request arrived");

    // Now the operator keeps working. Commands are accepted while the worker
    // owes answers, and then the client says no — naming the rule and the count,
    // because "the worker is slow" is not something the operator can act on.
    for (int i = 0; i < 200; ++i) {
        controller.setLayerEnabled(QStringLiteral("clock"), i % 2 == 0);
    }
    if (!check(waitUntil([&controller] {
                   return controller.statusText().contains(
                       QStringLiteral("requests are already waiting for the worker to reply"));
               }),
               "unanswered: past the in-flight bound a command is refused in the client")) {
        std::fprintf(stderr, "  status was: %s\n",
                     controller.statusText().toLocal8Bit().constData());
    }
    if (!check(controller.statusText().contains(QStringLiteral("64 requests")),
               "unanswered: the refusal says how many are outstanding, which is the "
               "difference between a wedged worker and a mistyped command")) {
        std::fprintf(stderr, "  status was: %s\n",
                     controller.statusText().toLocal8Bit().constData());
    }

    // The bound is on what the worker receives, not on what the client claims:
    // every request it accepted is counted by the peer, every refused one is
    // absent — no byte, and no pending entry waiting for an answer that cannot
    // come. The answered handshake is subtracted because it is not in flight.
    pumpEventsFor(300);
    counts = readReport(report);
    const std::int64_t outstanding =
        counts.at("requests").as_int64() - methodCount(counts, "hello");
    check(outstanding == 64,
          "unanswered: exactly the bound worth of requests reached the peer, from 200 "
          "attempts and counting");
    check(methodCount(counts, "output.status") == 1,
          "unanswered: and the refresh poll still has not piled a second question behind "
          "the first, 200 commands later");

    // Refusing is not hanging up: the worker is alive and may yet answer, and the
    // operator's session — the scene mirror, the restore, the intent — is worth
    // keeping. A client that "fixed" this by disconnecting would take the rig
    // down for a worker that was only behind.
    check(controller.isConnected(),
          "unanswered: a refused command does not end the session");

    // And supervision still ends the process on its own schedule: the shutdown
    // request itself is refused by the same bound, so what remains is the
    // grace-then-terminate path, which is bounded without needing the worker.
    const auto shutdownStarted = std::chrono::steady_clock::now();
    controller.shutdownNow();
    const long shutdownMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - shutdownStarted)
                                .count();
    check(shutdownMs <= 5000, "unanswered: shutdown stays bounded with a worker that answers "
                              "nothing (grace, then terminate)");

    counts = readReport(report);
    check(counts.at("requests").as_int64() - methodCount(counts, "hello") == 64,
          "unanswered: the bound held through shutdown too — the refused shutdown added "
          "nothing to the peer");
}

// S10 (task 6.8b, review R11): a worker that stops reading altogether. Here the
// client's own socket buffer is the thing that grows, so the rules are its
// backlog budget and the deadline that gives up on a peer which has taken no
// byte. The budget has to answer with a refusal that names it; the depth has to
// stop at the bound rather than run to whatever the operator's queue holds; the
// deadline has to close the connection in bounded time; and the reason has to
// reach the status line, because "worker connection lost" would send the
// operator looking for a crash in a worker that is still sitting there.
void scenarioWriteStallIsRefusedBoundedAndClosed() {
    ::setenv("OPENSHOW_FAKE_SCENARIO", "stop-reading", 1);
    const std::string report = tempPath("stop-reading");
    ::unlink(report.c_str());
    ::setenv("OPENSHOW_FAKE_REPORT", report.c_str(), 1);

    WorkerController controller;
    controller.start();
    if (!check(waitUntil([&controller] {
                   return controller.statusText().contains(QStringLiteral("connected to worker"));
               }),
               "stalled: handshake with the scripted peer")) {
        controller.shutdownNow();
        return;
    }
    pumpEventsFor(250);  // let the peer take the handshake, then go deaf

    // Commands with something behind them: 100 KiB of layer name each, so the
    // backlog crosses its budget in tens of requests rather than tens of
    // thousands. (The name is not a real layer on purpose: what is under test is
    // the transport's backlog, and 100 KiB is comfortably inside the frame
    // budget, so it is the backlog rule that answers and not 6.8a's.)
    const QString bigName(100u * 1024u, QChar('l'));
    qint64 deepestKiB = -1;
    bool namedTheRule = false;
    for (int i = 0; i < 90; ++i) {
        controller.setLayerEnabled(bigName, i % 2 == 0);
        const QString text = controller.statusText();
        if (text.contains(QStringLiteral("which is not reading them"))) {
            namedTheRule = true;
        }
        deepestKiB = std::max(deepestKiB, queuedKiBIn(text));
    }
    check(namedTheRule,
          "stalled: a request over the backlog budget is refused, and the status line says "
          "the worker is the half that is not reading");
    check(deepestKiB >= 3900,
          "stalled: the backlog really reached its budget, so the ceiling below is not "
          "passing on a flood too small to have needed one");
    check(deepestKiB <= 4200,
          "stalled: and it stopped at the budget — 90 commands of 100 KiB became a queue of "
          "the bound and nothing more");

    // A backlog refusal is a refusal, not a hangup: the worker may yet start
    // reading again, and dropping the session then would punish the operator for
    // the worker's own stall.
    check(controller.isConnected(),
          "stalled: crossing the backlog budget does not by itself close the connection");

    // The deadline is what closes it, on a peer that took no byte for the whole
    // window — and it says so, because the alternative is an operator hunting
    // for a crash that never happened.
    const auto armed = std::chrono::steady_clock::now();
    const bool closed = waitUntil([&controller] { return !controller.isConnected(); }, 4000);
    const long closedAfter = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - armed)
                                 .count();
    check(closed, "stalled: a peer that takes no byte for the whole deadline is given up on, "
                  "and the connection ends without the operator asking");
    check(closedAfter >= 1000,
          "stalled: the deadline is a window, not a hair trigger — a worker that resumes "
          "reading keeps its session");
    check(closedAfter <= 3600, "stalled: and it is prompt, so a stalled worker cannot hold "
                               "the UI's traffic open indefinitely");
    if (!check(controller.statusText().contains(QStringLiteral("has not read a byte")),
               "stalled: the operator is told why the connection went")) {
        std::fprintf(stderr, "  status was: %s\n",
                     controller.statusText().toLocal8Bit().constData());
    }
    check(queuedKiBIn(controller.statusText()) >= 3900,
          "stalled: the message carries how deep the queue was, because abort() discarded it "
          "and the log is the operator's only evidence");

    controller.shutdownNow();
    const boost::json::object counts = readReport(report);
    check(counts.at("stoppedReading").is_bool() && counts.at("stoppedReading").as_bool(),
          "stalled: the peer really did stop reading, so the checks above describe a stalled "
          "worker and not a merely busy one");
    check(methodCount(counts, "layer.setEnabled") == 0,
          "stalled: none of the flooded commands were even read by the peer, so the bound "
          "that mattered was the client's");
}

// S11 (task 6.8b, review R11): the peer that comes back. S10 is the scenario in
// which the write deadline has to close the connection; this is the one in which
// it must NOT, and the difference between the two is the only thing a deadline is
// allowed to measure — progress. The peer goes deaf long enough for the client to
// fill its backlog budget and arm the window, then reads again, slowly, so that
// at the moment of the verdict several megabytes have LEFT the queue while some of
// it is still owed. A deadline that closes regardless drops a worker that was on
// its way back; one that insists the queue be empty by then drops a worker that
// was merely behind. Neither is the rule R11 asked for, and only this scenario
// can tell them apart.
void scenarioStalledThenResumingPeerKeepsItsSession() {
    ::setenv("OPENSHOW_FAKE_SCENARIO", "stall-then-read", 1);
    const std::string report = tempPath("stall-then-read");
    ::unlink(report.c_str());
    ::setenv("OPENSHOW_FAKE_REPORT", report.c_str(), 1);

    WorkerController controller;
    controller.start();
    if (!check(waitUntil([&controller] {
                   return controller.statusText().contains(QStringLiteral("connected to worker"));
               }),
               "resumed: handshake with the scripted peer")) {
        controller.shutdownNow();
        return;
    }
    pumpEventsFor(200);  // let the peer take the handshake, then go deaf

    // The same flood as S10 and the same first outcome: the backlog reaches its
    // budget, requests are refused, and a refusal is what arms the window.
    // Without that there is no verdict to wait for, so this is the premise of
    // the scenario rather than one of its conclusions.
    const QString bigName(100u * 1024u, QChar('l'));
    qint64 deepestKiB = -1;
    bool armed = false;
    for (int i = 0; i < 90; ++i) {
        controller.setLayerEnabled(bigName, i % 2 == 0);
        const QString text = controller.statusText();
        if (text.contains(QStringLiteral("which is not reading them"))) {
            armed = true;
        }
        deepestKiB = std::max(deepestKiB, queuedKiBIn(text));
    }
    check(armed,
          "resumed: the backlog hit the budget, so the deadline below has a real decision to "
          "make about a real stalled worker");
    check(deepestKiB >= 3900,
          "resumed: and the queue really was full when the peer's attention came back");

    // Hold the loop open past the two-second verdict, with nothing to wait for
    // but the peer's own pace.
    pumpEventsFor(2600);
    check(controller.isConnected(),
          "resumed: a peer that starts reading again inside the window keeps its session — the "
          "deadline measures whether the backlog is moving, not how deep it found it");
    if (!check(!controller.statusText().contains(QStringLiteral("has not read a byte")),
               "resumed: and no one tells the operator the worker was given up on")) {
        std::fprintf(stderr, "  status was: %s\n",
                     controller.statusText().toLocal8Bit().constData());
    }

    // Surviving is not the same as being written off: what the client accepted is
    // still owed, and a recovered socket pays the debt in full at the peer's own
    // pace rather than having had it thrown away with the stall.
    std::int64_t arrived = 0;
    std::int64_t takenBytes = -1;
    int quietRounds = 0;
    for (int round = 0; round < 150 && quietRounds < 5; ++round) {
        const boost::json::object counts = readReport(report);
        const std::int64_t taken = reportNumber(counts, "bytesReceived");
        quietRounds = taken == takenBytes ? quietRounds + 1 : 0;
        takenBytes = taken;
        arrived = methodCount(counts, "layer.setEnabled");
        pumpEventsFor(100);
    }
    check(takenBytes >= 3 * 1024 * 1024,
          "resumed: the backlog was delivered over the recovered connection, not discarded the "
          "moment the peer became readable again");
    check(arrived >= 30,
          "resumed: every command the client had accepted eventually reached the worker "
          "(the ones it refused are S10's subject, and never went out at all)");
    check(controller.isConnected(),
          "resumed: and the session outlived the drain it was carrying");

    // And it is a working session, not merely an un-closed one: a later command
    // travels, both ways, on the connection that was nearly given up on.
    const std::int64_t beforeCommand = arrived;
    controller.setLayerEnabled(QStringLiteral("clock"), true);
    check(waitUntil(
              [&report, beforeCommand] {
                  return methodCount(readReport(report), "layer.setEnabled") > beforeCommand;
              },
              3000),
          "resumed: a command after the storm still arrives, so the conversation survived and "
          "not just the socket");

    controller.shutdownNow();
    const boost::json::object counts = readReport(report);
    const auto resumed = counts.find("resumed");
    check(resumed != counts.end() && resumed->value().is_bool() && resumed->value().as_bool(),
          "resumed: the peer really did stop and start again, so the checks above describe a "
          "recovering worker rather than one that was never stressed");
}

// S12 (task 6.12, review R7): a save MOVES an asset — the model now names
// the package's copy of the bytes — and tells this side through
// adoptAssetPaths. Every payload the client still holds has to stop naming
// the old path; the scene owed to the output must be RE-PREPARED but never
// re-presented (presenting would restart the audience's fade); a clear must
// compose against the rewritten payload and a restore re-prepare it; and a
// map that names nothing the session uses must send nothing at all. The
// scripted peer's per-prepare asset-path ledger is what proves the traffic
// pattern, because every other client-visible property stays unchanged by
// a rename done right. A second round runs go -> clear -> adopt -> restore,
// so the rewrite that is tested is the one into the RETAINED snapshot
// itself, not one inherited from a payload adoption had already rewritten.
void scenarioAdoptedPaths() {
    ::setenv("OPENSHOW_FAKE_SCENARIO", "clean", 1);
    const std::string report = tempPath("adopt");
    ::unlink(report.c_str());
    ::setenv("OPENSHOW_FAKE_REPORT", report.c_str(), 1);

    WorkerController controller;
    controller.start();
    if (!check(waitUntil([&controller] {
                   return controller.statusText().contains(QStringLiteral("connected to worker"));
               }),
               "adopt: handshake with the scripted peer")) {
        controller.shutdownNow();
        return;
    }

    const QString oldPath = QStringLiteral("/old/dir/still.jpg");
    const QString newPath = QStringLiteral("/new/dir/still.jpg");
    controller.presentSceneJson(
        QString::fromStdString(sceneStill("PIC-A", oldPath.toStdString())));
    if (!check(waitUntil([&controller] {
                   return controller.sceneActive() == 1 && controller.scenePending() == 0;
               }),
               "adopt: the scene naming the original path is on air")) {
        controller.shutdownNow();
        return;
    }

    // A map naming a path no payload uses changes nothing and SENDS nothing:
    // a phantom re-prepare would raise the peer's prepared count, and the
    // status poll below has had every chance to report it.
    controller.adoptAssetPaths({{QStringLiteral("/nowhere/file.jpg"), newPath}});
    digestThenRefresh(controller, 300);
    check(controller.scenePrepared() == 1,
          "adopt: naming a path no payload uses re-prepared nothing");

    // The adoption itself: exactly one new prepare, and no present — the
    // audience is watching the revision that is already on air, whose path
    // the worker has pinned, and a fresh present would restart the fade.
    controller.adoptAssetPaths({{oldPath, newPath}});
    check(waitUntil([&controller] {
              return controller.scenePrepared() == 2 && controller.scenePending() == 0;
          }),
          "adopt: the scene owed to the output was re-prepared with the new path");
    check(controller.sceneActive() == 1,
          "adopt: the re-prepare was not a go - the audience never moved");

    // An explicit present of the FIRST revision: with this, the pending
    // logical presentation comes from the revision MIRROR's entry, so the
    // snapshot the next clear retains has to carry the rewritten name —
    // proof the mirror was told, not just the payload the adoption itself
    // re-prepared.
    controller.presentScene(1);
    digestThenRefresh(controller, 200);

    // A clear composes against the REWRITTEN pending payload, and the
    // restore re-prepares the rewritten snapshot: together they prove every
    // cached holder was told, not just the one the next go happens to read.
    controller.clearBackground();
    check(waitUntil([&controller] {
              return controller.sceneActive() == 3 && controller.scenePending() == 0;
          }),
          "adopt: the clear presented over the adopted payload");
    controller.restoreOutput();
    check(waitUntil([&controller] {
              return controller.sceneActive() == 4 && controller.scenePending() == 0;
          }),
          "adopt: the restore brought the adopted scene back");

    // Round two, in the order that catches the lazy rewrite: the OLD-named
    // scene goes out again, a clear RETAINS its old-named payload, and ONLY
    // THEN does the adoption arrive — so the rewrite that matters is the one
    // into the retained snapshot itself, not one inherited from a payload
    // that had already been rewritten. The restore that follows must name
    // the new file, and the adoption must re-prepare nothing: the scene
    // owed to the output is the cleared one, which names no file at all.
    controller.presentSceneJson(
        QString::fromStdString(sceneStill("PIC-A", oldPath.toStdString())));
    check(waitUntil([&controller] {
              return controller.sceneActive() == 5 && controller.scenePending() == 0;
          }),
          "adopt: the old-named scene went out again for the snapshot test");
    controller.clearBackground();
    check(waitUntil([&controller] {
              return controller.sceneActive() == 6 && controller.scenePending() == 0;
          }),
          "adopt: the second clear retained the old-named snapshot");
    controller.adoptAssetPaths({{oldPath, newPath}});
    digestThenRefresh(controller, 300);
    check(controller.scenePrepared() == 6,
          "adopt: with the cleared scene owed, the adoption re-prepared nothing new - it "
          "rewrote the retained snapshot instead (the sixth prepare is that clear)");
    controller.restoreOutput();
    check(waitUntil([&controller] {
              return controller.sceneActive() == 7 && controller.scenePending() == 0;
          }),
          "adopt: the second restore came back after the snapshot-time adoption");

    controller.shutdownNow();
    const boost::json::object counts = readReport(report);
    check(reportNumber(counts, "prepares") == 7,
          "adopt: seven prepares - the go, the re-prepare, the clear, the restore, and the "
          "snapshot round's go, clear, restore");
    check(reportNumber(counts, "presents") == 7,
          "adopt: seven presents - every go and re-present; the adoptions themselves added none");
    std::string joined;
    std::size_t listed = 0;
    if (const auto entry = counts.find("assetPaths");
        entry != counts.end() && entry->value().is_array()) {
        for (const auto& value : entry->value().as_array()) {
            if (value.is_string()) {
                joined += (joined.empty() ? "" : " | ") + std::string(value.as_string());
                ++listed;
            }
        }
    }
    const std::string o = oldPath.toStdString();
    const std::string n = newPath.toStdString();
    const std::string expect = o + " | " + n + " |  | " + n + " | " + o + " |  | " + n;
    check(listed == 7 && joined == expect,
          ("adopt: each prepare named exactly the background it should, the old path never "
           "named by anything the adoption ran over (got [" + joined + "])")
              .c_str());
    if (const auto presented = counts.find("presented");
        presented != counts.end() && presented->value().is_array()) {
        const boost::json::array& revs = presented->value().as_array();
        const std::vector<std::int64_t> wantRevisions{1, 1, 3, 4, 5, 6, 7};
        bool revsOk = revs.size() == wantRevisions.size();
        for (std::size_t i = 0; revsOk && i < wantRevisions.size(); ++i) {
            revsOk = revs[i].to_number<std::int64_t>() == wantRevisions[i];
        }
        check(revsOk, "adopt: the audience moved only for real go's, never for an adoption");
    } else {
        check(false, "adopt: the peer recorded its presented revisions");
    }
}

}  // namespace

// S13 (the R15 UI half): the media properties bind to WORKER status, never
// to the click. The scripted peer only moves its transport state through
// the media.* commands, and the controller's media* properties only move
// through the status echo — so a control driven by local optimism (a Pause
// label flipped on press, a slider that stayed where the finger left it
// after a refused seek) fails here. The report then proves the exact
// method-and-params frames the controller put on the wire, including the
// refused standalone gain (the client does NOT second-guess the worker's
// source table — it sends, and the refusal leaves the properties on the
// worker's truth). Task 6.15b extends the same peer with the independent
// track: a mix that spans two transports, a probe refusal that changes
// nothing, and a removal that takes the track's row while the embedded
// source keeps its own gain.
void scenarioMediaTransport() {
    ::setenv("OPENSHOW_FAKE_SCENARIO", "media", 1);
    const std::string report = tempPath("media");
    ::unlink(report.c_str());
    ::setenv("OPENSHOW_FAKE_REPORT", report.c_str(), 1);

    WorkerController controller;
    controller.start();
    if (!check(waitUntil([&controller] {
                   return controller.statusText().contains(QStringLiteral("connected to worker"));
               }),
               "media: handshake with the scripted peer")) {
        controller.shutdownNow();
        return;
    }
    // The first status carries the transport: live, playing, looping by
    // default, at the peer's 6 s duration, with the video source at unity.
    if (!check(waitUntil([&controller] { return controller.mediaOnAir(); }),
               "media: status brings the transport on air")) {
        controller.shutdownNow();
        return;
    }
    check(controller.mediaPlaying(), "media: the playing state comes from status");
    check(controller.mediaLoop(), "media: loop reads true before any command");
    check(std::abs(controller.mediaDuration() - 6.0) < 0.001,
          "media: the duration mirrors the peer's clip");
    {  // The source list mirrors into a [{id, gain, muted}] list for QML.
        const QVariantList sources = controller.mediaSources();
        bool oneUnitySource = sources.size() == 1;
        if (oneUnitySource) {
            const QVariantMap video = sources.first().toMap();
            oneUnitySource = video.value("id").toString() == QLatin1String("video") &&
                             std::abs(video.value("gain").toDouble() - 1.0) < 0.001 &&
                             video.value("muted").toBool() == false;
        }
        check(oneUnitySource, "media: the video source mirrors at unity, unmuted");
    }

    controller.mediaPause();
    check(waitUntil([&controller] { return !controller.mediaPlaying(); }),
          "media: the hold arrives THROUGH status, not on press");
    controller.mediaPlay();
    check(waitUntil([&controller] { return controller.mediaPlaying(); }),
          "media: the resume likewise");

    controller.mediaSeek(3.5);
    check(waitUntil([&controller] { return std::abs(controller.mediaPosition() - 3.5) < 0.001; }),
          "media: the acknowledged position mirrors into the property");

    controller.setMediaLoop(false);
    check(waitUntil([&controller] { return !controller.mediaLoop(); }),
          "media: the loop mode mirrors back after the toggle");

    controller.setMediaGain("video", 0.4);
    const bool gainBack = waitUntil([&controller] {
        const QVariantList sources = controller.mediaSources();
        return sources.size() == 1 &&
               std::abs(sources.first().toMap().value("gain").toDouble() - 0.4) < 0.001;
    });
    check(gainBack, "media: the gain echo replaces the slider's value via status");
    controller.setMediaMute("video", true);
    const bool muteBack = waitUntil([&controller] {
        const QVariantList sources = controller.mediaSources();
        return sources.size() == 1 && sources.first().toMap().value("muted").toBool();
    });
    check(muteBack, "media: the mute mirrors back");
    controller.setMediaMute("video", false);
    check(waitUntil([&controller] {
        const QVariantList sources = controller.mediaSources();
        return sources.size() == 1 && !sources.first().toMap().value("muted").toBool();
    }),
          "media: unmute mirrors back too");

    // A refusal from the worker must leave the properties on the worker's
    // truth: the standalone gain is refused, and nothing about the video
    // source moves — the client never invents a row for a source that was
    // never listed.
    controller.setMediaGain("standalone", 0.5);
    digestThenRefresh(controller, 300);
    {
        const QVariantList sources = controller.mediaSources();
        bool stillOneVideo = sources.size() == 1 &&
                             sources.first().toMap().value("id").toString() ==
                                 QLatin1String("video") &&
                             std::abs(sources.first().toMap().value("gain").toDouble() - 0.4) <
                                 0.001;
        check(stillOneVideo,
              "media: a refused source change leaves the mirror on the worker's truth");
    }

    // --- task 6.15b (review R15): the independent track, same wire ---
    //
    // The scripted peer only ever moves this state through the frames below,
    // so every property has to arrive through status: the mix rows spanning
    // both transports, a refused selection that changes nothing (the real
    // session probes before it accepts), and a removal that takes the row
    // with it while the video source keeps its own values.
    controller.selectStandaloneTrack("fake-bed.m4a");
    check(waitUntil([&controller] { return controller.standaloneOnAir(); }),
          "media: the selection arrives as its own live block");
    check(controller.standalonePath() == QLatin1String("fake-bed.m4a"),
          "media: the block names the file the worker took");
    check(controller.standalonePosition() > 0.0,
          "media: and the track's position is its own, not the background's");
    {
        const QVariantList sources = controller.mediaSources();
        bool both = sources.size() == 2;
        if (both) {
            const QVariantMap video = sources[0].toMap();
            const QVariantMap track = sources[1].toMap();
            both = video.value("id").toString() == QLatin1String("video") &&
                   std::abs(video.value("gain").toDouble() - 0.4) < 0.001 &&
                   track.value("id").toString() == QLatin1String("standalone") &&
                   std::abs(track.value("gain").toDouble() - 1.0) < 0.001 &&
                   !track.value("muted").toBool();
        }
        check(both, "media: both sources are listed, each with its own mix");
    }

    // A path the worker refuses must leave the mirror exactly where it was:
    // the track on air was never touched, and the client does not paint a
    // hopeful path of its own.
    controller.selectStandaloneTrack("/tmp/missing-bed.m4a");
    digestThenRefresh(controller, 300);
    check(controller.standaloneOnAir() &&
              controller.standalonePath() == QLatin1String("fake-bed.m4a"),
          "media: a refused selection keeps the track and its path on the worker's truth");

    controller.setMediaGain("standalone", 0.25);
    check(waitUntil([&controller] {
              const QVariantList sources = controller.mediaSources();
              return sources.size() == 2 &&
                     std::abs(sources[1].toMap().value("gain").toDouble() - 0.25) < 0.001;
          }),
          "media: the track's gain echo arrives without moving the video row");
    controller.setMediaMute("standalone", true);
    check(waitUntil([&controller] {
              const QVariantList sources = controller.mediaSources();
              return sources.size() == 2 && sources[1].toMap().value("muted").toBool() &&
                     !sources[0].toMap().value("muted").toBool() &&
                     std::abs(sources[0].toMap().value("gain").toDouble() - 0.4) < 0.001;
          }),
          "media: muting the track leaves the embedded source exactly as it was");

    controller.selectStandaloneTrack("");
    check(waitUntil([&controller] {
              return !controller.standaloneOnAir() && controller.mediaSources().size() == 1;
          }),
          "media: the removal takes the live flag and the row with it");
    {  // The video source survived the whole track lifetime untouched.
        const QVariantMap video = controller.mediaSources().first().toMap();
        check(video.value("id").toString() == QLatin1String("video") &&
                  std::abs(video.value("gain").toDouble() - 0.4) < 0.001,
              "media: and the background's own mix is exactly where it was left");
    }
    controller.setMediaGain("standalone", 0.75);
    digestThenRefresh(controller, 300);
    check(controller.mediaSources().size() == 1 && !controller.standaloneOnAir(),
          "media: a gain for the removed track is refused, not queued onto anything");

    controller.shutdownNow();

    const boost::json::object counts = readReport(report);
    const boost::json::array& ops = counts.at("mediaOps").as_array();
    auto findOp = [&ops](const char* method) -> const boost::json::object* {
        for (const boost::json::value& op : ops) {
            if (op.as_object().at("method").as_string() == method) {
                return &op.as_object();
            }
        }
        return nullptr;
    };
    auto countOps = [&ops](const char* method) {
        std::size_t n = 0;
        for (const boost::json::value& op : ops) {
            if (op.as_object().at("method").as_string() == method) {
                ++n;
            }
        }
        return n;
    };
    check(countOps(proto::kMethodMediaPause) == 1 && countOps(proto::kMethodMediaPlay) == 1,
          "media: exactly one play and one pause frame reached the wire");
    const boost::json::object* seek = findOp(proto::kMethodMediaSeek);
    check(seek != nullptr && seek->at("params").as_object().at("position").as_double() == 3.5,
          "media: the seek frame carried its position");
    const boost::json::object* loop = findOp(proto::kMethodMediaLoop);
    check(loop != nullptr && loop->at("params").as_object().at("loop").as_bool() == false,
          "media: the loop frame carried its boolean");
    const boost::json::object* gain = findOp(proto::kMethodMediaGain);
    check(gain != nullptr && gain->at("params").as_object().at("source").as_string() == "video" &&
              std::abs(gain->at("params").as_object().at("gain").as_double() - 0.4) < 0.001,
          "media: the gain frame carried source and value");
    check(countOps(proto::kMethodMediaMute) == 3,
          "media: every mute frame — video on, video off, track on — reached the wire");
    {  // The 6.15a lesson, restated now that the source is real: the gain the
       // client could not verify was still SENT (0.5, refused by the worker),
       // and the totals count refusals as frames rather than dropping them.
        bool refusedGainRode = false;
        for (const boost::json::value& op : ops) {
            const boost::json::object& frame = op.as_object();
            if (frame.at("method").as_string() == proto::kMethodMediaGain) {
                const boost::json::object& params = frame.at("params").as_object();
                if (params.at("source").as_string() == "standalone" &&
                    std::abs(params.at("gain").as_double() - 0.5) < 0.001) {
                    refusedGainRode = true;
                }
            }
        }
        check(countOps(proto::kMethodMediaGain) == 4 && refusedGainRode,
              "media: all four gain frames rode the wire, the refused probe included");
    }
    {  // Task 6.15b: the selection frames, in order, with the paths the
       // operator typed — including the refused one and the "" removal.
        boost::json::array selected;
        for (const boost::json::value& op : ops) {
            const boost::json::object& frame = op.as_object();
            if (frame.at("method").as_string() == proto::kMethodMediaStandalone) {
                selected.push_back(frame.at("params").as_object().at("path"));
            }
        }
        check(selected.size() == 3 && selected[0].as_string() == "fake-bed.m4a" &&
                  selected[1].as_string() == "/tmp/missing-bed.m4a" &&
                  selected[2].as_string().empty(),
              "media: the selection, the refused path and the removal all rode the wire verbatim");
    }
}

int main(int argc, char* argv[]) {
    const char* scenario = std::getenv("OPENSHOW_FAKE_SCENARIO");
    if (scenario != nullptr) {
        // Child mode: the scripted peer. Find --socket among the arguments
        // the supervisor passed.
        std::string socketPath;
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
                socketPath = argv[i + 1];
            }
        }
        if (socketPath.empty()) {
            return 4;
        }
        return FakeWorker(socketPath, scenario,
                          std::getenv("OPENSHOW_FAKE_REPORT") ? std::getenv("OPENSHOW_FAKE_REPORT")
                                                              : std::string())
            .run();
    }

    QCoreApplication app(argc, argv);
    ::setenv("OPENSHOW_WORKER_PROGRAM", argv[0], 1);

    scenarioBurst();
    scenarioSlowReply();
    scenarioErrorReply();
    scenarioDisconnectAndReconnect();
    scenarioBackToBackClears();
    scenarioRestoreSurvivesStoreWindow();
    scenarioFailedRestoreRetry();
    scenarioRefusedLocally();
    scenarioUnansweredRequestsStayBounded();
    scenarioWriteStallIsRefusedBoundedAndClosed();
    scenarioStalledThenResumingPeerKeepsItsSession();
    scenarioAdoptedPaths();
    scenarioMediaTransport();

    if (gFailures != 0) {
        std::fprintf(stderr, "worker_controller_tests: %d failure(s)\n", gFailures);
        return 1;
    }
    std::printf("worker_controller_tests: reply correlation and latest-go intent pinned\n");
    return 0;
}
