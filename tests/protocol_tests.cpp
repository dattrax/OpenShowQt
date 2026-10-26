/*
 * Checks the shared protocol library: the task 0.4 offer round-trips through
 * Boost.JSON (the one JSON library both processes encode with), and the task
 * 1.2 wire: length-prefixed framing (including fragmentation and hostile
 * inputs), the JSON-RPC 2.0 envelopes and the hello handshake negotiation.
 * Task 6.8a (review R11) added the transport's own limits: the encoded-size
 * budget from the SENDER's side, the parse-depth ceiling, and the envelope
 * rules that refuse an unanswerable id or an unusable method name.
 */

#include <boost/json.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

#include "openshow/protocol/preview_wire.h"
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

// Boost.JSON keeps small integers in its signed 64-bit kind, so a wire number
// can arrive as either kind depending on how it was produced.
std::uint64_t asInteger(const boost::json::value& value) {
    if (value.is_uint64()) {
        return value.as_uint64();
    }
    return static_cast<std::uint64_t>(value.as_int64());
}

void testOffer() {
    const std::string payload = proto::offerPayload();

    boost::json::value parsed;
    try {
        parsed = boost::json::parse(payload);
    } catch (const boost::system::system_error& error) {
        check(false, "offer is not valid JSON");
        std::fprintf(stderr, "  %s (%s)\n", error.what(), payload.c_str());
        return;
    }

    if (!check(parsed.is_object(), "offer is a JSON object")) {
        return;
    }
    const boost::json::object& offer = parsed.as_object();
    if (!check(offer.contains("application") && offer.contains("protocol"),
               "offer has application and protocol members")) {
        return;
    }
    check(offer.at("application").is_string() &&
              offer.at("application").as_string() == "openshow-worker",
          "application names the worker");

    const boost::json::array& versions = offer.at("protocol").as_array();
    if (check(versions.size() == 2, "protocol holds a major and minor pair")) {
        check(asInteger(versions[0]) == proto::kProtocolMajor, "protocol major matches");
        check(asInteger(versions[1]) == proto::kProtocolMinor, "protocol minor matches");
    }
}

void testFrameRoundTrip() {
    boost::json::object message = proto::makeRequest(7, "output.status");

    std::string wire;
    proto::appendFrame(message, &wire);
    check(wire.size() >= 4, "framed message carries a length prefix");
    if (wire.size() >= 4) {
        const auto bytes = reinterpret_cast<const unsigned char*>(wire.data());
        const std::uint32_t length = static_cast<std::uint32_t>(bytes[0]) |
                                     (static_cast<std::uint32_t>(bytes[1]) << 8) |
                                     (static_cast<std::uint32_t>(bytes[2]) << 16) |
                                     (static_cast<std::uint32_t>(bytes[3]) << 24);
        check(length == wire.size() - 4, "prefix equals the payload length");
    }

    proto::Decoder decoder;
    decoder.feed(wire);
    boost::json::value out;
    std::string error;
    if (check(decoder.next(&out, &error), "round-trip decodes one message")) {
        check(out == boost::json::value(message), "decoded message equals the input");
    }
    check(!decoder.next(&out, &error), "no phantom second message");
    check(!decoder.failed(), "round trip leaves the decoder healthy");
}

void testFragmentedAndBatchedDelivery() {
    std::string wire;
    proto::appendFrame(proto::makeRequest(1, "shutdown"), &wire);
    proto::appendFrame(proto::makeResult(2, boost::json::object{{"ok", true}}), &wire);

    // Byte-at-a-time: no message pops until the final byte arrives.
    proto::Decoder decoder;
    boost::json::value out;
    std::string error;
    std::size_t popped = 0;
    for (std::size_t i = 0; i < wire.size(); ++i) {
        decoder.feed(std::string_view(wire).substr(i, 1));
        while (decoder.next(&out, &error)) {
            ++popped;
        }
    }
    check(popped == 2, "fragmented delivery yields both messages in order");
    check(!decoder.failed(), "fragmentation is not a protocol violation");

    // Everything at once: both messages pop from the same buffer.
    proto::Decoder batched;
    batched.feed(wire);
    if (check(batched.next(&out, &error), "first batched message pops")) {
        check(out.is_object() && out.as_object().contains("method"),
              "first batched message is the request");
    }
    if (check(batched.next(&out, &error), "second batched message pops")) {
        check(out.is_object() && out.as_object().contains("result"),
              "second batched message is the response");
    }
    check(!batched.next(&out, &error), "batched decoder is drained");
}

void testHostileFraming() {
    boost::json::value out;
    std::string error;

    {  // Length above the cap.
        proto::Decoder decoder;
        std::string wire;
        proto::appendFrame(boost::json::object{{"a", 1}}, &wire);
        wire[3] = static_cast<char>(0xFF);  // push the length past 1 MiB
        decoder.feed(wire);
        check(!decoder.next(&out, &error), "oversize length is refused");
        check(decoder.failed(), "oversize poisons the decoder");
    }
    {  // Zero length.
        proto::Decoder decoder;
        decoder.feed(std::string("\0\0\0\0", 4));
        check(!decoder.next(&out, &error), "zero length is refused");
        check(decoder.failed(), "zero length poisons the decoder");
    }
    {  // Valid length, garbage payload.
        proto::Decoder decoder;
        const std::string garbage = "{not json";
        std::string wire;
        wire.push_back(static_cast<char>(garbage.size()));
        wire.push_back(0);
        wire.push_back(0);
        wire.push_back(0);
        wire.append(garbage);
        decoder.feed(wire);
        check(!decoder.next(&out, &error), "malformed JSON is refused");
        check(decoder.failed(), "malformed JSON poisons the decoder");
        check(!decoder.next(&out, &error), "poisoned decoder stays failed");
    }
}

void testEnvelopes() {
    const boost::json::object request = proto::makeRequest(3, "layer.setEnabled");
    const boost::json::object result =
        proto::makeResult(3, boost::json::object{{"revision", 9}});
    const boost::json::object error =
        proto::makeError(4, proto::kMethodNotFound, "no such method");

    check(request.at("jsonrpc").as_string() == "2.0", "request carries jsonrpc 2.0");
    check(asInteger(request.at("id")) == 3 && request.at("method").as_string() ==
              "layer.setEnabled",
          "request carries id and method");
    check(result.at("id") == request.at("id") && result.at("result").is_object(),
          "result echoes the id");
    check(error.at("error").as_object().at("code").as_int64() == -32601,
          "error carries the JSON-RPC code");
    check(!error.at("error").as_object().contains("data"),
          "omitted data stays absent");
    const boost::json::object withData = proto::makeError(
        5, proto::kVersionMismatch, "mismatch", boost::json::object{{"worker", 0}});
    check(withData.at("error").as_object().contains("data"),
          "provided data is attached");

    check(proto::looksLikeJsonRpc(request), "a request looks like JSON-RPC");
    check(proto::looksLikeJsonRpc(result), "a response looks like JSON-RPC");
    check(!proto::looksLikeJsonRpc(boost::json::object{{"id", 1}}),
          "a bare id object is not JSON-RPC");
    boost::json::object wrongVersion = request;
    wrongVersion["jsonrpc"] = "1.0";
    check(!proto::looksLikeJsonRpc(wrongVersion), "wrong jsonrpc version is rejected");
}

void testHello() {
    boost::json::object result;
    std::string message;
    boost::json::value data;

    // Happy path: same major, any minor.
    proto::ErrorCode code = proto::kInternalError;
    if (check(proto::handleHello(proto::helloParams("openshow-ui", 0, 0), "0.1.0", &result,
                                 &code, &message, &data),
              "hello with the worker major succeeds")) {
        check(result.at("application").as_string() == "openshow-worker",
              "hello result names the worker");
        check(result.at("version").as_string() == "0.1.0",
              "hello result carries the version");
        check(result.at("protocol").as_object().at("major").is_number(),
              "hello result negotiates a protocol");
        check(result.at("capabilities").as_array().size() == 8, "hello lists capabilities");
        {  // Task 5.5d's media visibility is named by a capability.
            const auto& caps = result.at("capabilities").as_array();
            const bool hasMedia = std::any_of(
                caps.begin(), caps.end(), [](const boost::json::value& v) {
                    return v.is_string() && v.as_string() == "media.error.v1";
                });
            check(hasMedia, "capabilities name media.error.v1");
        }
        {  // Task 6.7c's typed audio lane is named by one too (R18).
            const auto& caps = result.at("capabilities").as_array();
            const bool hasAudioState = std::any_of(
                caps.begin(), caps.end(), [](const boost::json::value& v) {
                    return v.is_string() && v.as_string() == "media.audio-state.v1";
                });
            check(hasAudioState, "capabilities name media.audio-state.v1");
        }
        {  // Task 6.5's stream-identified preview release is named too.
            const auto& caps = result.at("capabilities").as_array();
            const bool hasStreamId = std::any_of(
                caps.begin(), caps.end(), [](const boost::json::value& v) {
                    return v.is_string() && v.as_string() == "preview.stream-id.v1";
                });
            check(hasStreamId, "capabilities name preview.stream-id.v1");
        }
    }

    // Major mismatch: structured refusal with both versions.
    code = proto::kInternalError;
    result.clear();
    data = nullptr;
    const bool mismatchRefused = !proto::handleHello(
        proto::helloParams("openshow-ui", 41, 0), "0.1.0", &result, &code, &message, &data);
    check(mismatchRefused && code == proto::kVersionMismatch, "foreign major is refused");
    check(data.is_object() && data.as_object().contains("worker") &&
              data.as_object().contains("requested"),
          "version mismatch carries both version pairs");

    // Malformed params.
    code = proto::kInternalError;
    check(!proto::handleHello(boost::json::value("hi"), "0.1.0", &result, &code, &message) &&
              code == proto::kInvalidParams,
          "non-object hello params are refused");
    code = proto::kInternalError;
    check(!proto::handleHello(boost::json::object{{"client", "x"}}, "0.1.0", &result, &code,
                              &message) &&
              code == proto::kInvalidParams,
          "hello without a protocol object is refused");
    code = proto::kInternalError;
    check(!proto::handleHello(boost::json::object{{"protocol",
                                                   boost::json::object{{"major", "zero"}}}},
                              "0.1.0", &result, &code, &message) &&
              code == proto::kInvalidParams,
          "non-numeric versions are refused");
}

// --- task 6.8a: the transport's own limits (review R11) --------------------

// A JSON object whose serialised payload is exactly `wanted` bytes. The
// boundary has to be built at the byte rather than estimated: "roughly a MiB"
// tests nothing about a limit stated in bytes. The filler is 'x', which JSON
// never escapes, so the serialised size grows exactly with the string.
boost::json::object payloadOfExactSize(std::size_t wanted) {
    boost::json::object message{{"jsonrpc", "2.0"},
                                {"id", 1},
                                {"method", "scene.prepare"},
                                {"params",
                                 boost::json::object{
                                     {"scene", boost::json::object{{"text", ""}}}}}};
    const std::size_t overhead = boost::json::serialize(message).size();
    if (wanted > overhead) {
        message["params"].as_object()["scene"].as_object()["text"] =
            std::string(wanted - overhead, 'x');
    }
    return message;
}

void testSendSideBudget() {
    const std::size_t limit = proto::kMaxMessageBytes;

    // The peer's decoder accepts payloads of 1...limit, so exactly `limit` is
    // the last frame a sender is allowed to build. It has to be accepted: a
    // budget that refused its own boundary would be a smaller budget than the
    // one the receiver implements.
    boost::json::object atLimitValue = payloadOfExactSize(limit);
    const std::size_t serialised = boost::json::serialize(atLimitValue).size();
    check(serialised == limit, "the at-limit fixture is exactly kMaxMessageBytes serialised");

    proto::Decoder peer;
    std::string wire;
    check(proto::appendFrame(atLimitValue, &wire), "a payload at the limit is framed");
    check(wire.size() == limit + 4, "the frame is the payload plus the length prefix");
    boost::json::value out;
    std::string error;
    peer.feed(wire);
    check(peer.next(&out, &error), "the peer decodes the largest legal frame");
    check(out == boost::json::value(atLimitValue), "the largest legal frame survives intact");

    // One byte over. This is R11's actual bug: the old sender serialised,
    // cast the size to uint32 and handed over a frame the receiver answered by
    // hanging up, so an operator pasting large text lost the worker instead of
    // being told the text is too big.
    std::string untouched = "PREVIOUS-BYTES";
    const boost::json::object overLimitValue = payloadOfExactSize(limit + 1);
    check(boost::json::serialize(overLimitValue).size() == limit + 1,
          "the over-limit fixture is exactly one byte over");
    check(!proto::appendFrame(overLimitValue, &untouched),
          "a payload over the limit is refused by the SENDER");
    check(untouched == "PREVIOUS-BYTES",
          "a refused appendFrame writes nothing into the caller's buffer");

    // Refusing is a decision about ONE message, not about the connection: the
    // same buffer and the same peer still carry everything afterwards.
    std::string outbound = untouched;
    check(proto::appendFrame(proto::makeRequest(9, "output.status"), &outbound),
          "a refused message does not stop the next one being framed");
    peer.feed(std::string_view(outbound).substr(untouched.size()));
    if (check(peer.next(&out, &error), "the message after a refusal still decodes")) {
        check(out.as_object().at("method").as_string() == "output.status",
              "and decodes as the message that was actually built");
    }
    check(!peer.failed(), "an oversized message never reached the peer's decoder");
}

// Container levels in a JSON value, counted the way Boost's parser counts them:
// a scalar is 0, an object or array is one more than its deepest child. The
// transport's kMaxParseDepth is stated in these units, and the check below
// pins that a real scene message stays inside it.
std::size_t depthOf(const boost::json::value& value) {
    std::size_t deepest = 0;
    if (value.is_object()) {
        for (const auto& entry : value.as_object()) {
            deepest = std::max(deepest, depthOf(entry.value()));
        }
        return 1 + deepest;
    }
    if (value.is_array()) {
        for (const auto& element : value.as_array()) {
            deepest = std::max(deepest, depthOf(element));
        }
        return 1 + deepest;
    }
    return 0;
}

void testParseDepthLimit() {
    // Nesting is what a hostile payload still has once it fits the byte budget:
    // eight braces cost eight bytes and eight levels of allocator work, so a
    // megabyte of them is a megabyte of structure no handler will ever read.
    const std::size_t limit = proto::kMaxParseDepth;
    auto nested = [](std::size_t depth) {
        return std::string(depth, '[') + std::string(depth, ']');
    };
    auto frameOf = [](const std::string& payload) {
        std::string wire;
        wire.push_back(static_cast<char>(payload.size() & 0xFF));
        wire.push_back(static_cast<char>((payload.size() >> 8) & 0xFF));
        wire.push_back(0);
        wire.push_back(0);
        wire.append(payload);
        return wire;
    };

    boost::json::value out;
    std::string error;
    proto::Decoder exact;
    exact.feed(frameOf(nested(limit)));
    check(exact.next(&out, &error), "nesting exactly at the limit parses");
    check(!exact.failed(), "the limit is allowed, not merely survived");

    proto::Decoder tooDeep;
    tooDeep.feed(frameOf(nested(limit + 1)));
    check(!tooDeep.next(&out, &error), "nesting past the limit is refused");
    check(tooDeep.failed(), "an over-deep frame poisons the connection like garbage");
    check(error.find("deep") != std::string::npos,
          "and says depth is what it refused, not 'malformed'");

    // The limit has to be above the deepest message this protocol really
    // sends, which scene_tests measures against the compiled scene (five
    // levels: params, items, item, lines). Here is the same shape by hand, so
    // a change to either number that puts them the wrong way round fails.
    const boost::json::object deepestLegit{
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "scene.prepare"},
        {"params",
         boost::json::object{
             {"showId", "s"},
             {"items",
              boost::json::array{boost::json::object{{"lines", boost::json::array{"word"}}}}}}}};
    check(depthOf(deepestLegit) < limit,
          "a scene.prepare is inside the parse depth the transport allows");
    std::string wire;
    proto::Decoder peer;
    if (check(proto::appendFrame(deepestLegit, &wire), "the deepest legal shape frames")) {
        peer.feed(wire);
        check(peer.next(&out, &error), "and the transport's parser accepts it");
    }
    std::printf("   depth: deepest supported scene message %zu levels, limit %zu\n",
                depthOf(deepestLegit), limit);
}

void testEchoClipping() {
    // The reply that says "that was too big" must not itself be too big.
    const std::string shortText = "clock";
    check(proto::clippedForEcho(shortText) == shortText, "short text is echoed unchanged");

    const std::string exact(proto::kMaxEchoedTextBytes, 'a');
    check(proto::clippedForEcho(exact).size() == proto::kMaxEchoedTextBytes,
          "text exactly at the budget is echoed unchanged");

    const std::string longText(proto::kMaxEchoedTextBytes * 40, 'b');
    const std::string clipped = proto::clippedForEcho(longText);
    check(clipped.size() == proto::kMaxEchoedTextBytes, "an over-long echo is cut to budget");
    check(clipped.size() <= proto::kMaxEchoedTextBytes,
          "an echoed clip can never outgrow the budget it reports");
    check(clipped.size() >= 3 && clipped.substr(clipped.size() - 3) == "...",
          "an elided clip says so");

    // The cut must land between characters. 'é' is two bytes and an emoji is
    // four, so a budget that stops on a continuation byte of one hands the peer
    // invalid UTF-8 in the very message explaining the problem. The probe puts a
    // multi-byte character every fifth byte, so whichever budget is tried the
    // cut lands somewhere interesting rather than in a run of ASCII.
    auto wellFormedUtf8 = [](const std::string& cut) {
        std::size_t index = 0;
        bool wellFormed = true;
        while (index < cut.size()) {
            const unsigned char lead = static_cast<unsigned char>(cut[index]);
            std::size_t width = 1;
            if ((lead & 0xE0u) == 0xC0u) {
                width = 2;
            } else if ((lead & 0xF0u) == 0xE0u) {
                width = 3;
            } else if ((lead & 0xF8u) == 0xF0u) {
                width = 4;
            }
            if (index + width > cut.size()) {
                wellFormed = false;  // a character cut in half
                break;
            }
            for (std::size_t part = 1; part < width; ++part) {
                if ((static_cast<unsigned char>(cut[index + part]) & 0xC0u) != 0x80u) {
                    wellFormed = false;
                }
            }
            index += width;
        }
        return wellFormed;
    };
    std::string probe;
    for (int byte = 0; byte < 60; ++byte) {
        probe += byte % 5 == 4 ? "é" : "x";
    }
    probe += "\U0001F3A4";  // a four-byte character, at the far end
    probe += std::string(60, 'y');
    for (std::size_t budget = 3; budget < probe.size(); ++budget) {
        const std::string cut = proto::clippedForEcho(probe, budget);
        check(cut.size() <= budget, "a clipped echo fits the budget it was given");
        check(wellFormedUtf8(cut), "clipping never splits a UTF-8 character");
    }
    check(wellFormedUtf8(proto::clippedForEcho(std::string(500, 'x') + "é" +
                                                  std::string(500, 'y'))),
          "and the default budget never splits one either");
}

bool envelopeRefused(const boost::json::value& message, proto::ErrorCode expected,
                     std::string* text = nullptr, std::int64_t* idOut = nullptr) {
    proto::RequestEnvelope envelope;
    proto::ErrorCode code = proto::kInternalError;
    std::string reason;
    bool drops = false;
    const bool ok = proto::parseRequestEnvelope(message, &envelope, &code, &reason, &drops);
    if (ok || code != expected) {
        return false;
    }
    if (drops) {
        return false;  // these are all answerable problems, not stream failures
    }
    if (text != nullptr) {
        *text = reason;
    }
    if (idOut != nullptr) {
        *idOut = envelope.id;
    }
    return true;
}

void testRequestEnvelope() {
    // The envelope borrows from the value it was handed, so the caller keeps it.
    const boost::json::value requestValue =
        proto::makeRequest(3, "layer.setEnabled", boost::json::object{{"layer", "clock"}});
    proto::RequestEnvelope envelope;
    proto::ErrorCode code = proto::kInternalError;
    std::string text;
    bool drops = true;

    if (check(proto::parseRequestEnvelope(requestValue, &envelope, &code, &text, &drops),
              "a normal request passes the envelope gate")) {
        check(envelope.hasId && envelope.id == 3, "and its id is carried for the reply");
        check(envelope.method == "layer.setEnabled", "and its method too");
        check(envelope.params != nullptr && envelope.params->is_object(),
              "and its params are handed to the handler");
        check(!drops, "a good request needs no verdict on the connection");
    }

    auto parse = [](const char* literal) {
        boost::system::error_code error;
        return boost::json::parse(literal, error);
    };

    // The id decides whether an answer is allowed at all, so an id that cannot
    // be answered honestly is refused rather than narrowed. Narrowing is the
    // dangerous direction: replies are correlated by id (task 6.3a), so a value
    // that lands on some OTHER request's id applies the wrong acknowledgement.
    std::int64_t answeredUnder = -1;
    std::string reason;
    check(envelopeRefused(parse(R"({"jsonrpc":"2.0","id":"7","method":"status"})"),
                          proto::kInvalidRequest, &reason, &answeredUnder),
          "a string id is refused");
    check(answeredUnder == 0, "and the answer goes out under id 0, not a guessed one");
    check(reason.find("\"7\"") != std::string::npos,
          "and the reason quotes the id the client actually sent");
    check(reason.size() < proto::kMaxEchoedTextBytes + 200,
          "and the reason stays a sentence, not a copy of the payload");
    check(envelopeRefused(parse(R"({"jsonrpc":"2.0","id":7.5,"method":"status"})"),
                          proto::kInvalidRequest, &reason),
          "a fractional id is refused rather than truncated");
    check(envelopeRefused(parse(R"({"jsonrpc":"2.0","id":true,"method":"status"})"),
                          proto::kInvalidRequest, &reason),
          "a boolean id is refused");
    check(envelopeRefused(parse(R"({"jsonrpc":"2.0","id":null,"method":"status"})"),
                          proto::kInvalidRequest, &reason),
          "a null id is refused");
    check(envelopeRefused(
              parse(R"({"jsonrpc":"2.0","id":18446744073709551615,"method":"status"})"),
              proto::kInvalidRequest, &reason, &answeredUnder),
          "an id past the signed 64-bit range is refused, not wrapped");
    check(answeredUnder == 0, "and nothing is answered under the wrapped value");

    // The whole signed range is usable, which a naive is_uint64()-only reader
    // silently broke for every id a Python client sent as a positive integer.
    if (check(proto::parseRequestEnvelope(
                  parse(R"({"jsonrpc":"2.0","id":9223372036854775807,"method":"status"})"),
                  &envelope, &code, &text, &drops),
              "the largest signed id is accepted")) {
        check(envelope.id == 9223372036854775807LL, "and carried without loss");
    }
    if (check(proto::parseRequestEnvelope(
                  parse(R"({"jsonrpc":"2.0","id":4294967296,"method":"status"})"), &envelope,
                  &code, &text, &drops),
              "an id above 32 bits is accepted")) {
        check(envelope.id == 4294967296, "and does not arrive truncated to 0");
    }

    // A response, sent as if it were a request: answered under its own id,
    // which is what the client that sent it can correlate.
    check(envelopeRefused(parse(R"({"jsonrpc":"2.0","id":5,"result":{}})"),
                          proto::kInvalidRequest, &reason, &answeredUnder),
          "a bare response is refused");
    check(answeredUnder == 5, "and refused under the id it carried");

    // The method name: routing indexes on it, and the reply quotes it.
    check(envelopeRefused(parse(R"({"jsonrpc":"2.0","id":1,"method":7})"),
                          proto::kInvalidRequest, &reason),
          "a non-string method is refused");
    check(envelopeRefused(parse(R"({"jsonrpc":"2.0","id":1,"method":""})"),
                          proto::kInvalidRequest, &reason),
          "an empty method is refused");
    const std::string hugeName =
        R"({"jsonrpc":"2.0","id":1,"method":")" + std::string(2000, 'm') + R"("})";
    check(envelopeRefused(parse(hugeName.c_str()), proto::kInvalidRequest, &reason),
          "a 2000-byte method is refused");
    const std::string longestName = R"({"jsonrpc":"2.0","id":1,"method":")" +
                                    std::string(proto::kMaxMethodNameBytes, 'm') + R"("})";
    check(proto::parseRequestEnvelope(parse(longestName.c_str()), &envelope, &code, &text),
          "a method of exactly the longest legal length is accepted");
    const std::string oneTooLong = R"({"jsonrpc":"2.0","id":1,"method":")" +
                                   std::string(proto::kMaxMethodNameBytes + 1, 'm') + R"("})";
    check(envelopeRefused(parse(oneTooLong.c_str()), proto::kInvalidRequest, &reason),
          "and one byte more is not");

    // Not JSON-RPC at all: a stream this worker cannot reason about, which is
    // the one case that ends the connection.
    proto::ErrorCode wrongCode = proto::kInternalError;
    for (const char* foreign : {R"({"id":1,"method":"status"})",
                                R"({"jsonrpc":"1.0","id":1,"method":"status"})",
                                R"([1,2,3])",
                                R"("hello")"}) {
        drops = false;
        const bool refused =
            !proto::parseRequestEnvelope(parse(foreign), &envelope, &wrongCode, &text, &drops);
        check(refused && drops,
              "a message without the version stamp is refused AND ends the stream");
    }

    // Notifications: no id means nothing may be answered. The one method this
    // protocol documents as fire-and-forget is preview.released; every other
    // no-id message is something a client sent by mistake, and executing a
    // command that can never be acknowledged is task 6.3a's lost update.
    if (check(proto::parseRequestEnvelope(
                  proto::makeNotification(proto::kMethodPreviewReleased,
                                          boost::json::object{{"slot", 1}}),
                  &envelope, &code, &text),
              "a preview.released notification passes the gate")) {
        check(!envelope.hasId, "and is recognised as one that may not be answered");
    }
    if (check(proto::parseRequestEnvelope(proto::makeNotification("scene.present"), &envelope,
                                          &code, &text),
              "a notification of any other method also passes the SHAPE gate")) {
        check(!envelope.hasId, "because it has no id either");
    }
    check(proto::isNotificationMethod(proto::kMethodPreviewReleased),
          "preview.released is a documented notification");
    check(!proto::isNotificationMethod("scene.present"),
          "scene.present is not: it must be a request or be ignored");
    check(!proto::isNotificationMethod("scene.prepare"), "nor is scene.prepare");
    check(!proto::isNotificationMethod(""), "nor is the empty method");
}

}  // namespace

void testNotification() {
    const boost::json::object notification = proto::makeNotification(
        proto::kMethodPreviewReady, boost::json::object{{"slot", 1}, {"seq", 8}});
    check(notification.at("jsonrpc").as_string() == "2.0",
          "notification carries jsonrpc 2.0");
    check(notification.at("method").as_string() == "preview.ready",
          "notification carries the method");
    check(!notification.contains("id"), "a notification has no id to answer");
    check(asInteger(notification.at("params").as_object().at("seq")) == 8,
          "notification params travel intact");
    check(proto::looksLikeJsonRpc(notification),
          "a notification looks like JSON-RPC (method, no id needed)");
}

void testPreviewWire() {
    // The layout both processes compile must stay exactly this geometry,
    // because two processes index into one shared mapping with it.
    check(sizeof(proto::PreviewSegmentHeader) == 64, "segment header is 64 bytes");
    check(sizeof(proto::PreviewSlotHeader) == 64, "slot header is 64 bytes");
    check(proto::kPreviewSlotCount == 3, "three slots are agreed");

    check(proto::previewSlotStrideBytes(320, 240) == 64 + 320u * 240u * 4u,
          "slot stride is header plus RGBA payload");
    check(proto::previewSegmentBytes(320, 240) ==
              64 + 3 * proto::previewSlotStrideBytes(320, 240),
          "segment is the fixed header plus three slot strides");
    check(proto::previewSlotOffset(0, 100) == 64, "slot zero starts after the segment header");
    check(proto::previewSlotOffset(2, 100) == 64 + 2 * 100, "slot offsets step by strides");

    // Defaults match what the producer writes at creation.
    proto::PreviewSlotHeader header;
    header.seq = 2;
    check(header.format == proto::kPreviewFourccRGBA8, "slots default to RGBA");
    check(header.seq == 2 && (header.seq & 1) == 0, "even seq means a stable frame");
}

int main() {
    testOffer();
    testFrameRoundTrip();
    testFragmentedAndBatchedDelivery();
    testHostileFraming();
    testSendSideBudget();
    testParseDepthLimit();
    testEchoClipping();
    testEnvelopes();
    testRequestEnvelope();
    testHello();
    testNotification();
    testPreviewWire();

    if (gFailures != 0) {
        std::fprintf(stderr, "%d protocol check(s) failed\n", gFailures);
        return 1;
    }
    std::printf("ok: protocol framing, envelopes, handshake and preview wire\n");
    return 0;
}
