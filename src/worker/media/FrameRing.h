/*
 * FrameRing: the bounded CPU frame queue (task 1.5b).
 *
 * Fixed-depth ring of I420 frames between the decode thread (producer) and
 * one consumer — the benchmark harness today, the GPU uploader in 1.5d. The
 * slot bookkeeping follows PreviewStream: a slot is claimable only once the
 * consumer has released it, and the queue never grows (PLAN.md: "decoding
 * queues... without growing"). The difference is direction of pressure: the
 * preview producer drops, this producer *waits*, because silently dropped
 * decode output would be a benchmark lie. A wait always honours stop(), so
 * no thread blocks forever.
 *
 * Slot buffers are allocated once at create() for a fixed resolution.
 * Frames cross as pointers into those buffers: the consumer copies (or
 * uploads) what it needs before release(). Frames are visible strictly in
 * production order; a clocked consumer skips late frames by polling and
 * releasing them unused.
 *
 * Threading: one producer and one consumer; claim/publish from the producer
 * thread, poll/release from the consumer thread. stop() may come from
 * anywhere. FFmpeg types live only in the .cpp (Impl).
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>

namespace openshow::media {

// Valid from claim() (producer) or poll() (consumer) until publish()/
// abortClaim()/release(). `data` points into ring-owned buffers.
struct VideoFrame {
    uint32_t slot = 0;
    int width = 0;
    int height = 0;
    uint8_t* data[4] = {};
    int linesize[4] = {};
    double ptsSeconds = 0.0;
    uint64_t sequence = 0;  // 1, 2, 3… in production order; never reset,
                            // not even across loops
};

class FrameRing {
public:
    FrameRing();
    ~FrameRing();

    FrameRing(const FrameRing&) = delete;
    FrameRing& operator=(const FrameRing&) = delete;

    // Allocates slotCount I420 frames at width x height. Returns false with
    // *error on bad parameters or allocation failure.
    bool create(unsigned slotCount, int width, int height, std::string* error);
    void destroy();

    unsigned slotCount() const;
    int width() const;
    int height() const;

    // --- producer (decode thread) ---

    // Waits for a slot whose previous frame the consumer released. Fills
    // dimensions and plane pointers; ptsSeconds is the caller's to set
    // before publish(). Returns false if stop() was requested while
    // waiting (or had been before).
    bool claim(VideoFrame* out);

    // Completes the claim: stamps pts, assigns the sequence number and
    // makes the frame visible to poll(). Takes the frame returned by claim().
    void publish(const VideoFrame& frame);

    // Gives the claimed slot back without making it visible (decode error
    // or shutdown between claim and publish).
    void abortClaim(uint32_t slot);

    // --- consumer (one thread) ---

    // Waits up to `timeout` for the oldest visible frame; the slot is then
    // owned by the consumer until release(). False on timeout or stop.
    bool poll(VideoFrame* out, std::chrono::milliseconds timeout);

    // Returns a polled slot to the free pool.
    void release(uint32_t slot);

    // Unblocks claim()/poll() and makes both fail-fast afterwards.
    void stop();

    struct Stats {
        uint64_t published = 0;   // frames made visible
        uint64_t consumed = 0;    // frames handed to the consumer
        uint64_t peakFilled = 0;  // high-water of visible-but-unconsumed
    };
    Stats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> fImpl;
};

}  // namespace openshow::media
