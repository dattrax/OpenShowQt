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
 *   preview          a real producer of the shared-memory preview stream plus
 *                    the off-air selection target (task 6.16a, review R16):
 *                    it creates the segment, publishes frames tagged with the
 *                    target and generation they answer, recycles a slot only
 *                    on a release that proves ownership, refuses a generation
 *                    that does not follow the accepted one, and delivers one
 *                    frame deliberately LATE — the only way to watch the
 *                    consumer drop imagery of an ask it has superseded
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
 *
 * Task 6.21e (review F4) added S16: the gradient background. The peer records
 * whether each prepare's background still named its second colour, which is
 * how the fixture proves the clear took the key off the wire rather than
 * merely moving a property in the client.
 *
 * Task 6.21f (review F6) added S17: the per-item foreground faults. The peer
 * (scenario "foreground") reports the worker's own `scene.foregroundErrors`
 * shape, arms and repairs it from the markers a prepared payload carries, and
 * keeps its transport faulted the whole time — so the fixture sees the notice
 * project path and reason, clear on an acknowledged repair, and leave the
 * media lane's separate report alone.
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
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <QCoreApplication>
#include <QString>
#include <QVariant>

#include "openshow/protocol/preview_wire.h"
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
        cleanupPreviewSegment();
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
            // A preview frame the peer is holding back. Pixels go into the
            // slot when the frame is DUE, not when it was asked for: the point
            // of the delay is that a newer ask has been accepted, and drawn,
            // by the time this one reaches the wire.
            for (auto entry = fPendingFrames.begin(); entry != fPendingFrames.end();) {
                if (entry->when <= flushBefore) {
                    // A held ROW stamps the generation accepted NOW unless it
                    // was armed to keep its own: those two are the mid-draw
                    // row and the slipped bookkeeping, respectively.
                    const std::uint32_t stamp =
                        entry->stampOwnRequest || entry->target != proto::kPreviewTargetThumbnail
                            ? entry->request
                            : static_cast<std::uint32_t>(fThumbnailAccepted);
                    publishPreviewFrame(entry->target, entry->revision,
                                        entry->width != 0 ? entry->width : kFrameWidth,
                                        entry->height != 0 ? entry->height : kFrameHeight,
                                        entry->legacy, stamp, entry->lieAboutGeometry);
                    entry = fPendingFrames.erase(entry);
                } else {
                    ++entry;
                }
            }
            publishOwedThumbnails();  // the slide list's band, one row per wake
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
            // Task 6.16a (review R16): the controller only asks for a stream
            // when the worker says it has one, and only offers the edit
            // canvas a worker-drawn picture when the SECOND capability is
            // present. Advertising both is what lets this peer be the one
            // that draws the frames the fixture reads.
            if (fScenario == "preview") {
                result["capabilities"] = boost::json::array{"preview.shm.v1",
                                                            "preview.stream-id.v1",
                                                            "preview.select.v1",
                                                            "preview.thumbnail.v1"};
            }
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
            };
            if (fScenario == "foreground") {
                // Task 6.21f (review F6): the per-item foreground faults,
                // shaped exactly as the real worker shapes them — a capped
                // list of {path, error} beside a monotonic event count —
                // and the transport's own lane faulted throughout, so the
                // fixture can see the two reports refuse to interfere.
                boost::json::array faults;
                for (const auto& fault : fForegroundFaults) {
                    faults.push_back(
                        boost::json::object{{"path", fault.first}, {"error", fault.second}});
                }
                scene["foregroundErrors"] = std::move(faults);
                scene["foregroundFailureEvents"] = fForegroundEvents;
                result["media"] =
                    boost::json::object{{"playing", true},
                                        {"path", "fake-clip.mp4"},
                                        {"error", "fake: background decode failed"},
                                        {"audio", "silent"},
                                        {"position", 1.0},
                                        {"live", true},
                                        {"loop", true},
                                        {"duration", 6.0},
                                        {"sources", boost::json::array{}},
                                        {"standalone", boost::json::object{{"live", false},
                                                                           {"error", ""}}}};
            }
            result["scene"] = std::move(scene);
            result["preview"] =
                boost::json::object{{"running", false},
                                    {"dropped", 0},
                                    {"error", ""}};
            // Task 6.16a: the scripted stream is RUNNING, and its second
            // target reports for itself. `stale` is derived, not pinned: the
            // peer counts a superseded draw when a new ask arrives while a
            // previously accepted one is still undelivered, so the count the
            // controller mirrors is the peer's state and not a number the
            // fixture chose. `active` goes false once after a prepare whose
            // slide is marked FORGET — a worker that has lost a target the
            // client believes it asked for, which the client must recover
            // from by asking again rather than by painting nothing.
            if (fScenario == "preview") {
                bool active = fSelectionActive;
                if (fForgetReportsLeft > 0) {
                    --fForgetReportsLeft;
                    active = false;
                }
                result["preview"] = boost::json::object{
                    {"running", true},
                    {"dropped", static_cast<std::uint64_t>(fPreviewDropped)},
                    {"error", ""},
                    {"selection", boost::json::object{{"active", active},
                                                      {"revision", fSelectionRevision},
                                                      {"request", fSelectionAccepted},
                                                      {"published", fSelectionPublished},
                                                      {"dropped", 0},
                                                      {"deferred", 0},
                                                      {"stale", fSelectionStale}}},
                    // The slide list (task 6.16b). `active` goes false once for
                    // a batch armed with FORGET — a worker that lost a band the
                    // client believes it asked for, which the client must answer
                    // by asking again — and `queued` is this peer's own debt, so
                    // the fixture can see a replaced band stop being owed rather
                    // than taking the client's word for it.
                    {"thumbnail",
                     boost::json::object{
                         {"active", fThumbnailForgetLeft > 0 ? (fThumbnailForgetLeft -= 1, false)
                                                             : fThumbnailActive},
                         {"request", fThumbnailAccepted},
                         {"width", fThumbnailWidth},
                         {"height", fThumbnailHeight},
                         {"queued", fThumbnailRows.size() + thumbnailRowsInFlight()},
                         {"decodePending", 0},
                         {"published", fThumbnailPublished},
                         {"dropped", 0},
                         {"deferred", 0},
                         {"stale", fThumbnailStale},
                         {"images", boost::json::object{{"budget", 12u * 1024u * 1024u},
                                                        {"decodedBytes", 0},
                                                        {"entries", 0},
                                                        {"evictions", 0}}}}},
                };
            }
            // Task 6.15a (review R15): a scripted transport whose state ONLY
            // the media.* commands move. The controller reads its media
            // properties from this echo and nowhere else, so a control that
            // ran on local optimism — a Pause label flipped by the click —
            // cannot survive here.
            if (fScenario == "preview") {
                // ... and the account behind it is refreshed on every poll,
                // because a fixture that is proving the client sent NOTHING has
                // nothing else to wake this peer with (task 6.16b).
                writeReport();
            }
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
            std::string slideId;
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
                const auto slide = params.find("slideId");
                if (slide != params.end() && slide->value().is_string()) {
                    slideId = std::string(slide->value().as_string());
                }
                // Task 6.21b (review F2): and every PATH-BEARING ITEM. A
                // foreground picture is the operator's file just as much as
                // a background one, so the adoption fixture asserts over
                // these too — space-joined, in stack order, per prepare.
                std::string itemPaths;
                const auto items = params.find("items");
                if (items != params.end() && items->value().is_array()) {
                    for (const auto& element : items->value().as_array()) {
                        if (!element.is_object()) {
                            continue;
                        }
                        const auto path = element.as_object().find("assetPath");
                        if (path != element.as_object().end() && path->value().is_string()) {
                            if (!itemPaths.empty()) {
                                itemPaths += " ";
                            }
                            itemPaths += std::string(path->value().as_string());
                        }
                    }
                }
                fItemPaths.push_back(std::move(itemPaths));
            }
            fAssetPaths.push_back(std::move(assetPath));
            // Task 6.21e (review F4): whether THIS prepare's background still
            // names a gradient. The clear must take the key with it, and the
            // fixture reads the wire rather than trusting the client's own
            // bookkeeping: "" means flat, anything else is the second colour.
            std::string gradient;
            if (request.contains("params") && request.at("params").is_object()) {
                const auto background = request.at("params").at("background");
                if (background.is_object()) {
                    const auto second = background.as_object().find("gradient");
                    if (second != background.as_object().end()) {
                        gradient = boost::json::serialize(second->value());
                    }
                }
            }
            fGradients.push_back(std::move(gradient));
            if (fScenario == "foreground") {
                // The marked prepares drive the fault lane (see above).
                // Each marker names ONE path, so the list and the count
                // grow the way the worker's do, and the repair clears the
                // list alone.
                const std::string wireForm = boost::json::serialize(request);
                if (wireForm.find("FGMISSING") != std::string::npos) {
                    fForegroundFaults = {{"./shows/gone.png",
                                          "fake: no such file or directory"}};
                    ++fForegroundEvents;
                } else if (wireForm.find("FGSECOND") != std::string::npos) {
                    fForegroundFaults.push_back(
                        {"./shows/plate.jpg", "fake: not an image file"});
                    ++fForegroundEvents;
                } else if (wireForm.find("FGREPAIR") != std::string::npos) {
                    fForegroundFaults.clear();
                }
            }
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
            fSceneSlides[revision] = slideId;  // the preview scenario's markers
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
        // --- the scripted preview stream (task 6.16a, review R16) ---------
        //
        // preview.start makes this peer a real producer of the transport: a
        // segment with three seqlocked slots, a ready per frame, and slot
        // recycling that waits for a release proving ownership (task 6.5's
        // rule, enforced here so a consumer that stops releasing is visible as
        // dropped frames rather than silently fine). preview.select then does
        // what the session does — refuse without a stream, refuse a revision
        // the store does not have, refuse a generation that does not advance,
        // stand down on revision 0 — and answers an accepted ask with a frame.
        // Two markers, carried in the prepared scene's slideId, arm the cases
        // that cannot be produced honestly any other way: SLOW delivers that
        // frame 500 ms late (so a newer ask really does overtake it), and
        // FORGET makes the peer report the target as gone once, so the
        // client's recovery is a behaviour under test instead of a comment.
        if (method == "preview.start") {
            if (fScenario != "preview") {
                sendError(id, "fake: this scenario has no preview stream");
                return;
            }
            std::string error;
            if (!startPreviewSegment(&error)) {
                sendError(id, error);
                return;
            }
            sendResult(id, boost::json::object{{"name", fPreviewName},
                                               {"width", static_cast<std::int64_t>(kFrameWidth)},
                                               {"height", static_cast<std::int64_t>(kFrameHeight)},
                                               {"slotCount", static_cast<std::int64_t>(kSlots)},
                                               {"bytesPerSlot",
                                                static_cast<std::int64_t>(fSlotStride)},
                                               {"fps", 15}});
            // The audience's own picture, immediately: the panel is live
            // before anybody asks for an edit, and stays that way.
            publishPreview(proto::kPreviewTargetAudience, 0, 7, false);
            return;
        }
        if (method == proto::kMethodPreviewSelect) {
            fSelects.push_back(boost::json::object{
                {"method", method},
                {"params", request.contains("params") ? request.at("params")
                                                      : boost::json::object{}}});
            writeReport();  // live: the fixture reads generations mid-scenario
            if (!fStreamStarted) {
                sendError(id, "fake: no preview stream is running: preview.start first");
                return;
            }
            boost::json::object args;
            if (request.contains("params") && request.at("params").is_object()) {
                args = request.at("params").as_object();
            }
            if (!args.contains("revision") || !args.contains("request")) {
                sendError(id, "fake: preview.select wants {revision, request}");
                return;
            }
            const std::int64_t revision = args.at("revision").to_number<std::int64_t>();
            const std::int64_t asked = args.at("request").to_number<std::int64_t>();
            if (revision != 0 && fSceneSlides.find(revision) == fSceneSlides.end()) {
                sendError(id, "fake: preview.select scene " + std::to_string(revision) +
                                  " is not in the prepared store");
                return;
            }
            if (static_cast<std::int32_t>(asked - static_cast<std::int64_t>(fSelectionAccepted)) <=
                0) {
                sendError(id, "fake: preview.select request " + std::to_string(asked) +
                                  " does not follow the accepted " +
                                  std::to_string(fSelectionAccepted));
                return;
            }
            // A draw the new ask overtook: the peer counts it exactly where
            // the real session counts one — an accepted ask still waiting for
            // its frame when the next one arrives.
            if (!fPendingFrames.empty()) {
                ++fSelectionStale;
            }
            fSelectionAccepted = static_cast<std::uint64_t>(asked);
            if (revision == 0) {
                fSelectionActive = false;
                fSelectionRevision = 0;
                sendResult(id, boost::json::object{{"accepted", true},
                                                   {"revision", 0},
                                                   {"request", asked}});
                // The audience keeps ticking after a stand-down, and THIS
                // frame arrives the way a version-1 worker sends one: no
                // target, no request. An absent target means the audience;
                // read it as an edit and the canvas lights up with the show's
                // own picture after the operator has stopped asking for one.
                publishPreview(proto::kPreviewTargetAudience, 0, 7, true);
                return;
            }
            fSelectionRevision = static_cast<std::uint64_t>(revision);
            fSelectionActive = true;
            sendResult(id, boost::json::object{{"accepted", true},
                                               {"revision", revision},
                                               {"request", asked}});
            const std::string marker = fSceneSlides[revision];
            // The audience owes a frame too, and a real worker sends it
            // BESIDE the edit rather than politely waiting for the next
            // quiet pass: two frames in a row on one segment, each under
            // perfectly good handles, one for each pane. That is exactly the
            // arrival R16's routing has to get right, so the scripted
            // producer creates it on every accepted ask.
            publishPreview(proto::kPreviewTargetAudience, 0, 7, false);
            if (marker == "SLOW") {
                fPendingFrames.push_back({std::chrono::steady_clock::now() +
                                              std::chrono::milliseconds(500),
                                          proto::kPreviewTargetSelection,
                                          static_cast<std::uint32_t>(asked),
                                          static_cast<std::uint32_t>(revision),
                                          false});
            } else {
                publishPreview(proto::kPreviewTargetSelection, static_cast<std::uint32_t>(asked),
                               static_cast<std::uint32_t>(revision), false);
            }
            if (marker == "FORGET" && !fForgetUsed) {
                fForgetUsed = true;
                fForgetReportsLeft = 1;  // the next status says the target is gone
            }
            return;
        }
        if (method == proto::kMethodPreviewThumbnails) {
            // Task 6.16b (review R16): the slide list's band. The peer is a
            // producer of these rows in the same sense it is a producer of
            // edit frames — real slots, real sequence numbers, one row per
            // wake — and its refusals are the real session's, judged by the
            // same shared helper, because the thing under test is the
            // CLIENT's behaviour at the edges: what it asks for, what it
            // believes, and what it refuses to paint.
            fThumbnailOps.push_back(thumbnailAccount(
                request.contains("params") && request.at("params").is_object()
                    ? request.at("params").as_object()
                    : boost::json::object{}));
            writeReport();  // live: the fixture reads generations mid-scenario
            if (!fStreamStarted) {
                sendError(id, "fake: no preview stream is running: preview.start first");
                return;
            }
            boost::json::object args;
            if (request.contains("params") && request.at("params").is_object()) {
                args = request.at("params").as_object();
            }
            const auto numberArg = [&args](const char* key) -> std::int64_t {
                const auto entry = args.find(key);
                return entry == args.end() || !entry->value().is_number()
                           ? -1
                           : entry->value().to_number<std::int64_t>();
            };
            const std::int64_t width = numberArg("width");
            const std::int64_t height = numberArg("height");
            const std::int64_t asked = numberArg("request");
            const auto scenesEntry = args.find("scenes");
            if (width < 0 || height < 0 || asked < 0 || scenesEntry == args.end() ||
                !scenesEntry->value().is_array()) {
                sendError(id,
                          "fake: preview.thumbnails wants {scenes, width, height, request}");
                return;
            }
            const boost::json::array& scenes = scenesEntry->value().as_array();
            // The cap, then the extent, then the generation: three comparisons
            // before anything is built, because an operator's scroll wheel
            // fires this command and a bad ask must cost a comparison. None of
            // them advances the accepted generation, so every refusal below can
            // be followed by a legal ask using the same number — which is how
            // the fixture knows nothing was reserved.
            if (scenes.size() > proto::kMaxThumbnailBatch) {
                sendError(id, "fake: preview.thumbnails takes at most " +
                                  std::to_string(proto::kMaxThumbnailBatch) + " scenes, got " +
                                  std::to_string(scenes.size()));
                return;
            }
            if (!proto::thumbnailExtentFits(static_cast<std::uint32_t>(width),
                                            static_cast<std::uint32_t>(height),
                                            static_cast<std::uint32_t>(fSlotStride))) {
                sendError(id, "fake: preview.thumbnails extent " + std::to_string(width) + "x" +
                                  std::to_string(height) + " is outside 32..512 pixels, or does "
                                  "not fit a " + std::to_string(fSlotStride) +
                                  "-byte preview slot");
                return;
            }
            if (static_cast<std::int32_t>(asked - static_cast<std::int64_t>(fThumbnailAccepted)) <=
                0) {
                sendError(id, "fake: preview.thumbnails request " + std::to_string(asked) +
                                  " does not follow the accepted " +
                                  std::to_string(fThumbnailAccepted));
                return;
            }
            // A row the older band still owed when this one arrived. The real
            // session counts one per superseded ask, in the same place.
            if (!fThumbnailRows.empty() || thumbnailRowsInFlight() != 0) {
                ++fThumbnailStale;
            }
            // Per-entry: a slide whose identity or content is unusable costs
            // itself alone. Keys are echoed CLIPPED (R11's rule about putting a
            // client's own text back on the wire).
            boost::json::array frames;
            boost::json::array refused;
            std::vector<ThumbnailRow> rows;
            for (const boost::json::value& item : scenes) {
                std::string key;
                std::string why;
                if (!item.is_object()) {
                    why = "the entry is not an object";
                } else {
                    const boost::json::object& entry = item.as_object();
                    const auto keyEntry = entry.find("key");
                    const auto sceneEntry = entry.find("scene");
                    if (keyEntry == entry.end() || !keyEntry->value().is_string()) {
                        why = "no key to identify the row by";
                    } else {
                        key = std::string(keyEntry->value().as_string());
                    }
                    if (why.empty() && key.empty()) {
                        why = "an empty key is not an identity";
                    }
                    if (why.empty() && key.size() > proto::kMaxThumbnailKeyLength) {
                        why = "key is " + std::to_string(key.size()) +
                              " characters, over the " +
                              std::to_string(proto::kMaxThumbnailKeyLength) + " limit";
                    }
                    if (why.empty() &&
                        (sceneEntry == entry.end() || !sceneEntry->value().is_object())) {
                        why = "no scene to draw";
                    }
                }
                if (!why.empty()) {
                    refused.push_back(
                        boost::json::object{{"key", key.substr(0, 32)}, {"why", why}});
                    continue;
                }
                if (++fThumbnailNextId == 0) {
                    fThumbnailNextId = 1;  // zero means "no id", everywhere
                }
                frames.push_back(boost::json::object{
                    {"key", key}, {"id", static_cast<std::int64_t>(fThumbnailNextId)}});
                // Every id this producer ever handed out, in order: the fixture
                // reads a row's colour back as (id, 200, 60), which is how
                // "the picture under THIS key is the one the ack gave THIS
                // slide" is checked rather than assumed.
                fThumbnailIssued.push_back(boost::json::object{
                    {"key", key}, {"id", static_cast<std::int64_t>(fThumbnailNextId)}});
                ThumbnailRow row;
                row.id = fThumbnailNextId;
                row.width = static_cast<std::uint32_t>(width);
                row.height = static_cast<std::uint32_t>(height);
                // Markers, carried in the row's own key the way SLOW and
                // FORGET ride a prepared scene's slideId: each arms a client
                // gate that cannot be produced honestly any other way.
                if (key.find("LIE") != std::string::npos) {
                    row.lieAboutGeometry = true;
                }
                if (key.find("FORGET") != std::string::npos && !fThumbnailForgetArmed) {
                    // ONCE, and the re-issue it provokes does not re-arm it: the
                    // recovered band carries the same key, so arming per ask
                    // would have the peer lose the list on every poll and the
                    // client ask again forever.
                    fThumbnailForgetArmed = true;
                    fThumbnailForgetLeft = 1;  // the next status says the band is gone
                }
                const bool late = key.find("LATE") != std::string::npos;
                const bool slipped = key.find("OLDSTAMP") != std::string::npos;
                if (late || slipped) {
                    // A draw that has already begun is not cancelled by a
                    // newer band, exactly as in the session: the row lands
                    // after its replacement has been acked. LATE stamps the
                    // generation accepted THEN (the session reads its current
                    // request at publish time); OLDSTAMP stamps one the client
                    // has certainly moved past.
                    fPendingFrames.push_back({std::chrono::steady_clock::now() +
                                                  std::chrono::milliseconds(late ? 500 : 120),
                                              proto::kPreviewTargetThumbnail,
                                              slipped ? static_cast<std::uint32_t>(
                                                  fThumbnailAccepted - 1)
                                                      : static_cast<std::uint32_t>(asked),
                                              fThumbnailNextId,
                                              false,
                                              row.width,
                                              row.height,
                                              row.lieAboutGeometry,
                                              slipped});
                } else {
                    rows.push_back(row);
                }
            }
            // REPLACE, which is the cancellation rule: whatever the older list
            // owed stops being owed. An empty band is a stand-down, and a
            // stand-down is a change, so it still consumed a generation.
            fThumbnailRows = std::move(rows);
            fThumbnailAccepted = static_cast<std::uint64_t>(asked);
            fThumbnailWidth = static_cast<std::uint32_t>(width);
            fThumbnailHeight = static_cast<std::uint32_t>(height);
            // `active` is a REGISTRATION, not a debt: true from an accepted
            // non-empty band until a stand-down, a restart, or a stream too
            // small to hold it. The real session's field means exactly that,
            // and the client's re-issue rule keys off it, so a peer that
            // reported "active" as "still has rows to draw" makes a completed
            // list look lost and the client asks again every single poll — the
            // first run of this scenario re-asked one band four times, which is
            // the spin this comment is here to keep nobody from writing again.
            // `queued` is the debt, and it is reported separately.
            fThumbnailActive = !frames.empty();
            sendResult(id, boost::json::object{{"request", asked},
                                               {"width", width},
                                               {"height", height},
                                               {"frames", std::move(frames)},
                                               {"refused", std::move(refused)}});
            writeReport();  // ... and the ids this ack handed out are live
            if (fThumbnailActive) {
                // The audience owes a frame beside the list's, as a live worker
                // sends it whenever both targets are owed: two frames on one
                // segment, one for each pane, is the arrival a consumer has to
                // route.
                publishPreview(proto::kPreviewTargetAudience, 0, 7, false);
            }
            return;
        }
        if (method == proto::kMethodPreviewReleased) {
            ++fReleases;
            if (request.contains("params") && request.at("params").is_object()) {
                const auto& params = request.at("params").as_object();
                const auto slot = params.find("slot");
                const auto seq = params.find("seq");
                const auto stream = params.find("stream");
                if (slot != params.end() && seq != params.end() && stream != params.end() &&
                    stream->value().is_string() &&
                    std::string_view(stream->value().as_string()) == fPreviewName) {
                    const std::int64_t index = slot->value().to_number<std::int64_t>();
                    const std::uint64_t sequence = seq->value().to_number<std::uint64_t>();
                    // The exact sequence, or nothing is freed: a release that
                    // names a stale sequence leaves the slot owned, which is
                    // how a consumer that mis-counts shows up as drops.
                    if (index >= 0 && index < kSlots && fSlotSeq[index] == sequence) {
                        fSlotOwned[index] = false;
                    }
                }
            }
            writeReport();
            return;
        }
        if (method == "shutdown") {
            sendResult(id, boost::json::object{{"ok", true}});
            writeReport();
            cleanupPreviewSegment();
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
        boost::json::array foregroundPaths;
        for (const std::string& itemPath : fItemPaths) {
            foregroundPaths.emplace_back(itemPath);
        }
        // Task 6.21e: per-prepare gradient spelling ("" = the flat answer).
        boost::json::array gradients;
        for (const std::string& gradient : fGradients) {
            gradients.emplace_back(gradient);
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
            {"foregroundPaths", std::move(foregroundPaths)},
            {"gradients", std::move(gradients)},
            {"methods", std::move(methods)},
            {"requests", total},
            {"bytesReceived", fBytesReceived},
            {"stoppedReading", fStoppedReading},
            {"resumed", fResumed},
            {"mediaOps", fMediaOps},  // task 6.15a: every transport command, verbatim
            // Task 6.16a: every preview.select verbatim (the generations the
            // client actually put on the wire), plus what the scripted
            // producer did with them and how many slot releases came back.
            {"selects", fSelects},
            // Task 6.16b: every preview.thumbnails verbatim, plus what the
            // scripted producer did with the band. The published count is what
            // lets a fixture say "the frame reached the socket and the CLIENT
            // threw it away" instead of assuming nobody sent one.
            {"thumbnailOps", fThumbnailOps},
            {"thumbnailIssued", fThumbnailIssued},
            {"thumbnailPublished", fThumbnailPublished},
            {"thumbnailStale", fThumbnailStale},
            {"thumbnailAccepted", fThumbnailAccepted},
            {"thumbnailQueued", fThumbnailRows.size() + thumbnailRowsInFlight()},
            {"mirrorPublished", fMirrorPublished},
            {"mirrorDropped", fMirrorDropped},
            {"releases", fReleases},
            {"previewDropped", fPreviewDropped},
            {"selectionPublished", fSelectionPublished},
            {"selectionStale", fSelectionStale},
            {"selectionAccepted", fSelectionAccepted},
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

    // --- the scripted preview producer (task 6.16a, review R16) -----------
    //
    // A frame's colour IS its answer: an audience frame is (1, 2, 3) and an
    // edit frame is (revision, 77, 155), so "these are the pixels of the
    // scene that ask named" and "no edit frame ever reached the audience's
    // pane" are pixel tests rather than hopes. 8x8 keeps the segment at a
    // kilobyte; the seqlock, the ownership rule and the routing do not care.
    // 256x144 rather than a token 8x8 for one reason: a thumbnail's pixels
    // share these slots, and the box the protocol allows starts at 32x32, so a
    // slot smaller than that could not carry a LEGAL row and the consumer's
    // bounds rule could not be exercised at all. At this extent the mirror is
    // 147 KB, a small row is 3.8 KB, and a whole band of mirror-sized rows is
    // more than the client's picture budget — which is what the LRU leg below
    // measures.
    static constexpr std::uint32_t kFrameWidth = 256;
    static constexpr std::uint32_t kFrameHeight = 144;
    static constexpr int kSlots = 3;
    static_assert(static_cast<int>(proto::kPreviewSlotCount) == kSlots,
                  "the scripted stream has the slot count the wire promises");

    bool startPreviewSegment(std::string* error) {
        fPreviewName = "/openshow-fakepv-" + std::to_string(static_cast<long>(::getpid()));
        ::shm_unlink(fPreviewName.c_str());  // a previous run's leftover
        fSlotStride = proto::previewSlotStrideBytes(kFrameWidth, kFrameHeight);
        const std::size_t total = 64 + static_cast<std::size_t>(kSlots) * fSlotStride;
        const int fd = ::shm_open(fPreviewName.c_str(), O_CREAT | O_RDWR, 0600);
        if (fd < 0) {
            *error = "fake: shm_open failed";
            return false;
        }
        if (::ftruncate(fd, static_cast<off_t>(total)) != 0) {
            ::close(fd);
            *error = "fake: ftruncate failed";
            return false;
        }
        void* map = ::mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (map == MAP_FAILED) {
            ::close(fd);
            *error = "fake: mmap failed";
            return false;
        }
        auto* segment = static_cast<proto::PreviewSegmentHeader*>(map);
        segment->magic = proto::kPreviewMagic;
        segment->layoutVersion = proto::kPreviewLayoutVersion;
        segment->slotCount = proto::kPreviewSlotCount;
        segment->bytesPerSlot = static_cast<std::uint32_t>(fSlotStride);
        segment->width = kFrameWidth;
        segment->height = kFrameHeight;
        segment->format = proto::kPreviewFourccRGBA8;
        fSlotMap = static_cast<std::uint8_t*>(map);
        fSlotFd = fd;
        fStreamStarted = true;
        return true;
    }

    void cleanupPreviewSegment() {
        if (fSlotMap != nullptr) {
            ::munmap(fSlotMap, 64 + static_cast<std::size_t>(kSlots) * fSlotStride);
            fSlotMap = nullptr;
        }
        if (fSlotFd >= 0) {
            ::close(fSlotFd);
            fSlotFd = -1;
        }
        if (!fPreviewName.empty()) {
            ::shm_unlink(fPreviewName.c_str());
        }
    }

    void publishPreview(std::uint32_t target, std::uint32_t request, std::uint32_t revision,
                        bool legacyFields) {
        publishPreviewFrame(target, revision, kFrameWidth, kFrameHeight, legacyFields, request,
                            false);
    }

    // Writes one frame of `width` x `height` into a free slot and announces
    // it, dropping it rather than waiting — the rule task 6.5 established for
    // a consumer that stops releasing. Returns whether it reached the wire.
    //
    // The two arguments that exist for task 6.16b are the two a real producer
    // can get wrong in a way a single-extent stream never could:
    //
    //   - `stampRequest`: a row whose DRAW started before a newer band was
    //     accepted carries that newer generation, because the session reads
    //     its current request at publish time; a producer whose bookkeeping
    //     slipped carries the generation it was ASKED under instead. Both
    //     states occur, and the client's two refusal gates — "not this
    //     generation" and "not an id I was given" — are each the only thing
    //     that catches one of them.
    //   - `lieAboutGeometry`: the slot holds a perfectly good frame of one
    //     size while the notification announces another. Nothing else in the
    //     notification is inconsistent (its rows are packed and its payload
    //     fits), so only the cross-check against the header the same sequence
    //     stamps can stop a consumer from copying the wrong number of bytes.
    bool publishPreviewFrame(std::uint32_t target, std::uint32_t revision, std::uint32_t width,
                             std::uint32_t height, bool legacyFields, std::uint32_t stampRequest,
                             bool lieAboutGeometry) {
        int slot = -1;
        for (int i = 0; i < kSlots; ++i) {
            if (!fSlotOwned[i]) {
                slot = i;
                break;
            }
        }
        const std::uint32_t headWidth = lieAboutGeometry ? width + 8u : width;
        if (fSlotMap == nullptr || slot < 0 ||
            proto::previewSlotStrideBytes(headWidth, height) > fSlotStride) {
            // The producer drops rather than waits. Counted by WHOSE frame it
            // was: a mirror frame dropped for want of a slot is what a live
            // worker does at its own cadence, while a ROW that never made it
            // means the consumer stopped releasing.
            if (target == proto::kPreviewTargetThumbnail) {
                ++fPreviewDropped;
            } else {
                ++fMirrorDropped;
            }
            return false;
        }
        std::uint8_t* base =
            fSlotMap + proto::previewSlotOffset(static_cast<std::uint32_t>(slot), fSlotStride);
        auto* header = reinterpret_cast<proto::PreviewSlotHeader*>(base);
        const std::uint64_t seq = fSlotSeq[slot] + 2;
        std::atomic_ref<std::uint64_t>(header->seq).store(seq - 1);  // odd: mid-write
        header->width = headWidth;
        header->height = height;
        header->rowBytes = headWidth * 4;
        header->format = proto::kPreviewFourccRGBA8;
        header->revision = revision;
        header->target = target;
        header->request = stampRequest;
        header->publishedNs = 1000 + seq;
        // A frame's colour IS its identity: the audience's is (1, 2, 3), an
        // edit's is (revision, 77, 155) and a row's is (id, 200, 60), so
        // "these are the pixels of the slide that ack named, in the pane that
        // belongs to them" is a pixel test rather than a hope.
        const auto paint = [&](std::uint8_t* out) {
            if (target == proto::kPreviewTargetThumbnail) {
                out[0] = static_cast<std::uint8_t>(revision & 0xFFu);
                out[1] = 200;
                out[2] = 60;
            } else if (target == proto::kPreviewTargetSelection) {
                out[0] = static_cast<std::uint8_t>(revision & 0xFFu);
                out[1] = 77;
                out[2] = 155;
            } else {
                out[0] = 1;
                out[1] = 2;
                out[2] = 3;
            }
            out[3] = 255;
        };
        for (std::uint32_t pixel = 0; pixel < headWidth * height; ++pixel) {
            paint(base + 64 + pixel * 4);
        }
        std::atomic_ref<std::uint64_t>(header->seq).store(seq);  // even: published
        fSlotSeq[slot] = seq;
        fSlotOwned[slot] = true;
        if (target == proto::kPreviewTargetSelection) {
            ++fSelectionPublished;
        }
        if (target == proto::kPreviewTargetThumbnail) {
            ++fThumbnailPublished;
        }
        if (target == proto::kPreviewTargetAudience) {
            ++fMirrorPublished;
        }
        boost::json::object params{
            {"slot", static_cast<std::int64_t>(slot)},
            {"seq", static_cast<std::int64_t>(seq)},
            {"width", static_cast<std::int64_t>(width)},
            {"height", static_cast<std::int64_t>(height)},
            {"rowBytes", static_cast<std::int64_t>(width * 4)},
            {"revision", static_cast<std::int64_t>(revision)},
            {"publishedNs", static_cast<std::int64_t>(1000 + seq)},
            {"stream", fPreviewName}};
        // A version-1 worker sends neither field, and the client must read
        // that as the audience rather than as an edit.
        if (!legacyFields) {
            params["target"] = static_cast<std::int64_t>(target);
            params["request"] = static_cast<std::int64_t>(stampRequest);
        }
        std::string wire;
        proto::appendFrame(
            proto::makeNotification(proto::kMethodPreviewReady, std::move(params)), &wire);
        send(wire);
        return true;
    }

    // Draws what the accepted band still owes, one row per wake and no more
    // than a free slot allows — the same shape as the session's render pass,
    // so a client that stops releasing shows up as `previewDropped` in this
    // peer's ledger instead of as a hang.
    // One accepted (or refused) ask, recorded without its scenes: what a
    // fixture needs is the generation, the extent, and the keys.
    static boost::json::object thumbnailAccount(const boost::json::object& args) {
        boost::json::array keys;
        std::int64_t asked = 0;
        const auto scenes = args.find("scenes");
        if (scenes != args.end() && scenes->value().is_array()) {
            for (const boost::json::value& item : scenes->value().as_array()) {
                if (!item.is_object()) {
                    continue;
                }
                const auto key = item.as_object().find("key");
                const std::string clipped =
                    key != item.as_object().end() && key->value().is_string()
                        ? std::string(key->value().as_string()).substr(0, 32)
                        : std::string();
                keys.push_back(boost::json::value_from(clipped));
            }
            asked = static_cast<std::int64_t>(scenes->value().as_array().size());
        }
        const auto number = [&args](const char* name) -> std::int64_t {
            const auto entry = args.find(name);
            return entry == args.end() || !entry->value().is_number()
                       ? -1
                       : entry->value().to_number<std::int64_t>();
        };
        return boost::json::object{{"request", number("request")},
                                   {"width", number("width")},
                                   {"height", number("height")},
                                   {"asked", asked},
                                   {"keys", std::move(keys)}};
    }

    std::uint64_t thumbnailRowsInFlight() const {
        std::uint64_t held = 0;
        for (const PendingFrame& frame : fPendingFrames) {
            if (frame.target == proto::kPreviewTargetThumbnail) {
                ++held;
            }
        }
        return held;
    }

    // A slot nobody holds, or -1. Claiming one is what a producer does before
    // it draws; "there was no slot" is a reason to wait, not a dropped frame,
    // and counting it as one would make this peer's `dropped` mean "the client
    // is keeping up at its own speed", which is nothing at all.
    int freeSlot() const {
        for (int i = 0; i < kSlots; ++i) {
            if (!fSlotOwned[i]) {
                return i;
            }
        }
        return -1;
    }

    void publishOwedThumbnails() {
        while (!fThumbnailRows.empty()) {
            if (freeSlot() < 0) {
                return;  // every slot is owned; the row stays OWED, not lost
            }
            const ThumbnailRow row = fThumbnailRows.front();
            const bool published = publishPreviewFrame(
                proto::kPreviewTargetThumbnail, row.id, row.width, row.height, false,
                fThumbnailAccepted, row.lieAboutGeometry);
            // A row that failed with a slot in hand can never succeed (its
            // extent was bounds-checked against this very stride when the ask
            // was accepted), so it is given up rather than retried forever.
            fThumbnailRows.erase(fThumbnailRows.begin());
            if (!published) {
                return;
            }
        }
    }

    bool fStreamStarted = false;
    std::string fPreviewName;
    std::size_t fSlotStride = 0;
    std::uint8_t* fSlotMap = nullptr;
    int fSlotFd = -1;
    std::uint64_t fSlotSeq[kSlots] = {};
    bool fSlotOwned[kSlots] = {};
    std::uint64_t fPreviewDropped = 0;  // ROWS lost to a full segment: must stay 0
    std::uint64_t fMirrorDropped = 0;    // mirror frames the peer dropped at cadence
    std::uint64_t fMirrorPublished = 0;  // ... and the frames it did publish, by target
    std::uint64_t fSelectionPublished = 0;
    std::uint64_t fSelectionStale = 0;
    std::uint64_t fReleases = 0;
    bool fSelectionActive = false;
    std::uint64_t fSelectionRevision = 0;
    std::uint64_t fSelectionAccepted = 0;
    bool fForgetUsed = false;
    int fForgetReportsLeft = 0;
    boost::json::array fSelects;
    // Task 6.16b: the slide list's target, its ledger, and the counters the
    // client mirrors. `fThumbnailNextId` is monotonic for the whole session —
    // uniqueness only has to hold within one batch, and a producer that reused
    // ids across bands could not tell a row from a replaced band apart.
    boost::json::array fThumbnailOps;
    boost::json::array fThumbnailIssued;
    std::uint64_t fThumbnailAccepted = 0;
    std::uint64_t fThumbnailPublished = 0;
    std::uint64_t fThumbnailStale = 0;
    std::uint32_t fThumbnailNextId = 0;
    std::uint32_t fThumbnailWidth = 0;
    std::uint32_t fThumbnailHeight = 0;
    bool fThumbnailActive = false;
    bool fThumbnailForgetArmed = false;
    int fThumbnailForgetLeft = 0;
    std::map<std::int64_t, std::string> fSceneSlides;
    struct PendingFrame {
        std::chrono::steady_clock::time_point when;
        std::uint32_t target;
        std::uint32_t request;
        std::uint32_t revision;
        bool legacy;
        // Task 6.16b: a held row's own size (0 = the stream's), whether its
        // slot header should contradict its notification, and whether the
        // generation it stamps is the one it was asked under rather than the
        // one currently accepted.
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        bool lieAboutGeometry = false;
        // true: publish with the `request` above verbatim, however old the
        // peer's state has become since. That is the point of the case.
        bool stampOwnRequest = false;
    };
    std::vector<PendingFrame> fPendingFrames;

    // One owed row of the accepted band (task 6.16b). The real session keeps
    // a queue of these and draws ONE per render pass, so the scripted peer
    // does the same: a band arrives over several wakes, and a replacement
    // throws the undrawn part of it away. Publishing thirty rows into three
    // slots in one function call would only prove the peer is not a producer.
    struct ThumbnailRow {
        std::uint32_t id = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        bool lieAboutGeometry = false;
    };
    std::vector<ThumbnailRow> fThumbnailRows;

    std::vector<std::string> fHeld;  // hold3: acks awaiting the burst
    std::vector<DelayedReply> fQueue;  // slow-first: acks awaiting their due
    std::vector<int> fPresented;       // revisions the controller went to
    std::vector<std::string> fAssetPaths;  // each prepare's background file (task 6.12)
    std::vector<std::string> fItemPaths;   // and its path-bearing items, space-joined (task 6.21b)
    std::vector<std::string> fGradients;   // and its background gradient, or "" (task 6.21e)
    // Task 6.21f (review F6): the per-item foreground faults the status
    // scene block reports. Marked payloads arm them (FGMISSING/FGSECOND)
    // and a later marked prepare repairs the list (FGREPAIR) — the same
    // edge the real worker gives its own decode: a prepare that reads the
    // path again takes the report away, and never un-counts the event.
    std::vector<std::pair<std::string, std::string>> fForegroundFaults;
    std::uint64_t fForegroundEvents = 0;
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

// How many entries a ledger array of the peer's holds. Absent or malformed is
// -1, so a fixture that expects none asks for == 0 and cannot read garbage as
// agreement.
std::int64_t jsonSize(const boost::json::object& report, const char* key) {
    const auto entry = report.find(key);
    if (entry == report.end() || !entry->value().is_array()) {
        return -1;
    }
    return static_cast<std::int64_t>(entry->value().as_array().size());
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
        scene::SceneItem item;
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

// Task 6.21b (review F2): a text line PLUS a foreground picture item — the
// payload shape the background-only adoption fixtures never sent. The peer's
// per-prepare item ledger asserts on these paths.
std::string sceneForeground(const char* slideId, const std::string& assetPath) {
    scene::Scene scene = authoredScene({"FG SCENE"}, 0xFFFFFFu);
    scene.slideId = slideId;
    scene::SceneItem picture;
    picture.isImage = true;
    picture.assetId = std::string("asset-") + slideId;
    picture.assetPath = assetPath;
    scene.items.push_back(std::move(picture));
    return boost::json::serialize(scene::toJson(scene));
}

// Task 6.21e (review F4): a background with a SECOND colour. With a black
// top the payload reads as "no background" to anything that looks only at
// `backgroundColor` — the exact shape that left the clear button disabled
// while the canvas glowed — so the fixtures drive it explicitly.
std::string sceneGradient(const char* slideId, unsigned top, unsigned bottom,
                          const std::vector<std::string>& lines) {
    scene::Scene scene = authoredScene(lines, top);
    scene.slideId = slideId;
    scene.background.gradient = bottom;
    scene.background.hasGradient = true;
    return boost::json::serialize(scene::toJson(scene));
}

// The payload the edit canvas hands the controller: a compiled scene whose
// slideId the scripted peer reads as its marker (SLOW delivers that frame late,
// FORGET makes the peer report the target gone once).
std::string sceneForSelection(const char* slideId, const char* line, unsigned colour) {
    scene::Scene scene = authoredScene({std::string(line)}, colour);
    scene.slideId = slideId;
    return boost::json::serialize(scene::toJson(scene));
}

// --- slide-list rows (task 6.16b) ---------------------------------------
//
// What the UI hands `requestThumbnails` is one entry per visible slide: the
// slide's own identity, and the compiled scene the worker should picture it
// with. A row's key is the ONLY thing that ties a delivered frame back to a
// row, so these fixtures give rows distinct keys and identical-looking scenes
// wherever the point is that the key, and not the picture, is what routed it.

QVariantMap listRow(const QString& key, const QString& sceneJson) {
    return QVariantMap{{QStringLiteral("key"), key}, {QStringLiteral("scene"), sceneJson}};
}

QVariantList bandOf(const std::vector<std::string>& keys) {
    QVariantList band;
    for (const std::string& key : keys) {
        band.push_back(listRow(QString::fromStdString(key),
                               QString::fromStdString(sceneForSelection("R", "line", 0x101030u))));
    }
    return band;
}

// A report read ONCE, no retry: the preview fixture polls the ledger between
// pumping its own event loop, and a helper that sleeps for a second waiting
// for a file that does not exist yet stops the client from answering.
boost::json::object readReportNow(const std::string& path) {
    if (std::FILE* file = std::fopen(path.c_str(), "rb")) {
        // Big enough for a slide-list ledger: a dozen asks of thirty keys
        // each, which the 6.16b scenario writes before it reads.
        char buffer[262144];
        const size_t count = std::fread(buffer, 1, sizeof(buffer) - 1, file);
        std::fclose(file);
        buffer[count] = '\0';
        boost::system::error_code error;
        boost::json::value parsed = boost::json::parse(std::string(buffer), error);
        if (!error && parsed.is_object()) {
            return parsed.as_object();
        }
    }
    return {};
}

// The peer's preview.select ledger as (revision, request) pairs in arrival
// order. Every promise the generation counter makes is read out of this list:
// one entry per accepted ask, the numbers strictly advancing, the stand-down
// revision 0, and nothing sent twice for an unchanged slide.
std::vector<std::pair<std::int64_t, std::int64_t>> selectLedger(
    const boost::json::object& report) {
    std::vector<std::pair<std::int64_t, std::int64_t>> out;
    const auto entry = report.find("selects");
    if (entry == report.end() || !entry->value().is_array()) {
        return out;
    }
    for (const auto& value : entry->value().as_array()) {
        if (!value.is_object() || !value.at("params").is_object()) {
            continue;
        }
        const auto& params = value.at("params").as_object();
        const auto revision = params.find("revision");
        const auto request = params.find("request");
        if (revision == params.end() || request == params.end()) {
            continue;
        }
        out.emplace_back(revision->value().to_number<std::int64_t>(),
                         request->value().to_number<std::int64_t>());
    }
    return out;
}

// The scripted producer paints (1, 2, 3) for the audience and
// (revision, 77, 155) for an edit, so an image says both which target it was
// drawn for and which ask answered it. Read as bytes: the controller copies
// RGBA8888 straight out of the slot, and that is the layout under test.
bool pixelsAre(const QImage& image, int red, int green, int blue) {
    if (image.isNull() || image.sizeInBytes() < 3 ||
        image.format() != QImage::Format_RGBA8888) {
        return false;
    }
    const auto* bytes = static_cast<const uchar*>(image.constBits());
    return bytes[0] == red && bytes[1] == green && bytes[2] == blue;
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

// Task 6.21b (review F2): adoption must rewrite the ITEMS too, not just the
// background. The S12 journey again — but with a foreground picture, the
// shape no background-only fixture ever sent: present text plus image,
// adopt the old path, clear ALL (the retained snapshot is the
// foreground-bearing payload), restore. Round two adopts WHILE the picture's
// own prepare is still in flight, so the rewrite proven is the one against
// the ask the transport holds and everything derived from it — mirror,
// desired content, clear snapshot, restore — not merely against a settled
// payload. The peer's per-prepare item ledger is the evidence: no prepare
// after an adoption names the deleted path, and both restores come back
// wearing the service path.
void scenarioAdoptedForeground() {
    ::setenv("OPENSHOW_FAKE_SCENARIO", "clean", 1);
    const std::string report = tempPath("adoptfg");
    ::unlink(report.c_str());
    ::setenv("OPENSHOW_FAKE_REPORT", report.c_str(), 1);

    WorkerController controller;
    controller.start();
    if (!check(waitUntil([&controller] {
                   return controller.statusText().contains(QStringLiteral("connected to worker"));
               }),
               "adoptfg: handshake with the scripted peer")) {
        controller.shutdownNow();
        return;
    }

    // The review's own filenames: a recovered autosave file the save moved
    // into the package.
    const QString oldPath = QStringLiteral("/old/autosave/assets/picture.png");
    const QString newPath = QStringLiteral("/package/assets/picture.png");

    // Round one: the settled adoption — the go's prepare has acked, so the
    // re-prepare the adoption owes is the only new traffic.
    controller.presentSceneJson(
        QString::fromStdString(sceneForeground("FG-A", oldPath.toStdString())));
    if (!check(waitUntil([&controller] {
                   return controller.sceneActive() == 1 && controller.scenePending() == 0;
               }),
               "adoptfg: the foreground scene naming the original path is on air")) {
        controller.shutdownNow();
        return;
    }
    controller.adoptAssetPaths({{oldPath, newPath}});
    check(waitUntil([&controller] {
              return controller.scenePrepared() == 2 && controller.scenePending() == 0;
          }),
          "adoptfg: the owed scene was re-prepared with the service path");
    check(controller.sceneActive() == 1,
          "adoptfg: the re-prepare was not a go - the audience never moved");

    // Clear-all retains the FOREGROUND payload and restore re-prepares it:
    // the snapshot is exactly the thing the adoption had to rewrite.
    controller.clearAll();
    check(waitUntil([&controller] {
              return controller.sceneActive() == 3 && controller.scenePending() == 0;
          }),
          "adoptfg: the clear-all presented over the adopted payload");
    controller.restoreOutput();
    check(waitUntil([&controller] {
              return controller.sceneActive() == 4 && controller.scenePending() == 0;
          }),
          "adoptfg: the restore brought the adopted foreground back");

    // Round two: the adoption lands while the picture's own prepare is
    // still in flight — no event loop spins between the present and the
    // adoption — so what the rewrite catches is the ask on the transport
    // and every payload derived from it after the ack.
    controller.presentSceneJson(
        QString::fromStdString(sceneForeground("FG-B", oldPath.toStdString())));
    controller.adoptAssetPaths({{oldPath, newPath}});
    check(waitUntil([&controller] {
              return controller.sceneActive() == 5 && controller.scenePending() == 0;
          }),
          "adoptfg: the in-flight round settled with the scene on air");
    controller.clearAll();
    check(waitUntil([&controller] {
              return controller.sceneActive() == 7 && controller.scenePending() == 0;
          }),
          "adoptfg: the in-flight round's clear-all presented");
    controller.restoreOutput();
    check(waitUntil([&controller] {
              return controller.sceneActive() == 8 && controller.scenePending() == 0;
          }),
          "adoptfg: the in-flight round's restore names the service path too");

    controller.shutdownNow();
    const boost::json::object counts = readReport(report);

    // The item ledger, entry per prepare in order. The two original goes
    // necessarily named the operator's file — it was on the wire before the
    // adoption existed — and nothing after an adoption may name it again.
    // The adoption's re-prepares and both restores name the service path;
    // the clear-alls name no file at all.
    std::vector<std::string> itemPaths;
    if (const auto entry = counts.find("foregroundPaths");
        entry != counts.end() && entry->value().is_array()) {
        for (const auto& value : entry->value().as_array()) {
            if (value.is_string()) {
                itemPaths.push_back(std::string(value.as_string()));
            }
        }
    }
    std::string joined;
    for (const std::string& path : itemPaths) {
        joined += (joined.empty() ? "" : " | ") + path;
    }
    const std::string o = oldPath.toStdString();
    const std::string n = newPath.toStdString();
    const std::vector<std::string> want{o, n, "", n, o, n, "", n};
    bool ledgerOk = itemPaths.size() == want.size();
    for (std::size_t i = 0; ledgerOk && i < want.size(); ++i) {
        ledgerOk = itemPaths[i] == want[i];
    }
    check(ledgerOk,
          ("adoptfg: every prepare carried the item path it should - the deleted path never "
           "named again, both restores service paths (got [" +
           joined + "])")
              .c_str());
    check(reportNumber(counts, "presents") == 6,
          "adoptfg: six presents - two goes, two clear-alls, two restores; the adoptions "
          "themselves added none");
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
// source keeps its own gain. Task UI.4b reads that refusal a second way: not
// as "nothing changed", but as the verdict a media bin marks its row with —
// which the status block alone can never say.
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
    // Task UI.4b, the half the block above cannot express: the status echo
    // describes the track that kept playing and NEVER the file that was turned
    // down, so a refusal needs its own answer or the bin cannot mark the row it
    // belongs to. This is the seam where the worker's words become a property.
    check(controller.standaloneRefusalPath() == QLatin1String("/tmp/missing-bed.m4a")
              && controller.standaloneRefusal().contains(QLatin1String("cannot be opened")),
          "media: a refused probe arrives as a verdict naming the path that was asked for");

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
    // And the verdict STAYS. The polls either side of that mute describe the
    // track on air, which is a different file — none of them has anything to
    // say about the ask that was turned down, so none may clear its "cannot
    // play" (task UI.4b: the operator sees it until they act again).
    check(controller.standaloneRefusalPath() == QLatin1String("/tmp/missing-bed.m4a")
              && !controller.standaloneRefusal().isEmpty(),
          "media: later status echoes leave the refusal standing");

    controller.selectStandaloneTrack("");
    check(waitUntil([&controller] {
              return !controller.standaloneOnAir() && controller.mediaSources().size() == 1;
          }),
          "media: the removal takes the live flag and the row with it");
    check(controller.standaloneRefusal().isEmpty() && controller.standaloneRefusalPath().isEmpty(),
          "media: the removal retires the verdict the last ask left behind");
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
    // Task UI.5: there is no file-picker entry point to test any more — the
    // interim chooser was replaced by cue activation from prepared assets,
    // so every selection above is exactly what the rail and the bin can
    // send, and nothing reaches the wire that a document did not name.

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

// S14 (task 6.16a, review R16): the editor's picture on the client's side of
// the socket.
//
// The live fixture proves the worker draws an off-air scene into the shared
// stream and destroys a frame a newer ask overtakes. This scenario is the
// other half — the consumer, which has to route two kinds of frame onto two
// different panes, refuse to paint imagery of an ask it has already
// superseded, and never let an edit look like a present. It needs a producer
// that will actually deliver a stale frame, which is what the peer's SLOW
// marker is for: without it, the frame the client must drop would never exist.
void scenarioSelectionPreview() {
    ::setenv("OPENSHOW_FAKE_SCENARIO", "preview", 1);
    const std::string report = tempPath("preview");
    ::unlink(report.c_str());
    ::setenv("OPENSHOW_FAKE_REPORT", report.c_str(), 1);

    WorkerController controller;
    controller.start();
    if (!check(waitUntil([&controller] {
                   return controller.statusText().contains(QStringLiteral("connected to worker"));
               }),
               "preview: handshake with the scripted producer")) {
        controller.shutdownNow();
        return;
    }
    check(controller.selectionSupported(),
          "preview: the capability is what offers a worker-drawn edit");
    if (!check(waitUntil([&controller] { return controller.previewActive(); }),
               "preview: preview.start's answer maps the stream")) {
        controller.shutdownNow();
        return;
    }
    check(waitUntil([&controller] { return controller.previewFrames() >= 1; }),
          "preview: the audience's mirror arrives without being asked");
    check(pixelsAre(controller.previewImage(), 1, 2, 3),
          "preview: the mirror is the audience's own picture");
    check(!controller.selectionReady() && controller.selectionFrames() == 0,
          "preview: and nothing has been asked for, so the canvas has no edit yet");

    // --- an ask is a prepare and a select, and it presents nothing -------
    const QString first = QString::fromStdString(sceneForSelection("A", "verse one", 0x101030u));
    controller.requestSelectionPreview(first);
    const bool editArrives = waitUntil([&controller] { return controller.selectionReady(); });
    if (!check(editArrives, "preview: the off-air render reaches the edit target")) {
        controller.shutdownNow();
        return;
    }
    {
        const auto ledger = selectLedger(readReportNow(report));
        const bool shape =
            ledger.size() == 1 && ledger[0].first == 1 && ledger[0].second == 1;
        check(shape, "preview: prepare then preview.select, naming the revision and generation 1");
    }
    check(pixelsAre(controller.selectionImage(), 1, 77, 155),
          "preview: the pixels answer the revision the ask named");
    check(pixelsAre(controller.previewImage(), 1, 2, 3),
          "preview: the audience's pane is untouched by the edit's frame");
    check(reportNumber(readReportNow(report), "presents") == 0,
          "preview: looking at a slide is not a present - the peer saw no scene.present");

    // --- an unchanged slide is not re-asked ------------------------------
    controller.requestSelectionPreview(first);
    pumpEventsFor(500);
    {
        const boost::json::object settled = readReportNow(report);
        check(selectLedger(settled).size() == 1 && reportNumber(settled, "prepares") == 1,
              "preview: the same payload does not cost a second prepare and select");
    }

    // --- a changed slide is a new render, tagged with the new revision ----
    const QString second = QString::fromStdString(sceneForSelection("B", "chorus", 0x301010u));
    const qint64 audienceBefore = controller.previewFrames();
    controller.requestSelectionPreview(second);
    check(waitUntil([&controller] { return controller.selectionFrames() >= 2; }),
          "preview: a changed slide costs a new render");
    check(pixelsAre(controller.selectionImage(), 2, 77, 155),
          "preview: and the picture answers the NEW revision, not the first one");
    // The peer published the audience's own frame immediately behind this
    // edit's, as a live worker does whenever both targets are owed. Wait for
    // THAT copy, then ask whether either pane traded pictures: the edit image
    // must still be the edit's and the mirror still the show's. Routing by
    // the target field is the only thing that makes that true, so a client
    // that took either frame for either pane fails here and nowhere else.
    const qint64 editCopiesWithTheNewPicture = controller.selectionFrames();
    if (check(waitUntil([&controller, audienceBefore] {
                  return controller.previewFrames() > audienceBefore;
              }),
              "preview: the audience's frame that followed the ask arrives on the same stream")) {
        check(pixelsAre(controller.selectionImage(), 2, 77, 155),
              "preview: an audience frame did not become the edit's picture");
        check(pixelsAre(controller.previewImage(), 1, 2, 3),
              "preview: and the edit's frame never entered the audience's pane");
        check(controller.selectionFrames() == editCopiesWithTheNewPicture,
              "preview: the audience's copy did not count as an edit frame");
    }
    const qint64 audienceFrames = controller.previewFrames();
    pumpEventsFor(300);
    check(controller.previewFrames() == audienceFrames,
          "preview: edit frames never count as audience frames");

    // --- the frame that arrives after the operator moved on ---------------
    //
    // The peer accepts the SLOW ask and holds its frame for half a second,
    // which is the render thread still drawing yesterday's slide when the
    // operator picks something else. The newer ask lands first; the late one
    // arrives afterwards and must never be painted.
    const QString slow = QString::fromStdString(sceneForSelection("SLOW", "bridge", 0x201020u));
    controller.requestSelectionPreview(slow);
    check(waitUntil([&report] { return selectLedger(readReportNow(report)).size() == 3; }),
          "preview: the slow ask is accepted while its frame is still being drawn");
    const qint64 copiesBefore = controller.selectionFrames();
    controller.requestSelectionPreview(second);
    check(waitUntil([&controller] { return pixelsAre(controller.selectionImage(), 4, 77, 155); }),
          "preview: the newer ask's frame is the one painted");
    pumpEventsFor(900);  // the held frame is due at +500 ms and is delivered
    check(pixelsAre(controller.selectionImage(), 4, 77, 155),
          "preview: the superseded frame never gets painted over the newer one");
    check(controller.selectionFrames() == copiesBefore + 1,
          "preview: the late frame is dropped before the copy, not after it");
    {
        const boost::json::object superseded = readReportNow(report);
        check(reportNumber(superseded, "selectionPublished") == 4,
              "preview: the producer did publish all four - so the drop is the consumer's");
    }
    // The property is a MIRROR of the worker's counter, so what this leg
    // waits for is "non-zero reached the client". The exact number belongs to
    // the peer's ledger below: the ledger is the authority on how many draws
    // were superseded, and the property is only required to follow it.
    check(waitUntil([&controller] { return controller.selectionStale() >= 1; }),
          "preview: the worker's superseded-draw counter mirrors into the property");

    // --- a worker that lost the target gets asked again ------------------
    //
    // FORGET arms the peer to report the target as unregistered once. The
    // canvas showing nothing because the worker forgot is the failure this
    // recovers from; recovering twice for one forget would be a spin, so the
    // ask is counted.
    const QString forget = QString::fromStdString(sceneForSelection("FORGET", "ending", 0x002020u));
    controller.requestSelectionPreview(forget);
    // The observable is the PICTURE. The peer writes its ledger the instant it
    // accepts an ask, so waiting on the ledger would only prove the ask was
    // sent; waiting on the pixels proves the re-issue came all the way back.
    const bool recovered =
        waitUntil([&controller] { return pixelsAre(controller.selectionImage(), 6, 77, 155); },
                  6000);
    check(recovered,
          "preview: a worker that reports the target gone gets asked again, and the "
          "re-issued render is what ends up shown");
    pumpEventsFor(700);  // two status polls' worth: an unsettled canvas would spin
    {
        const boost::json::object settled = readReportNow(report);
        check(selectLedger(settled).size() == 6 && reportNumber(settled, "prepares") == 6,
              "preview: the recovery asks exactly once - no prepare storm");
    }

    // --- the stand-down, and a frame from a version-1 worker --------------
    controller.clearSelectionPreview();
    check(!controller.selectionReady(),
          "preview: standing down drops the edit picture immediately");
    check(waitUntil([&report] {
              const auto ledger = selectLedger(readReportNow(report));
              return !ledger.empty() && ledger.back().first == 0;
          }),
          "preview: the stand-down reaches the worker as revision 0");
    {
        const auto ledger = selectLedger(readReportNow(report));
        // The whole conversation, in one picture of the client's counter:
        // every accepted ask one generation newer than the last, unchanged
        // slides absent (the dedupe leg sent nothing), and the stand-down a
        // generation of its own. A client that reused a number, or advanced it
        // for a refusal, cannot produce this sequence.
        const std::vector<std::pair<std::int64_t, std::int64_t>> expected = {
            {1, 1},  // the first ask: prepare 1, generation 1
            {2, 2},  // a changed slide
            {3, 3},  // the SLOW ask, whose frame the peer held back
            {4, 4},  // the ask that overtook it
            {5, 5},  // the FORGET ask
            {6, 6},  // the single re-issue after the worker forgot
            {0, 7},  // the stand-down: revision 0, and still advancing
        };
        check(ledger == expected,
              "preview: the generation ledger is one strictly increasing ask per change, "
              "and nothing else");
    }
    check(waitUntil([&controller] { return controller.previewFrames() >= 2; }),
          "preview: the audience keeps its mirror after the edit goes away");
    check(!controller.selectionReady(),
          "preview: a frame with no target field is the audience's, never an edit");
    {
        const boost::json::object settled = readReportNow(report);
        check(reportNumber(settled, "selectionStale") == 1,
              "preview: exactly one draw was superseded in the whole conversation - the "
              "counter is derived, not incremented for every edit frame");
        check(controller.selectionStale() == reportNumber(settled, "selectionStale"),
              "preview: and the property ended where the worker's counter ended");
        check(reportNumber(settled, "previewDropped") == 0,
              "preview: no frame was dropped for want of a slot - the consumer released every "
              "one it was given");
        check(reportNumber(settled, "releases") >= reportNumber(settled, "selectionPublished"),
              "preview: releases keep pace with the frames published");
    }
    controller.shutdownNow();
}

// S15 (task 6.16b, review R16): the slide list on the CONSUMER's half of the
// socket.
//
// The live fixture proves the worker draws a band of slides into the shared
// stream, decodes into a pool of its own, and leaves the show alone. This
// scenario is the other half — the client, which receives a THIRD kind of
// frame on a stream it already had to route, and whose whole job is to be
// certain which picture belongs to which row:
//
//   - identity: a row's pixels are the ones the ack gave THAT slide, in the
//     cache under THAT key. The scripted producer paints (id, 200, 60) and
//     keeps a ledger of the ids it issued, so this is measured, not hoped;
//   - the three ways a frame must NOT be painted: it answers a generation the
//     client has moved past (OLDSTAMP), it belongs to a band the client has
//     replaced (LATE), or its header and its notification disagree about its
//     size (LIE). Each needs a producer willing to make the bad frame, which
//     is what those markers are for;
//   - the client's own bounds, measured through public properties: an ask that
//     cannot be honoured is refused before a byte is written, a band that is
//     unchanged costs nothing, an edited slide loses its old picture the
//     instant it is edited, and the picture cache stays under its byte ceiling
//     while rows keep arriving at the top of it;
//   - and the audience's pane, which is the reason any of this needs care: a
//     row that entered the mirror or the edit canvas would be R16 again, in
//     the opposite direction.
void scenarioThumbnailPreview() {
    ::setenv("OPENSHOW_FAKE_SCENARIO", "preview", 1);
    const std::string report = tempPath("thumb");
    ::unlink(report.c_str());
    ::setenv("OPENSHOW_FAKE_REPORT", report.c_str(), 1);

    WorkerController controller;
    controller.start();
    if (!check(waitUntil([&controller] {
                   return controller.statusText().contains(QStringLiteral("connected to worker"));
               }),
               "rows: handshake with the scripted producer")) {
        controller.shutdownNow();
        return;
    }
    check(controller.thumbnailSupported(),
          "rows: preview.thumbnail.v1 is the whole negotiation for a worker-drawn list");
    if (!check(waitUntil([&controller] { return controller.previewActive(); }),
               "rows: the stream is mapped before a row is asked for")) {
        controller.shutdownNow();
        return;
    }
    check(waitUntil([&controller] { return controller.previewFrames() >= 1; }),
          "rows: the audience's mirror arrives without being asked");
    check(controller.thumbnailFrames() == 0 && controller.thumbnailCachedBytes() == 0,
          "rows: nothing is wanted, so the list holds no pictures and costs no bytes");

    // A row's colour IS its id, and the producer's ledger says which id its
    // ack gave which key.
    const auto issuedId = [&report](const char* key) -> std::int64_t {
        const boost::json::object body = readReportNow(report);
        const auto entry = body.find("thumbnailIssued");
        if (entry == body.end() || !entry->value().is_array()) {
            return -1;
        }
        std::int64_t found = -1;
        for (const auto& value : entry->value().as_array()) {
            if (!value.is_object()) {
                continue;
            }
            const boost::json::object& item = value.as_object();
            const auto name = item.find("key");
            const auto id = item.find("id");
            if (name != item.end() && name->value().is_string() && id != item.end() &&
                std::string(name->value().as_string()) == key) {
                found = id->value().to_number<std::int64_t>();  // the newest one wins
            }
        }
        return found;
    };
    const auto rowIsPainted = [&controller, &issuedId](const char* key) {
        const std::int64_t id = issuedId(key);
        return id > 0 &&
               pixelsAre(controller.thumbnail(QString::fromLatin1(key)),
                         static_cast<int>(id & 0xFF), 200, 60);
    };

    // --- three asks that must never reach the socket ----------------------
    //
    // The scroll wheel fires this command, so the cost of a bad ask is decided
    // here rather than on the worker: the cap, the box and the slot, all three
    // with the offending number in the operator's status line and no byte
    // written. The peer's method count is what proves the second half.
    {
        std::vector<std::string> crowded;
        for (int i = 0; i < 33; ++i) {
            crowded.push_back("crowded-" + std::to_string(i));
        }
        controller.requestThumbnails(bandOf(crowded), 64, 36);
        check(controller.statusText().contains(QStringLiteral("takes 32")),
              "rows: a band of thirty-three is refused in the client, naming the cap");
        controller.requestThumbnails(bandOf({"tiny"}), 16, 16);
        check(controller.statusText().contains(QStringLiteral("16x16")) &&
                  controller.statusText().contains(QStringLiteral("32..512")),
              "rows: a 16x16 row is refused, and the refusal names the box it failed");
        controller.requestThumbnails(bandOf({"wide"}), 512, 100);
        const QString fit = controller.statusText();
        check(fit.contains(QStringLiteral("512x100")) &&
                  fit.contains(QStringLiteral("147520")),
              "rows: a 512x100 row is refused because this stream's slots hold 147520 bytes");
        check(waitUntil([&report] {
                  const boost::json::object body = readReportNow(report);
                  return !body.empty() && methodCount(body, "preview.thumbnails") == 0;
              }),
              "rows: all three were refused before a byte was written - the peer saw none");
        check(controller.thumbnailFrames() == 0, "and none of them cost a picture either");
    }

    // --- a band, with one row the producer will not take ------------------
    const QString sceneOne = QString::fromStdString(sceneForSelection("R", "one", 0x101030u));
    const QString sceneTwo = QString::fromStdString(sceneForSelection("R", "two", 0x201010u));
    QVariantList band;
    band.push_back(listRow(QStringLiteral("row-one"), sceneOne));
    band.push_back(listRow(QString::fromStdString(std::string(200, 'k')), sceneOne));
    band.push_back(listRow(QStringLiteral("row-two"), sceneTwo));
    controller.requestThumbnails(band, 64, 36);
    if (!check(waitUntil([&rowIsPainted] {
                   return rowIsPainted("row-one") && rowIsPainted("row-two");
               }),
               "rows: both good rows arrive, each in the colour of the id its OWN slide was "
               "given")) {
        controller.shutdownNow();
        return;
    }
    check(controller.thumbnail(QString::fromStdString(std::string(200, 'k'))).isNull(),
          "rows: the 200-character key has no picture - a row the producer refused by name is a "
          "row that keeps its text line, not a blank guess");
    check(controller.statusText().contains(QStringLiteral("1 of 3 slides refused")),
          "and the refusal is on the status line, counted, rather than silent");
    check(pixelsAre(controller.previewImage(), 1, 2, 3),
          "rows: the audience's pane is still the show's own picture");
    check(controller.selectionFrames() == 0 && controller.selectionImage().isNull(),
          "and no row ever entered the edit target: three targets, three panes");

    // --- an unchanged band costs nothing ---------------------------------
    {
        const std::int64_t asksBefore = jsonSize(readReportNow(report), "thumbnailOps");
        const qint64 bytesBefore = controller.thumbnailCachedBytes();
        controller.requestThumbnails(band, 64, 36);
        pumpEventsFor(600);
        check(jsonSize(readReportNow(report), "thumbnailOps") == asksBefore,
              "rows: asking again for the band the newest ask already carries costs no second "
              "command - a scroll handler that fires twice is normal");
        check(controller.thumbnailCachedBytes() == bytesBefore,
              "and no further picture memory");
    }

    // --- an edited slide loses its old picture immediately ---------------
    {
        QVariantList edited;
        edited.push_back(listRow(QStringLiteral("row-one"),
                                 QString::fromStdString(
                                     sceneForSelection("R", "one, edited", 0x302010u))));
        edited.push_back(listRow(QStringLiteral("row-two"), sceneTwo));
        controller.requestThumbnails(edited, 64, 36);
        check(controller.thumbnail(QStringLiteral("row-one")).isNull(),
              "rows: the instant a slide's text changes, its old picture is gone - a stale row "
              "is a lie about the slide it is listed under");
        check(!controller.thumbnail(QStringLiteral("row-two")).isNull(),
              "while the row nobody edited keeps the picture it already had");
        check(waitUntil([&rowIsPainted] { return rowIsPainted("row-one"); }),
              "rows: and the edited slide comes back as a new render, under the new id");
    }

    // --- the row a replaced band was already drawing ---------------------
    //
    // The producer has begun this row's draw, so a newer band cannot take it
    // back: it lands after the replacement has been acked, carrying the
    // generation accepted at publish time. The client has no id for it any
    // more, and painting it would put a slide the operator has scrolled away
    // from under a row that is on screen.
    controller.requestThumbnails(bandOf({"LATE-row"}), 64, 36);
    if (!check(waitUntil([&report] {
                   return reportNumber(readReportNow(report), "thumbnailQueued") >= 1;
               }),
               "rows: the producer is still drawing that row when the operator scrolls on")) {
        controller.shutdownNow();
        return;
    }
    controller.requestThumbnails(bandOf({"next-row"}), 64, 36);
    if (!check(waitUntil([&rowIsPainted] { return rowIsPainted("next-row"); }),
               "rows: the band the operator scrolled to arrives first")) {
        controller.shutdownNow();
        return;
    }
    {
        const qint64 copiesWithNewest = controller.thumbnailFrames();
        const std::int64_t publishedBefore =
            reportNumber(readReportNow(report), "thumbnailPublished");
        pumpEventsFor(900);  // the held row is due at +500 ms and is delivered
        check(reportNumber(readReportNow(report), "thumbnailPublished") > publishedBefore,
              "rows: the superseded row really did reach the socket, so the drop below is the "
              "consumer's and not an untested producer");
        check(controller.thumbnail(QStringLiteral("LATE-row")).isNull(),
              "rows: a row of a replaced band is never painted");
        check(controller.thumbnailFrames() == copiesWithNewest,
              "and it is dropped before the copy - no picture, no bytes, no frame counted");
        check(rowIsPainted("next-row"),
              "and the row that survived the scroll still shows its own slide");
        check(waitUntil([&controller] { return controller.thumbnailStale() >= 1; }),
              "rows: the worker's superseded-draw counter mirrors into the property");
    }

    // --- a frame wearing somebody else's generation ---------------------
    //
    // The other gate, which the case above cannot exercise: this row IS on the
    // list, its id IS in the map the ack installed, and the only thing wrong
    // with the frame is that it answers an ask the client has moved past. A
    // producer whose bookkeeping slipped by one generation is a producer that
    // exists, and the picture would be a plausible one.
    {
        const qint64 copiesBefore = controller.thumbnailFrames();
        const std::int64_t publishedBefore =
            reportNumber(readReportNow(report), "thumbnailPublished");
        controller.requestThumbnails(bandOf({"OLDSTAMP-row"}), 64, 36);
        pumpEventsFor(700);
        check(reportNumber(readReportNow(report), "thumbnailPublished") > publishedBefore,
              "rows: the slipped frame reached the socket");
        check(controller.thumbnail(QStringLiteral("OLDSTAMP-row")).isNull(),
              "rows: a frame whose generation is not the accepted one is dropped even though "
              "its id names a row on this list");
        check(controller.thumbnailFrames() == copiesBefore,
              "and nothing was copied for it");
    }

    // --- a frame whose header contradicts its notification ---------------
    //
    // The bytes in the slot are a perfectly good frame of one size and the
    // notification announces another, self-consistent one that also fits. Only
    // the cross-check against the header the same sequence stamps can stop the
    // copy, and the difference is a row whose image is the wrong shape: too
    // few bytes for a wide row, or reading past the frame for a tall one.
    {
        const qint64 copiesBefore = controller.thumbnailFrames();
        const std::int64_t publishedBefore =
            reportNumber(readReportNow(report), "thumbnailPublished");
        controller.requestThumbnails(bandOf({"LIE-row"}), 64, 36);
        const bool lied = waitUntil([&report, publishedBefore] {
            return reportNumber(readReportNow(report), "thumbnailPublished") > publishedBefore;
        });
        if (!check(lied, "rows: the lying row is published")) {
            controller.shutdownNow();
            return;
        }
        pumpEventsFor(400);  // the client has had several turns to copy it
        check(controller.thumbnail(QStringLiteral("LIE-row")).isNull(),
              "rows: a frame whose header and notification disagree about its size is not "
              "copied at all - the safe answer, because neither number is trustworthy");
        check(controller.thumbnailFrames() == copiesBefore,
              "and it is not counted as a picture the list received");
    }

    // --- the picture cache's ceiling, in bytes --------------------------
    //
    // Thirty-two rows at the stream's own extent are 4,718,592 bytes, over the
    // client's ceiling, and the document that does this is an operator reading
    // a long service. The bound has to be a number rather than a habit, so the
    // ask is: every row arrives, the cache does not go over its ceiling, and
    // what disappears is the top of the list rather than the rows being looked
    // at.
    {
        std::vector<std::string> long1;
        for (int i = 0; i < 32; ++i) {
            long1.push_back("fill-" + std::to_string(i));
        }
        const qint64 copiesBefore = controller.thumbnailFrames();
        constexpr std::int64_t kRowBytes = 256 * 144 * 4;
        controller.requestThumbnails(bandOf(long1), 256, 144);
        if (!check(waitUntil([&controller] {
                       return !controller.thumbnail(QStringLiteral("fill-31")).isNull();
                   },
                   12000),
                   "rows: the last row of a thirty-two row band at the stream's extent arrives")) {
            controller.shutdownNow();
            return;
        }
        check(controller.thumbnailFrames() == copiesBefore + 32,
              "every one of the thirty-two was received - the cache is not a filter");
        check(controller.thumbnailCachedBytes() <= 4 * 1024 * 1024,
              "and the cache stayed under its 4 MiB ceiling while they arrived");
        check(controller.thumbnailCachedBytes() > 4 * 1024 * 1024 - kRowBytes,
              "with the ceiling, and not a halfway-full cache, doing the stopping");
        check(controller.thumbnail(QStringLiteral("fill-0")).isNull(),
              "the oldest rows are the ones that went...");
        check(!controller.thumbnail(QStringLiteral("fill-31")).isNull(),
              "...and the newest stayed: the bound is an LRU, not a trim");
    }

    // --- a worker that lost the band gets asked again -------------------
    {
        const std::int64_t asksBefore = jsonSize(readReportNow(report), "thumbnailOps");
        controller.requestThumbnails(bandOf({"FORGET-row"}), 64, 36);
        if (!check(waitUntil([&report, asksBefore] {
                       return jsonSize(readReportNow(report), "thumbnailOps") == asksBefore + 2;
                   },
                   6000),
                   "rows: a worker that reports the band gone is asked again, and the re-issued "
                   "band is what the list ends up showing")) {
            controller.shutdownNow();
            return;
        }
        pumpEventsFor(1200);  // two polls' worth: a list that never settles would keep asking
        check(jsonSize(readReportNow(report), "thumbnailOps") == asksBefore + 2,
              "and the recovery asks exactly once - no command storm per poll");
    }

    // --- standing down --------------------------------------------------
    {
        controller.clearThumbnails();
        check(controller.thumbnailCachedBytes() == 0 &&
                  controller.thumbnail(QStringLiteral("fill-31")).isNull(),
              "rows: a stand-down drops every picture it was holding - a list nobody is looking "
              "at stops costing memory at once");
        if (!check(waitUntil([&report] {
                       const boost::json::object body = readReportNow(report);
                       const std::int64_t asks = jsonSize(body, "thumbnailOps");
                       if (asks <= 0) {
                           return false;
                       }
                       const auto entry = body.find("thumbnailOps");
                       const boost::json::object& last =
                           entry->value().as_array()[static_cast<std::size_t>(asks - 1)]
                               .as_object();
                       const auto asked = last.find("asked");
                       return asked != last.end() && asked->value().to_number<std::int64_t>() == 0;
                   }),
                   "rows: the stand-down reaches the worker as an empty band")) {
            controller.shutdownNow();
            return;
        }
        const std::int64_t asksAtRest = jsonSize(readReportNow(report), "thumbnailOps");
        const qint64 copiesAtRest = controller.thumbnailFrames();
        pumpEventsFor(900);
        check(jsonSize(readReportNow(report), "thumbnailOps") == asksAtRest &&
                  controller.thumbnailFrames() == copiesAtRest,
              "and a closed list asks for nothing further: no re-issue storm for a target the "
              "operator has left");
    }

    // --- the whole conversation, as one number --------------------------
    {
        const boost::json::object settled = readReportNow(report);
        const auto entry = settled.find("thumbnailOps");
        std::vector<std::int64_t> generations;
        if (entry != settled.end() && entry->value().is_array()) {
            for (const auto& value : entry->value().as_array()) {
                const auto request = value.as_object().find("request");
                generations.push_back(request->value().to_number<std::int64_t>());
            }
        }
        bool advances = !generations.empty();
        for (std::size_t i = 0; i < generations.size(); ++i) {
            if (generations[i] != static_cast<std::int64_t>(i) + 1) {
                advances = false;
            }
        }
        check(advances,
              "rows: the generation ledger is one strictly increasing ask per change, with no "
              "replay and no gap - every refusal above reserved nothing, because none of them "
              "reached the wire to be refused");
        check(reportNumber(settled, "thumbnailStale") == 1,
              "rows: exactly one draw was superseded in the whole conversation, and the counter "
              "is derived rather than incremented per frame");
        check(controller.thumbnailStale() == reportNumber(settled, "thumbnailStale"),
              "and the property ended where the worker's counter ended");
        check(reportNumber(settled, "previewDropped") == 0,
              "rows: no ROW was ever lost for want of a slot - the consumer released every row "
              "it was given, painted or not");
        check(reportNumber(settled, "releases") ==
                  reportNumber(settled, "thumbnailPublished") +
                      reportNumber(settled, "selectionPublished") +
                      reportNumber(settled, "mirrorPublished"),
              "and every frame published on the stream, in any of its three targets, had its "
              "ownership handed back: the dropped frames of this run are mirror frames the peer "
              "chose not to wait for, which is the producer's own cadence and not a leak");
        check(reportNumber(settled, "presents") == 0,
              "rows: through a whole slide list, the peer saw no scene.present at all");
    }
    controller.shutdownNow();
}

// S16 (task 6.21e, review F4): a gradient background IS a background. The
// black-to-colour payload the flat flag used to miss goes on air with the
// indicator lit, the clear takes the gradient off the wire (the key, not a
// zeroed value — presence is what the renderer reads), the acked state then
// reads as empty with the words untouched, and the restore brings the ramp
// back. The scripted peer's per-prepare gradient ledger is what proves the
// second point on the wire rather than in the client's own books.
void scenarioGradientClear() {
    ::setenv("OPENSHOW_FAKE_SCENARIO", "clean", 1);
    const std::string report = tempPath("gradient");
    ::unlink(report.c_str());
    ::setenv("OPENSHOW_FAKE_REPORT", report.c_str(), 1);

    WorkerController controller;
    controller.start();
    if (!check(waitUntil([&controller] {
                   return controller.statusText().contains(QStringLiteral("connected to worker"));
               }),
               "gradient: handshake with the scripted peer")) {
        controller.shutdownNow();
        return;
    }

    // Black top, amber bottom, one line of words. Nothing about the top
    // colour says "there is a background"; only the gradient key does.
    controller.presentSceneJson(QString::fromStdString(
        sceneGradient("GRADED", 0x000000u, 0xFF8000u, {"Graduated"})));
    const bool onAir = waitUntil([&controller] {
        return controller.sceneActive() == 1 && controller.wordsOnAir() &&
               controller.backgroundOnAir();
    });
    if (!check(onAir,
               "gradient: a black-to-colour gradient is on air WITH its background indicator "
               "lit (the button the old flag left dead)")) {
        controller.shutdownNow();
        return;
    }

    controller.clearBackground();
    const bool cleared = waitUntil([&controller] {
        return controller.sceneActive() == 2 && controller.scenePending() == 0;
    });
    if (!check(cleared, "gradient: the background clear is acknowledged on air")) {
        controller.shutdownNow();
        return;
    }
    digestThenRefresh(controller, 50);
    check(!controller.backgroundOnAir(),
          "gradient: the acked output reads as no background — the UI and the audience agree");
    check(controller.wordsOnAir(), "gradient: the words rode through the gradient clear");
    check(controller.canRestore(), "gradient: restore is available after a gradient clear");

    controller.restoreOutput();
    const bool restored = waitUntil([&controller] {
        return controller.sceneActive() == 3 && controller.scenePending() == 0 &&
               controller.backgroundOnAir() && controller.wordsOnAir();
    });
    check(restored, "gradient: the restore brings the ramp back, indicator and all");

    controller.shutdownNow();
    const boost::json::object counts = readReport(report);
    // The peer's ledger: go carries the second colour, the clear's payload
    // does not even name the key, the restore names it again.
    std::vector<std::string> gradients;
    if (const auto entry = counts.find("gradients");
        entry != counts.end() && entry->value().is_array()) {
        for (const auto& value : entry->value().as_array()) {
            if (value.is_string()) {
                gradients.push_back(std::string(value.as_string()));
            }
        }
    }
    std::string joined;
    for (const std::string& gradient : gradients) {
        joined += (joined.empty() ? "" : " | ") + (gradient.empty() ? "flat" : gradient);
    }
    std::fprintf(stderr, "6.21e: gradient ledger per prepare: [%s]\n", joined.c_str());
    check(gradients.size() == 3 && !gradients[0].empty() && gradients[1].empty() &&
              !gradients[2].empty(),
          ("gradient: go and restore carry the second colour, the cleared prepare does not "
           "name it (got [" +
           joined + "])")
              .c_str());
    check(reportNumber(counts, "presents") == 3,
          "gradient: three presents - the go, the clear, the restore");
}

// S17 (task 6.21f, review F6): the foreground fault lane reaches the
// operator. The worker has reported per-item picture failures since 6.20d;
// this is the frontend half — the projection into the properties QML binds,
// the growth of the capped list beside its monotonic event count, the
// repair transition the operator can cause by fixing the file and going
// again, and the independence of the transport's own fault line, which the
// foreground notice must neither evict nor be evicted by.
void scenarioForegroundFaults() {
    ::setenv("OPENSHOW_FAKE_SCENARIO", "foreground", 1);
    const std::string report = tempPath("foreground");
    ::unlink(report.c_str());
    ::setenv("OPENSHOW_FAKE_REPORT", report.c_str(), 1);

    WorkerController controller;
    controller.start();
    if (!check(waitUntil([&controller] {
                   return controller.statusText().contains(QStringLiteral("connected to worker"));
               }),
               "fgfault: handshake with the scripted peer")) {
        controller.shutdownNow();
        return;
    }

    // The media lane is faulted from the first poll and stays so for the
    // whole scenario — the yardstick for "the new notice joined the
    // reports, it did not replace one".
    check(waitUntil([&controller] {
              return controller.mediaError().contains(QStringLiteral("background decode"));
          }),
          "fgfault: the transport reports its own fault, undisturbed");
    const QString mediaFault = controller.mediaError();
    check(controller.foregroundErrors().isEmpty() && controller.foregroundFailureEvents() == 0,
          "fgfault: nothing is reported before a picture has failed");

    controller.presentSceneJson(
        QString::fromStdString(sceneForSelection("FGMISSING", "One", 0xFFFFFFu)));
    const bool armed = waitUntil([&controller] {
        return controller.foregroundErrors().size() == 1 &&
               controller.foregroundFailureEvents() == 1;
    });
    if (!check(armed, "fgfault: a failed picture is projected from the status poll")) {
        std::fprintf(stderr, "fgfault: errors=%lld events=%lld\n",
                     static_cast<long long>(controller.foregroundErrors().size()),
                     static_cast<long long>(controller.foregroundFailureEvents()));
        controller.shutdownNow();
        return;
    }
    const QVariantMap first = controller.foregroundErrors().first().toMap();
    check(first.value(QStringLiteral("path")).toString() == QLatin1String("./shows/gone.png") &&
              first.value(QStringLiteral("error"))
                  .toString()
                  .contains(QStringLiteral("no such file")),
          "fgfault: the notice names the path AND the reason, verbatim from the worker");
    check(controller.sceneActive() == 1 && controller.wordsOnAir(),
          "fgfault: a missing picture is not a failed go — the scene is on air");
    check(controller.mediaError() == mediaFault,
          "fgfault: the transport's line is untouched by the foreground report");

    controller.presentSceneJson(
        QString::fromStdString(sceneForSelection("FGSECOND", "Two", 0xFFFFFFu)));
    const bool grew = waitUntil([&controller] {
        return controller.foregroundErrors().size() == 2 &&
               controller.foregroundFailureEvents() == 2;
    });
    check(grew, "fgfault: a second broken path extends the list and the count");
    const QVariantList faults = controller.foregroundErrors();
    check(faults.size() == 2 &&
              faults[0].toMap().value(QStringLiteral("path")) !=
                  faults[1].toMap().value(QStringLiteral("path")),
          "fgfault: the list reports one entry per asset path, not one incident");

    // The repair: the operator puts the file back and goes again. The
    // worker's later prepare decodes the path and drops the report; the
    // notice follows the WORKER's answer, and the event count — monotonic,
    // like presentFailures — does not un-happen.
    controller.presentSceneJson(
        QString::fromStdString(sceneForSelection("FGREPAIR", "Three", 0xFFFFFFu)));
    const bool repaired = waitUntil([&controller] {
        return controller.foregroundErrors().isEmpty() &&
               controller.foregroundFailureEvents() == 2;
    });
    check(repaired,
          "fgfault: the notice clears when the repair is acknowledged, and the lifetime count "
          "stays counted");
    check(controller.mediaError() == mediaFault,
          "fgfault: clearing the foreground notice did not clear the transport's own fault");
    std::fprintf(stderr,
                 "fgfault: %d fault(s) projected with path+reason, repaired to %d, events "
                 "steady at %lld, media lane unchanged\n",
                 2, static_cast<int>(controller.foregroundErrors().size()),
                 static_cast<long long>(controller.foregroundFailureEvents()));
    controller.shutdownNow();
    const boost::json::object counts = readReport(report);
    check(reportNumber(counts, "prepares") == 3,
          "fgfault: three prepares - the broken go, the second path, the repair");
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
    scenarioAdoptedForeground();
    scenarioMediaTransport();
    scenarioSelectionPreview();
    scenarioThumbnailPreview();
    scenarioGradientClear();
    scenarioForegroundFaults();

    if (gFailures != 0) {
        std::fprintf(stderr, "worker_controller_tests: %d failure(s)\n", gFailures);
        return 1;
    }
    std::printf("worker_controller_tests: reply correlation and latest-go intent pinned\n");
    return 0;
}
