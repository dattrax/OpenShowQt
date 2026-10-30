/*
 * RpcServer implementation. Raw AF_UNIX stream socket, one client, poll()
 * loop with 50 ms ticks so stop() and requestStop() are noticed promptly.
 * All framing and JSON-RPC shape lives in openshow::protocol, and since
 * task 6.19a4 so does the meaning of a method: this file is the connection —
 * accept, framed read, the hello gate, the bounded queue and its write
 * deadline, the drain, the deferred-answer bag and the shutdown.
 */

#include "rpc/OutboundQueue.h"
#include "rpc/RpcMethods.h"
#include "rpc/RpcServer.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "openshow/protocol/protocol.h"

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

}  // namespace

RpcServer::RpcServer(OutputSession& session, Options options)
    : fSession(session), fOptions(std::move(options)) {
    // The meaning of a request lives in the table (task 6.19a4); the one
    // thing it needs back from the connection is where a deferred answer
    // goes, and that arrives through this sink alone.
    fMethods = std::make_unique<MethodTable>(
        session, [this](boost::json::value frame) { submitDeferred(std::move(frame)); });
}

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

    // Everything the agreed protocol names is the table's: argument decode,
    // per-method refusal, result encode and the session call (task 6.19a4).
    // The connection keeps only what it must keep — the envelope, the
    // notification policy, the hello gate above — because it is the only
    // thing allowed to end the session, and the table never will.
    fMethods->dispatch(envelope, outbound);
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
        std::string frame;
        if (proto::appendFrame(
                proto::makeNotification(proto::kMethodPreviewReady, previewReadyParams(ready)),
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
                proto::makeNotification(proto::kMethodMediaError, mediaErrorParams(mediaError)),
                &frame)) {
            outbound->push_back(QueuedFrame{"media.error", std::move(frame)});
        }
    }
}

void RpcServer::submitDeferred(boost::json::value frame) {
    // The method table's scene.prepare completion, arriving on the executor
    // thread (or inline when there was no file work to wait for). The bag and
    // the wake share this mutex with stop(), which is what keeps a late
    // completion off an already-recycled descriptor: a full pipe means a wake
    // is ALREADY pending, so EAGAIN is success too.
    std::lock_guard<std::mutex> lock(fDeferredMutex);
    fDeferredFrames.push_back(std::move(frame));
    // The byte that shortens the serve loop's poll.
    if (fWakeFds[1] >= 0) {
        const char signal = 1;
        (void)::write(fWakeFds[1], &signal, 1);
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
