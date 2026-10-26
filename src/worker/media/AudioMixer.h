/*
 * AudioMixer (task 5.3): many bounded inputs, one output, independent
 * volume and mute per input — the production mixing stage that sits in
 * front of PipeWireSink (PLAN.md: PipeWire output and stereo mixing).
 *
 * Each input is a bounded SPSC ring, so a source (an AudioFileFeeder for
 * a video's audio, another for a standalone audio file) paces itself on
 * short writes exactly as it did against the sink directly, and no
 * input can grow its queue. The mixer thread is the single consumer:
 * every block it pulls each input's frames (zero-padding the
 * shortfall — that input starved, counted), applies the input's gain
 * and sums into interleaved stereo F32, which goes out through the
 * injected write callback (PipeWireSink::feed in production; a fake
 * device in tests — the mixer knows nothing about PipeWire).
 *
 * Volume and mute are control-plane atomics: a change takes effect over
 * a short ramp, interpolated per frame, so toggling either mid-playback
 * produces no discontinuity. A muted input keeps draining (its feeder
 * stays in step and unmuting picks up live audio, not a backlog).
 *
 * Thread discipline: inputs are registered before start(); write() runs
 * on each source's thread (one per input — the SPSC pairing is the
 * contract); the mixer thread alone reads inputs and calls the write
 * callback; setVolume/setMute/position-style getters are safe anywhere.
 * requestStop() + join() unwind the mixer thread, cutting its retry
 * sleeps short; the object can restart with the same input table.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "media/AudioRing.h"

namespace openshow::media {

class AudioMixer {
public:
    // Consumes interleaved stereo F32 frames, returns how many were
    // taken; non-blocking by contract (same shape as the sink's feed).
    using WriteFn = std::function<size_t(const float*, size_t)>;

    static constexpr int kNoInput = -1;
    static constexpr int kMaxInputs = 8;

    // inputCapacityFrames bounds every input identically (fixed at
    // construction; two seconds at a 48 k rate is a reasonable default).
    explicit AudioMixer(size_t inputCapacityFrames = 96000);
    ~AudioMixer();

    AudioMixer(const AudioMixer&) = delete;
    AudioMixer& operator=(const AudioMixer&) = delete;

    // Registers a source slot before start(); returns the input id, or
    // kNoInput once the table is full or the mixer has started (the mix
    // thread walks the table lock-free).
    int addInput();
    unsigned inputCount() const { return fInputCount; }

    // Control plane — effective from the next ramp step. Volume clamps
    // to [0, 1]; mute forces silence without stopping the drain.
    void setVolume(int input, float volume);
    float volume(int input) const;
    void setMute(int input, bool muted);
    bool muted(int input) const;

    // Source side: never blocks; returns frames accepted (a short count
    // is the pacing signal).
    size_t write(int input, const float* interleavedStereo, size_t frames);

    // Transport side (task 5.4): asks the mix thread to discard this
    // input's queued-but-unmixed audio before its next read — the pairing
    // a seek needs (feeder jumps, then flushes so stale content already
    // in the input ring is not mixed after the jump). Counted per input.
    void requestInputFlush(int input);

    // The mix thread: pulls blocks at the given rate until stopped.
    bool start(WriteFn out, uint32_t rate, std::string* error);
    void requestStop();  // idempotent, safe before start
    void join();

    // Back to the pre-start state (task 6.7a, review R2): re-arms start(),
    // drops every input slot with its ring, and zeroes the counters — a
    // stopped-then-reset mixer accepts addInput() and start() exactly like
    // a fresh one, with no leftover queue from the previous transport.
    // Valid only once the mix thread has been joined; a no-op while the
    // thread could still run, because run() walks the table lock-free.
    void reset();

    struct InputStats {
        uint64_t framesWritten = 0;  // accepted from the source
        uint64_t framesMixed = 0;    // consumed and summed
        uint64_t starvedBlocks = 0;  // blocks that zero-padded this input
        uint64_t flushes = 0;        // transport flushes performed
    };
    struct Stats {
        unsigned inputs = 0;
        uint64_t blocks = 0;
        uint64_t deliveredFrames = 0;
        uint64_t writeRetries = 0;  // short writes to the output (pacing)
        InputStats input[kMaxInputs];
    };
    // Safe from any thread; counters move while running.
    Stats stats() const;

private:
    struct Input {
        AudioRing ring;
        std::atomic<float> volume{1.0f};
        std::atomic<bool> mute{false};
        std::atomic<bool> flushRequested{false};  // transport -> mix thread
        // Stats: one writer per field (the mix thread, or the source for
        // framesWritten), relaxed atomics so stats() is safe from any
        // thread.
        std::atomic<uint64_t> framesMixed{0};
        std::atomic<uint64_t> starvedBlocks{0};
        std::atomic<uint64_t> framesWritten{0};
        std::atomic<uint64_t> flushes{0};
    };

    void run();

    const size_t fInputCapacity;
    std::vector<std::unique_ptr<Input>> fInputs;  // fixed after start
    unsigned fInputCount = 0;
    uint32_t fRate = 0;
    WriteFn fOut;

    std::thread fThread;
    std::atomic<bool> fStop{false};
    std::atomic<bool> fStarted{false};

    mutable std::mutex fStatsMutex;
    uint64_t fBlocks = 0;          // fStatsMutex
    uint64_t fDelivered = 0;       // fStatsMutex
    uint64_t fWriteRetries = 0;    // fStatsMutex
};

}  // namespace openshow::media
