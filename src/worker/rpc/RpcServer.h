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
 * Lifecycle: listen() binds and accepts one connection; when that client
 * disconnects or a shutdown is served, requestStop() is called so the
 * worker exits with its frontend gone. Nothing is served before a
 * successful "hello".
 */

#pragma once

#include <atomic>
#include <string>
#include <thread>

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
    void run();
    void serve(int connectionFd);
    // Handles one decoded message; returns false to drop the connection.
    bool handle(const boost::json::value& message, bool handshakeDone,
                bool* handshakeNow, std::string* outbound);
    // Sends any queued preview.ready notifications; false when the socket
    // write side is gone. Called on every poll tick, including timeouts,
    // so notifications travel within one tick of being queued.
    bool flushNotifications(int connectionFd);

    OutputSession& fSession;
    Options fOptions;
    int fListenFd = -1;
    int fConnectionFd = -1;
    std::atomic<bool> fRunning{false};
    std::thread fThread;
};

}  // namespace openshow::worker
