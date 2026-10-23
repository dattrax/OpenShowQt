/*
 * PipeWireSink: playback sink and clock owner (task 1.5c).
 *
 * One F32 stereo playback stream at the negotiated rate. The process
 * callback is the clock (PLAN.md rule): it drains AudioRing, zero-pads the
 * shortfall and counts it as an underrun, and hands the server's own
 * stream-time report to AudioClock — so "zero underruns while fed" and
 * "clock tracks the wall" are both measurable claims, not assumptions.
 * Callback arrival intervals go into a fixed histogram (the callback itself
 * only increments a bucket — no allocation, no lock, safe in the data
 * thread).
 *
 * Threading: pw_main_loop runs on its own thread; feed() is the producer
 * side and never blocks; stats()/clock accessors are safe from any thread.
 * Failure to reach a daemon is reported by start() so callers can skip
 * rather than fail (PLAN.md: environment gaps are honest skips).
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "media/AudioClock.h"
#include "media/AudioRing.h"

namespace openshow::media {

class PipeWireSink {
public:
    PipeWireSink();
    ~PipeWireSink();

    PipeWireSink(const PipeWireSink&) = delete;
    PipeWireSink& operator=(const PipeWireSink&) = delete;

    // Connects the playback stream asking for preferredRate and waits up to
    // 1.5 s for STREAMING. false means no daemon or a refused connection
    // (*error explains); nothing is running afterwards.
    bool start(uint32_t preferredRate, std::string* error);
    void stop();  // idempotent

    // Producer side: enqueue interleaved stereo F32 at the negotiated rate.
    // Returns the frames accepted (0..frames); the ring is bounded and this
    // never blocks — pacing is the caller's business.
    size_t feed(const float* interleavedStereo, size_t frames);

    // Negotiated from the server's Format param; 0 before streaming.
    uint32_t negotiatedRate() const;

    const AudioClock& clock() const;

    struct Stats {
        uint32_t negotiatedRate = 0;
        uint64_t deliveredFrames = 0;
        uint64_t callbacks = 0;
        uint64_t underrunEvents = 0;  // callbacks that found the ring short
        uint64_t underrunFrames = 0;  // silent frames padded in
        double callbackP99Ms = 0.0;   // from the fixed callback-interval histogram
        double clockSeconds = 0.0;
        double wallSeconds = 0.0;
    };
    Stats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> fImpl;
};

}  // namespace openshow::media
