/*
 * ND.3: the shared frame-ring layout, pinned from the application side as well
 * as the helper's own tests. Layout and alignment are static; the runtime
 * checks cover publishing, newest-wins filtering, a slot the writer is still
 * filling, a destination that is too small, and corrupt headers.
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "live/shm_frame_reader.h"
#include "openshow/live/frame_ring.h"

namespace {

bool gFailed = false;

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        gFailed = true;
    }
    return condition;
}

}  // namespace

int main() {
    using namespace openshow::live;

    static_assert(sizeof(FrameRingHeader) == 128, "frame ring header is 128 bytes");
    static_assert(sizeof(FrameSlotHeader) == 64, "frame slot header is 64 bytes");
    static_assert(kFrameRingDataOffset == 320, "pixel data starts after the headers");
    static_assert(alignof(FrameRingHeader) >= 8, "header is 8-byte aligned");
    static_assert(alignof(FrameSlotHeader) >= 8, "slot header is 8-byte aligned");

    constexpr std::uint32_t width = 64;
    constexpr std::uint32_t height = 36;
    constexpr std::uint32_t stride = width * 4;
    const std::uint64_t bytes = frameRingBytes(stride, height);
    check(frameRingSlotBytes(stride, height) % kFrameRingAlign == 0,
          "ring: a slot is aligned");
    check(bytes == kFrameRingDataOffset +
                       kFrameRingSlots * frameRingSlotBytes(stride, height),
          "ring: total size is headers plus aligned slots");

    std::vector<std::uint8_t> mapping(static_cast<std::size_t>(bytes), 0);
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(stride) * height);
    Frame frame;
    std::string error;

    FrameRing ring(mapping.data(), mapping.size());
    check(!ring.validate(&error), "ring: a blank mapping is refused");

    check(ring.initForWrite(width, height, stride,
                            static_cast<std::uint32_t>(FrameFormat::BGRA8), &error),
          "ring: initialises");
    check(ring.validate(&error), "ring: the header validates");
    check(readNewestFrame(ring, 0, &frame, &error) == FrameStatus::NoFrame,
          "ring: nothing is published yet");

    std::memset(pixels.data(), 0x11, pixels.size());
    FrameView first;
    first.data = pixels.data();
    first.width = width;
    first.height = height;
    first.stride = stride;
    first.format = static_cast<std::uint32_t>(FrameFormat::BGRA8);
    first.flags = kFrameFlagAlpha;
    first.timestampNs = 1000;
    check(ring.write(first, &error), "ring: publishes a frame");

    check(readNewestFrame(ring, 0, &frame, &error) == FrameStatus::Frame,
          "ring: reads the published frame");
    check(frame.width == width && frame.height == height && frame.stride == stride,
          "ring: the frame carries its dimensions");
    check(frame.pixels.size() == static_cast<std::size_t>(stride) * height,
          "ring: the copy is the whole frame");
    check(frame.pixels[0] == 0x11, "ring: pixels are copied");
    check(frame.sequence >= 1, "ring: the sequence is published");
    check(frame.alpha(), "ring: the alpha flag survives");
    check(readNewestFrame(ring, frame.sequence, &frame, &error) == FrameStatus::NoFrame,
          "ring: the same sequence is not re-read");

    std::memset(pixels.data(), 0x22, pixels.size());
    FrameView second = first;
    second.timestampNs = 2000;
    check(ring.write(second, &error), "ring: publishes a newer frame");
    check(readNewestFrame(ring, 0, &frame, &error) == FrameStatus::Frame,
          "ring: reads the newer frame");
    check(frame.pixels[0] == 0x22 && frame.timestampNs == 2000,
          "ring: the newest frame wins");

    // A slot the writer has not finished must not be copied.
    const std::uint32_t newest = ring.header()->writeIndex;
    auto* slot = reinterpret_cast<FrameSlotHeader*>(
        mapping.data() + kFrameRingHeaderBytes +
        static_cast<std::size_t>(newest) * kFrameRingSlotHeaderBytes);
    slot->state = 1;
    check(readNewestFrame(ring, 0, &frame, &error) == FrameStatus::NoFrame,
          "ring: a slot still being written is skipped");
    check(error.empty(), "ring: a skipped slot is not an error");
    slot->state = 0;

    Frame tooSmall;
    tooSmall.pixels.resize(8);
    // readNewestFrame owns the buffer size, so prove the raw ring refuses a
    // destination it cannot fill instead.
    FrameView view;
    check(!ring.read(&view, tooSmall.pixels.data(), tooSmall.pixels.size(), &error),
          "ring: a destination smaller than the frame is refused");
    check(!error.empty(), "ring: the refusal explains itself");

    auto* header = reinterpret_cast<FrameRingHeader*>(mapping.data());
    const std::uint32_t version = header->version;
    header->version = version + 1;
    check(!ring.validate(&error), "ring: an unsupported version is refused");
    check(readNewestFrame(ring, 0, &frame, &error) == FrameStatus::Error,
          "ring: reading a bad version is an error");
    header->version = version;

    header->magic = 0;
    check(!ring.validate(&error), "ring: a bad magic is refused");
    header->magic = kFrameRingMagic;
    check(ring.validate(&error), "ring: the header is valid again");

    FrameView wrong = first;
    wrong.width = width + 4;
    check(!ring.write(wrong, &error), "ring: a changed frame size is refused");

    // UYVY 4:2:2: two bytes per pixel, no alpha.
    constexpr std::uint32_t uyvyStride = width * 2;
    std::vector<std::uint8_t> uyvyMapping(
        static_cast<std::size_t>(frameRingBytes(uyvyStride, height)), 0);
    std::vector<std::uint8_t> uyvyPixels(static_cast<std::size_t>(uyvyStride) * height, 0x44);
    FrameRing uyvyRing(uyvyMapping.data(), uyvyMapping.size());
    check(uyvyRing.initForWrite(width, height, uyvyStride,
                                static_cast<std::uint32_t>(FrameFormat::UYVY), &error),
          "ring: initialises UYVY");
    FrameView uyvyFrame;
    uyvyFrame.data = uyvyPixels.data();
    uyvyFrame.width = width;
    uyvyFrame.height = height;
    uyvyFrame.stride = uyvyStride;
    uyvyFrame.format = static_cast<std::uint32_t>(FrameFormat::UYVY);
    check(uyvyRing.write(uyvyFrame, &error), "ring: publishes UYVY");
    Frame uyvyRead;
    check(readNewestFrame(uyvyRing, 0, &uyvyRead, &error) == FrameStatus::Frame,
          "ring: reads UYVY");
    check(uyvyRead.format == static_cast<std::uint32_t>(FrameFormat::UYVY) &&
              uyvyRead.stride == uyvyStride && uyvyRead.pixels[0] == 0x44,
          "ring: UYVY format, stride and bytes survive");
    check(!uyvyRead.alpha(), "ring: UYVY has no alpha flag");
    FrameView formatChange = uyvyFrame;
    formatChange.format = static_cast<std::uint32_t>(FrameFormat::BGRA8);
    check(!uyvyRing.write(formatChange, &error), "ring: a changed format is refused");

    FrameRing unmapped(nullptr, 0);
    check(!unmapped.validate(&error), "ring: no mapping is refused");

    if (gFailed) {
        std::fprintf(stderr, "frame_ring_tests: FAILED\n");
        return 1;
    }
    std::printf("frame_ring_tests: all checks passed\n");
    return 0;
}
