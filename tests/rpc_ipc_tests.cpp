/*
 * End-to-end task 1.2 checks against a real openshow-worker process:
 * the JSON-RPC handshake (including refusing commands before hello),
 * a layer command that observably changes the composed output state,
 * structured errors, and both ways the worker must exit — a served
 * "shutdown" and the frontend simply disconnecting.
 *
 * Needs a display (the worker opens its output window), so it belongs to
 * the desktop preset.
 */

#include <boost/json.hpp>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "openshow/protocol/protocol.h"

namespace {

namespace proto = openshow::protocol;

int gFailures = 0;

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++gFailures;
    }
    return condition;
}

std::string socketPath(const char* tag) {
    return "/tmp/openshow-rpc-" + std::to_string(getpid()) + "-" + tag + ".sock";
}

std::string gWorkerBinary = "./openshow-worker";

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

// Sends one framed request and waits (bounded) for the next framed reply.
bool call(int fd, std::int64_t id, const char* method, boost::json::object params,
          boost::json::value* reply) {
    std::string wire;
    proto::appendFrame(proto::makeRequest(id, method, std::move(params)), &wire);
    if (::send(fd, wire.data(), wire.size(), MSG_NOSIGNAL) < 0) {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd pending{fd, POLLIN, 0};
        const int ready = ::poll(&pending, 1, 100);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready > 0) {
            char buffer[4096];
            const ssize_t count = ::recv(fd, buffer, sizeof(buffer), 0);
            if (count <= 0) {
                return false;
            }
            gDecoder.feed(std::string_view(buffer, static_cast<std::size_t>(count)));
        }
        std::string error;
        if (gDecoder.next(reply, &error)) {
            return true;
        }
    }
    return false;
}

bool isErrorCode(const boost::json::value& reply, proto::ErrorCode code) {
    return reply.is_object() && reply.as_object().contains("error") &&
           reply.as_object().at("error").as_object().at("code").as_int64() ==
               static_cast<int>(code);
}

bool fieldEquals(const boost::json::value& object, const char* field, const char* expected) {
    if (!object.is_object()) {
        return false;
    }
    const auto found = object.as_object().find(field);
    return found != object.as_object().end() && found->value().is_string() &&
           found->value().as_string() == expected;
}

std::uint64_t numberValue(const boost::json::value& value) {
    return value.is_uint64() ? value.as_uint64()
                             : static_cast<std::uint64_t>(value.as_int64());
}

// Polls output.status until it reports the given monitor (or timeout).
bool waitForMonitor(int fd, int wanted, std::uint64_t atLeastRevision, int seconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    std::int64_t id = 100;
    while (std::chrono::steady_clock::now() < deadline) {
        boost::json::value reply;
        if (call(fd, ++id, "output.status", {}, &reply)) {
            const boost::json::value& status = reply.at("result");
            if (status.as_object().at("monitor").as_int64() == wanted &&
                numberValue(status.as_object().at("revision")) >= atLeastRevision) {
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

// Waits (bounded) for waitpid to report an exit; returns the exit code or -1.
int waitExit(pid_t pid, int seconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        int status = 0;
        const pid_t done = ::waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return -1;
}

void killWorker(pid_t pid) {
    ::kill(pid, SIGKILL);
    int status = 0;
    ::waitpid(pid, &status, 0);
}

// Scenario A: full session — gated commands, hello, live change, shutdown.
void testServedSession() {
    const std::string path = socketPath("serve");
    const pid_t pid = spawnWorker(path);
    if (!check(pid > 0, "worker spawned")) {
        return;
    }
    const int fd = connectRetry(path);
    if (!check(fd >= 0, "connected to the worker socket")) {
        killWorker(pid);
        return;
    }

    // Commands before the handshake are refused, not served.
    boost::json::value reply;
    check(call(fd, 1, "output.status", {}, &reply) &&
              isErrorCode(reply, proto::kNotHandshaked),
          "output.status before hello is refused with kNotHandshaked");

    // Handshake.
    if (check(call(fd, 2, "hello", proto::helloParams("ipc-test", 0, 1), &reply) &&
                  fieldEquals(reply.at("result"), "application", "openshow-worker"),
              "hello succeeds after connecting")) {
        const boost::json::value& result = reply.at("result");
        check(result.as_object().at("protocol").as_object().at("major").is_number(),
              "hello negotiates a protocol version");
        check(result.as_object().at("capabilities").as_array().size() == 3,
              "hello lists worker capabilities");
    }

    // Baseline status.
    if (check(call(fd, 3, "output.status", {}, &reply) &&
                  reply.at("result").is_object(),
              "output.status answers after hello")) {
        check(reply.at("result").as_object().at("layers").as_object().at("images")
                  .as_bool(),
              "images layer starts enabled");
    }

    // The live command: turn the images layer off and watch the render
    // thread apply it (status revision catches up with the ack).
    std::uint64_t ackedRevision = 0;
    if (check(call(fd, 4, "layer.setEnabled",
                   boost::json::object{{"layer", "images"}, {"enabled", false}}, &reply) &&
                  reply.as_object().contains("result"),
              "layer.setEnabled images=false is acknowledged")) {
        ackedRevision = reply.at("result").as_object().at("revision").is_uint64()
                            ? reply.at("result").as_object().at("revision").as_uint64()
                            : static_cast<std::uint64_t>(
                                  reply.at("result").as_object().at("revision").as_int64());
        bool applied = false;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            if (call(fd, 5, "output.status", {}, &reply)) {
                const boost::json::value& status = reply.at("result");
                const std::uint64_t revision =
                    status.as_object().at("revision").is_uint64()
                        ? status.as_object().at("revision").as_uint64()
                        : static_cast<std::uint64_t>(
                              status.as_object().at("revision").as_int64());
                if (!status.as_object().at("layers").as_object().at("images").as_bool() &&
                    revision >= ackedRevision) {
                    applied = true;
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        check(applied, "the command changed the composed output state on the render thread");
    }

    // Unknown layer names and methods answer with structured errors.
    check(call(fd, 6, "layer.setEnabled",
               boost::json::object{{"layer", "holograms"}, {"enabled", true}}, &reply) &&
              isErrorCode(reply, proto::kInvalidParams),
          "unknown layer name is a structured invalid-params error");
    check(call(fd, 7, "dance.for.me", {}, &reply) &&
              isErrorCode(reply, proto::kMethodNotFound),
          "unknown method is a structured method-not-found error");

    // Served shutdown: the worker exits cleanly by itself.
    if (check(call(fd, 8, "shutdown", {}, &reply) &&
                  reply.at("result").as_object().at("ok").as_bool(),
              "shutdown is acknowledged")) {
        close(fd);
        check(waitExit(pid, 5) == 0, "worker exits with status 0 after shutdown");
    } else {
        killWorker(pid);
    }
}

// Scenario B: no shutdown served — the frontend just goes away.
void testDisconnectStopsWorker() {
    const std::string path = socketPath("gone");
    const pid_t pid = spawnWorker(path);
    if (!check(pid > 0, "worker spawned (disconnect scenario)")) {
        return;
    }
    const int fd = connectRetry(path);
    if (!check(fd >= 0, "connected to the worker socket")) {
        killWorker(pid);
        return;
    }
    boost::json::value reply;
    check(call(fd, 1, "hello", proto::helloParams("ipc-test", 0, 1), &reply),
          "hello succeeded before disconnect");
    close(fd);  // the frontend dies mid-session
    check(waitExit(pid, 5) == 0, "worker exits with status 0 when its frontend disconnects");
}

// Scenario C (task 1.3): destination control over the wire. Enumerate the
// panels, refuse bad selections, move the output onto the first panel and
// back to windowed, waiting for the status to confirm each arrival.
void testDisplaySelection() {
    const std::string path = socketPath("displays");
    const pid_t pid = spawnWorker(path);
    if (!check(pid > 0, "worker spawned (display scenario)")) {
        return;
    }
    const int fd = connectRetry(path);
    if (!check(fd >= 0, "connected to the worker socket")) {
        killWorker(pid);
        return;
    }
    boost::json::value reply;
    if (!check(call(fd, 1, "hello", proto::helloParams("ipc-test", 0, 1), &reply),
               "hello succeeded (display scenario)")) {
        killWorker(pid);
        return;
    }

    if (check(call(fd, 2, "output.displays", {}, &reply) && reply.at("result").is_array(),
              "output.displays answers with an array")) {
        const boost::json::array& displays = reply.at("result").as_array();
        check(!displays.empty(), "at least one display is enumerated");
        if (!displays.empty()) {
            const boost::json::object& first = displays[0].as_object();
            check(numberValue(first.at("index")) == 0 && first.at("name").is_string() &&
                      numberValue(first.at("width")) > 0,
                  "display entries carry index, name and size");
        }

        check(call(fd, 3, "output.select", boost::json::object{{"monitor", 99}}, &reply) &&
                  isErrorCode(reply, proto::kInvalidParams),
              "a nonexistent monitor is refused");
        check(call(fd, 4, "output.select", {}, &reply) &&
                  isErrorCode(reply, proto::kInvalidParams),
              "output.select without a monitor is refused");

        if (!displays.empty() &&
            check(call(fd, 5, "output.select", boost::json::object{{"monitor", 0}}, &reply) &&
                      reply.as_object().contains("result"),
                  "output.select monitor 0 is acknowledged")) {
            check(waitForMonitor(fd, 0, numberValue(reply.at("result").as_object().at("revision")),
                                 6),
                  "status reports the output arrived on monitor 0");
        }
        if (check(call(fd, 6, "output.select", boost::json::object{{"monitor", -1}}, &reply) &&
                      reply.as_object().contains("result"),
                  "output.select windowed is acknowledged")) {
            check(waitForMonitor(fd, -1,
                                 numberValue(reply.at("result").as_object().at("revision")), 6),
                  "status reports the output returned to windowed");
        }
    }

    call(fd, 7, "shutdown", {}, &reply);
    close(fd);
    check(waitExit(pid, 5) == 0, "display-scenario worker exits cleanly");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1) {
        gWorkerBinary = argv[1];
    }
    testServedSession();
    testDisconnectStopsWorker();
    testDisplaySelection();

    if (gFailures != 0) {
        std::fprintf(stderr, "%d IPC check(s) failed\n", gFailures);
        return 1;
    }
    std::printf("ok: worker RPC session, live command and both exits\n");
    return 0;
}
