/*
 * PreviewStream: producer side of the shared-memory preview (task 1.4).
 *
 * Creates one POSIX shared-memory segment laid out by
 * openshow/protocol/preview_wire.h and owns the three-slot claim /
 * publish / release bookkeeping. The consumer (the Qt frontend) maps the
 * segment by the name returned from create() and reads slots under the
 * per-slot seqlock; this class only ever runs producer-side code.
 *
 * Thread-safety: the sequence bookkeeping sits on one mutex, so
 * claim/publish/release may come from different threads (render vs RPC).
 * The pixel payload between claim() and publish() is written by the single
 * producer thread without the mutex held — safe because a claimed slot has
 * an odd seqlock, which tells any consumer "mid-write, do not read".
 */

#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include "openshow/protocol/preview_wire.h"

namespace openshow::worker {

class PreviewStream {
public:
    PreviewStream() = default;
    ~PreviewStream();

    PreviewStream(const PreviewStream&) = delete;
    PreviewStream& operator=(const PreviewStream&) = delete;

    // Creates and initialises the segment under the given shm object name
    // (the session names streams so the preview.start reply is truthful
    // before the render thread has actually built it).
    bool create(const std::string& name, uint32_t width, uint32_t height,
                std::string* error);
    void destroy();

    const std::string& name() const { return fName; }
    uint32_t width() const { return fWidth; }
    uint32_t height() const { return fHeight; }
    uint32_t rowBytes() const { return fWidth * 4; }
    // One slot's whole stride (header + pixels). A frame whose size is NOT the
    // stream's own — a thumbnail (task 6.16b) — has to be checked against this
    // before a byte is written: the segment was reserved for the stream's
    // geometry, so an extent one stream can publish another cannot, and
    // preview.start can replace a stream while a batch stands.
    uint32_t bytesPerSlot() const { return fBytesPerSlot; }

    // Producer: claim a slot the consumer has already released (or one
    // never written) and open its seqlock. Returns -1 when every slot is
    // still owned downstream — the caller must drop the frame.
    int claim();

    // Payload pointer and header of a claimed slot (producer thread).
    std::uint8_t* slotPixels(int slot);
    protocol::PreviewSlotHeader* slotHeader(int slot);

    // Fills the remaining header fields and closes the seqlock; returns
    // the sequence number consumers must match to copy this frame.
    //
    // `target` and `request` (task 6.16a, review R16) say WHOSE picture
    // this is and WHICH request it answers: one stream carries the
    // audience's mirror and the editor's off-air render, and a consumer
    // cannot tell them apart from bytes. Both are written inside the odd
    // seqlock window, so an even sequence is always a consistent
    // (target, request, pixels) triple.
    //
    // `extent` (task 6.16b) says how BIG this frame is. A thumbnail shares
    // the segment without sharing its size, and the header's geometry is
    // per-slot state, so it is stamped on EVERY publish rather than left at
    // what create() wrote: a slot that held a 160x90 thumbnail and now holds
    // the 640x360 mirror must not still claim to be 160 wide. The default is
    // the stream's own geometry, which is what every frame that is not a
    // thumbnail is. Rows are packed (rowBytes == width * 4), which is the
    // one thing that lets a consumer copy a frame of any size with a single
    // memcpy — and the reason the caller must read back with the frame's own
    // stride, not the stream's.
    struct FrameExtent {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t rowBytes = 0;
    };
    std::uint64_t publish(int slot, std::uint32_t revision, std::uint32_t target,
                          std::uint32_t request, std::uint64_t publishedNs,
                          const FrameExtent* extent = nullptr);

    // A write failed mid-claim: close the seqlock with nothing changed.
    void abort(int slot);

    // Consumer finished copying the frame with this sequence. Only the
    // exact sequence this slot last published counts (proof of
    // ownership); odd, stale, future and duplicate releases do nothing.
    void release(std::uint32_t slot, std::uint64_t seq);

private:
    std::uint8_t* slotBase(std::uint32_t slot) const;

    void* fMapping = nullptr;
    int fFd = -1;
    std::string fName;
    std::uint32_t fWidth = 0;
    std::uint32_t fHeight = 0;
    std::uint32_t fBytesPerSlot = 0;

    std::mutex fMutex;
    std::uint64_t fPublished[protocol::kPreviewSlotCount] = {};
    std::uint64_t fReleased[protocol::kPreviewSlotCount] = {};
};

}  // namespace openshow::worker
