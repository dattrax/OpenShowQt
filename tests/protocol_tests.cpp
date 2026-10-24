/*
 * Checks the shared protocol library: the task 0.4 offer round-trips through
 * Boost.JSON (the one JSON library both processes encode with), and the task
 * 1.2 wire: length-prefixed framing (including fragmentation and hostile
 * inputs), the JSON-RPC 2.0 envelopes and the hello handshake negotiation.
 */

#include <boost/json.hpp>

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
        check(result.at("capabilities").as_array().size() == 5, "hello lists capabilities");
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
    testEnvelopes();
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
