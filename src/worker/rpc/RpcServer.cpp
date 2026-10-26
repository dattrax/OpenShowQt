/*
 * RpcServer implementation. Raw AF_UNIX stream socket, one client, poll()
 * loop with 50 ms ticks so stop() and requestStop() are noticed promptly.
 * All framing and JSON-RPC shape lives in openshow::protocol; this file is
 * routing, the hello gate and the session calls — nothing else.
 */

#include "rpc/RpcServer.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

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

bool RpcServer::flushNotifications(int connectionFd) {
    OutputSession::PreviewReady ready;
    OutputSession::MediaError mediaError;
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
                                                        {"publishedNs", ready.publishedNs},
                                                        // Stream identity: the release
                                                        // must echo it (task 6.5).
                                                        {"stream", ready.stream}}),
            &outbound);
    }
    // Background media failure (task 5.5d): immediate visibility, the
    // same text output.status.media.error carries until it is replaced.
    while (fSession.takeMediaError(&mediaError)) {
        proto::appendFrame(
            proto::makeNotification(proto::kMethodMediaError,
                                    boost::json::object{{"path", mediaError.path},
                                                        {"message", mediaError.message}}),
            &outbound);
    }
    return outbound.empty() || writeAll(connectionFd, outbound);
}

}  // namespace openshow::worker
