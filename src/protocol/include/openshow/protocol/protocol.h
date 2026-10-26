/*
 * Wire types shared by the Qt frontend and the Qt-free worker (task 1.2).
 *
 * Transport: Unix-domain socket, byte stream. Framing: each message is a
 * 4-byte little-endian payload length followed by that many bytes of UTF-8
 * JSON (JSON-RPC 2.0). One message per frame, no pipelining requirement.
 *
 * The handshake is the first exchange: the client sends "hello" with the
 * protocol version it wants; the worker accepts only its own major version
 * and answers with the effective version and its capability list. Nothing
 * else is served before a successful hello.
 */

#pragma once

#include <boost/json.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace openshow::protocol {

// Wire format version. Frontend and worker must agree on the major value
// before any other message is exchanged. Minor 3 (task 5.5d) adds the
// media.error notification and the status "media" object; minor 4 (task
// 6.5) adds the stream identity that preview.ready carries and
// preview.released must echo. Minor-3 additions a frontend ignores are
// harmless — the "media.error.v1" capability names them. The minor-4
// release rule is stricter, not additive: releases without the stream
// identity are refused, so "preview.stream-id.v1" is the capability a
// frontend checks before expecting its preview to keep flowing.
inline constexpr std::uint32_t kProtocolMajor = 0;
inline constexpr std::uint32_t kProtocolMinor = 4;

// Worker-to-frontend notification (task 5.5d): a background media asset
// failed. {"path":...,"message":...} — sent when the failure happens,
// while output.status "media.error" carries the same text until a
// successful open replaces it (the frontend polls; it must not miss a
// failure that lands between polls).
inline constexpr const char* kMethodMediaError = "media.error";

// A framed message larger than this is treated as a protocol error: the
// only legitimate payloads are control messages, never pixels.
inline constexpr std::uint32_t kMaxMessageBytes = 1u << 20;  // 1 MiB

// JSON-RPC 2.0 error codes. The -320xx values are ours.
enum ErrorCode : int {
    kParseError = -32700,
    kInvalidRequest = -32600,
    kMethodNotFound = -32601,
    kInvalidParams = -32602,
    kInternalError = -32603,
    kVersionMismatch = -32001,
    kNotHandshaked = -32002,
};

// The offer the worker would send during a handshake, for example
// {"application":"openshow-worker","protocol":[0,1]}.
std::string offerPayload();

// Appends one framed message (length prefix + serialised JSON) to *out.
void appendFrame(const boost::json::value& message, std::string* out);

// Incremental stream decoder. Feed() bytes as they arrive from the socket;
// next() pops one complete parsed message per call. A framing violation
// (oversize length, malformed JSON) is fatal for the connection: feed/next
// then report failure, failed() stays true.
class Decoder {
public:
    void feed(std::string_view bytes);

    // Returns true and stores the message when one is complete. Returns
    // false when more bytes are needed. On a protocol violation returns
    // false, fills *error and poisons the decoder.
    bool next(boost::json::value* out, std::string* error);

    bool failed() const { return fFailed; }

    // Forget buffered bytes and a poisoned state. A connection teardown
    // must not let the next connection interpret the old stream's tail
    // (task 6.3a: reply correlation belongs to one connection).
    void reset() {
        fBuffer.clear();
        fFailed = false;
    }

private:
    std::string fBuffer;
    bool fFailed = false;
};

// --- JSON-RPC 2.0 envelopes -------------------------------------------------

boost::json::object makeRequest(std::int64_t id, std::string_view method,
                                boost::json::object params = {});
boost::json::object makeResult(std::int64_t id, boost::json::value result);
boost::json::object makeError(std::int64_t id, ErrorCode code, std::string_view message,
                              boost::json::value data = nullptr);

// A notification: method and params but no id, so nothing may ever answer
// it. Used for per-frame preview traffic (task 1.4).
boost::json::object makeNotification(std::string_view method,
                                     boost::json::object params = {});

// True when the value is an object carrying "jsonrpc":"2.0" and a method
// or an id; the full shape is validated by whoever serves it.
bool looksLikeJsonRpc(const boost::json::value& value);

// --- handshake ---------------------------------------------------------------

// {"client":"openshow-ui","protocol":{"major":0,"minor":1}}
boost::json::object helloParams(std::string_view clientName, std::uint32_t major,
                                std::uint32_t minor);

// Server side of "hello". Validates params, then builds
// {"application":"openshow-worker","version":workerVersion,
//  "protocol":{"major":0,"minor":N},"capabilities":[...]}
// On version mismatch or bad params returns false with the JSON-RPC code
// and message to send back; a mismatch also fills *errorData with both
// version pairs.
bool handleHello(const boost::json::value& params, std::string_view workerVersion,
                 boost::json::object* result, ErrorCode* code, std::string* message,
                 boost::json::value* errorData = nullptr);

}  // namespace openshow::protocol
