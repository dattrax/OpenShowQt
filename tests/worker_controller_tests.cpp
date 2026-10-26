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
 *
 * scene.present of a revision more than kStoreWindow (16, mirroring the
 * real worker's bounded store) behind the newest handed out is refused, so
 * a restore that still depended on the store being asked cannot hide that.
 *
 * Each child writes a small report (prepares, presents, requests for methods it
 * does not model, presented revision list) to OPENSHOW_FAKE_REPORT so the parent
 * can prove WHICH requests the controller actually sent, not just the final
 * revision. No display, no GPU, no real worker.
 */

#include <boost/json.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
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
          fReportPath(std::move(reportPath)) {}

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
            if (ready > 0) {
                char buffer[4096];
                const ssize_t count = ::recv(fClient, buffer, sizeof(buffer), 0);
                if (count <= 0) {
                    return;  // the controller hung up
                }
                fDecoder.feed(std::string_view(buffer, static_cast<std::size_t>(count)));
                boost::json::value message;
                std::string error;
                while (fDecoder.next(&message, &error)) {
                    if (message.is_object() && message.as_object().contains("method")) {
                        handle(message.as_object());
                    }
                }
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

    void sendError(std::int64_t id, const char* text) {
        std::string wire;
        proto::appendFrame(proto::makeError(id, proto::ErrorCode::kInvalidParams, text), &wire);
        send(wire);
    }

    void handle(const boost::json::object& request) {
        const std::int64_t id = request.contains("id") && request.at("id").is_int64()
                                    ? request.at("id").as_int64()
                                    : 0;
        const std::string method(request.at("method").as_string());
        if (method == "hello") {
            boost::json::object result{
                {"application", "openshow-worker"},
                {"version", "fake-for-6.3a"},
                {"protocol", boost::json::object{{"major", 0}, {"minor", 1}}},
                {"capabilities", boost::json::array{}},
            };
            sendResult(id, std::move(result));
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
            sendResult(id, std::move(result));
            return;
        }
        if (method == "scene.prepare") {
            ++fPrepares;
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
        boost::json::object report{
            {"scenario", fScenario},
            {"prepares", fPrepares},
            {"presents", fPresents},
            {"unknown", fUnknown},
            {"presented", std::move(presented)},
        };
        if (std::FILE* file = std::fopen(fReportPath.c_str(), "wb")) {
            const std::string text = boost::json::serialize(boost::json::value(report));
            std::fwrite(text.data(), 1, text.size(), file);
            std::fclose(file);
        }
    }

    std::vector<std::string> fHeld;  // hold3: acks awaiting the burst
    std::vector<DelayedReply> fQueue;  // slow-first: acks awaiting their due
    std::vector<int> fPresented;       // revisions the controller went to

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
};

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

}  // namespace

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

    if (gFailures != 0) {
        std::fprintf(stderr, "worker_controller_tests: %d failure(s)\n", gFailures);
        return 1;
    }
    std::printf("worker_controller_tests: reply correlation and latest-go intent pinned\n");
    return 0;
}
