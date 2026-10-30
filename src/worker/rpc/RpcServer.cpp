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

// A ceiling on a seek target, in seconds (task 6.15a): a bound on
// nonsense, not a promise about durations — no service runs 24 hours of
// background video, and a target past any real clip ends the same way a
// target at its length does.
constexpr double kMaxSeekSeconds = 86400.0;

// Reads a JSON number as a double within [low, high] (task 6.15a): seek
// targets and gain are fractional, and every number kind Boost.JSON
// stores — int64, uint64, double — is a legitimate spelling of "1" or
// "0.5". NaN and both infinities are refused by the range test after a
// finite-only accept, because comparisons lie about NaN: an out-of-range
// check written alone would wave it through.
bool jsonDoubleInRange(const boost::json::object& args, const char* key, double low,
                       double high, double* out) {
    const auto entry = args.find(key);
    if (entry == args.end()) {
        return false;
    }
    double value = 0.0;
    if (entry->value().is_double()) {
        value = entry->value().as_double();
    } else if (entry->value().is_int64()) {
        value = static_cast<double>(entry->value().as_int64());
    } else if (entry->value().is_uint64()) {
        value = static_cast<double>(entry->value().as_uint64());
    } else {
        return false;
    }
    if (!(value >= low) || !(value <= high)) {  // false for NaN on either leg
        return false;
    }
    *out = value;
    return true;
}

boost::json::value layersObject(uint32_t mask) {
    boost::json::object layers;
    for (uint32_t i = 0; i < static_cast<uint32_t>(LayerId::kCount); ++i) {
        const LayerId id = static_cast<LayerId>(i);
        layers[std::string(layerName(id))] = (mask & layerBit(id)) != 0;
    }
    return layers;
}

// One entry per registered audio source (task 6.15a, review R15). An
// empty list honestly means "no audio identity is live", which is also
// the state of a clip with no decodable stream.
boost::json::array sourcesArray(const OutputSession::MediaStatus& media) {
    boost::json::array sources;
    for (const OutputSession::MediaStatus::Source& source : media.sources) {
        sources.push_back(boost::json::object{{"name", source.name},
                                              {"gain", source.gain},
                                              {"muted", source.muted}});
    }
    return sources;
}

boost::json::object standaloneStatus(const OutputSession::MediaStatus::Standalone& standalone) {
    // The track's own block (task 6.15b): its lifetime is not the
    // background's, so its report is not the background's either. The
    // 3.4 rule as usual — media.standalone answers what the selection
    // became.
    return boost::json::object{{"live", standalone.live},
                               {"path", standalone.path},
                               {"playing", standalone.playing},
                               {"position", standalone.position},
                               {"duration", standalone.duration},
                               {"error", standalone.error},
                               {"audio", standalone.audio},
                               {"loops", standalone.loops}};
}

boost::json::object statusResult(const OutputSession::Status& status) {
    const OutputSession::PreviewStatus& preview = status.preview;
    // The editor's off-air target of the same stream (task 6.16a, review
    // R16). `active` is the accept of a preview.select; revision/request name
    // what it accepted. Then the same three questions the mirror answers —
    // published counts frames that reached the socket, dropped the ones the
    // consumer's slots blocked, deferred the ones that stood down for the
    // audience — plus the one only a generation-managed target has: stale,
    // the frames a newer ask superseded before they could be published.
    const boost::json::object selection{
        {"active", preview.selection.active},
        {"revision", preview.selection.revision},
        {"request", preview.selection.request},
        {"published", preview.selection.published},
        {"dropped", preview.selection.dropped},
        {"deferred", preview.selection.deferred},
        {"stale", preview.selection.stale}};
    // The slide list's target (task 6.16b, review R16). Same questions as
    // the other two, answered for a BATCH: queued is what the worker still
    // owes a picture of and decodePending the part of it waiting on a codec
    // on the command thread, which is the difference between "working through
    // the list" and "idle with a list it will never draw". `images` is the
    // list's own decoded-picture cache: its budget differs from
    // status.scene.images' by design, and that is the evidence on the wire
    // that a scrolling document costs a bounded, SEPARATE pool.
    const boost::json::object thumbnails{
        {"active", preview.thumbnails.active},
        {"request", preview.thumbnails.request},
        {"width", preview.thumbnails.width},
        {"height", preview.thumbnails.height},
        {"queued", preview.thumbnails.queued},
        {"decodePending", preview.thumbnails.decodePending},
        {"published", preview.thumbnails.published},
        {"dropped", preview.thumbnails.dropped},
        {"deferred", preview.thumbnails.deferred},
        {"stale", preview.thumbnails.stale},
        {"images", boost::json::object{{"decodedBytes", preview.thumbnails.decodedBytes},
                                      {"entries", preview.thumbnails.entries},
                                      {"evictions", preview.thumbnails.evictions},
                                      {"budget", preview.thumbnails.budget}}}};
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
                                                               // Task 6.11a (review R13): what the
                                                               // preview costs the output, and how
                                                               // often it stood down for the
                                                               // audience's sake.
                                                               {"deferred", preview.deferred},
                                                               {"worstMs", preview.worstMs},
                                                               {"selection", selection},
                                                               {"thumbnail", thumbnails},
                                                               {"error", preview.error}}},
                               {"scene", boost::json::object{{"active", status.scene.active},
                                                             {"pending", status.scene.pending},
                                                             {"transitioning",
                                                              status.scene.transitioning},
                                                             {"prepared", status.scene.prepared},
                                                             {"preparing", status.scene.preparing},
                                                             // The decoded-still cache (task 6.10,
                                                             // review R12): what the worker holds
                                                             // in CPU picture memory, what of it
                                                             // is pinned content, and whether the
                                                             // budget has begun evicting.
                                                             {"images",
                                                              boost::json::object{
                                                                  {"decodedBytes",
                                                                   status.scene.images.decodedBytes},
                                                                  {"entries",
                                                                   status.scene.images.entries},
                                                                  {"pins", status.scene.images.pins},
                                                                  {"evictions",
                                                                   status.scene.images.evictions},
                                                                  // preloadHits (task 6.9a): the
                                                                  // worker's own word that a
                                                                  // re-prepare found the still
                                                                  // already decoded, which no ack
                                                                  // latency on the wire can say.
                                                                  {"preloadHits",
                                                                   status.scene.images
                                                                       .preloadHits},
                                                                  // The draw-boundary trio
                                                                  // (task 6.9b, review R10):
                                                                  // what the frame loop still
                                                                  // had to build for itself.
                                                                  {"drawDecodes",
                                                                   status.scene.images
                                                                       .drawDecodes},
                                                                  {"drawBackdrops",
                                                                   status.scene.images
                                                                       .drawBackdrops},
                                                                  {"layoutBuilds",
                                                                   status.scene.images
                                                                       .layoutBuilds}}}}},
                               // Background media (task 5.5d): the video
                               // transport's live state; error is sticky
                               // until a successful open replaces it.
                               // "audio" (task 6.7c, review R18) is the
                               // typed audible lane: none | silent |
                               // no-output | degraded | paused | live.
                               // loop/duration/sources (task 6.15a,
                               // review R15) acknowledge the transport
                               // commands: the rewind intent EOF will
                               // honour, the clip's probed length (-1 =
                               // no transport or unknown), and every
                               // REGISTERED audio source with its mixer
                               // echo — what status says is what the
                               // audience has.
                               {"media", boost::json::object{{"playing", status.media.playing},
                                                             {"path", status.media.path},
                                                             {"error", status.media.error},
                                                             {"audio", status.media.audio},
                                                             {"position", status.media.position},
                                                             {"live", status.media.live},
                                                             {"loop", status.media.loop},
                                                             {"duration", status.media.duration},
                                                             {"sources", sourcesArray(status.media)},
                                                             {"standalone",
                                                              standaloneStatus(status.media.standalone)}}}};
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
    // The deferred-reply self-wake (task 6.9a). Nonblocking so a full
    // pipe is a no-op rather than a parked executor thread, and
    // close-on-exec so a forked helper never inherits the socket's wake.
    if (::pipe2(fWakeFds, O_CLOEXEC | O_NONBLOCK) != 0) {
        if (error) {
            *error = "could not create the deferred-reply wake pipe";
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
    // The wake pipe goes last and under the deferred lock: an executor
    // completion that lands after the join writes whatever number the
    // members now hold — a live fd nobody reads (harmless, the frames go
    // nowhere) or -1 (an EBADF nobody sees) — but never a recycled fd
    // number, which is the race the lock actually buys (task 6.9a).
    {
        std::lock_guard<std::mutex> lock(fDeferredMutex);
        for (int& fd : fWakeFds) {
            if (fd >= 0) {
                close(fd);
                fd = -1;
            }
        }
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
        // The connection and the self-wake share the poll (task 6.9a): a
        // deferred reply arriving from the executor thread must not sit in
        // the deferred bag until the tick expires, because the tick is 50 ms
        // of the FRONTEND'S clock — see fWakeFds.
        pollfd watched[2] = {
            pollfd{connectionFd, POLLIN | POLLRDHUP, 0},
            pollfd{fWakeFds[0], POLLIN, 0},
        };
        if (!outbound.empty()) {
            watched[0].events |= POLLOUT;
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
        const int ready = ::poll(watched, 2, timeoutMs);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        if (watched[1].revents & POLLIN) {
            // Empty the self-wake: it is level-triggered, and a byte left
            // behind is a poll that never sleeps again. Whatever the byte
            // was waking the loop for is collected below this iteration.
            char drain[64];
            while (::read(fWakeFds[0], drain, sizeof(drain)) > 0) {
            }
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
        //
        // One thumbnail background per tick (task 6.16b, review R16). The
        // rule that keeps a frame clean is that a draw never opens a file
        // (task 6.10), so the pictures a slide list needs are decoded HERE,
        // on the thread with nothing frame-shaped to protect — and ONE per
        // tick, because this is also the thread that answers the operator's
        // present. A band of thirty backdrops decodes over thirty ticks: the
        // list fills in over a second and a half while every command is
        // answered inside the tick it arrived in. With no batch queued this
        // returns after one locked check of an empty vector.
        fSession.pumpThumbnailDecode();
        std::vector<QueuedFrame> notifications;
        collectNotifications(&notifications);
        collectDeferred(&notifications);  // prepare acks from the executor thread (6.9a)
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
        if ((watched[0].revents & (POLLIN | POLLRDHUP | POLLHUP | POLLERR | POLLNVAL)) == 0) {
            continue;  // nothing but POLLOUT (and the wake, already drained),
                       // and both have been handled above
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
        // Task 6.9a (review R10): the store answer is immediate, the file
        // work is not. The revision is registered now (the pins too); the
        // ack itself travels when the executor finishes this prepare's
        // decodes and probe, so the loop goes on answering status, clear,
        // releases and shutdown while a cold file is being read. A refusal
        // to schedule (closing, or kMaxQueued prepares already waiting) is
        // answered here, explicitly.
        std::uint64_t revision = 0;
        const bool scheduled = fSession.prepareScene(
            scene, static_cast<std::uint64_t>(id),
            [this, id](std::uint64_t readyRevision, bool cancelled) {
                boost::json::value frame =
                    cancelled
                        ? proto::makeError(id, proto::kInternalError,
                                           "scene.prepare was retired before its file work ran")
                        : proto::makeResult(id, boost::json::object{
                                                    {"revision", readyRevision}});
                std::lock_guard<std::mutex> lock(fDeferredMutex);
                fDeferredFrames.push_back(std::move(frame));
                // The byte that shortens the serve loop's poll. A full pipe
                // means a wake is ALREADY pending, so EAGAIN is success too;
                // the fd is read under this same lock by stop(), which is
                // what keeps a late completion off a recycled descriptor.
                if (fWakeFds[1] >= 0) {
                    const char signal = 1;
                    (void)::write(fWakeFds[1], &signal, 1);
                }
            },
            &revision);
        if (!scheduled) {
            proto::appendFrame(
                proto::makeError(id, proto::kInternalError,
                                 "scene.prepare could not schedule its file work"),
                outbound);
            return true;
        }
        // The ack answers from the executor thread (or, with no background
        // media, fired inline before this returned): nothing more to write
        // for this request on this thread.
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
    } else if (name == proto::kMethodPreviewSelect) {
        // The editor's off-air render (task 6.16a, review R16): "draw THIS
        // prepared scene into the preview stream without presenting it".
        // Both arguments are required, in their own widths, for the usual
        // reason — a revision that silently truncated to 0 would read as a
        // stand-down, and a request that wrapped into a smaller number would
        // be refused as stale by a client that believes it advanced.
        if (!given.is_object()) {
            proto::appendFrame(proto::makeError(id, proto::kInvalidParams,
                                                "preview.select wants {revision, request}"),
                               outbound);
            return true;
        }
        const boost::json::object& args = given.as_object();
        std::uint64_t revision = 0;
        std::uint32_t request = 0;
        if (!jsonUint64(args, "revision", &revision) || !jsonUint32(args, "request", &request)) {
            proto::appendFrame(
                proto::makeError(id, proto::kInvalidParams,
                                 "preview.select wants a whole-number revision and a "
                                 "request between 0 and 4294967295"),
                outbound);
            return true;
        }
        std::string error;
        if (!fSession.selectPreviewScene(revision, request, &error)) {
            // The refusal names what the client can act on: no stream, a
            // generation that does not follow the accepted one (both numbers
            // in the text), or a revision the bounded store no longer holds.
            proto::appendFrame(
                proto::makeError(id, proto::kInvalidParams, error), outbound);
            return true;
        }
        // The answer is the accept, not the picture: whether a frame landed
        // is status.preview.selection.published's report, because the render
        // thread owns when it draws and this socket must not wait on it.
        proto::appendFrame(proto::makeResult(id, boost::json::object{
                                                     {"accepted", true},
                                                     {"revision", revision},
                                                     {"request", request}}),
                           outbound);
    } else if (name == proto::kMethodPreviewThumbnails) {
        // The slide list (task 6.16b, review R16): "picture these scenes at
        // this size, off the record". These scenes are NOT prepared — they
        // enter no store, pin no full-size decode and hold no revision anyone
        // can present — so this handler is the only place their cost is paid,
        // and the bounds below are what keep that cost proportionate to a
        // list of rows rather than to whatever a buggy client can send.
        if (!given.is_object()) {
            proto::appendFrame(
                proto::makeError(id, proto::kInvalidParams,
                                 "preview.thumbnails wants {scenes, width, height, request}"),
                outbound);
            return true;
        }
        const boost::json::object& args = given.as_object();
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t request = 0;
        if (!jsonUint32(args, "width", &width) || !jsonUint32(args, "height", &height) ||
            !jsonUint32(args, "request", &request)) {
            proto::appendFrame(
                proto::makeError(id, proto::kInvalidParams,
                                 "preview.thumbnails wants width, height and request as "
                                 "whole numbers between 0 and 4294967295"),
                outbound);
            return true;
        }
        const auto scenesEntry = args.find("scenes");
        if (scenesEntry == args.end() || !scenesEntry->value().is_array()) {
            proto::appendFrame(
                proto::makeError(id, proto::kInvalidParams,
                                 "preview.thumbnails wants a \"scenes\" array (an empty "
                                 "one stands the list down)"),
                outbound);
            return true;
        }
        const boost::json::array& scenes = scenesEntry->value().as_array();
        // THE CAP, BEFORE ANYTHING IS PARSED. Three hundred scenes would be
        // three hundred scene objects compiled before the worker said no;
        // judging the count first makes the refusal cost one comparison,
        // which is the only way a cap bounds the work a request can cause
        // rather than the work it succeeds at.
        if (scenes.size() > proto::kMaxThumbnailBatch) {
            proto::appendFrame(
                proto::makeError(id, proto::kInvalidParams,
                                 "preview.thumbnails takes at most " +
                                     std::to_string(proto::kMaxThumbnailBatch) +
                                     " scenes, asked for " + std::to_string(scenes.size()) +
                                     " (request the visible band)"),
                outbound);
            return true;
        }
        // Per-entry parsing, per-entry refusal: a slide whose JSON does not
        // compile loses its own picture, and the rest of the band still gets
        // its. A key over the limit is refused rather than clipped INTO the
        // identity — an accepted entry always carries the exact key it was
        // sent — and any key that is echoed back on the wire is clipped
        // first, R11's rule about putting a client's text in a reply.
        std::vector<OutputSession::ThumbnailEntry> batch;
        batch.reserve(scenes.size());
        std::vector<std::pair<std::string, std::string>> refused;
        for (const boost::json::value& item : scenes) {
            if (!item.is_object()) {
                refused.emplace_back(std::string(), "entry is not an object");
                continue;
            }
            const boost::json::object& entry = item.as_object();
            const auto key = entry.find("key");
            if (key == entry.end() || !key->value().is_string() ||
                key->value().as_string().empty()) {
                refused.emplace_back(std::string(), "entry has no \"key\"");
                continue;
            }
            const std::string_view rawKey = key->value().as_string();
            if (rawKey.size() > proto::kMaxThumbnailKeyLength) {
                refused.emplace_back(proto::clippedForEcho(rawKey),
                                     "key is over the " +
                                         std::to_string(proto::kMaxThumbnailKeyLength) +
                                         "-character limit");
                continue;
            }
            const auto sceneEntry = entry.find("scene");
            if (sceneEntry == entry.end() || !sceneEntry->value().is_object()) {
                refused.emplace_back(std::string(rawKey), "entry has no \"scene\" object");
                continue;
            }
            openshow::scene::Scene compiled;
            std::string sceneError;
            if (!openshow::scene::fromJson(sceneEntry->value(), &compiled, &sceneError)) {
                refused.emplace_back(std::string(rawKey),
                                     sceneError.empty() ? "not a compiled scene" : sceneError);
                continue;
            }
            OutputSession::ThumbnailEntry job;
            job.key = std::string(rawKey);
            job.scene = std::move(compiled);
            batch.push_back(std::move(job));
        }
        OutputSession::ThumbnailAck ack;
        std::string error;
        // Everything the session judges — no stream, an extent that could not
        // fit a slot, a replayed generation — refuses the WHOLE batch, and it
        // refuses it without having touched the pending list, so a refusal
        // leaves the previous band exactly as it stood.
        if (!fSession.requestThumbnails(batch, width, height, request, &ack, &error)) {
            proto::appendFrame(proto::makeError(id, proto::kInvalidParams, error), outbound);
            return true;
        }
        // The accept with the id each slide's frames will carry. Pixels are
        // not here and never will be: they arrive as preview.ready frames of
        // target "thumbnail" whose `revision` is this id.
        boost::json::array frames;
        for (const std::pair<std::string, std::uint32_t>& accepted : ack.accepted) {
            frames.push_back(
                boost::json::object{{"key", accepted.first}, {"id", accepted.second}});
        }
        boost::json::array refusals;
        for (const std::pair<std::string, std::string>& why : ack.refused) {
            refused.push_back(why);
        }
        for (const std::pair<std::string, std::string>& why : refused) {
            refusals.push_back(boost::json::object{{"key", why.first}, {"why", why.second}});
        }
        proto::appendFrame(proto::makeResult(id, boost::json::object{
                                                     {"accepted", true},
                                                     {"request", ack.request},
                                                     {"width", ack.width},
                                                     {"height", ack.height},
                                                     {"frames", std::move(frames)},
                                                     {"refused", std::move(refusals)}}),
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
    } else if (name == proto::kMethodMediaPlay || name == proto::kMethodMediaPause) {
        // The media transport commands (task 6.15a, review R15): what the
        // benchmark's AvPlayer could already do, now answerable over the
        // socket. The shape is the session's existing one — the command
        // hands an intent to the transport's thread and answers
        // {"pending":true}; output.status.media is the acknowledgement
        // (playing flips, a held position holds), exactly how scenes and
        // layers confirm. Commands with no transport are refused NOW,
        // naming the reason, rather than dying unheard on a render thread
        // that was never asked.
        if (!fSession.mediaTransportLive()) {
            proto::appendFrame(proto::makeError(id, proto::kInvalidParams,
                                                "no background media is playing to " +
                                                    std::string(name)),
                               outbound);
        } else {
            if (name == proto::kMethodMediaPlay) {
                fSession.requestMediaPlay();
            } else {
                fSession.requestMediaPause();
            }
            proto::appendFrame(
                proto::makeResult(id, boost::json::object{{"pending", true}}), outbound);
        }
    } else if (name == proto::kMethodMediaSeek) {
        // The position is validated before it is believed: NaN fails the
        // range legs, a string or bool is not a position, and 24 hours of
        // media is a ceiling no real clip tests (the number is a bound on
        // nonsense, not a promise about durations).
        double target = 0.0;
        if (!given.is_object() ||
            !jsonDoubleInRange(given.as_object(), "position", 0.0, kMaxSeekSeconds, &target)) {
            proto::appendFrame(proto::makeError(id, proto::kInvalidParams,
                                                "media.seek requires a numeric position "
                                                "within [0, 86400] seconds"),
                               outbound);
        } else if (!fSession.mediaTransportLive()) {
            proto::appendFrame(proto::makeError(id, proto::kInvalidParams,
                                                "no background media is playing to seek"),
                               outbound);
        } else {
            fSession.requestMediaSeek(target);
            proto::appendFrame(proto::makeResult(id, boost::json::object{{"pending", true},
                                                                         {"target", target}}),
                               outbound);
        }
    } else if (name == proto::kMethodMediaLoop) {
        if (!given.is_object()) {
            proto::appendFrame(proto::makeError(id, proto::kInvalidParams,
                                                "media.loop requires params"),
                               outbound);
        } else {
            const boost::json::object& args = given.as_object();
            const auto loop = args.find("loop");
            if (loop == args.end() || !loop->value().is_bool()) {
                proto::appendFrame(proto::makeError(id, proto::kInvalidParams,
                                                    "media.loop requires a loop boolean"),
                                   outbound);
            } else if (!fSession.mediaTransportLive()) {
                proto::appendFrame(proto::makeError(id, proto::kInvalidParams,
                                                    "no background media is playing to loop"),
                                   outbound);
            } else {
                const bool enabled = loop->value().as_bool();
                fSession.requestMediaLoop(enabled);
                // The EOF decision is the acknowledgement: with loop off
                // the clip ends once — status goes playing:false with no
                // fault and the position rests at the end while the
                // last-drawn frame holds on screen.
                proto::appendFrame(proto::makeResult(id, boost::json::object{{"pending", true},
                                                                             {"loop", enabled}}),
                                   outbound);
            }
        }
    } else if (name == proto::kMethodMediaGain || name == proto::kMethodMediaMute) {
        const bool isGain = name == proto::kMethodMediaGain;
        if (!given.is_object()) {
            proto::appendFrame(proto::makeError(id, proto::kInvalidParams,
                                                std::string(name) + " requires params"),
                               outbound);
        } else {
            const boost::json::object& args = given.as_object();
            const auto source = args.find("source");
            const std::string sourceName =
                source != args.end() && source->value().is_string()
                    ? std::string(source->value().as_string())
                    : std::string();
            openshow::worker::OutputSession::AudioSource identity =
                openshow::worker::OutputSession::AudioSource::kVideo;
            if (sourceName == proto::kMediaSourceVideo) {
                identity = openshow::worker::OutputSession::AudioSource::kVideo;
            } else if (sourceName == proto::kMediaSourceStandalone) {
                identity = openshow::worker::OutputSession::AudioSource::kStandalone;
            }
            double gain = 0.0;
            bool muted = false;
            bool valueOk = false;
            if (isGain) {
                valueOk = jsonDoubleInRange(args, "gain", 0.0, 1.0, &gain);
            } else {
                const auto mute = args.find("mute");
                valueOk = mute != args.end() && mute->value().is_bool();
                if (valueOk) {
                    muted = mute->value().as_bool();
                }
            }
            std::string refusal;
            if (sourceName != proto::kMediaSourceVideo &&
                sourceName != proto::kMediaSourceStandalone) {
                refusal = "unknown audio source '" + proto::clippedForEcho(sourceName) +
                          "'; expected 'video' or 'standalone'";
            } else if (!valueOk) {
                refusal = isGain ? "media.gain requires a gain within [0, 1]"
                                 : "media.mute requires a mute boolean";
            } else {
                const bool accepted =
                    isGain ? fSession.requestMediaGain(identity, gain, &refusal)
                           : fSession.requestMediaMute(identity, muted, &refusal);
                if (accepted) {
                    refusal.clear();
                }
            }
            if (!refusal.empty()) {
                // A source this transport never registered is a refusal
                // the caller hears — the no-op AvPlayer passthrough for
                // unregistered identities stays below, but the socket
                // never lets a command evaporate.
                proto::appendFrame(proto::makeError(id, proto::kInvalidParams, refusal),
                                   outbound);
            } else {
                boost::json::object ack{{"pending", true}, {"source", sourceName}};
                // Not one ternary: `isGain ? gain : muted` would quietly
                // promote bool to double and send 0/1 where the contract
                // says true/false.
                if (isGain) {
                    ack["gain"] = gain;
                } else {
                    ack["muted"] = muted;
                }
                proto::appendFrame(proto::makeResult(id, ack), outbound);
            }
        }
    } else if (name == proto::kMethodMediaStandalone) {
        // The independent production track (task 6.15b, review R15). The
        // path is a string, "" is the removal; the session PROBEs before
        // accepting, so "nothing readable at that path" is answered here
        // and now — the old track keeps playing and the caller hears why.
        // An accepted path swaps on the transport's thread, and status's
        // standalone block is the acknowledgement: live, path, playing,
        // position, the lane, and the track's own sticky error.
        if (!given.is_object()) {
            proto::appendFrame(proto::makeError(id, proto::kInvalidParams,
                                                "media.standalone requires params"),
                               outbound);
        } else {
            const boost::json::object& args = given.as_object();
            const auto path = args.find("path");
            if (path == args.end() || !path->value().is_string()) {
                proto::appendFrame(proto::makeError(id, proto::kInvalidParams,
                                                    "media.standalone requires a path string "
                                                    "(\"\" removes the track)"),
                                   outbound);
            } else {
                const std::string wanted(path->value().as_string());
                std::string refusal;
                if (!fSession.requestStandaloneAudio(wanted, &refusal)) {
                    proto::appendFrame(
                        proto::makeError(id, proto::kInvalidParams,
                                         refusal.empty() ? "standalone track refused" : refusal),
                        outbound);
                } else {
                    proto::appendFrame(proto::makeResult(id, boost::json::object{
                                                                 {"pending", true},
                                                                 {"path", wanted}}),
                                       outbound);
                }
            }
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
                                   // Whose picture this is and which ask it
                                   // answers (task 6.16a). The consumer needs
                                   // them BEFORE copying: one stream now
                                   // carries the audience's mirror and the
                                   // editor's off-air render, and a frame of
                                   // the wrong kind under the right handles
                                   // is precisely the stale imagery R16 was
                                   // about. They are the same values the slot
                                   // header carries, published under the same
                                   // seqlock, so metadata and pixels agree.
                                   {"target", ready.target},
                                   {"request", ready.request},
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

void RpcServer::collectDeferred(std::vector<QueuedFrame>* outbound) {
    // Task 6.9a: swaps the whole bag out under the lock so the executor
    // thread's next completion never waits on this one, and pushes each
    // frame with an EMPTY key: a deferred reply is a request answer, so
    // the queue's newest-wins rules must never coalesce or drop it.
    std::vector<boost::json::value> frames;
    {
        std::lock_guard<std::mutex> lock(fDeferredMutex);
        if (fDeferredFrames.empty()) {
            return;
        }
        frames.swap(fDeferredFrames);
    }
    for (boost::json::value& frame : frames) {
        std::string bytes;
        if (proto::appendFrame(std::move(frame), &bytes)) {
            outbound->push_back(QueuedFrame{std::string(), std::move(bytes)});
        }
    }
}

}  // namespace openshow::worker
