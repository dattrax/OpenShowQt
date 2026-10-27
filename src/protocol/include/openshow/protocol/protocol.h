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
// flowing. Minor 6 (task 6.15a, review R15) adds the media transport: the
// media.play/pause/seek/loop/gain/mute commands and the status fields
// (loop, duration, per-source gain/muted) that confirm them, named by
// "media.transport.v1" — all additive, so a minor-5 frontend still drives
// everything it knew. Minor 7 (task 6.15b, review R15) adds the standalone
// production track: the media.standalone selection command and the status
// "standalone" object (live, path, playing, position, duration, error,
// audio) that confirms it, named by "media.standalone.v1" — the track has
// its OWN lifetime (scene changes neither restart nor stop it) and its own
// status block, so the minor-6 background-transport fields keep meaning
// exactly what they did. Minor 8 (task 6.16a, review R16) adds the
// editor's off-air preview target: the preview.select command, the
// "target"/"request" fields every preview.ready now carries alongside the
// shm slot header's own copy of them (layout version 2), and the
// status.preview.selection block, named by "preview.select.v1". A frontend
// that ignores all three still mirrors the audience — the new ready fields
// are additive and the mirror's request is always 0 — but it cannot ask
// for a scene the audience is not shown, which is what the capability is
// for. preview.select's refusal is not a compatibility question: a worker
// that does not know the method answers "method not found", which is
// exactly the signal to keep the QML text representation instead.
// Minor 9 (task 6.16b, review R16) adds the slide list's thumbnails: the
// preview.thumbnails command, a third preview target ("thumbnail", 2) on
// that same stream, and the status.preview.thumbnail block, named by
// "preview.thumbnail.v1". It is additive in the same way minor 8 was —
// frames the older frontend never asked for are the only new traffic, and
// its own mirror keeps its request at 0 — but the capability is what a
// frontend must check before asking, because a worker too old for the
// method replies "method not found" and a client too old for target 2
// would read a thumbnail as the audience's picture. It does not: a worker
// draws a thumbnail only for a frontend that asked.
inline constexpr std::uint32_t kProtocolMajor = 0;
inline constexpr std::uint32_t kProtocolMinor = 9;

// preview.select {"revision":r,"request":g} (task 6.16a, review R16):
// render the PREPARED scene `r` into the running preview stream without
// presenting it, so the edit canvas can show the worker's own Skia picture
// of a slide that is not on air. `request` is the frontend's own
// generation counter and must come strictly after the last accepted one
// (modulo 2^32): replaying an old ask is refused rather than allowed to
// resurrect imagery of a scene the operator has moved past. Revision 0
// stands the target down. Every frame the stream carries names the target
// it was drawn for and the request it answers, and a consumer that has
// asked again since drops what it receives — so a stale edit is impossible
// by construction, not unlikely by timing. Nothing about the audience
// changes: no present, no scene apply, no crossfade advance, no transport
// command. output.status.preview.selection is the acknowledgement
// (active/revision/request, then published/dropped/deferred/stale), the
// 3.4 rule again — the accept says the ask was understood, the counters
// say what the render thread managed to draw. The method name itself lives
// beside the transport it travels on: openshow/protocol/preview_wire.h,
// next to preview.ready and preview.released.

// preview.thumbnails {"scenes":[{"key":"slide-7","scene":{...}}, ...],
//                     "width":160,"height":90,"request":g} (task 6.16b,
// review R16): render each scene as a picture of that size, into the same
// running preview stream, as target "thumbnail".
//
// The scenes travel as text, exactly the payload scene.prepare takes, and
// they are NOT prepared: they enter no store, pin no full-size decode,
// hold no revision anyone can present, and change nothing about what the
// audience is shown. That is the whole shape of the request. A list of
// slides is not show content — it is a picture of what a slide would look
// like — so it must not spend the 16-entry store or the pinned-decode
// budget that guarantee the operator can put a prepared scene on air.
// Pixels still never ride the socket in either direction.
//
// The batch REPLACES the pending set wholesale, which is what makes
// coalescing and cancellation one act: the newest list is the only thing
// the worker still owes, and a slide the operator has scrolled past has no
// render queued for it. `request` follows preview.select's rule (strictly
// after the accepted one, modulo 2^32); an empty `scenes` array is the
// stand-down, and it needs a new request like any other change.
//
// Bounds, enforced by both halves and declared once, in preview_wire.h: at
// most kMaxThumbnailBatch scenes, a key of 1..kMaxThumbnailKeyLength
// characters, each dimension in [kMinThumbnailDimension,
// kMaxThumbnailDimension] AND small enough that its pixels fit the slot
// they would be published into (thumbnailExtentFits). A batch over the cap
// is refused naming the cap; a scene that does not compile is refused
// per-entry in the ack without costing the other slides their pictures.
//
// The answer is {"accepted":true,"request":g,"width":w,"height":h,
// "frames":[{"key":...,"id":n},...],"refused":[{"key":...,"why":...}]} —
// and `id` is what identifies a thumbnail on the wire. These scenes have
// no prepared revision to name, so the worker assigns each accepted entry a
// per-slide id from its own counter, every frame it publishes carries that
// id in the slot header's `revision` field beside `request` = the batch
// generation, and the consumer maps id back to its own key. A frame whose
// request is not the newest batch the client sent is dropped before it is
// copied: a scrolled-past list's picture never lands under a visible row.
//
// output.status.preview.thumbnail is the acknowledgement — active/request/
// width/height, then queued (accepted, not yet drawn), decodePending (its
// background still being decoded on the command thread), drawn, dropped,
// deferred and stale — the 3.4 rule unchanged: the accept says the batch
// was understood, the counters say what the render thread got to.

// Worker-to-frontend notification (task 5.5d): a background media asset
// failed. {"path":...,"message":...} — sent when the failure happens,
// while output.status "media.error" carries the same text until a
// successful open replaces it (the frontend polls; it must not miss a
// failure that lands between polls).
inline constexpr const char* kMethodMediaError = "media.error";

// Frontend-to-worker media transport commands (task 6.15a, review R15).
// These graduate what the AvPlayer already did for the benchmark into
// session operations the operator drives over the socket. The contract
// they share:
//   media.play, media.pause      — no params; hold and resume the
//                                  transport (a held clip keeps its
//                                  frame and reports "paused").
//   media.seek   {"position":s}  — s >= 0, seconds; a refusal the audio
//                                  gives is the typed kSync media fault,
//                                  never a silently swallowed request.
//   media.loop   {"loop":on}     — live; the value at EOF decides
//                                  rewind or final-frame hold.
//   media.gain   {"source":n,"gain":g}   — g in [0,1].
//   media.mute   {"source":n,"mute":m}.
// Source names are the registered audio identities below; asking for one
// this transport does not have is a refused request naming it, not a
// silent no-op. Every command answers {"pending":true} — the ops are
// applied by the transport's own thread — and output.status.media is the
// acknowledgement (loop, duration, position, sources[].gain/muted), the
// same rule scenes and layers already follow: confirmation comes from
// what the audience actually has.
inline constexpr const char* kMethodMediaPlay = "media.play";
inline constexpr const char* kMethodMediaPause = "media.pause";
inline constexpr const char* kMethodMediaSeek = "media.seek";
inline constexpr const char* kMethodMediaLoop = "media.loop";
inline constexpr const char* kMethodMediaGain = "media.gain";
inline constexpr const char* kMethodMediaMute = "media.mute";

// media.standalone {"path":p} (task 6.15b, review R15): select the
// INDEPENDENT production audio track — a file of its own with a lifetime
// of its own. Scene changes neither restart nor stop it: it survives
// lyric updates, background-video replacement, and the background going
// away entirely. "" removes the track. The path is probed synchronously
// (nothing readable at that path is a refused request, and the old track
// keeps playing untouched); once accepted the swap runs on the
// transport's thread and output.status.media.standalone is the
// acknowledgement, the 3.4 rule again. Gain and mute for this source ride
// the minor-6 media.gain/media.mute commands with source "standalone";
// the loop mode the transport holds is always on (a production bed that
// falls silent at EOF is its own surprise — removal is the stop).
//
// output.status.media.standalone answers with {live, path, playing,
// position, duration, error, audio, loops}. "live" is the commandability
// mirror (the transport is up); "position" is the HEARD clock the transport
// paces itself by, so on a looping bed it runs on past "duration" instead
// of indexing the file — "loops" is how many times the source rewound,
// which is the only field that separates a bed that wrapped from one that
// ran out and left the sink pacing silence. "error" is sticky and always
// names this block's own path (the 5.5d pairing rule, read inside the
// block: the background's fields never describe this track and these never
// describe it).
inline constexpr const char* kMethodMediaStandalone = "media.standalone";

// Audio source identities on the wire (task 6.15a). "video" is the clip's
// embedded audio; "standalone" is the separate track — named now so the
// wire does not change when 6.15b gives it a production lifetime.
inline constexpr const char* kMediaSourceVideo = "video";
inline constexpr const char* kMediaSourceStandalone = "standalone";

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
