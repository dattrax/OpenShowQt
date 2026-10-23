/*
 * RpcServer implementation. Raw AF_UNIX stream socket, one client, poll()
 * loop with 50 ms ticks so stop() and requestStop() are noticed promptly.
 * All framing and JSON-RPC shape lives in openshow::protocol; this file is
 * routing, the hello gate and the session calls — nothing else.
 */

#include "rpc/RpcServer.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "openshow/protocol/preview_wire.h"
#include "openshow/protocol/protocol.h"

#include "render/Layer.h"
#include "session/OutputSession.h"

namespace openshow::worker {

namespace {

namespace proto = openshow::protocol;

constexpr int kPollTimeoutMs = 50;

bool writeAll(int fd, const std::string& data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t written = send(fd, data.data() + offset, data.size() - offset,
                                     MSG_NOSIGNAL);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

std::int64_t jsonId(const boost::json::object& message) {
    const auto id = message.find("id");
    if (id == message.end()) {
        return 0;
    }
    if (id->value().is_int64()) {
        return id->value().as_int64();
    }
    if (id->value().is_uint64()) {
        return static_cast<std::int64_t>(id->value().as_uint64());
    }
    return 0;
}

// Reads a non-negative JSON integer. Boost.JSON stores small numbers as
// int64 and only reaches for uint64 past the signed range, so checking
// is_uint64() alone silently rejects every slot index a client sends.
bool jsonUint(const boost::json::object& args, const char* key, std::uint64_t* out) {
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
                                                               {"error", preview.error}}}};
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
    if (fConnectionFd >= 0) {
        ::shutdown(fConnectionFd, SHUT_RDWR);
    }
    if (fListenFd >= 0) {
        ::shutdown(fListenFd, SHUT_RDWR);
    }
    if (fThread.joinable()) {
        fThread.join();
    }
    if (fConnectionFd >= 0) {
        close(fConnectionFd);
        fConnectionFd = -1;
    }
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
        fConnectionFd = connection;
        std::fprintf(stderr, "openshow-worker: frontend connected on %s\n",
                     fOptions.socketPath.c_str());
        serve(connection);
        close(connection);
        fConnectionFd = -1;
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

    while (fRunning) {
        pollfd pending{connectionFd, POLLIN, 0};
        const int ready = ::poll(&pending, 1, kPollTimeoutMs);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        // Every tick, timeout included: the render thread queues preview
        // notifications and this thread is the only one holding the socket.
        if (!flushNotifications(connectionFd)) {
            return;
        }
        if (ready == 0) {
            continue;
        }
        const ssize_t count = ::recv(connectionFd, buffer, sizeof(buffer), 0);
        if (count == 0) {
            return;  // orderly disconnect
        }
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;  // reset or shutdown() from stop()
        }

        decoder.feed(std::string_view(buffer, static_cast<std::size_t>(count)));
        boost::json::value message;
        std::string decodeError;
        while (decoder.next(&message, &decodeError)) {
            std::string outbound;
            if (!handle(message, handshakeDone, &handshakeDone, &outbound)) {
                std::fprintf(stderr,
                             "openshow-worker: dropping connection after a message "
                             "that was not JSON-RPC 2.0\n");
                return;
            }
            if (!outbound.empty() && !writeAll(connectionFd, outbound)) {
                return;  // write side of the socket is gone
            }
        }
        if (decoder.failed()) {
            std::fprintf(stderr, "openshow-worker: framing error: %s\n",
                         decodeError.c_str());
            return;
        }
    }
}

bool RpcServer::handle(const boost::json::value& message, bool handshakeDone,
                       bool* handshakeNow, std::string* outbound) {
    if (!proto::looksLikeJsonRpc(message)) {
        proto::appendFrame(
            proto::makeError(0, proto::kInvalidRequest, "expected a JSON-RPC 2.0 message"),
            outbound);
        return false;
    }
    const boost::json::object& request = message.as_object();
    const auto method = request.find("method");
    if (method == request.end() || !method->value().is_string()) {
        proto::appendFrame(
            proto::makeError(jsonId(request), proto::kInvalidRequest,
                             "only requests are accepted, not bare responses"),
            outbound);
        return true;
    }

    const std::string_view name = method->value().as_string();
    const auto params = request.find("params");
    const boost::json::value nullParams = nullptr;
    const boost::json::value& given =
        params == request.end() ? nullParams : params->value();
    const std::int64_t id = jsonId(request);

    if (name == "hello") {
        boost::json::object result;
        proto::ErrorCode code = proto::kInternalError;
        std::string text;
        boost::json::value data;
        if (proto::handleHello(given, fOptions.workerVersion, &result, &code, &text,
                               &data)) {
            *handshakeNow = true;
            proto::appendFrame(proto::makeResult(id, result), outbound);
        } else {
            proto::appendFrame(proto::makeError(id, code, text, data), outbound);
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
        const auto monitor = given.as_object().find("monitor");
        std::int64_t index = -2;  // sentinel: absent or non-numeric is invalid
        if (monitor != given.as_object().end()) {
            if (monitor->value().is_int64()) {
                index = monitor->value().as_int64();
            } else if (monitor->value().is_uint64()) {
                index = static_cast<std::int64_t>(monitor->value().as_uint64());
            }
        }
        std::uint64_t revision = 0;
        if (index < -1 || !fSession.selectMonitor(static_cast<int>(index), &revision)) {
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
                                                    std::string(layer->value().as_string()) +
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
    } else if (name == "preview.start") {
        uint32_t width = 640;
        uint32_t height = 360;
        uint32_t fps = 15;
        if (given.is_object()) {
            const boost::json::object& args = given.as_object();
            std::uint64_t value = 0;
            if (jsonUint(args, "width", &value)) {
                width = static_cast<uint32_t>(value);
            }
            if (jsonUint(args, "height", &value)) {
                height = static_cast<uint32_t>(value);
            }
            if (jsonUint(args, "fps", &value)) {
                fps = static_cast<uint32_t>(value);
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
        // client bothered to identify it.
        if (given.is_object()) {
            const boost::json::object& args = given.as_object();
            std::uint64_t slot = 0;
            std::uint64_t seq = 0;
            if (jsonUint(args, "slot", &slot) && jsonUint(args, "seq", &seq)) {
                fSession.releasePreviewSlot(static_cast<uint32_t>(slot), seq);
            }
        }
        if (request.contains("id")) {
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

bool RpcServer::flushNotifications(int connectionFd) {
    OutputSession::PreviewReady ready;
    std::string outbound;
    while (fSession.takePreviewReady(&ready)) {
        proto::appendFrame(
            proto::makeNotification(proto::kMethodPreviewReady,
                                    boost::json::object{{"slot", ready.slot},
                                                        {"seq", ready.seq},
                                                        {"width", ready.width},
                                                        {"height", ready.height},
                                                        {"rowBytes", ready.rowBytes},
                                                        {"revision", ready.revision},
                                                        {"publishedNs", ready.publishedNs}}),
            &outbound);
    }
    return outbound.empty() || writeAll(connectionFd, outbound);
}

}  // namespace openshow::worker
