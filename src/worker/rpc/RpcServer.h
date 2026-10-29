/*
 * RpcServer: the worker side of the JSON-RPC 2.0 transport (task 1.2).
 *
 * A single frontend connects over a Unix-domain stream socket. The server
 * runs on its own thread with a poll() loop so neither decoding IPC nor a
 * stalled frontend can block the render thread — the session's operations
 * it calls are the thread-safe ones. It is the only way into the session
 * besides --render-test: every command is answered by a session operation,
 * never by reaching into scene internals (PLAN.md graduation rule).
 *
 * Nothing here waits for the frontend (task 6.8b, review R11). The socket is
 * O_NONBLOCK and everything owed to it goes through a bounded, coalescing
 * outbound queue that the loop drains when the socket says POLLOUT, so a peer
 * that stops reading costs a queue full of bytes and a logged disconnect
 * within a deadline — not a server thread parked inside send(), which is what
 * used to stop command handling, disconnect detection and shutdown alike. No
 * session lock is held across a socket operation: handlers finish their
 * session call and return the frame, and only the loop touches the descriptor.
 *
 * Lifecycle: listen() binds and accepts one connection; when that client
 * disconnects, is served a shutdown, or proves it is not reading, the session
 * is asked to stop so the worker exits with its frontend gone. Nothing is
 * served before a successful "hello".
 */

#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <boost/json.hpp>

namespace openshow::worker {

class OutputSession;

class RpcServer {
public:
    struct Options {
        std::string socketPath;
        std::string workerVersion;
    };

    RpcServer(OutputSession& session, Options options);
    ~RpcServer();

    RpcServer(const RpcServer&) = delete;
    RpcServer& operator=(const RpcServer&) = delete;

    // Binds and listens synchronously so the socket path exists before the
    // frontend is told to connect. Returns false and fills error on failure.
    bool listen(std::string* error);

    // Accepts the connection and serves it on a worker thread. Returns
    // immediately; the thread runs until stop() or client disconnect.
    bool start(std::string* error);

    // Ends service and joins the thread, then closes and unlinks the socket.
    void stop();

    const std::string& socketPath() const { return fOptions.socketPath; }

private:
    // One message owed to the frontend. `key` says whether it may be replaced
    // by a newer one of the same kind: empty means it answers a request and is
    // therefore never dropped or reordered, and a non-empty key (one per
    // preview slot, one for media.error) means the newest wins, because the
    // consumer of those wants the current state and not a transcript of how it
    // got there.
    struct QueuedFrame {
        std::string key;
        std::string bytes;
    };

    void run();
    void serve(int connectionFd);
    // Handles one decoded message; returns false to drop the connection.
    bool handle(const boost::json::value& message, bool handshakeDone,
                bool* handshakeNow, std::string* outbound);
    // Drains the notifications the session is holding — queued by the render
    // thread, already reduced to the newest per slot — into frames for the
    // outbound queue. Called on every poll tick, timeout included, so a
    // notification travels within one tick of being queued. It cannot fail:
    // nothing is written here, which is what keeps the session's lock out of
    // the socket's path.
    void collectNotifications(std::vector<QueuedFrame>* outbound);

    // Deferred replies (task 6.9a, review R10): a scene.prepare whose file
    // work went to the prepare executor is answered from the executor
    // thread, and this thread is the only one holding the socket. The
    // executor pushes finished replies here; the serve loop moves them
    // into the outbound queue as request answers (never coalesced, never
    // dropped). A push writes one byte into the self-pipe below so the
    // serve loop stops waiting on its own — a completion travels within
    // milliseconds of readiness, not at the mercy of the poll tick. That
    // is not politeness: the budget the preview draws arm themselves is
    // timed in seconds, and a 50 ms hold of the frontend's NEXT command
    // was enough to move a live selection ask behind one of those
    // deferrals (6.16a's supersede scenario proved it). They may still
    // pass a later request's reply on the wire — ids correlate, ordering
    // was never promised (task 6.3a).
    std::mutex fDeferredMutex;
    std::vector<boost::json::value> fDeferredFrames;
    void collectDeferred(std::vector<QueuedFrame>* outbound);

    // The self-wake for the deferred queue: [0] rides the serve loop's
    // poll beside the connection, [1] is what a completion writes. Both
    // nonblocking and close-on-exec; both are closed under fDeferredMutex
    // by stop(), which is what lets a late executor completion write an
    // already-retired number harmlessly rather than into a recycled fd.
    int fWakeFds[2] = {-1, -1};

    OutputSession& fSession;
    Options fOptions;
    int fListenFd = -1;
    // The accepted descriptor belongs to the serving thread and only that
    // thread closes it. stop() may wake it with shutdown(), but only while
    // holding fConnectionMutex, and run() clears the member under the same
    // mutex BEFORE closing — so the descriptor number stop() acts on is one
    // that is still open, never a closed number the kernel has since handed to
    // another thread's socket or pipe. Before this, run() and stop() read and
    // wrote a plain int with nothing between them (review R11).
    std::mutex fConnectionMutex;
    int fConnectionFd = -1;
    std::atomic<bool> fRunning{false};
    std::thread fThread;
};

}  // namespace openshow::worker
