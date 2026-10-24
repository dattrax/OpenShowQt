#include "openshow/protocol/protocol.h"

#include <exception>
#include <utility>

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

void appendFrame(const boost::json::value& message, std::string* out) {
    const std::string payload = boost::json::serialize(message);
    putU32LittleEndian(out, static_cast<std::uint32_t>(payload.size()));
    out->append(payload);
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
    boost::json::value parsed;
    try {
        parsed = boost::json::parse(payload);
    } catch (const std::exception& exception) {
        fFailed = true;
        fBuffer.clear();
        if (error) {
            *error = std::string("malformed JSON frame: ") + exception.what();
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
                            "scene.store.v1", "shutdown.v1"}}};
    return true;
}

}  // namespace openshow::protocol
