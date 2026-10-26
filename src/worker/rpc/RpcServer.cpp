/*
 * RpcServer implementation. Raw AF_UNIX stream socket, one client, poll()
 * loop with 50 ms ticks so stop() and requestStop() are noticed promptly.
 * All framing and JSON-RPC shape lives in openshow::protocol; this file is
 * routing, the hello gate and the session calls — nothing else.
 */

#include "rpc/OutboundQueue.h"
#include "rpc/RpcServer.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "openshow/protocol/preview_wire.h"
#include "openshow/protocol/protocol.h"
#include "openshow/scene/scene.h"

#include "render/Layer.h"
#include "session/OutputSession.h"

namespace openshow::worker {

namespace {

namespace proto = openshow::protocol;

constexpr int kPollTimeoutMs = 50;

// How long the queue may go without ONE byte reaching the socket before the
// frontend counts as gone. Any peer that reads at all makes progress and resets
// it, so this never fires on a busy but living connection; it exists because
// without it a peer that froze with a full socket buffer was served forever —
// commands queued, disconnect undetected, shutdown waiting on a thread that
// had no reason to notice.
constexpr auto kWriteDeadline = std::chrono::milliseconds(2000);

// Puts as much of the queue on the socket as the socket will take right now and
// reports whether the socket is still usable. Nothing here waits: the
// connection is O_NONBLOCK, so a frontend that has stopped reading costs one
// EWOULDBLOCK and the bytes stay in the bounded queue for the next POLLOUT.
// This replaces the blocking send() that review R11 pointed at — that call
// parked the server thread inside the kernel for as long as the peer stayed
// unread, which stopped command handling, stopped disconnect detection, and
// meant the render thread's work piled up behind a frontend nobody was serving.
bool drainToSocket(int fd, OutboundQueue* queue,
                   std::chrono::steady_clock::time_point* lastProgress) {
    while (!queue->empty()) {
        const ssize_t written =
            ::send(fd, queue->headData(), queue->headRemaining(), MSG_NOSIGNAL);
        if (written > 0) {
            queue->didWrite(static_cast<std::size_t>(written));
            *lastProgress = std::chrono::steady_clock::now();
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true;  // full: the queue holds it, poll will report POLLOUT
        }
        return false;  // EPIPE or ECONNRESET: the frontend is gone
    }
    return true;
}

// An integer argument in a stated range, checked BEFORE any narrowing
// (task 6.8a, review R11). Casting first and validating after is how
// 4294967296 becomes 0: a caller that meant "no such monitor" gets monitor
// zero, which is a real destination, and the audience's output moves on an
// argument nobody sent. Absent, non-integer, or out of range all answer
// false, and the handler says which.
bool jsonIntInRange(const boost::json::object& args, const char* key, std::int64_t low,
                    std::int64_t high, std::int64_t* out) {
    const auto entry = args.find(key);
    if (entry == args.end()) {
        return false;
    }
    std::int64_t value = 0;
    if (entry->value().is_int64()) {
        value = entry->value().as_int64();
    } else if (entry->value().is_uint64() &&
               entry->value().as_uint64() <=
                   static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        value = static_cast<std::int64_t>(entry->value().as_uint64());
    } else {
        return false;
    }
    if (value < low || value > high) {
        return false;
    }
    *out = value;
    return true;
}

// A 32-bit argument — the same rule as above, spelled for the values the
// session's own range checks are written in.
bool jsonUint32(const boost::json::object& args, const char* key, std::uint32_t* out) {
    std::int64_t value = 0;
    if (!jsonIntInRange(args, key, 0, std::numeric_limits<std::uint32_t>::max(), &value)) {
        return false;
    }
    *out = static_cast<std::uint32_t>(value);
    return true;
}

// Reads a non-negative JSON integer at full width. Boost.JSON stores small
// numbers as int64 and only reaches for uint64 past the signed range, so
// checking is_uint64() alone silently rejects every slot index a client
// sends. Nothing narrows the result, so no range is imposed here.
bool jsonUint64(const boost::json::object& args, const char* key, std::uint64_t* out) {
    const auto entry = args.find(key);
    if (entry == args.end()) {
        return false;
    }
    if (entry->value().is_uint64()) {
        *out = entry->value().as_uint64();
        return true;
    }
    if (entry->value().is_int64() && entry->value().as_int64() >= 0) {
        *out = static_cast<std::uint64_t>(entry->value().as_int64());
        return true;
    }
    return false;
}

boost::json::value layersObject(uint32_t mask) {
    boost::json::object layers;
    for (uint32_t i = 0; i < static_cast<uint32_t>(LayerId::kCount); ++i) {
        const LayerId id = static_cast<LayerId>(i);
        layers[std::string(layerName(id))] = (mask & layerBit(id)) != 0;
    }
    return layers;
}

boost::json::object statusResult(const OutputSession::Status& status) {
    const OutputSession::PreviewStatus& preview = status.preview;
    return boost::json::object{{"device", status.deviceName},
                               {"width", status.width},
                               {"height", status.height},
                               {"frames", status.frames},
                               // Failed acquires/presents since start (task 6.4): the honest
                               // evidence that a confirmation took the long way around.
                               {"presentFailures", status.presentFailures},
                               {"revision", status.revision},
                               {"monitor", status.monitor},
                               {"monitorName", status.monitorName},
                               {"layers", layersObject(status.layerMask)},
                               {"preview", boost::json::object{{"running", preview.running},
                                                               {"name", preview.name},
                                                               {"width", preview.width},
                                                               {"height", preview.height},
                                                               {"fps", preview.fps},
                                                               {"published", preview.published},
                                                               {"dropped", preview.dropped},
                                                               {"error", preview.error}}},
                               {"scene", boost::json::object{{"active", status.scene.active},
                                                             {"pending", status.scene.pending},
                                                             {"transitioning",
                                                              status.scene.transitioning},
                                                             {"prepared", status.scene.prepared}}},
                               // Background media (task 5.5d): the video
                               // transport's live state; error is sticky
                               // until a successful open replaces it.
                               // "audio" (task 6.7c, review R18) is the
                               // typed audible lane: none | silent |
                               // no-output | degraded | paused | live.
                               {"media", boost::json::object{{"playing", status.media.playing},
                                                             {"path", status.media.path},
                                                             {"error", status.media.error},
                                                             {"audio", status.media.audio},
                                                             {"position", status.media.position}}}};
}

boost::json::value displaysResult(const std::vector<OutputSession::DisplayInfo>& displays) {
    boost::json::array array;
    for (const auto& display : displays) {
        array.push_back(boost::json::object{{"index", display.index},
                                            {"name", display.name},
                                            {"width", display.width},
                                            {"height", display.height},
                                            {"refreshHz", display.refreshHz}});
    }
    return array;
}

}  // namespace

RpcServer::RpcServer(OutputSession& session, Options options)
    : fSession(session), fOptions(std::move(options)) {}

RpcServer::~RpcServer() {
    stop();
}

bool RpcServer::listen(std::string* error) {
    if (fOptions.socketPath.empty()) {
        if (error) {
            *error = "empty socket path";
        }
        return false;
    }
    if (fOptions.socketPath.size() >= sizeof(sockaddr_un::sun_path)) {
        if (error) {
            *error = "socket path too long";
        }
        return false;
    }

    fListenFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fListenFd < 0) {
        if (error) {
            *error = std::strerror(errno);
        }
        return false;
    }

    // A worker killed uncleanly can leave the path behind; bind would fail
    // with EADDRINUSE on the stale file.
    ::unlink(fOptions.socketPath.c_str());

    sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, fOptions.socketPath.c_str(),
                 sizeof(address.sun_path) - 1);
    if (::bind(fListenFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        if (error) {
            *error = std::strerror(errno);
        }
        close(fListenFd);
        fListenFd = -1;
        return false;
    }
    if (::listen(fListenFd, 1) != 0) {
        if (error) {
            *error = std::strerror(errno);
        }
        close(fListenFd);
        fListenFd = -1;
        return false;
    }
    return true;
}

bool RpcServer::start(std::string* error) {
    if (fListenFd < 0) {
        if (error) {
            *error = "server is not listening";
        }
        return false;
    }
    fRunning = true;
    fThread = std::thread(&RpcServer::run, this);
    return true;
}

void RpcServer::stop() {
    fRunning = false;
    {
        // Waking the serving thread is all this may do with the descriptor: it
        // is owned by that thread, which closes it. Holding the mutex is what
        // makes the shutdown() safe rather than a guess about a number — see
        // fConnectionMutex. poll() returns immediately on a shut-down socket
        // and a nonblocking write never blocks, so this is prompt whatever the
        // frontend is doing (the 50 ms tick is the worst case).
        std::lock_guard<std::mutex> lock(fConnectionMutex);
        if (fConnectionFd >= 0) {
            ::shutdown(fConnectionFd, SHUT_RDWR);
        }
    }
    if (fListenFd >= 0) {
        ::shutdown(fListenFd, SHUT_RDWR);
    }
    if (fThread.joinable()) {
        fThread.join();
    }
    // The thread is gone: it either closed the connection itself, under the
    // mutex and before clearing the member, or never accepted one. There is
    // nothing of ours left to close, and closing what the member still held
    // here was precisely the unsynchronised access review R11 named.
    if (fListenFd >= 0) {
        close(fListenFd);
        fListenFd = -1;
    }
    if (!fOptions.socketPath.empty()) {
        ::unlink(fOptions.socketPath.c_str());
    }
}

void RpcServer::run() {
    while (fRunning) {
        pollfd pending{fListenFd, POLLIN, 0};
        const int ready = ::poll(&pending, 1, kPollTimeoutMs);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (ready == 0) {
            continue;
        }
        const int connection = ::accept(fListenFd, nullptr, nullptr);
        if (connection < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK ||
                errno == ECONNABORTED) {
                continue;
            }
            break;
        }
        // Nonblocking from the first instant: every write to this socket is
        // one the server thread must be able to walk away from (review R11).
        const int flags = ::fcntl(connection, F_GETFL, 0);
        if (flags >= 0) {
            ::fcntl(connection, F_SETFL, flags | O_NONBLOCK);
        }
        {
            std::lock_guard<std::mutex> lock(fConnectionMutex);
            if (!fRunning) {
                // stop() got here first: nothing is served, and the descriptor
                // is closed here because it never became the member.
                close(connection);
                break;
            }
            fConnectionFd = connection;
        }
        std::fprintf(stderr, "openshow-worker: frontend connected on %s\n",
                     fOptions.socketPath.c_str());
        serve(connection);
        {
            // Cleared before the close, under the mutex, so stop() can never be
            // handed a number that has already gone back to the kernel.
            std::lock_guard<std::mutex> lock(fConnectionMutex);
            fConnectionFd = -1;
        }
        close(connection);
        // The worker lives only while connected to its frontend: whoever
        // disconnected, the answer is to exit.
        std::fprintf(stderr, "openshow-worker: frontend disconnected, stopping\n");
        fSession.requestStop();
        return;
    }
}

void RpcServer::serve(int connectionFd) {
    proto::Decoder decoder;
    bool handshakeDone = false;
    char buffer[8192];
    OutboundQueue outbound;
    auto lastProgress = std::chrono::steady_clock::now();

    while (fRunning) {
        pollfd pending{connectionFd, POLLIN | POLLRDHUP, 0};
        if (!outbound.empty()) {
            pending.events |= POLLOUT;
        }
        // The tick stays short whatever the queue is doing: it bounds how long
        // stop() and a notification can wait, and it is what makes the write
        // deadline below observable rather than a promise. With bytes owed the
        // poll also wakes for the deadline itself, so a stalled frontend is
        // noticed on time rather than at the next thing that happens to arrive.
        int timeoutMs = kPollTimeoutMs;
        if (!outbound.empty()) {
            const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - lastProgress);
            timeoutMs = static_cast<int>(
                std::max<int64_t>(0, std::min<int64_t>(kPollTimeoutMs,
                                                       (kWriteDeadline - since).count())));
        }
        const int ready = ::poll(&pending, 1, timeoutMs);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        // Try the queue on EVERY tick, not only when poll reports POLLOUT. That
        // is not a workaround for a kernel quirk: POLLOUT is the promise that the
        // peer's queue has room, and on a stream socket whose receive buffer is
        // small compared with what is owed it stays off for whole seconds while
        // the peer is reading all the same — a frontend reading 1 KiB every 50 ms
        // to clear a 200 KiB debt. Gating the drain on it produced exactly the
        // stall this loop exists to avoid: nothing written, progress not made,
        // the deadline fired, and a live frontend was dropped for reading slowly.
        // A nonblocking send costs one syscall to answer EAGAIN, so the loop can
        // simply ask the socket every tick and let the deadline judge the answer.
        if (!outbound.empty() && !drainToSocket(connectionFd, &outbound, &lastProgress)) {
            return;  // write side of the socket is gone
        }
        // Every tick, timeout included: the render thread queues preview
        // notifications and this thread is the only one holding the socket.
        // Collecting is not writing — no session lock is held anywhere near the
        // socket, which is the other half of what R11 asked for.
        std::vector<QueuedFrame> notifications;
        collectNotifications(&notifications);
        for (QueuedFrame& note : notifications) {
            outbound.push(note.key, std::move(note.bytes));
        }
        if (!outbound.empty()) {
            // The deadline is measured against PROGRESS, not against the queue
            // being full: a frontend that drains a byte every 10 ms is slow and
            // stays connected, one that reads nothing is dropped in bounded
            // time with the reason and the depth of the backlog in the log.
            const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - lastProgress);
            if (since > kWriteDeadline) {
                std::fprintf(stderr,
                             "openshow-worker: frontend has not taken a byte of the %zu KiB "
                             "owed to it for %lld ms: it is gone whether or not its socket "
                             "says so\n",
                             outbound.bytes() / 1024,
                             static_cast<long long>(since.count()));
                return;
            }
        }
        if (ready == 0) {
            continue;
        }
        if ((pending.revents & (POLLIN | POLLRDHUP | POLLHUP | POLLERR | POLLNVAL)) == 0) {
            continue;  // nothing but POLLOUT, and that has been handled above
        }
        const ssize_t count = ::recv(connectionFd, buffer, sizeof(buffer), 0);
        if (count == 0) {
            return;  // orderly disconnect — also the answer to POLLRDHUP/POLLHUP
        }
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;  // woken for something else, or drained by the same read
            }
            return;  // reset or shutdown() from stop()
        }

        decoder.feed(std::string_view(buffer, static_cast<std::size_t>(count)));
        boost::json::value message;
        std::string decodeError;
        for (;;) {
            if (!decoder.next(&message, &decodeError)) {
                break;
            }
            std::string reply;
            const bool served = handle(message, handshakeDone, &handshakeDone, &reply);
            if (!reply.empty() && !outbound.push(std::string(), std::move(reply))) {
                // A reply this server will not buffer means the frontend has
                // stopped reading hard enough that the queue is at its budget.
                // The command it answers is not going to be answered, and the
                // only honest thing left is to say so in the log and hang up —
                // not to grow, and not to sit on it.
                std::fprintf(stderr,
                             "openshow-worker: the queue for the frontend is at its %zu KiB "
                             "budget and a reply would not fit: dropping it\n",
                             kMaxQueuedBytes / 1024);
                return;
            }
            if (!served) {
                // The stream is no longer interpretable, but the refusal that
                // explains it is worth one last attempt: write what is queued
                // and then hang up.
                drainToSocket(connectionFd, &outbound, &lastProgress);
                std::fprintf(stderr,
                             "openshow-worker: dropping connection after a message "
                             "that was not JSON-RPC 2.0\n");
                return;
            }
        }
        // Best-effort drain as soon as the commands of this read are handled,
        // rather than waiting for the next tick's POLLOUT: a `shutdown` reply
        // has to reach the frontend before the process is gone, and a reading
        // peer should see its answer with this read, not one poll later.
        if (!drainToSocket(connectionFd, &outbound, &lastProgress)) {
            return;  // the frontend took what it was owed and then went away
        }
        if (decoder.failed()) {
            std::fprintf(stderr, "openshow-worker: framing error: %s\n",
                         decodeError.c_str());
            return;
        }
    }
    if (!outbound.empty()) {
        std::fprintf(stderr,
                     "openshow-worker: stopping with %zu frame(s), %zu KiB still owed to the "
                     "frontend (%zu notification(s) coalesced, %zu dropped)\n",
                     outbound.frames(), outbound.bytes() / 1024, outbound.coalesced,
                     outbound.droppedNotifications);
    }
}

bool RpcServer::handle(const boost::json::value& message, bool handshakeDone,
                       bool* handshakeNow, std::string* outbound) {
    // One gate for the envelope (task 6.8a, review R11): version stamp, a
    // usable method name, and an id the reply can honestly carry. Every
    // handler below therefore gets a method it can index on and an id it does
    // not have to second-guess.
    proto::RequestEnvelope envelope;
    proto::ErrorCode code = proto::kInternalError;
    std::string text;
    bool dropConnection = false;
    if (!proto::parseRequestEnvelope(message, &envelope, &code, &text, &dropConnection)) {
        proto::appendFrame(proto::makeError(envelope.id, code, text), outbound);
        // A stream that lost the version stamp is not worth serving; the caller
        // logs it and hangs up. Everything else was answered above and stays
        // connected: the operator's paste of large text must not cost them the
        // worker (review R11).
        return !dropConnection;
    }

    // A notification — no id — may not be answered, not even with an error.
    // preview.released is the one method this protocol documents as sending
    // one; anything else arrives ignored, loudly in the log and quietly on
    // the wire, because executing a command that can never be acknowledged is
    // the lost-update shape task 6.3a closed: the operator would see no
    // revision, no confirmation, and an output that had in fact moved.
    const bool notification = !envelope.hasId;
    if (notification && !proto::isNotificationMethod(envelope.method)) {
        std::fprintf(stderr,
                     "openshow-worker: ignored '%s' sent without an id: only %s may be a "
                     "notification, and nothing answers it either way\n",
                     std::string(envelope.method).c_str(), proto::kMethodPreviewReleased);
        return true;
    }

    const std::string_view name = envelope.method;
    const boost::json::value nullParams = nullptr;
    const boost::json::value& given =
        envelope.params == nullptr ? nullParams : *envelope.params;
    const std::int64_t id = envelope.id;

    if (name == "hello") {
        boost::json::object result;
        proto::ErrorCode helloCode = proto::kInternalError;
        std::string helloText;
        boost::json::value data;
        if (proto::handleHello(given, fOptions.workerVersion, &result, &helloCode, &helloText,
                               &data)) {
            *handshakeNow = true;
            proto::appendFrame(proto::makeResult(id, result), outbound);
        } else {
            proto::appendFrame(proto::makeError(id, helloCode, helloText, data), outbound);
        }
        return true;
    }

    // Everything else requires an agreed protocol version first.
    if (!handshakeDone) {
        proto::appendFrame(proto::makeError(id, proto::kNotHandshaked,
                                            "say hello before using the worker"),
                           outbound);
        return true;
    }

    if (name == "ping") {
        proto::appendFrame(
            proto::makeResult(id, boost::json::object{{"pong", true}}), outbound);
    } else if (name == "output.status") {
        proto::appendFrame(
            proto::makeResult(id, statusResult(fSession.status())), outbound);
    } else if (name == "output.displays") {
        proto::appendFrame(
            proto::makeResult(id, displaysResult(fSession.displays())), outbound);
    } else if (name == "output.select") {
        if (!given.is_object()) {
            proto::appendFrame(proto::makeError(id, proto::kInvalidParams,
                                                "output.select requires params"),
                               outbound);
            return true;
        }
        // The range is checked here, at the wire, and NOT by the cast below:
        // `static_cast<int>(4294967296)` is 0, which is a real destination, so
        // an argument nobody sent would move the audience's output (review
        // R11). -1 is windowed; the top of the range is what an `int` index
        // can name at all, and the session still decides whether a panel
        // exists.
        std::int64_t index = 0;
        if (!jsonIntInRange(given.as_object(), "monitor", -1,
                            std::numeric_limits<int>::max(), &index)) {
            proto::appendFrame(
                proto::makeError(id, proto::kInvalidParams,
                                 "output.select requires \"monitor\": -1 (windowed) "
                                 "or an index from output.displays"),
                outbound);
            return true;
        }
        std::uint64_t revision = 0;
        if (!fSession.selectMonitor(static_cast<int>(index), &revision)) {
            proto::appendFrame(
                proto::makeError(id, proto::kInvalidParams,
                                 "output.select requires \"monitor\": -1 (windowed) "
                                 "or an index from output.displays"),
                outbound);
            return true;
        }
        // The move happens on the render thread's next frame; revision
        // tells the frontend which status confirms arrival.
        proto::appendFrame(proto::makeResult(id, boost::json::object{
                                                     {"revision", revision},
                                                     {"pending", true}}),
                           outbound);
    } else if (name == "layer.setEnabled") {
        if (!given.is_object()) {
            proto::appendFrame(proto::makeError(id, proto::kInvalidParams,
                                                "layer.setEnabled requires params"),
                               outbound);
            return true;
        }
        const boost::json::object& args = given.as_object();
        const auto layer = args.find("layer");
        const auto enabled = args.find("enabled");
        if (layer == args.end() || !layer->value().is_string() ||
            enabled == args.end() || !enabled->value().is_bool()) {
            proto::appendFrame(proto::makeError(id, proto::kInvalidParams,
                                                "layer.setEnabled requires a layer name "
                                                "and an enabled boolean"),
                               outbound);
            return true;
        }
        std::uint64_t revision = 0;
        if (!fSession.setLayerEnabled(layer->value().as_string(),
                                      enabled->value().as_bool(), &revision)) {
            proto::appendFrame(proto::makeError(id, proto::kInvalidParams,
                                                "unknown layer '" +
                                                    proto::clippedForEcho(
                                                        layer->value().as_string()) +
                                                    "'"),
                               outbound);
            return true;
        }
        // The toggle lands on the next composed frame; revision tells the
        // frontend which status confirms it.
        proto::appendFrame(proto::makeResult(id, boost::json::object{
                                                     {"revision", revision},
                                                     {"pending", true}}),
                           outbound);
    } else if (name == "scene.prepare") {
        // The compiled scene travels as JSON (the frontend's compiler
        // produced it); validate it into the session's scene type before
        // storing. Nothing renders here — prepare only grows the store.
        if (!given.is_object()) {
            proto::appendFrame(proto::makeError(id, proto::kInvalidParams,
                                                "scene.prepare requires a scene object"),
                               outbound);
            return true;
        }
        openshow::scene::Scene scene;
        std::string sceneError;
        if (!openshow::scene::fromJson(given, &scene, &sceneError)) {
            proto::appendFrame(
                proto::makeError(id, proto::kInvalidParams,
                                 sceneError.empty() ? "scene.prepare received an invalid scene"
                                                    : sceneError),
                outbound);
            return true;
        }
        std::uint64_t revision = 0;
        if (!fSession.prepareScene(scene, &revision)) {
            proto::appendFrame(
                proto::makeError(id, proto::kInternalError, "scene.prepare failed"),
                outbound);
            return true;
        }
        // The store revision names this scene for scene.present.
        proto::appendFrame(
            proto::makeResult(id, boost::json::object{{"revision", revision}}), outbound);
    } else if (name == "scene.present") {
        if (!given.is_object()) {
            proto::appendFrame(proto::makeError(id, proto::kInvalidParams,
                                                "scene.present requires params"),
                               outbound);
            return true;
        }
        std::uint64_t prepared = 0;
        if (!jsonUint64(given.as_object(), "revision", &prepared)) {
            proto::appendFrame(proto::makeError(id, proto::kInvalidParams,
                                                "scene.present requires a \"revision\""),
                               outbound);
            return true;
        }
        // Optional transition (task 3.2): "fade" (the default) or "cut".
        SceneTransitionKind kind = SceneTransitionKind::kFade;
        const boost::json::object& args = given.as_object();
        if (const auto it = args.find("transition"); it != args.end()) {
            if (!it->value().is_string()) {
                proto::appendFrame(proto::makeError(id, proto::kInvalidParams,
                                                    "scene.present \"transition\" must be "
                                                    "a string"),
                                   outbound);
                return true;
            }
            const std::string_view transition = it->value().as_string();
            if (transition == "fade") {
                kind = SceneTransitionKind::kFade;
            } else if (transition == "cut") {
                kind = SceneTransitionKind::kCut;
            } else {
                proto::appendFrame(proto::makeError(id, proto::kInvalidParams,
                                                    "scene.present \"transition\" must be "
                                                    "\"fade\" or \"cut\""),
                                   outbound);
                return true;
            }
        }
        std::uint64_t revision = 0;
        if (!fSession.presentScene(prepared, &revision, kind)) {
            proto::appendFrame(
                proto::makeError(id, proto::kInvalidParams,
                                 "no prepared scene with revision " +
                                     std::to_string(prepared)),
                outbound);
            return true;
        }
        proto::appendFrame(proto::makeResult(id, boost::json::object{
                                                     {"revision", revision},
                                                     {"pending", true}}),
                           outbound);
    } else if (name == "preview.start") {
        // Each argument is optional, but a PRESENT argument must be a real
        // value: silently defaulting a "width" of "wide" (or letting 2^32+640
        // truncate back to 640) makes the client's request succeed with
        // geometry nobody asked for. The session then decides the sane band.
        uint32_t width = 640;
        uint32_t height = 360;
        uint32_t fps = 15;
        if (given.is_object()) {
            const boost::json::object& args = given.as_object();
            const struct {
                const char* key;
                uint32_t* slot;
            } numbers[] = {{"width", &width}, {"height", &height}, {"fps", &fps}};
            for (const auto& argument : numbers) {
                if (args.find(argument.key) == args.end()) {
                    continue;  // absent: the default above stands
                }
                uint32_t value = 0;
                if (!jsonUint32(args, argument.key, &value)) {
                    proto::appendFrame(
                        proto::makeError(id, proto::kInvalidParams,
                                         std::string("preview.start \"") + argument.key +
                                             "\" wants a whole number between 0 and " +
                                             std::to_string(4294967295u)),
                        outbound);
                    return true;
                }
                *argument.slot = value;
            }
        }
        OutputSession::PreviewInfo info;
        std::string error;
        if (!fSession.startPreview(width, height, fps, &info, &error)) {
            proto::appendFrame(
                proto::makeError(id, proto::kInvalidParams, error), outbound);
            return true;
        }
        // Pixels never ride the socket: this names the shm segment, the
        // consumer maps it and waits for preview.ready notifications.
        proto::appendFrame(proto::makeResult(id, boost::json::object{
                                                     {"name", info.name},
                                                     {"width", info.width},
                                                     {"height", info.height},
                                                     {"slotCount", info.slotCount},
                                                     {"bytesPerSlot", info.bytesPerSlot},
                                                     {"format", "RGBA8888"}}),
                           outbound);
    } else if (name == "preview.stop") {
        fSession.stopPreview();
        proto::appendFrame(proto::makeResult(id, boost::json::object{{"ok", true}}),
                           outbound);
    } else if (name == proto::kMethodPreviewReleased) {
        // A notification from the consumer: it finished copying that slot
        // frame, so the producer may claim it again. Answered only if the
        // client bothered to identify it. The stream name is part of the
        // proof of ownership (task 6.5, review R5): a release that does
        // not name the live stream frees nothing.
        if (given.is_object()) {
            const boost::json::object& args = given.as_object();
            std::uint64_t seq = 0;
            std::uint32_t slot = 0;
            const auto stream = args.find("stream");
            // The slot is a 32-bit index and the sequence is not, so each is
            // read in its own width — a slot of 2^32+1 must not arrive as
            // slot 1 and release a frame the consumer never copied. The name
            // is bounded for the same reason R5 made it part of the proof:
            // it is compared, not stored, and an over-long one is not a
            // segment name by any construction the producer uses.
            const bool named = stream != args.end() && stream->value().is_string() &&
                               stream->value().as_string().size() <= 256;
            if (jsonUint32(args, "slot", &slot) && jsonUint64(args, "seq", &seq) && named) {
                fSession.releasePreviewSlot(slot, seq,
                                            std::string(stream->value().as_string()));
            }
        }
        if (envelope.hasId) {
            proto::appendFrame(proto::makeResult(id, boost::json::object{{"ok", true}}),
                               outbound);
        }
    } else if (name == "shutdown") {
        proto::appendFrame(proto::makeResult(id, boost::json::object{{"ok", true}}),
                           outbound);
        fSession.requestStop();
    } else {
        proto::appendFrame(
            proto::makeError(id, proto::kMethodNotFound,
                             "no such method: " + std::string(name)),
            outbound);
    }
    return true;
}

void RpcServer::collectNotifications(std::vector<QueuedFrame>* outbound) {
    OutputSession::PreviewReady ready;
    OutputSession::MediaError mediaError;
    // One frame per slot that has something to say, keyed by slot: three slots,
    // three frames, however many publishes piled up while the frontend was not
    // listening. The consumer copies the newest content of a slot, so a frame
    // that has since been superseded is not information, it is backlog.
    while (fSession.takePreviewReady(&ready)) {
        boost::json::object params{{"slot", ready.slot},
                                   {"seq", ready.seq},
                                   {"width", ready.width},
                                   {"height", ready.height},
                                   {"rowBytes", ready.rowBytes},
                                   {"revision", ready.revision},
                                   {"publishedNs", ready.publishedNs},
                                   // Stream identity: the release
                                   // must echo it (task 6.5).
                                   {"stream", ready.stream}};
        std::string frame;
        if (proto::appendFrame(
                proto::makeNotification(proto::kMethodPreviewReady, std::move(params)),
                &frame)) {
            outbound->push_back(
                QueuedFrame{"preview.ready:" + std::to_string(ready.slot), std::move(frame)});
        }
    }
    // Background media failure (task 5.5d): immediate visibility, the
    // same text output.status.media.error carries until it is replaced.
    while (fSession.takeMediaError(&mediaError)) {
        std::string frame;
        if (proto::appendFrame(
                proto::makeNotification(proto::kMethodMediaError,
                                        boost::json::object{{"path", mediaError.path},
                                                            {"message", mediaError.message}}),
                &frame)) {
            outbound->push_back(QueuedFrame{"media.error", std::move(frame)});
        }
    }
}

}  // namespace openshow::worker
