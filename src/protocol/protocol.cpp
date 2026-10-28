#include "openshow/protocol/protocol.h"

#include <cstdint>
#include <limits>
#include <utility>

#include "openshow/protocol/preview_wire.h"

namespace openshow::protocol {

namespace {

void putU32LittleEndian(std::string* out, std::uint32_t value) {
    out->push_back(static_cast<char>(value & 0xFFu));
    out->push_back(static_cast<char>((value >> 8) & 0xFFu));
    out->push_back(static_cast<char>((value >> 16) & 0xFFu));
    out->push_back(static_cast<char>((value >> 24) & 0xFFu));
}

std::uint32_t getU32LittleEndian(const char* bytes) {
    const auto b = reinterpret_cast<const unsigned char*>(bytes);
    return static_cast<std::uint32_t>(b[0]) | (static_cast<std::uint32_t>(b[1]) << 8) |
           (static_cast<std::uint32_t>(b[2]) << 16) | (static_cast<std::uint32_t>(b[3]) << 24);
}

// Boost.JSON keeps small integers in the signed kind, so a wire number can
// arrive as either kind depending on who produced it.
bool asUnsigned(const boost::json::value& value, std::uint64_t* out) {
    if (value.is_uint64()) {
        *out = value.as_uint64();
        return true;
    }
    if (value.is_int64() && value.as_int64() >= 0) {
        *out = static_cast<std::uint64_t>(value.as_int64());
        return true;
    }
    return false;
}

}  // namespace

std::string offerPayload() {
    boost::json::object offer;
    offer["application"] = "openshow-worker";
    offer["protocol"] = boost::json::array{kProtocolMajor, kProtocolMinor};
    return boost::json::serialize(offer);
}

bool appendFrame(const boost::json::value& message, std::string* out) {
    if (out == nullptr) {
        return false;
    }
    const std::string payload = boost::json::serialize(message);
    // The peer's decoder accepts payload sizes 1...kMaxMessageBytes and treats
    // anything else as a framing violation that kills the connection. Cast the
    // size here and the sender becomes the author of someone else's disconnect.
    if (payload.empty() || payload.size() > kMaxMessageBytes) {
        return false;
    }
    putU32LittleEndian(out, static_cast<std::uint32_t>(payload.size()));
    out->append(payload);
    return true;
}

std::string clippedForEcho(std::string_view text, std::size_t maxBytes) {
    if (text.size() <= maxBytes) {
        return std::string(text);
    }
    if (maxBytes < 3) {
        return std::string();  // not even an ellipsis fits: quote nothing at all
    }
    // Reserve room for the ellipsis, then step back off a partial codepoint: a
    // continuation byte (10xxxxxx) at the cut means the last character was cut
    // in half, and half a character is not valid UTF-8.
    std::size_t cut = maxBytes > 3 ? maxBytes - 3 : 0;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0u) == 0x80u) {
        --cut;
    }
    return std::string(text.substr(0, cut)) + "...";
}

void Decoder::feed(std::string_view bytes) {
    if (!fFailed) {
        fBuffer.append(bytes);
    }
}

bool Decoder::next(boost::json::value* out, std::string* error) {
    if (fFailed) {
        if (error) {
            *error = "connection poisoned by an earlier protocol violation";
        }
        return false;
    }
    if (fBuffer.size() < 4) {
        return false;  // length prefix incomplete
    }
    const std::uint32_t length = getU32LittleEndian(fBuffer.data());
    if (length == 0 || length > kMaxMessageBytes) {
        fFailed = true;
        fBuffer.clear();
        if (error) {
            *error = "message length " + std::to_string(length) +
                     " is outside 1..." + std::to_string(kMaxMessageBytes);
        }
        return false;
    }
    if (fBuffer.size() < size_t(4) + length) {
        return false;  // payload incomplete
    }
    const std::string_view payload(fBuffer.data() + 4, length);
    // The frame is already inside the byte budget, so what a hostile payload
    // still has is DEPTH: nesting multiplies allocator work per byte received.
    // kMaxParseDepth is measured against the real messages (see the header).
    boost::system::error_code parseError;
    boost::json::parse_options options;
    options.max_depth = kMaxParseDepth;
    boost::json::value parsed =
        boost::json::parse(payload, parseError, boost::json::storage_ptr{}, options);
    if (parseError) {
        fFailed = true;
        fBuffer.clear();
        if (error) {
            *error = parseError == boost::json::error::too_deep
                         ? "message nests deeper than " + std::to_string(kMaxParseDepth) +
                               " levels"
                         : std::string("malformed JSON frame: ") + parseError.message();
        }
        return false;
    }
    fBuffer.erase(0, size_t(4) + length);
    *out = std::move(parsed);
    return true;
}

boost::json::object makeRequest(std::int64_t id, std::string_view method,
                                boost::json::object params) {
    boost::json::object request;
    request["jsonrpc"] = "2.0";
    request["id"] = id;
    request["method"] = std::string(method);
    request["params"] = std::move(params);
    return request;
}

boost::json::object makeResult(std::int64_t id, boost::json::value result) {
    boost::json::object response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["result"] = std::move(result);
    return response;
}

boost::json::object makeError(std::int64_t id, ErrorCode code, std::string_view message,
                              boost::json::value data) {
    boost::json::object error;
    error["code"] = static_cast<int>(code);
    error["message"] = std::string(message);
    if (!data.is_null()) {
        error["data"] = std::move(data);
    }
    boost::json::object response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["error"] = std::move(error);
    return response;
}

boost::json::object makeNotification(std::string_view method, boost::json::object params) {
    boost::json::object notification;
    notification["jsonrpc"] = "2.0";
    notification["method"] = std::string(method);
    notification["params"] = std::move(params);
    return notification;
}

bool looksLikeJsonRpc(const boost::json::value& value) {
    if (!value.is_object()) {
        return false;
    }
    const boost::json::object& object = value.as_object();
    const auto jsonrpc = object.find("jsonrpc");
    if (jsonrpc == object.end() || !jsonrpc->value().is_string() ||
        jsonrpc->value().as_string() != "2.0") {
        return false;
    }
    return object.contains("method") || object.contains("id");
}

boost::json::object helloParams(std::string_view clientName, std::uint32_t major,
                                std::uint32_t minor) {
    boost::json::object params;
    params["client"] = std::string(clientName);
    params["protocol"] =
        boost::json::object{{"major", major}, {"minor", minor}};
    return params;
}

bool handleHello(const boost::json::value& params, std::string_view workerVersion,
                 boost::json::object* result, ErrorCode* code, std::string* message,
                 boost::json::value* errorData) {
    if (!params.is_object()) {
        *code = kInvalidParams;
        *message = "hello params must be an object";
        return false;
    }
    const boost::json::object& object = params.as_object();
    const auto protocol = object.find("protocol");
    if (protocol == object.end() || !protocol->value().is_object()) {
        *code = kInvalidParams;
        *message = "hello requires a protocol object";
        return false;
    }
    const boost::json::object& wanted = protocol->value().as_object();
    std::uint64_t major = 0;
    std::uint64_t minor = 0;
    const auto majorValue = wanted.find("major");
    const auto minorValue = wanted.find("minor");
    if (majorValue == wanted.end() || minorValue == wanted.end() ||
        !asUnsigned(majorValue->value(), &major) || !asUnsigned(minorValue->value(), &minor)) {
        *code = kInvalidParams;
        *message = "hello protocol requires integer major and minor";
        return false;
    }
    if (major != kProtocolMajor) {
        *code = kVersionMismatch;
        *message = "worker speaks protocol major " + std::to_string(kProtocolMajor);
        if (errorData) {
            *errorData = boost::json::object{
                {"worker", boost::json::array{kProtocolMajor, kProtocolMinor}},
                {"requested", boost::json::array{major, minor}}};
        }
        return false;
    }

    *result = boost::json::object{
        {"application", "openshow-worker"},
        {"version", std::string(workerVersion)},
        {"protocol", boost::json::object{{"major", kProtocolMajor},
                                         {"minor", kProtocolMinor}}},
        {"capabilities",
         boost::json::array{"output.status.v1", "output.layers.v1", "preview.shm.v1",
                            "preview.stream-id.v1", "preview.select.v1",
                            "preview.thumbnail.v1", "scene.store.v1", "shutdown.v1",
                            "media.error.v1", "media.audio-state.v1", "media.transport.v1",
                            "media.standalone.v1", "scene.image-items.v1",
                            "scene.background-fit.v1", "scene.background-fit-blur.v1"}}};
    return true;
}

bool isNotificationMethod(std::string_view method) {
    // One method, per frame, whose whole contract is "I do not want a reply".
    // A client that wants a receipt sends it with an id, which makes it a
    // request like any other.
    return method == kMethodPreviewReleased;
}

bool parseRequestEnvelope(const boost::json::value& message, RequestEnvelope* out,
                          ErrorCode* code, std::string* text, bool* dropsConnection) {
    if (out == nullptr || code == nullptr || text == nullptr) {
        return false;
    }
    *out = RequestEnvelope{};
    if (dropsConnection) {
        *dropsConnection = false;
    }
    auto refuse = [&](ErrorCode refused, std::string reason, bool fatal) {
        *code = refused;
        *text = std::move(reason);
        if (dropsConnection) {
            *dropsConnection = fatal;
        }
        return false;
    };

    // A frame that is not JSON-RPC 2.0 at all is not a message with a problem,
    // it is a stream this worker cannot reason about: same answer as the
    // framing violations the decoder treats as fatal.
    if (!looksLikeJsonRpc(message)) {
        return refuse(kInvalidRequest, "expected a JSON-RPC 2.0 message", true);
    }
    const boost::json::object& request = message.as_object();

    // The id first: it decides whether an answer is allowed, and an untrustworthy
    // one must be refused rather than narrowed. `as_int64()` on a double, or a
    // cast of a uint64 past the signed range, both land on some OTHER request's
    // id, and replies are correlated by id (task 6.3a) — a silent collision here
    // applies the wrong command's acknowledgement.
    const auto id = request.find("id");
    if (id != request.end()) {
        const boost::json::value& idValue = id->value();
        if (idValue.is_int64()) {
            out->id = idValue.as_int64();
            out->hasId = true;
        } else if (idValue.is_uint64() &&
                   idValue.as_uint64() <=
                       static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            out->id = static_cast<std::int64_t>(idValue.as_uint64());
            out->hasId = true;
        } else {
            std::string kind = "a number";
            if (idValue.is_string()) {
                kind = "the string \"" + clippedForEcho(idValue.as_string()) + "\"";
            } else if (idValue.is_bool()) {
                kind = "a boolean";
            } else if (idValue.is_null()) {
                kind = "null";
            } else if (idValue.is_array() || idValue.is_object()) {
                kind = "a compound value";
            }
            // Answered under 0 — JSON-RPC's "the id could not be determined".
            return refuse(kInvalidRequest,
                          "request \"id\" must be an integer in the signed 64-bit range, "
                          "not " +
                              kind,
                          false);
        }
    }

    const auto method = request.find("method");
    if (method == request.end()) {
        return refuse(kInvalidRequest, "only requests are accepted, not bare responses",
                      false);
    }
    if (!method->value().is_string()) {
        return refuse(kInvalidRequest, "request \"method\" must be a string", false);
    }
    const std::string_view name = method->value().as_string();
    if (name.empty()) {
        return refuse(kInvalidRequest, "request \"method\" must not be empty", false);
    }
    if (name.size() > kMaxMethodNameBytes) {
        return refuse(kInvalidRequest,
                      "request \"method\" is longer than " +
                          std::to_string(kMaxMethodNameBytes) + " bytes",
                      false);
    }
    out->method = name;

    const auto params = request.find("params");
    if (params != request.end()) {
        out->params = &params->value();
    }
    return true;
}

}  // namespace openshow::protocol
