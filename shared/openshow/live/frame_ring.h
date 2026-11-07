/*
 * MIT License
 *
 * Copyright (c) 2026 OpenShowQt contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/*
 * The shared-memory frame ring between the NDI helper (writer) and the GPL
 * worker (reader). This header is the whole interface: a fixed-size header, a
 * fixed-size slot header per slot, and the pixel bytes. It is a POD layout with
 * no pointers and no platform types, so the helper and the app agree on it
 * without linking a library.
 *
 * Newest wins. The writer fills the next slot and publishes a sequence; the
 * reader copies the newest slot and retries if the slot's sequence changed
 * underneath it. There is no growing queue and no per-frame socket traffic.
 *
 * This file is MIT, not GPL: the helper is a separate program and includes the
 * same layout. The GPL application may include this MIT header; MIT is
 * GPL-compatible. See docs/ndi-camera-input.md.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace openshow::live {

inline constexpr std::uint32_t kFrameRingMagic = 0x4F534E44;  // "OSND"
inline constexpr std::uint32_t kFrameRingVersion = 1;
inline constexpr std::uint32_t kFrameRingSlots = 3;
inline constexpr std::uint32_t kFrameRingHeaderBytes = 128;
inline constexpr std::uint32_t kFrameRingSlotHeaderBytes = 64;
inline constexpr std::uint32_t kFrameRingDataOffset = 320;  // 128 + 3 * 64
inline constexpr std::uint32_t kFrameRingAlign = 64;
inline constexpr std::uint32_t kFrameRingMaxWidth = 4096;
inline constexpr std::uint32_t kFrameRingMaxHeight = 2160;
inline constexpr std::uint32_t kFrameRingMaxStride = kFrameRingMaxWidth * 4;
inline constexpr std::uint64_t kFrameRingMaxFrameBytes =
    static_cast<std::uint64_t>(kFrameRingMaxStride) * kFrameRingMaxHeight;
inline constexpr std::uint64_t kFrameRingMaxBytes =
    kFrameRingDataOffset +
    static_cast<std::uint64_t>(kFrameRingSlots) * kFrameRingMaxFrameBytes;

enum class FrameFormat : std::uint32_t {
    BGRA8 = 0,
    RGBA8 = 1,
    UYVY = 2,
};

// Bit 0 means the pixel bytes carry alpha. Bit 1 means the source reported a
// loss/error on the frame that produced this slot.
inline constexpr std::uint32_t kFrameFlagAlpha = 1u << 0;
inline constexpr std::uint32_t kFrameFlagSourceLost = 1u << 1;

// The header sits at offset 0. The slot headers sit at kFrameRingHeaderBytes,
// one kFrameRingSlotHeaderBytes each. Pixel data starts at kFrameRingDataOffset,
// one aligned slot per slot. Every field is little-endian on the wire; both
// ends are the same machine in v1 (POSIX shared memory).
struct FrameRingHeader {
    std::uint32_t magic = kFrameRingMagic;
    std::uint32_t version = kFrameRingVersion;
    std::uint32_t slots = kFrameRingSlots;
    std::uint32_t headerBytes = kFrameRingHeaderBytes;
    std::uint32_t slotHeaderBytes = kFrameRingSlotHeaderBytes;
    std::uint32_t dataOffset = kFrameRingDataOffset;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride = 0;
    std::uint32_t format = static_cast<std::uint32_t>(FrameFormat::BGRA8);
    std::uint32_t flags = 0;
    std::uint32_t writeIndex = 0;
    std::uint64_t frameBytes = 0;
    std::uint64_t slotBytes = 0;
    std::uint64_t totalBytes = 0;
    std::uint64_t sequence = 0;
    std::uint64_t timestampNs = 0;
    std::uint32_t reserved[10] = {};
};
static_assert(sizeof(FrameRingHeader) == kFrameRingHeaderBytes);
static_assert(alignof(FrameRingHeader) >= 8);

struct FrameSlotHeader {
    std::uint64_t sequence = 0;
    std::uint32_t state = 0;  // 1 while the writer is copying, 0 when stable
    std::uint32_t reserved = 0;
    std::uint64_t timestampNs = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride = 0;
    std::uint32_t format = 0;
    std::uint32_t flags = 0;
    std::uint32_t reserved2[5] = {};
};
static_assert(sizeof(FrameSlotHeader) == kFrameRingSlotHeaderBytes);
static_assert(alignof(FrameSlotHeader) >= 8);

// One frame as the writer sees it. `data` must hold stride * height bytes.
struct FrameView {
    const std::uint8_t* data = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride = 0;
    std::uint32_t format = static_cast<std::uint32_t>(FrameFormat::BGRA8);
    std::uint32_t flags = 0;
    std::uint64_t timestampNs = 0;
};

// Aligned bytes one slot occupies for a frame of `stride * height` bytes.
inline std::uint64_t frameRingSlotBytes(std::uint32_t stride, std::uint32_t height) {
    const std::uint64_t raw = static_cast<std::uint64_t>(stride) * height;
    return (raw + kFrameRingAlign - 1) & ~static_cast<std::uint64_t>(kFrameRingAlign - 1);
}

// Total bytes a ring needs for one frame size.
inline std::uint64_t frameRingBytes(std::uint32_t stride, std::uint32_t height) {
    return kFrameRingDataOffset +
           static_cast<std::uint64_t>(kFrameRingSlots) * frameRingSlotBytes(stride, height);
}

inline const char* frameFormatName(std::uint32_t format) {
    switch (static_cast<FrameFormat>(format)) {
        case FrameFormat::BGRA8:
            return "BGRA";
        case FrameFormat::RGBA8:
            return "RGBA";
        case FrameFormat::UYVY:
            return "UYVY";
    }
    return "unknown";
}

// Bytes one pixel occupies in a slot. UYVY is packed 4:2:2 and the RGB formats
// are 8-bit 4:4:4; nothing else is accepted, which keeps a corrupt header from
// turning a stride check into a buffer overrun.
inline std::uint32_t frameRingBytesPerPixel(std::uint32_t format) {
    switch (static_cast<FrameFormat>(format)) {
        case FrameFormat::BGRA8:
        case FrameFormat::RGBA8:
            return 4;
        case FrameFormat::UYVY:
            return 2;
    }
    return 0;
}

// A mapped ring. Construct it over an existing mapping to read; call
// initForWrite() on a fresh, zeroed mapping before the writer's first frame.
class FrameRing {
public:
    FrameRing(void* base, std::size_t bytes) : fBase(static_cast<std::uint8_t*>(base)), fBytes(bytes) {}

    // Initialise a fresh mapping. Returns false when the dimensions or the
    // mapping are unusable. Zeroes the whole mapping first, so a reader that
    // races the writer never sees a stale magic.
    bool initForWrite(std::uint32_t width, std::uint32_t height, std::uint32_t stride,
                      std::uint32_t format, std::string* why = nullptr) {
        if (!fBase) {
            return fail(why, "frame ring: no mapping");
        }
        const std::uint32_t bytesPerPixel = frameRingBytesPerPixel(format);
        if (bytesPerPixel == 0) {
            return fail(why, "frame ring: unsupported pixel format");
        }
        if (width == 0 || height == 0 || stride < width * bytesPerPixel ||
            width > kFrameRingMaxWidth || height > kFrameRingMaxHeight ||
            stride > kFrameRingMaxStride) {
            return fail(why, "frame ring: unusable frame dimensions");
        }
        const std::uint64_t slotBytes = frameRingSlotBytes(stride, height);
        const std::uint64_t totalBytes = frameRingBytes(stride, height);
        if (totalBytes > fBytes) {
            return fail(why, "frame ring: mapping is smaller than the frame");
        }
        std::memset(fBase, 0, static_cast<std::size_t>(totalBytes));
        auto* header = headerPtr();
        header->magic = kFrameRingMagic;
        header->version = kFrameRingVersion;
        header->slots = kFrameRingSlots;
        header->headerBytes = kFrameRingHeaderBytes;
        header->slotHeaderBytes = kFrameRingSlotHeaderBytes;
        header->dataOffset = kFrameRingDataOffset;
        header->width = width;
        header->height = height;
        header->stride = stride;
        header->format = format;
        header->flags = 0;
        header->writeIndex = kFrameRingSlots - 1;
        header->frameBytes = static_cast<std::uint64_t>(stride) * height;
        header->slotBytes = slotBytes;
        header->totalBytes = totalBytes;
        header->sequence = 0;
        header->timestampNs = 0;
        std::atomic_thread_fence(std::memory_order_release);
        return true;
    }

    // True when the mapping holds a complete, self-consistent ring header.
    bool validate(std::string* why = nullptr) const {
        if (!fBase) {
            return fail(why, "frame ring: no mapping");
        }
        if (fBytes < kFrameRingDataOffset) {
            return fail(why, "frame ring: mapping is too small");
        }
        const auto* header = headerPtr();
        if (header->magic != kFrameRingMagic) {
            return fail(why, "frame ring: bad magic");
        }
        if (header->version != kFrameRingVersion) {
            return fail(why, "frame ring: unsupported version");
        }
        if (header->slots != kFrameRingSlots || header->headerBytes != kFrameRingHeaderBytes ||
            header->slotHeaderBytes != kFrameRingSlotHeaderBytes ||
            header->dataOffset != kFrameRingDataOffset) {
            return fail(why, "frame ring: layout does not match this build");
        }
        if (header->width == 0 || header->height == 0) {
            return fail(why, "frame ring: unusable frame dimensions");
        }
        const std::uint32_t bytesPerPixel = frameRingBytesPerPixel(header->format);
        if (bytesPerPixel == 0 || header->stride < header->width * bytesPerPixel) {
            return fail(why, "frame ring: unusable frame dimensions");
        }
        if (header->slotBytes < header->frameBytes ||
            header->totalBytes != frameRingBytes(header->stride, header->height) ||
            header->totalBytes > fBytes) {
            return fail(why, "frame ring: inconsistent size fields");
        }
        return true;
    }

    // Publish one frame. Returns false if the ring is invalid or the frame is
    // bigger than the ring's slot. The writer is single-threaded.
    bool write(const FrameView& frame, std::string* why = nullptr) {
        if (!validate(why)) {
            return false;
        }
        auto* header = headerPtr();
        if (frame.width != header->width || frame.height != header->height ||
            frame.stride != header->stride || frame.format != header->format) {
            return fail(why, "frame ring: frame dimensions changed");
        }
        const std::uint64_t bytes = static_cast<std::uint64_t>(frame.stride) * frame.height;
        if (bytes > header->slotBytes) {
            return fail(why, "frame ring: frame is larger than the slot");
        }
        if (!frame.data && bytes != 0) {
            return fail(why, "frame ring: frame has no pixels");
        }

        const std::uint32_t slot = (header->writeIndex + 1) % kFrameRingSlots;
        FrameSlotHeader* slotHeader = slotHeaderPtr(slot);
        auto& state = *reinterpret_cast<std::atomic<std::uint32_t>*>(&slotHeader->state);
        auto& seq = *reinterpret_cast<std::atomic<std::uint64_t>*>(&slotHeader->sequence);
        state.store(1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        if (bytes != 0) {
            std::memcpy(slotData(slot), frame.data, static_cast<std::size_t>(bytes));
        }
        slotHeader->timestampNs = frame.timestampNs;
        slotHeader->width = frame.width;
        slotHeader->height = frame.height;
        slotHeader->stride = frame.stride;
        slotHeader->format = frame.format;
        slotHeader->flags = frame.flags;
        const std::uint64_t nextSeq = header->sequence + 1;
        seq.store(nextSeq, std::memory_order_relaxed);
        state.store(0, std::memory_order_release);

        auto& headerSeq = *reinterpret_cast<std::atomic<std::uint64_t>*>(&header->sequence);
        auto& writeIndex = *reinterpret_cast<std::atomic<std::uint32_t>*>(&header->writeIndex);
        headerSeq.store(nextSeq, std::memory_order_release);
        writeIndex.store(slot, std::memory_order_release);
        header->timestampNs = frame.timestampNs;
        header->flags = frame.flags;
        return true;
    }

    // Copy the newest frame into `dst` (at least header->frameBytes long).
    // Returns false when nothing has been published or the copy raced an
    // overwrite. The caller may retry; the next successful read is the newest
    // frame at that moment. `out` describes the copied bytes; its `data` points
    // at `dst`, not at the mapping.
    bool read(FrameView* out, std::uint8_t* dst, std::size_t dstBytes, std::string* why = nullptr) const {
        if (!out) {
            return fail(why, "frame ring: no output view");
        }
        if (!validate(why)) {
            return false;
        }
        const auto* header = headerPtr();
        auto& headerSeq = *reinterpret_cast<const std::atomic<std::uint64_t>*>(&header->sequence);
        auto& writeIndex = *reinterpret_cast<const std::atomic<std::uint32_t>*>(&header->writeIndex);
        const std::uint64_t seq = headerSeq.load(std::memory_order_acquire);
        if (seq == 0) {
            return false;
        }
        const std::uint32_t slot = writeIndex.load(std::memory_order_acquire) % kFrameRingSlots;
        const FrameSlotHeader* slotHeader = slotHeaderPtr(slot);
        auto& state = *reinterpret_cast<const std::atomic<std::uint32_t>*>(&slotHeader->state);
        auto& slotSeq = *reinterpret_cast<const std::atomic<std::uint64_t>*>(&slotHeader->sequence);
        if (state.load(std::memory_order_acquire) != 0) {
            return false;
        }
        const std::uint64_t seen = slotSeq.load(std::memory_order_acquire);
        if (seen != seq) {
            return false;
        }
        const std::uint64_t bytes = header->frameBytes;
        if (dstBytes < bytes) {
            return fail(why, "frame ring: destination is too small");
        }
        if (bytes != 0) {
            std::memcpy(dst, slotData(slot), static_cast<std::size_t>(bytes));
        }
        if (slotSeq.load(std::memory_order_acquire) != seen) {
            return false;  // overwritten during the copy; caller retries
        }
        out->data = dst;
        out->width = slotHeader->width;
        out->height = slotHeader->height;
        out->stride = slotHeader->stride;
        out->format = slotHeader->format;
        out->flags = slotHeader->flags;
        out->timestampNs = slotHeader->timestampNs;
        return true;
    }

    const FrameRingHeader* header() const { return headerPtr(); }
    std::size_t bytes() const { return fBytes; }

private:
    FrameRingHeader* headerPtr() const { return reinterpret_cast<FrameRingHeader*>(fBase); }
    FrameSlotHeader* slotHeaderPtr(std::uint32_t slot) const {
        return reinterpret_cast<FrameSlotHeader*>(fBase + kFrameRingHeaderBytes +
                                                  static_cast<std::size_t>(slot) * kFrameRingSlotHeaderBytes);
    }
    std::uint8_t* slotData(std::uint32_t slot) const {
        return fBase + kFrameRingDataOffset + static_cast<std::size_t>(slot) * headerPtr()->slotBytes;
    }
    static bool fail(std::string* why, const char* message) {
        if (why) {
            *why = message;
        }
        return false;
    }

    std::uint8_t* fBase = nullptr;
    std::size_t fBytes = 0;
};

}  // namespace openshow::live
