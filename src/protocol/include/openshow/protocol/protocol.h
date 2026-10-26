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
// preview.released must echo; minor 5 (task 6.7c) adds the typed audio
// lane status.media.audio carries ("none", "silent", "no-output",
// "degraded", "paused", "live"), which is what tells a frontend why an
// on-air background with sound is silent. Minor-3 additions a frontend
// ignores are harmless — the "media.error.v1" capability names them, and
// "media.audio-state.v1" names the lane (an unknown extra field in a
// status object costs nothing, so a minor-5 worker still serves an older
// frontend). The minor-4 release rule is stricter, not additive: releases
// without the stream identity are refused, so "preview.stream-id.v1" is
// the capability a frontend checks before expecting its preview to keep
// flowing.
inline constexpr std::uint32_t kProtocolMajor = 0;
inline constexpr std::uint32_t kProtocolMinor = 5;

// Worker-to-frontend notification (task 5.5d): a background media asset
// failed. {"path":...,"message":...} — sent when the failure happens,
// while output.status "media.error" carries the same text until a
// successful open replaces it (the frontend polls; it must not miss a
// failure that lands between polls).
inline constexpr const char* kMethodMediaError = "media.error";

// A framed message larger than this is a protocol error: the only legitimate
// payloads are control messages, never pixels. The number is BOTH directions'
// budget, and it is the payload (the frame adds the 4-byte prefix): senders
// refuse to build a frame the peer's decoder would reject (appendFrame) and the
// receiver refuses to accept one (Decoder::next). It is measured against the
// supported model rather than picked for comfort — the largest scene every
// application budget allows fits a scene.prepare at 39% of it, which
// scene_tests prints and pins.
inline constexpr std::uint32_t kMaxMessageBytes = 1u << 20;  // 1 MiB

// How deeply a frame may nest (task 6.8a, review R11). Boost.JSON allows 32
// levels by default, so one 1 MiB frame of braces spends the receive buffer on
// structure no handler will ever read. The deepest legitimate message in this
// protocol is a scene.prepare, which counts five levels — the message object,
// params, items, one item, lines — measured by protocol_tests and scene_tests
// against a compiled scene; every reply nests more shallowly still. Eight
// leaves a scene field room to grow a level without a wire change.
inline constexpr std::size_t kMaxParseDepth = 8;

// Method names are short fixed identifiers from a closed list; a megabyte-long
// "method" is an attack or a bug, and the reply that reports it must not be
// another megabyte.
inline constexpr std::size_t kMaxMethodNameBytes = 64;

// Any text an error reply echoes back from client input (a rejected key, layer
// or asset name). Without a limit, the error carrying "this key is too long"
// is itself the oversized message R11 is about.
inline constexpr std::size_t kMaxEchoedTextBytes = 160;

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
//
// Returns false WITHOUT touching *out when the serialised payload will not fit
// kMaxMessageBytes (task 6.8a, review R11). This is a send limit, not only a
// receive one: an unchecked sender used to write the length as
// `static_cast<uint32_t>(payload.size())` and hand over a frame the peer's
// decoder rejected, so an operator pasting very large text did not get a
// validation error — the worker hung up (the review measured 1,048,650 bytes
// on the wire against a decoder that refuses 1,048,646). A false return is the
// caller's cue to refuse the operation LOCALLY and say why; the connection
// stays up and useful.
bool appendFrame(const boost::json::value& message, std::string* out);

// `text`, cut to at most `maxBytes` and ending at a UTF-8 boundary, for error
// messages that quote client input. A clip that stopped mid-codepoint would
// hand the peer invalid UTF-8 in the very reply that explains the problem;
// an elided clip ends in "...".
std::string clippedForEcho(std::string_view text, std::size_t maxBytes = kMaxEchoedTextBytes);

// Incremental stream decoder. Feed() bytes as they arrive from the socket;
// next() pops one complete parsed message per call. A framing violation
// (oversize length, malformed JSON, nesting past kMaxParseDepth) is fatal for
// the connection: feed/next then report failure, failed() stays true.
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

// --- a request's shape, validated before any handler runs (task 6.8a, R11) ---

// What routing needs, checked once: the version stamp, a usable method name,
// an id the reply can honestly carry, and the params pointer.
struct RequestEnvelope {
    std::int64_t id = 0;   // meaningful only when hasId is set
    bool hasId = false;    // false: a NOTIFICATION — nothing may ever answer it
    std::string_view method;
    const boost::json::value* params = nullptr;  // null when the key is absent
};

// Validates the envelope and fills *out. Returns true when the message may be
// routed; `out->hasId` then says whether an answer is permitted at all.
// `out->method` and `out->params` BORROW from `message`, so *out is only valid
// while `message` is (route on the value you parsed, do not keep the view).
//
// On false the caller answers with *code and *message (and keeps the
// connection unless *dropsConnection says the frame is not JSON-RPC 2.0 at
// all — a stream that has lost its version stamp is not worth serving).
// *out is still filled well enough to answer: `id` carries the id to reply
// under, which is 0 whenever the message's own id could not be trusted, and
// `hasId` is true in that case because the client did ask for an answer.
//
// The rules R11 asked for:
//   * "id", when present, must be a JSON integer inside the signed 64-bit
//     range this protocol's ids live in. Anything else — a string, a bool, a
//     double, a uint64 past INT64_MAX — is refused rather than silently
//     narrowed to 0, which would quietly collide with another request's reply
//     (task 6.3a correlates by id).
//   * a message without "id" is a notification, so a server reply is not
//     merely useless, it is forbidden; routing decides whether the METHOD is
//     one of the notification methods (preview.released) and ignores it if
//     not, because a command that can never be acknowledged is exactly the
//     lost-update shape 6.3a closed.
//   * "method" must be a string of 1..kMaxMethodNameBytes.
//   * "jsonrpc" must be exactly "2.0".
bool parseRequestEnvelope(const boost::json::value& message, RequestEnvelope* out,
                          ErrorCode* code, std::string* text,
                          bool* dropsConnection = nullptr);

// True for the methods this protocol documents as fire-and-forget: the sender
// never wants an answer, so no answer is ever sent for them (not even an
// error). `preview.released` per frame is the case; it may still carry an id
// if the client wants a receipt, which makes it a request.
bool isNotificationMethod(std::string_view method);

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
