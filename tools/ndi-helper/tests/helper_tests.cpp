/*
 * The helper's unit tests. No NDI SDK and no camera: the fake backend drives
 * the protocol, and the shared frame ring is tested over an ordinary buffer and
 * over a real POSIX segment. Exit status is non-zero when any check fails.
 *
 * Covered here:
 *   - JSON parse/write: objects, arrays, numbers, escapes, \u and surrogate
 *     pairs, duplicate keys, deep nesting, trailing bytes;
 *   - frame ring: layout self-consistency, write/read, newest-wins overwrite,
 *     a slot the writer is still filling, a too-small destination and corrupt
 *     headers;
 *   - protocol server with the fake backend: probe, list, status, open, close,
 *     unknown and malformed commands, version refusal, oversized lines, EOF;
 *   - a real mapped segment: the helper's published name and the newest frame.
 */

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fake_backend.h"
#include "json.h"
#include "openshow/live/frame_ring.h"
#include "protocol.h"

namespace {

bool gFailed = false;

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        gFailed = true;
    }
    return condition;
}

std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out;
    std::string current;
    for (const char c : text) {
        if (c == '\n') {
            out.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        out.push_back(current);
    }
    return out;
}

void testJson() {
    using openshow::ndi::json::Value;

    Value parsed;
    std::string error;
    check(openshow::ndi::json::parse(
              R"({"cmd":"open","source":"Camera \u00e9 \ud83d\ude00","waitMs":12.5,"ok":true,"none":null,"list":[1,2,3]})",
              &parsed, &error),
          "json: nested object parses");
    check(parsed.type == Value::Type::Object, "json: top level is an object");
    check(openshow::ndi::json::stringOr(parsed, "cmd") == "open", "json: string member");
    check(openshow::ndi::json::stringOr(parsed, "source") ==
              std::string("Camera \xC3\xA9 \xF0\x9F\x98\x80"),
          "json: \\u escapes decode to UTF-8 (surrogate pair included)");
    double wait = 0.0;
    check(openshow::ndi::json::numberOr(parsed, "waitMs", &wait) && wait == 12.5,
          "json: fractional number");
    check(openshow::ndi::json::boolOr(parsed, "ok", false), "json: boolean member");
    const Value* list = openshow::ndi::json::find(parsed, "list");
    check(list && list->type == Value::Type::Array && list->array.size() == 3,
          "json: array member");

    const std::string written = openshow::ndi::json::write(parsed);
    Value reparsed;
    check(openshow::ndi::json::parse(written, &reparsed, &error), "json: output reparses");
    check(openshow::ndi::json::write(reparsed) == written, "json: write is stable");

    Value tainted = Value::makeObject();
    tainted.object.push_back({"text", Value::makeString("a\"b\\c\n\r\t\x01")});
    const std::string escaped = openshow::ndi::json::write(tainted);
    Value roundTripped;
    check(openshow::ndi::json::parse(escaped, &roundTripped, &error),
          "json: escaped text reparses");
    check(openshow::ndi::json::stringOr(roundTripped, "text") == "a\"b\\c\n\r\t\x01",
          "json: escapes round trip");

    check(!openshow::ndi::json::parse(R"({"a":1,"a":2})", &parsed, &error),
          "json: duplicate keys are refused");
    check(!openshow::ndi::json::parse(R"json({"a":1}) trailing)json", &parsed, &error),
          "json: trailing bytes are refused");
    check(!openshow::ndi::json::parse("{\"a\":", &parsed, &error),
          "json: truncated input is refused");
    check(!openshow::ndi::json::parse("\"\\ud800\"", &parsed, &error),
          "json: unpaired surrogate is refused");

    std::string deep(64, '[');
    deep += "0";
    deep += std::string(64, ']');
    check(!openshow::ndi::json::parse(deep, &parsed, &error), "json: deep nesting is refused");

    check(Value::makeInteger(-7).text == "-7", "json: integer literal");
    check(Value::makeNumber(30.0).text == "30", "json: whole double prints without a dot");
}

void testFrameRing() {
    using namespace openshow::live;
    constexpr std::uint32_t width = 64;
    constexpr std::uint32_t height = 36;
    constexpr std::uint32_t stride = width * 4;
    const std::uint64_t bytes = frameRingBytes(stride, height);
    check(bytes == kFrameRingDataOffset + kFrameRingSlots * frameRingSlotBytes(stride, height),
          "ring: size formula adds the aligned slots");

    std::vector<std::uint8_t> mapping(static_cast<std::size_t>(bytes), 0);
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(stride) * height);
    std::vector<std::uint8_t> received(pixels.size());
    std::string error;

    FrameRing ring(mapping.data(), mapping.size());
    check(!ring.validate(&error), "ring: a blank mapping has no magic");

    check(ring.initForWrite(width, height, stride,
                            static_cast<std::uint32_t>(FrameFormat::BGRA8), &error),
          "ring: initialises for writing");
    check(ring.validate(&error), "ring: header validates after init");
    check(ring.header()->width == width && ring.header()->height == height &&
              ring.header()->stride == stride,
          "ring: header carries the frame size");

    FrameView out;
    check(!ring.read(&out, received.data(), received.size(), &error),
          "ring: nothing published yet");

    std::memset(pixels.data(), 0x11, pixels.size());
    FrameView first;
    first.data = pixels.data();
    first.width = width;
    first.height = height;
    first.stride = stride;
    first.format = static_cast<std::uint32_t>(FrameFormat::BGRA8);
    first.flags = kFrameFlagAlpha;
    first.timestampNs = 1000;
    check(ring.write(first, &error), "ring: writes a frame");

    check(ring.read(&out, received.data(), received.size(), &error), "ring: reads the frame");
    check(out.width == width && out.height == height && out.stride == stride,
          "ring: read reports the frame size");
    check(out.timestampNs == 1000, "ring: read reports the timestamp");
    check(received[0] == 0x11, "ring: pixels arrive");
    const std::uint64_t afterFirst = ring.header()->sequence;
    check(afterFirst >= 1, "ring: sequence advances");

    std::memset(pixels.data(), 0x22, pixels.size());
    FrameView second = first;
    second.timestampNs = 2000;
    check(ring.write(second, &error), "ring: overwrites with a newer frame");
    check(ring.read(&out, received.data(), received.size(), &error),
          "ring: reads the newer frame");
    check(received[0] == 0x22 && out.timestampNs == 2000, "ring: newest frame wins");
    check(ring.header()->sequence > afterFirst, "ring: sequence advances again");

    std::vector<std::uint8_t> small(16);
    check(!ring.read(&out, small.data(), small.size(), &error),
          "ring: a too-small destination is refused");

    // A slot the writer has not finished: the reader must not copy it.
    const std::uint32_t newestSlot = ring.header()->writeIndex;
    auto* slot = reinterpret_cast<FrameSlotHeader*>(
        mapping.data() + kFrameRingHeaderBytes +
        static_cast<std::size_t>(newestSlot) * kFrameRingSlotHeaderBytes);
    slot->state = 1;
    check(!ring.read(&out, received.data(), received.size(), &error),
          "ring: a slot being written is skipped");
    slot->state = 0;

    FrameView wrong = first;
    wrong.width = width + 4;
    check(!ring.write(wrong, &error), "ring: a changed frame size is refused");

    // UYVY is packed 4:2:2: two bytes per pixel, no alpha, and the stride rule
    // must follow the format rather than assuming BGRA.
    constexpr std::uint32_t uyvyStride = width * 2;
    const std::uint64_t uyvyBytes = frameRingBytes(uyvyStride, height);
    std::vector<std::uint8_t> uyvyMapping(static_cast<std::size_t>(uyvyBytes), 0);
    std::vector<std::uint8_t> uyvyPixels(static_cast<std::size_t>(uyvyStride) * height, 0x33);
    FrameRing uyvyRing(uyvyMapping.data(), uyvyMapping.size());
    check(uyvyRing.initForWrite(width, height, uyvyStride,
                                static_cast<std::uint32_t>(FrameFormat::UYVY), &error),
          "ring: initialises UYVY 4:2:2");
    FrameView uyvyFrame;
    uyvyFrame.data = uyvyPixels.data();
    uyvyFrame.width = width;
    uyvyFrame.height = height;
    uyvyFrame.stride = uyvyStride;
    uyvyFrame.format = static_cast<std::uint32_t>(FrameFormat::UYVY);
    uyvyFrame.flags = 0;
    check(uyvyRing.write(uyvyFrame, &error), "ring: writes UYVY");
    FrameView uyvyOut;
    std::vector<std::uint8_t> uyvyReceived(uyvyPixels.size());
    check(uyvyRing.read(&uyvyOut, uyvyReceived.data(), uyvyReceived.size(), &error),
          "ring: reads UYVY");
    check(uyvyOut.format == static_cast<std::uint32_t>(FrameFormat::UYVY) &&
              uyvyOut.stride == uyvyStride && uyvyReceived[0] == 0x33,
          "ring: UYVY bytes and stride survive");
    check((uyvyOut.flags & kFrameFlagAlpha) == 0, "ring: UYVY carries no alpha");

    FrameView changedFormat = uyvyFrame;
    changedFormat.format = static_cast<std::uint32_t>(FrameFormat::BGRA8);
    check(!uyvyRing.write(changedFormat, &error), "ring: a changed format is refused");

    std::vector<std::uint8_t> shortMapping(static_cast<std::size_t>(uyvyBytes), 0);
    FrameRing shortRing(shortMapping.data(), shortMapping.size());
    check(!shortRing.initForWrite(width, height, width,
                                  static_cast<std::uint32_t>(FrameFormat::UYVY), &error),
          "ring: a UYVY stride below two bytes per pixel is refused");
    check(!shortRing.initForWrite(width, height, uyvyStride, 99, &error),
          "ring: an unknown pixel format is refused");

    auto* header = reinterpret_cast<FrameRingHeader*>(mapping.data());
    const std::uint32_t savedVersion = header->version;
    header->version = savedVersion + 1;
    check(!ring.validate(&error), "ring: an unsupported version is refused");
    header->version = savedVersion;

    header->magic = 0;
    check(!ring.validate(&error), "ring: a bad magic is refused");
    header->magic = kFrameRingMagic;
    check(ring.validate(&error), "ring: the header is valid again");

    FrameRing unmapped(nullptr, 0);
    check(!unmapped.validate(&error), "ring: no mapping is refused");
}

void testProtocolBasics() {
    using namespace openshow::ndi;

    std::istringstream in(
        "{\"cmd\":\"probe\"}\n"
        "{\"cmd\":\"status\"}\n"
        "{\"cmd\":\"list\",\"waitMs\":10}\n"
        "{\"cmd\":\"open\",\"source\":\"Fake camera 1\"}\n"
        "{\"cmd\":\"status\"}\n"
        "{\"cmd\":\"close\"}\n"
        "{\"cmd\":\"status\"}\n"
        "{\"cmd\":\"nonsense\"}\n"
        "not json\n"
        "{\"cmd\":\"probe\",\"protocol\":99}\n"
        "{\"cmd\":\"probe\",\"protocol\":1}\n"
        "{\"cmd\":\"quit\"}\n");
    std::ostringstream out;

    FakeBackend backend;
    ProtocolServer server(backend, in, out);
    check(server.run() == 0, "protocol: run finishes cleanly");

    const auto replyLines = lines(out.str());
    check(replyLines.size() == 12, "protocol: one reply per command");
    for (const std::string& line : replyLines) {
        json::Value value;
        std::string error;
        if (!check(json::parse(line, &value, &error), "protocol: every stdout line is JSON")) {
            std::fprintf(stderr, "  offending line: %s\n", line.c_str());
        }
    }

    json::Value probe;
    check(json::parse(replyLines[0], &probe, nullptr), "protocol: probe reply parses");
    check(json::boolOr(probe, "ok", false), "protocol: probe succeeds");
    check(json::stringOr(probe, "helper") == kHelperVersion, "protocol: probe names the helper");
    check(json::stringOr(probe, "ndi") == "fake", "protocol: probe names the backend");
    double protocolVersion = 0.0;
    check(json::numberOr(probe, "protocol", &protocolVersion) &&
              static_cast<int>(protocolVersion) == kProtocolVersion,
          "protocol: probe reports the protocol version");

    json::Value status;
    check(json::parse(replyLines[1], &status, nullptr), "protocol: status reply parses");
    check(json::boolOr(status, "ok", false) && !json::boolOr(status, "open", true),
          "protocol: status starts closed");

    json::Value list;
    check(json::parse(replyLines[2], &list, nullptr), "protocol: list reply parses");
    const json::Value* sources = json::find(list, "sources");
    check(sources && sources->type == json::Value::Type::Array && sources->array.size() == 2,
          "protocol: list returns the fake sources");

    json::Value open;
    check(json::parse(replyLines[3], &open, nullptr), "protocol: open reply parses");
    check(json::boolOr(open, "ok", false), "protocol: open succeeds");
    const std::string shm = json::stringOr(open, "shm");
    check(shm.rfind("/openshow-ndi-", 0) == 0, "protocol: open names a shared segment");
    double width = 0.0;
    double height = 0.0;
    double stride = 0.0;
    check(json::numberOr(open, "width", &width) && width == 320.0, "protocol: open width");
    check(json::numberOr(open, "height", &height) && height == 180.0, "protocol: open height");
    check(json::numberOr(open, "stride", &stride) && stride == 320.0 * 4, "protocol: open stride");
    check(json::stringOr(open, "format") == "BGRA", "protocol: open format");

    json::Value streaming;
    check(json::parse(replyLines[4], &streaming, nullptr), "protocol: open status parses");
    check(json::boolOr(streaming, "open", false), "protocol: status reports the open source");
    check(json::stringOr(streaming, "source") == "Fake camera 1",
          "protocol: status echoes the source");

    json::Value closed;
    check(json::parse(replyLines[5], &closed, nullptr), "protocol: close reply parses");
    check(json::boolOr(closed, "ok", false), "protocol: close succeeds");
    check(!backend.streaming(), "protocol: close stops the backend");

    json::Value afterClose;
    check(json::parse(replyLines[6], &afterClose, nullptr), "protocol: closed status parses");
    check(!json::boolOr(afterClose, "open", true), "protocol: status is closed again");

    json::Value unknown;
    check(json::parse(replyLines[7], &unknown, nullptr), "protocol: unknown reply parses");
    check(!json::boolOr(unknown, "ok", true), "protocol: unknown command is refused");
    check(json::stringOr(unknown, "error").find("unknown command") != std::string::npos,
          "protocol: unknown command names the error");

    json::Value malformed;
    check(json::parse(replyLines[8], &malformed, nullptr), "protocol: malformed reply parses");
    check(!json::boolOr(malformed, "ok", true), "protocol: malformed JSON is refused");

    json::Value wrongVersion;
    check(json::parse(replyLines[9], &wrongVersion, nullptr),
          "protocol: version refusal parses");
    check(!json::boolOr(wrongVersion, "ok", true), "protocol: wrong version is refused");
    check(json::stringOr(wrongVersion, "error").find("protocol version") != std::string::npos,
          "protocol: wrong version is named");

    json::Value accepted;
    check(json::parse(replyLines[10], &accepted, nullptr), "protocol: matching version parses");
    check(json::boolOr(accepted, "ok", false), "protocol: matching version is accepted");

    json::Value quit;
    check(json::parse(replyLines[11], &quit, nullptr), "protocol: quit reply parses");
    check(json::boolOr(quit, "ok", false), "protocol: quit succeeds");
}

void testProtocolBounds() {
    using namespace openshow::ndi;

    std::string huge(ProtocolServer::kMaxLineBytes + 1024, 'x');
    std::istringstream in(huge + "\n" + "{\"cmd\":\"probe\"}\n");
    std::ostringstream out;
    FakeBackend backend;
    ProtocolServer server(backend, in, out);
    check(server.run() == 0, "bounds: run finishes after an oversized line");

    const auto replyLines = lines(out.str());
    check(replyLines.size() == 2, "bounds: oversized line gets one refusal");
    json::Value refused;
    check(json::parse(replyLines[0], &refused, nullptr), "bounds: refusal is JSON");
    check(!json::boolOr(refused, "ok", true), "bounds: oversized line is refused");
    check(json::stringOr(refused, "error").find("65536") != std::string::npos,
          "bounds: refusal names the limit");
    json::Value probe;
    check(json::parse(replyLines[1], &probe, nullptr), "bounds: next command still works");
    check(json::boolOr(probe, "ok", false), "bounds: server survived the oversized line");

    // EOF with a final unterminated command is a clean exit and still answered.
    std::istringstream eof("{\"cmd\":\"probe\"}");
    std::ostringstream eofOut;
    FakeBackend eofBackend;
    ProtocolServer eofServer(eofBackend, eof, eofOut);
    check(eofServer.run() == 0, "bounds: EOF ends the server");
    check(lines(eofOut.str()).size() == 1, "bounds: final unterminated line is answered");

    // EOF with no data at all is a clean exit.
    std::istringstream empty("");
    std::ostringstream emptyOut;
    FakeBackend emptyBackend;
    ProtocolServer emptyServer(emptyBackend, empty, emptyOut);
    check(emptyServer.run() == 0, "bounds: empty stdin is a clean exit");
    check(emptyOut.str().empty(), "bounds: empty stdin writes nothing");
}

void testMappedSegment() {
    using namespace openshow::ndi;
    using namespace openshow::live;

    FakeBackend backend;
    std::string error;
    OpenInfo info;
    check(backend.open("Fake camera 1", &info, &error), "segment: fake backend opens");
    check(!info.shmName.empty(), "segment: open names the segment");
    check(info.width == 320 && info.height == 180, "segment: fake frame size");

    OpenInfo refusedInfo;
    check(!backend.open("Fake camera 2", &refusedInfo, &error),
          "segment: a second open is refused");
    check(backend.streaming(), "segment: the refused open leaves the first source running");
    check(info.shmName.rfind("/openshow-ndi-", 0) == 0,
          "segment: the first source keeps its name");

    const int fd = ::shm_open(info.shmName.c_str(), O_RDONLY, 0);
    check(fd >= 0, "segment: the published name maps");
    struct stat status {};
    check(::fstat(fd, &status) == 0 && static_cast<std::uint64_t>(status.st_size) == info.bytes,
          "segment: the segment is the published size");
    void* map = ::mmap(nullptr, static_cast<std::size_t>(info.bytes), PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);
    check(map != MAP_FAILED, "segment: maps read-only");
    if (map != MAP_FAILED) {
        FrameRing ring(map, static_cast<std::size_t>(info.bytes));
        check(ring.validate(&error), "segment: the reader validates the header");

        std::vector<std::uint8_t> frame(static_cast<std::size_t>(info.stride) * info.height);
        FrameView view;
        bool got = false;
        for (int attempt = 0; attempt < 100 && !got; ++attempt) {
            got = ring.read(&view, frame.data(), frame.size(), &error);
            if (!got) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        check(got, "segment: a reader gets a frame");
        if (got) {
            check(view.width == info.width && view.height == info.height,
                  "segment: the frame matches the open dimensions");
            bool anyPixel = false;
            for (std::size_t i = 0; i + 3 < frame.size(); i += 4) {
                if (frame[i] != 0 || frame[i + 1] != 0 || frame[i + 2] != 0) {
                    anyPixel = true;
                    break;
                }
            }
            check(anyPixel, "segment: the frame carries a picture");
            const std::uint64_t firstSequence = ring.header()->sequence;
            bool advanced = false;
            for (int attempt = 0; attempt < 100 && !advanced; ++attempt) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                advanced = ring.header()->sequence > firstSequence;
            }
            check(advanced, "segment: the writer keeps producing frames");
        }
        ::munmap(map, static_cast<std::size_t>(info.bytes));
    }

    const std::string name = info.shmName;
    backend.close(nullptr);
    const int gone = ::shm_open(name.c_str(), O_RDONLY, 0);
    check(gone < 0, "segment: close unlinks the name");
    if (gone >= 0) {
        ::close(gone);
    }
}

}  // namespace


void testFakeUyvy() {
    using namespace openshow::ndi;
    FakeOptions options;
    options.uyvy = true;
    FakeBackend backend(options);
    std::string error;
    OpenInfo info;
    check(backend.open("Fake camera 1", &info, &error), "fake uyvy: opens");
    check(info.format == static_cast<std::uint32_t>(openshow::live::FrameFormat::UYVY),
          "fake uyvy: reports UYVY");
    check(info.stride == 640, "fake uyvy: stride is two bytes per pixel");
    check(info.bytes == openshow::live::frameRingBytes(info.stride, info.height),
          "fake uyvy: ring size matches the frame");

    const int fd = ::shm_open(info.shmName.c_str(), O_RDONLY, 0);
    check(fd >= 0, "fake uyvy: segment maps");
    if (fd >= 0) {
        void* map = ::mmap(nullptr, static_cast<std::size_t>(info.bytes), PROT_READ,
                           MAP_SHARED, fd, 0);
        ::close(fd);
        check(map != MAP_FAILED, "fake uyvy: maps read-only");
        if (map != MAP_FAILED) {
            openshow::live::FrameRing ring(map, static_cast<std::size_t>(info.bytes));
            std::vector<std::uint8_t> pixels(
                static_cast<std::size_t>(info.stride) * info.height);
            openshow::live::FrameView view;
            bool got = false;
            for (int attempt = 0; attempt < 100 && !got; ++attempt) {
                got = ring.read(&view, pixels.data(), pixels.size(), &error);
                if (!got) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
            check(got, "fake uyvy: a frame arrives");
            if (got) {
                check(view.format ==
                          static_cast<std::uint32_t>(openshow::live::FrameFormat::UYVY),
                      "fake uyvy: the slot is UYVY");
                check(view.stride == 640, "fake uyvy: the slot stride is packed");
                check((view.flags & openshow::live::kFrameFlagAlpha) == 0,
                      "fake uyvy: no alpha flag");
                bool picture = false;
                for (std::uint8_t value : pixels) {
                    if (value != 0) {
                        picture = true;
                        break;
                    }
                }
                check(picture, "fake uyvy: the slot carries a picture");
            }
            ::munmap(map, static_cast<std::size_t>(info.bytes));
        }
    }
    check(backend.close(&error), "fake uyvy: closes");
    const int gone = ::shm_open(info.shmName.c_str(), O_RDONLY, 0);
    check(gone < 0, "fake uyvy: close unlinks the segment");
    if (gone >= 0) {
        ::close(gone);
    }
}

void testFakeSourceLoss() {
    using namespace openshow::ndi;
    FakeOptions options;
    options.loseSourceAfterMs = 60;
    FakeBackend backend(options);
    std::mutex mutex;
    Event captured;
    bool got = false;
    backend.setEventSink([&](const Event& event) {
        std::lock_guard<std::mutex> lock(mutex);
        captured = event;
        got = true;
    });
    std::string error;
    OpenInfo info;
    check(backend.open("Fake camera 1", &info, &error), "fake loss: opens");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (got) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        check(got, "fake loss: emits an event");
        check(captured.kind == "source-lost", "fake loss: reports source-lost");
        check(captured.source == "Fake camera 1", "fake loss: names the source");
    }
    check(!backend.streaming(), "fake loss: stops streaming");
    openshow::ndi::ProbeInfo probe;
    check(backend.probe(&probe, &error) && probe.ndiVersion == "fake",
          "fake loss: the backend still answers probe");
    check(backend.close(&error), "fake loss: closes");
}

int main() {
    testJson();
    testFrameRing();
    testProtocolBasics();
    testProtocolBounds();
    testMappedSegment();
    testFakeUyvy();
    testFakeSourceLoss();

    if (gFailed) {
        std::fprintf(stderr, "helper_tests: FAILED\n");
        return 1;
    }
    std::printf("helper_tests: all checks passed\n");
    return 0;
}
