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
 *
 * Transport (task 5.4): the stream connects INACTIVE, so the server
 * consumes nothing until resume() — the clock therefore starts when the
 * owner has audio actually flowing (a silent pre-fill of the ring would
 * otherwise shift heard content against the clock by its length).
 * pause()/resume() freeze/continue the server position and the clock
 * together; requestFlush() discards the queued-but-unheard audio (the
 * process callback performs the drop — it owns the ring's consumer end)
 * and rebaseClock() jumps the media position to a seek target.
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
    // (*error explains); nothing is running afterwards. capacityFrames
    // bounds the unheard queue: task 5.4's pacing analysis makes a small
    // ring (hundreds of ms) the right production choice — the 2 s default
    // fits the sprint-fed bench shape.
    bool start(uint32_t preferredRate, std::string* error, size_t capacityFrames = 96000);
    void stop();  // idempotent

    // Producer side: enqueue interleaved stereo F32 at the negotiated rate.
    // Returns the frames accepted (0..frames); the ring is bounded and this
    // never blocks — pacing is the caller's business.
    size_t feed(const float* interleavedStereo, size_t frames);

    // --- transport (task 5.4; the sink's stream starts inactive) ---
    // resume() begins (or continues) server consumption; pause() stops it
    // and freezes the clock at the current position. requestFlush() asks
    // the process callback to discard queued audio; rebaseClock() jumps
    // the media position (typically together with a flush, at a seek).
    void resume();
    void pause();
    bool active() const;
    void requestFlush();
    void rebaseClock(double mediaSeconds);

    // Negotiated from the server's Format param; 0 before streaming.
    uint32_t negotiatedRate() const;

    const AudioClock& clock() const;

    struct Stats {
        uint32_t negotiatedRate = 0;
        uint64_t deliveredFrames = 0;
        uint64_t callbacks = 0;
        uint64_t underrunEvents = 0;  // callbacks that found the ring short
        uint64_t underrunFrames = 0;  // silent frames padded in
        uint64_t flushes = 0;         // transport flushes performed
        bool active = false;          // server consumption state
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
