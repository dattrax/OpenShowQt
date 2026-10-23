/*
 * Shared-memory preview transport (task 1.4, PLAN.md "Preview").
 *
 * Pixels never travel over the RPC socket. The worker creates one POSIX
 * shared-memory segment holding three slots; the socket carries only the
 * preview.start result (which names the segment) and per-frame ready and
 * release notifications. The frontend maps the segment by name.
 *
 * Layout inside the mapped segment (little-endian, fixed-size POD headers,
 * same machine on both ends):
 *
 *   offset 0     PreviewSegmentHeader                    (64 bytes)
 *   slot i       PreviewSlotHeader                       (64 bytes)
 *   slot i + 64  pixels: rowBytes * height, RGBA8 top-down
 *
 * Slot ownership is a seqlock. `seq` even means the payload is stable;
 * odd means the producer is mid-write. A consumer copies the payload and
 * re-reads `seq`: unchanged means the copy is valid. The producer, on its
 * side, only claims a slot whose last published frame the consumer has
 * already released (see preview.released) or that was never used; when no
 * slot qualifies the preview frame is dropped. Output is never blocked by
 * a preview consumer, and a slot still owned by the consumer is never
 * overwritten — the two properties PLAN.md demands.
 *
 * The header is deliberately Qt-free and dependency-free so both processes
 * compile the exact same definition.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace openshow::protocol {

inline constexpr std::uint32_t kPreviewMagic = 0x4F565350;  // "OPSV" little-endian
inline constexpr std::uint32_t kPreviewLayoutVersion = 1;
inline constexpr std::uint32_t kPreviewSlotCount = 3;
inline constexpr std::uint32_t kPreviewFourccRGBA8 = 0x52474241;  // "RGBA"

// Notification and method names shared by both sides.
inline constexpr const char* kMethodPreviewReady = "preview.ready";
inline constexpr const char* kMethodPreviewReleased = "preview.released";

struct PreviewSegmentHeader {
    std::uint32_t magic = kPreviewMagic;
    std::uint32_t layoutVersion = kPreviewLayoutVersion;
    std::uint32_t slotCount = kPreviewSlotCount;
    std::uint32_t bytesPerSlot = 0;   // header + pixels; slot stride
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t format = kPreviewFourccRGBA8;
    std::uint32_t reserved = 0;
    std::uint8_t padding[32] = {};
};

struct PreviewSlotHeader {
    // Seqlock counter, see file comment. Publish protocol for one frame:
    // seq += 1 (odd), write header fields then pixels, seq += 1 (even).
    std::uint64_t seq = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t rowBytes = 0;       // width * 4
    std::uint32_t format = kPreviewFourccRGBA8;
    std::uint32_t revision = 0;       // session revision the frame shows
    std::uint64_t publishedNs = 0;    // worker steady clock at publish time
    std::uint8_t padding[24] = {};
};

static_assert(sizeof(PreviewSegmentHeader) == 64, "segment header must be one cache line");
static_assert(sizeof(PreviewSlotHeader) == 64, "slot header must be one cache line");
static_assert(alignof(PreviewSegmentHeader) <= 64 && alignof(PreviewSlotHeader) <= 64,
              "headers must fit their fixed stride");

// Geometry helpers: the sizes both sides agree on for a given frame size.
inline constexpr std::size_t previewSlotStrideBytes(std::uint32_t width,
                                                    std::uint32_t height) {
    return 64 + static_cast<std::size_t>(width) * height * 4;
}

inline constexpr std::size_t previewSegmentBytes(std::uint32_t width,
                                                 std::uint32_t height) {
    return 64 + kPreviewSlotCount * previewSlotStrideBytes(width, height);
}

// Byte offset of slot `index` (its header; pixels follow at +64).
inline constexpr std::size_t previewSlotOffset(std::uint32_t index,
                                               std::size_t bytesPerSlot) {
    return 64 + static_cast<std::size_t>(index) * bytesPerSlot;
}

}  // namespace openshow::protocol
