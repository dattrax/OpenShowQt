/*
 * AudioRing: lock-free SPSC ring of interleaved stereo F32 (task 1.5c).
 *
 * Producer: the resample/feed thread. Consumer: the PipeWire process
 * callback, which is a real-time context — read() therefore takes no
 * locks and never allocates; atomics and two memcpys are the whole cost.
 * The ring is created at a fixed capacity and never grows (PLAN.md).
 *
 * Head and tail are monotonically increasing frame counts, so "full" and
 * "empty" are unambiguous without a sentinel slot.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace openshow::media {

class AudioRing {
public:
    AudioRing() = default;

    // capacityFrames interleaved stereo frames; allocations happen here
    // and nowhere else.
    bool create(size_t capacityFrames);
    void reset();  // producer-side clear (restart playback without realloc)

    size_t capacity() const { return fCapacity; }
    size_t available() const;  // frames readable
    size_t free() const;       // frames writable

    // Producer: writes up to `frames`; returns how many were accepted
    // (0..frames). Never blocks — pacing is the producer's choice.
    size_t write(const float* interleavedStereo, size_t frames);

    // Consumer: reads up to `frames` and returns how many were filled; the
    // caller zero-fills the remainder (that shortfall is an underrun).
    size_t read(float* interleavedStereo, size_t frames);

private:
    std::vector<float> fSamples;  // 2 * capacity
    size_t fCapacity = 0;
    std::atomic<size_t> fHead{0};  // frames produced (monotonic)
    std::atomic<size_t> fTail{0};  // frames consumed (monotonic)
};

}  // namespace openshow::media
