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

    // Producer: claim a slot the consumer has already released (or one
    // never written) and open its seqlock. Returns -1 when every slot is
    // still owned downstream — the caller must drop the frame.
    int claim();

    // Payload pointer and header of a claimed slot (producer thread).
    std::uint8_t* slotPixels(int slot);
    protocol::PreviewSlotHeader* slotHeader(int slot);

    // Fills the remaining header fields and closes the seqlock; returns
    // the sequence number consumers must match to copy this frame.
    std::uint64_t publish(int slot, std::uint32_t revision, std::uint64_t publishedNs);

    // A write failed mid-claim: close the seqlock with nothing changed.
    void abort(int slot);

    // Consumer finished copying the frame with this sequence (monotone).
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
