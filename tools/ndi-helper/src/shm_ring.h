// POSIX shared-memory ownership for one open NDI source. The layout and the
// newest-wins protocol live in shared/openshow/live/frame_ring.h; this class
// only creates the segment, initialises the header and unlinks it on close.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "openshow/live/frame_ring.h"

namespace openshow::ndi {

class ShmRing {
public:
    ShmRing() = default;
    ~ShmRing();
    ShmRing(const ShmRing&) = delete;
    ShmRing& operator=(const ShmRing&) = delete;

    // Creates a segment named /openshow-ndi-<pid>-<counter> and initialises the
    // header for the given frame size. On failure nothing is left behind.
    bool create(std::uint32_t width, std::uint32_t height, std::uint32_t stride,
                std::uint32_t format, std::string* error);

    bool write(const openshow::live::FrameView& frame, std::string* error);

    // Unmaps and unlinks. Safe to call more than once. A reader that already
    // mapped the segment keeps its mapping until it unmaps; the name is gone.
    void close();

    bool valid() const;
    const std::string& name() const { return fName; }
    std::uint64_t bytes() const { return fBytes; }
    const openshow::live::FrameRingHeader* header() const;

private:
    std::string fName;
    void* fMap = nullptr;
    std::size_t fBytes = 0;
    std::unique_ptr<openshow::live::FrameRing> fRing;
};

}  // namespace openshow::ndi
