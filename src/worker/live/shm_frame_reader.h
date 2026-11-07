/*
 * Read side of the helper's POSIX frame ring (ND.3). It maps the segment the
 * helper named, validates the MIT layout in shared/openshow/live/frame_ring.h,
 * and copies the newest stable frame into a worker-owned Frame.
 *
 * The mapping is read-only: the worker never writes the helper's ring. A reader
 * that races the writer gets NoFrame (not a torn picture) and tries again on
 * its next pass.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "live/live_source.h"
#include "openshow/live/frame_ring.h"

namespace openshow::live {

// Copies the newest frame from `ring` when its sequence is greater than
// `afterSequence`. `error` is set only for an unusable ring or destination.
FrameStatus readNewestFrame(FrameRing& ring, std::uint64_t afterSequence, Frame* frame,
                            std::string* error);

class ShmFrameReader {
public:
    ShmFrameReader() = default;
    ~ShmFrameReader();
    ShmFrameReader(const ShmFrameReader&) = delete;
    ShmFrameReader& operator=(const ShmFrameReader&) = delete;

    // Maps `name`, expecting at least `bytes` bytes (0 asks the segment size).
    // The header must validate before mapping succeeds.
    bool map(const std::string& name, std::uint64_t bytes, std::string* error);
    void unmap();

    bool mapped() const { return fRing != nullptr; }
    const FrameRingHeader* header() const;
    FrameStatus read(std::uint64_t afterSequence, Frame* frame, std::string* error);

private:
    std::string fName;
    void* fMap = nullptr;
    std::size_t fBytes = 0;
    std::unique_ptr<FrameRing> fRing;
};

}  // namespace openshow::live
