/*
 * ND.4/ND.6: UYVY to NV12 conversion for live sources. The compositor's
 * multimedia layer takes one source at a time — an NDI feed, an FFmpeg
 * playback or a still — and the NDI feed uses the same NV12 uploader as the
 * FFmpeg video path. These checks pin the luma deinterleave, the vertical
 * chroma average (including an odd last row), stride padding and the refusals.
 */

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "live/uyvy_to_nv12.h"

namespace {

bool gFailed = false;

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        gFailed = true;
    }
    return condition;
}

using openshow::live::Frame;
using openshow::live::FrameFormat;
using openshow::live::Nv12Frame;

// A 4-pixel UYVY row: [U0 Y0 V0 Y1 U1 Y2 V1 Y3].
std::vector<std::uint8_t> uyvyRow(std::uint8_t u0, std::uint8_t y0, std::uint8_t v0,
                                  std::uint8_t y1, std::uint8_t u1, std::uint8_t y2,
                                  std::uint8_t v1, std::uint8_t y3) {
    return {u0, y0, v0, y1, u1, y2, v1, y3};
}

Frame makeFrame(std::uint32_t width, std::uint32_t height, std::uint32_t stride,
                const std::vector<std::vector<std::uint8_t>>& rows) {
    Frame frame;
    frame.width = width;
    frame.height = height;
    frame.stride = stride;
    frame.format = static_cast<std::uint32_t>(FrameFormat::UYVY);
    frame.sequence = 7;
    frame.timestampNs = 9000;
    frame.pixels.assign(static_cast<std::size_t>(stride) * height, 0xAA);
    for (std::uint32_t row = 0; row < rows.size() && row < height; ++row) {
        std::copy(rows[row].begin(), rows[row].end(),
                  frame.pixels.begin() + static_cast<std::size_t>(row) * stride);
    }
    return frame;
}

const std::vector<std::vector<std::uint8_t>> kRows = {
    uyvyRow(10, 100, 20, 110, 30, 120, 40, 130),
    uyvyRow(50, 140, 60, 150, 70, 160, 80, 170),
    uyvyRow(90, 200, 100, 210, 110, 220, 120, 230),
    uyvyRow(130, 240, 140, 250, 150, 242, 160, 252),
};

const std::vector<std::uint8_t> kY = {
    100, 110, 120, 130,
    140, 150, 160, 170,
    200, 210, 220, 230,
    240, 250, 242, 252,
};

void testEvenHeight() {
    const Frame frame = makeFrame(4, 4, 8, kRows);
    Nv12Frame nv12;
    std::string error;
    check(openshow::live::convertUyvyToNv12(frame, &nv12, &error),
          "uyvy: converts a 4x4 frame");
    check(nv12.width == 4 && nv12.height == 4, "uyvy: keeps the dimensions");
    check(nv12.sequence == 7 && nv12.timestampNs == 9000,
          "uyvy: keeps the sequence and timestamp");
    check(nv12.y.size() == 16, "uyvy: luma has one byte per pixel");
    check(nv12.uv.size() == 8, "uyvy: chroma has two rows of four bytes");
    check(nv12.y == kY, "uyvy: luma is deinterleaved in order");
    const std::vector<std::uint8_t> expectedUv = {30, 40, 50, 60, 110, 120, 130, 140};
    check(nv12.uv == expectedUv, "uyvy: chroma rows are averaged vertically");
    check(nv12.yRowBytes() == 4 && nv12.uvRowBytes() == 4,
          "uyvy: the uploader rows are the visible width");
}

void testOddHeight() {
    const std::vector<std::vector<std::uint8_t>> rows = {kRows[0], kRows[1], kRows[2]};
    const Frame frame = makeFrame(4, 3, 8, rows);
    Nv12Frame nv12;
    std::string error;
    check(openshow::live::convertUyvyToNv12(frame, &nv12, &error),
          "uyvy: converts an odd height");
    check(nv12.uv.size() == 8, "uyvy: an odd height still has two chroma rows");
    const std::vector<std::uint8_t> expectedUv = {30, 40, 50, 60, 90, 100, 110, 120};
    check(nv12.uv == expectedUv, "uyvy: the last row is averaged with itself");
}

void testPaddedStride() {
    // 4 visible pixels (8 bytes) in a 12-byte row; the padding is 0xAA.
    const Frame frame = makeFrame(4, 4, 12, kRows);
    Nv12Frame nv12;
    std::string error;
    check(openshow::live::convertUyvyToNv12(frame, &nv12, &error),
          "uyvy: converts a padded stride");
    check(nv12.y == kY, "uyvy: padding does not leak into luma");
    const std::vector<std::uint8_t> expectedUv = {30, 40, 50, 60, 110, 120, 130, 140};
    check(nv12.uv == expectedUv, "uyvy: padding does not leak into chroma");
}

void testReuse() {
    const Frame frame = makeFrame(4, 4, 8, kRows);
    Nv12Frame nv12;
    std::string error;
    check(openshow::live::convertUyvyToNv12(frame, &nv12, &error), "uyvy: first conversion");
    const std::size_t firstCapacity = nv12.y.capacity();
    check(openshow::live::convertUyvyToNv12(frame, &nv12, &error), "uyvy: reuses the buffers");
    check(nv12.y.capacity() == firstCapacity, "uyvy: the luma buffer is not reallocated");
}

// Independent scalar reference for the wide (SIMD) cases.
Nv12Frame reference(const Frame& source) {
    Nv12Frame out;
    out.width = source.width;
    out.height = source.height;
    out.y.resize(static_cast<std::size_t>(source.width) * source.height);
    out.uv.resize(static_cast<std::size_t>(source.width) * ((source.height + 1) / 2));
    for (std::uint32_t row = 0; row < source.height; ++row) {
        const std::uint8_t* src = source.pixels.data() + static_cast<std::size_t>(row) * source.stride;
        for (std::uint32_t x = 0; x < source.width; ++x) {
            out.y[static_cast<std::size_t>(row) * source.width + x] = src[x * 2 + 1];
        }
    }
    for (std::uint32_t row = 0; row < source.height; row += 2) {
        const std::uint8_t* row0 = source.pixels.data() + static_cast<std::size_t>(row) * source.stride;
        const std::uint8_t* row1 =
            row + 1 < source.height ? row0 + source.stride : row0;
        for (std::uint32_t x = 0; x < source.width; x += 2) {
            out.uv[static_cast<std::size_t>(row / 2) * source.width + x] =
                static_cast<std::uint8_t>((row0[x * 2] + row1[x * 2] + 1) >> 1);
            out.uv[static_cast<std::size_t>(row / 2) * source.width + x + 1] =
                static_cast<std::uint8_t>((row0[x * 2 + 2] + row1[x * 2 + 2] + 1) >> 1);
        }
    }
    return out;
}

void testWideWidths() {
    // 8/16/64 cover the SSSE3 blocks exactly; 10 leaves a remainder. Padding
    // proves the SIMD loads never run past the visible row.
    for (const std::uint32_t width : {8u, 10u, 16u, 64u}) {
        for (const std::uint32_t height : {1u, 3u, 4u}) {
            const std::uint32_t stride = width * 2 + 8;
            Frame frame;
            frame.width = width;
            frame.height = height;
            frame.stride = stride;
            frame.format = static_cast<std::uint32_t>(FrameFormat::UYVY);
            frame.sequence = 11;
            frame.pixels.assign(static_cast<std::size_t>(stride) * height, 0xCD);
            for (std::uint32_t row = 0; row < height; ++row) {
                std::uint8_t* dst = frame.pixels.data() + static_cast<std::size_t>(row) * stride;
                for (std::uint32_t pair = 0; pair < width / 2; ++pair) {
                    dst[pair * 4] = static_cast<std::uint8_t>((pair * 17 + row * 31) & 0xFF);
                    dst[pair * 4 + 1] =
                        static_cast<std::uint8_t>((pair * 3 + row * 7) & 0xFF);
                    dst[pair * 4 + 2] =
                        static_cast<std::uint8_t>((pair * 23 + row * 11) & 0xFF);
                    dst[pair * 4 + 3] =
                        static_cast<std::uint8_t>((pair * 5 + row * 13) & 0xFF);
                }
            }
            Nv12Frame nv12;
            std::string error;
            if (!check(openshow::live::convertUyvyToNv12(frame, &nv12, &error),
                       "uyvy: wide conversion succeeds")) {
                std::fprintf(stderr, "  width=%u height=%u: %s\n", width, height,
                             error.c_str());
                continue;
            }
            const Nv12Frame expected = reference(frame);
            if (!check(nv12.y == expected.y && nv12.uv == expected.uv,
                       "uyvy: SIMD and remainder match the reference")) {
                std::fprintf(stderr, "  width=%u height=%u\n", width, height);
            }
        }
    }
}

void testRefusals() {
    Nv12Frame nv12;
    std::string error;

    Frame bgra = makeFrame(4, 4, 8, kRows);
    bgra.format = static_cast<std::uint32_t>(FrameFormat::BGRA8);
    check(!openshow::live::convertUyvyToNv12(bgra, &nv12, &error),
          "uyvy: a non-UYVY frame is refused");

    Frame empty = makeFrame(4, 4, 8, kRows);
    empty.width = 0;
    check(!openshow::live::convertUyvyToNv12(empty, &nv12, &error),
          "uyvy: a zero width is refused");

    Frame odd = makeFrame(4, 4, 8, kRows);
    odd.width = 3;
    check(!openshow::live::convertUyvyToNv12(odd, &nv12, &error),
          "uyvy: an odd width is refused");

    Frame shortStride = makeFrame(4, 4, 8, kRows);
    shortStride.stride = 7;
    check(!openshow::live::convertUyvyToNv12(shortStride, &nv12, &error),
          "uyvy: a short stride is refused");

    Frame shortPixels = makeFrame(4, 4, 8, kRows);
    shortPixels.pixels.resize(16);
    check(!openshow::live::convertUyvyToNv12(shortPixels, &nv12, &error),
          "uyvy: a short pixel buffer is refused");

    const Frame good = makeFrame(4, 4, 8, kRows);
    check(!openshow::live::convertUyvyToNv12(good, nullptr, &error),
          "uyvy: a missing destination is refused");
}

}  // namespace

int main() {
    testEvenHeight();
    testOddHeight();
    testPaddedStride();
    testWideWidths();
    testReuse();
    testRefusals();

    if (gFailed) {
        std::fprintf(stderr, "live_yuv_tests: FAILED\n");
        return 1;
    }
    std::printf("live_yuv_tests: all checks passed\n");
    return 0;
}
