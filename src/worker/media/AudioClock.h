/*
 * AudioClock: the audio playback clock (task 1.5c).
 *
 * PLAN.md fixes the rule: the PipeWire process callback is the clock
 * whenever audio is running. The authoritative reading comes from the
 * server's stream-time report (pw_time: "the current time that the remote
 * end is reading/writing"), handed over by the sink on every callback and
 * extrapolated between callbacks at the reported rate — so the clock ticks
 * smoothly at server discipline, not at buffer-slice granularity.
 *
 * Before the first stream-time report (and in tests without PipeWire) the
 * clock falls back to delivered frames / rate, anchored to a monotonic
 * instant. Video (1.5e) picks frames against clockSeconds(); nothing here
 * knows about video.
 *
 * Transport semantics (task 5.4): the clock is a MEDIA clock. pause()
 * freezes clockSeconds() at the current position — the server stops
 * advancing stream time while a stream is inactive, but the between-
 * callback extrapolation would keep running, so resume() converts the
 * stale extrapolation overshoot into a permanent offset. rebase() jumps
 * the position (a seek's media target) the same way. pause/resume/rebase
 * come from the single transport thread; readers anywhere.
 *
 * setStreamTime()/addDelivered() are called from the single PipeWire
 * thread; the other accessors are safe from any thread.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>

namespace openshow::media {

class AudioClock {
public:
    void setRate(uint32_t rate) { fRate.store(rate, std::memory_order_relaxed); }
    uint32_t rate() const { return fRate.load(std::memory_order_relaxed); }

    // Start/restart the clock: drop the stream-time report, reset
    // delivered, clear any transport offset and re-anchor to now.
    void anchor() {
        fDelivered.store(0, std::memory_order_relaxed);
        fHasStreamTime.store(false, std::memory_order_release);
        fOffsetSeconds.store(0.0, std::memory_order_relaxed);
        fHoldSeconds.store(0.0, std::memory_order_relaxed);
        fPaused.store(false, std::memory_order_release);
        fAnchor = std::chrono::steady_clock::now();
    }

    // --- transport (task 5.4, single transport thread) ---

    // Freeze the reported position. The server stops advancing stream
    // time while the sink's stream is inactive; the extrapolation this
    // clock runs between callbacks would not, so pause latches the
    // TRANSPORTED position (raw minus the current offset — a rebase
    // before the pause must hold the rebased value, not the raw one).
    void pause() {
        fHoldSeconds.store(rawSeconds() - fOffsetSeconds.load(std::memory_order_relaxed),
                           std::memory_order_relaxed);
        fPaused.store(true, std::memory_order_release);
    }
    void resume() {
        // rawSeconds() has been advancing past the hold for the whole
        // pause (stale extrapolation); re-anchoring the offset to it
        // makes the position continue from the hold with no jump.
        fOffsetSeconds.store(rawSeconds() - fHoldSeconds.load(std::memory_order_relaxed),
                             std::memory_order_relaxed);
        fPaused.store(false, std::memory_order_release);
    }
    // Jump the position to a media time (a seek's target). While paused
    // the held position jumps too, so a parked transport shows the target
    // (resume re-anchors the offset from the hold).
    void rebase(double mediaSeconds) {
        if (fPaused.load(std::memory_order_relaxed)) {
            fHoldSeconds.store(mediaSeconds, std::memory_order_relaxed);
        } else {
            fOffsetSeconds.store(rawSeconds() - mediaSeconds, std::memory_order_relaxed);
        }
    }
    bool paused() const { return fPaused.load(std::memory_order_acquire); }

    // Single (PipeWire) thread only.
    void addDelivered(uint64_t frames) {
        fDelivered.fetch_add(frames, std::memory_order_relaxed);
    }

    // Single (PipeWire) thread only. tickFrames is the playback position
    // (frames) at tickNowNs (CLOCK_MONOTONIC nanoseconds), advancing at
    // framesPerSecond. A three-field update; readers run a seqlock.
    void setStreamTime(uint64_t tickFrames, int64_t tickNowNs, double framesPerSecond) {
        const uint32_t sequence = fSequence.load(std::memory_order_relaxed) + 1;
        fSequence.store(sequence, std::memory_order_release);  // odd: mid-update
        fTickFrames.store(tickFrames, std::memory_order_relaxed);
        fTickNowNs.store(tickNowNs, std::memory_order_relaxed);
        fFramesPerSecond.store(framesPerSecond, std::memory_order_relaxed);
        fHasStreamTime.store(true, std::memory_order_release);
        fSequence.store(sequence + 1, std::memory_order_release);  // even: consistent
    }

    // Media time since playback began (transport offsets applied).
    double clockSeconds() const {
        if (fPaused.load(std::memory_order_acquire)) {
            return fHoldSeconds.load(std::memory_order_relaxed);
        }
        return rawSeconds() - fOffsetSeconds.load(std::memory_order_relaxed);
    }

    // Wall time since anchor, for drift = wallElapsed - clockSeconds.
    double wallElapsedSeconds() const {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - fAnchor).count();
    }

    uint64_t deliveredFrames() const {
        return fDelivered.load(std::memory_order_relaxed);
    }

    static int64_t monotonicNs() {
        struct timespec ts = {};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
    }

private:
    // The untransported position: stream time extrapolated to now, or
    // delivered frames before the first report.
    double rawSeconds() const {
        if (fHasStreamTime.load(std::memory_order_acquire)) {
            // Retry while the writer is mid-update; the window is nanoseconds.
            for (int attempt = 0; attempt < 8; ++attempt) {
                const uint32_t before = fSequence.load(std::memory_order_acquire);
                if (before & 1u) {
                    continue;
                }
                const uint64_t tick = fTickFrames.load(std::memory_order_relaxed);
                const int64_t tickNow = fTickNowNs.load(std::memory_order_relaxed);
                const double fps = fFramesPerSecond.load(std::memory_order_relaxed);
                if (fSequence.load(std::memory_order_acquire) != before || fps <= 0.0) {
                    continue;
                }
                const double elapsed = static_cast<double>(monotonicNs() - tickNow) * 1e-9;
                return (static_cast<double>(tick) + elapsed * fps) / fps;
            }
        }
        const uint32_t rate = fRate.load(std::memory_order_relaxed);
        if (rate == 0) {
            return 0.0;
        }
        return static_cast<double>(fDelivered.load(std::memory_order_relaxed)) /
               static_cast<double>(rate);
    }

    std::atomic<uint32_t> fRate{0};
    std::atomic<uint64_t> fDelivered{0};
    std::chrono::steady_clock::time_point fAnchor{};

    // Transport state (task 5.4): one writer (the transport thread).
    std::atomic<double> fOffsetSeconds{0.0};
    std::atomic<double> fHoldSeconds{0.0};
    std::atomic<bool> fPaused{false};

    // Seqlock-protected stream-time triple.
    std::atomic<uint32_t> fSequence{0};
    std::atomic<uint64_t> fTickFrames{0};
    std::atomic<int64_t> fTickNowNs{0};
    std::atomic<double> fFramesPerSecond{0.0};
    std::atomic<bool> fHasStreamTime{false};
};

}  // namespace openshow::media
