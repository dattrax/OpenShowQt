/*
 * FrameRing: the bounded NV12/P010 frame queue (tasks 1.5b, 1.5i).
 *
 * Fixed-depth ring of two-plane 4:2:0 frames between the decode thread
 * (producer) and one consumer — the benchmark harness and, since 1.5i, the
 * GPU uploader whose host-visible slot images *are* the slot storage. The
 * slot bookkeeping follows PreviewStream: a slot is claimable only once the
 * consumer has released it, and the queue never grows (PLAN.md: "decoding
 * queues... without growing"). The difference is direction of pressure: the
 * preview producer drops, this producer *waits*, because silently dropped
 * decode output would be a benchmark lie. A wait always honours stop(), so
 * no thread blocks forever.
 *
 * Slot buffers are allocated once at create() for a fixed resolution —
 * either by the ring itself (plain memory) or by an attached FrameBacking
 * (the GPU slot pair's mapped linear image, task 1.5i). Frames cross as
 * pointers into those buffers. With a backing attached, publish() commits
 * the slot first: a frame only becomes visible to poll() once the GPU can
 * read its pixels (the LINEAR→OPTIMAL copy has completed), which is what
 * lets the consumer draw slot contents with no further synchronisation.
 * Frames are visible strictly in production order; a clocked consumer skips
 * late frames by polling and releasing them unused.
 *
 * Plane conventions follow FFmpeg's semi-planar formats: data[0] is the Y
 * plane, data[1] the interleaved UV plane and data[2] = data[1] + 1; every
 * row is addressed by its linesize, which an external backing may pad
 * beyond the visible width (a Vulkan LINEAR image's rowPitch).
 *
 * Threading: one producer and one consumer; claim/publish from the producer
 * thread, poll/release from the consumer thread. stop() may come from
 * anywhere. FFmpeg types live only in the .cpp (Impl).
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace openshow::media {

// Two-plane 4:2:0 slot layouts: 8-bit NV12 and 16-bit little-endian P010.
enum class FrameFormat {
    kNV12,
    kP010,
};

// Optional external owner of the ring's slot pixels (task 1.5i: the GPU
// uploader hands out mappings of its host-visible LINEAR images, so the
// decode side's destination pointer *is* the GPU-visible staging memory).
class FrameBacking {
public:
    virtual ~FrameBacking() = default;

    // Fills the semi-planar plane pointers and linesizes for one slot
    // (data[2] = data[1] + 1, data[3] = nullptr). Called once per slot by
    // FrameRing::create(); the memory must stay valid until destroy().
    virtual bool slotPlanes(unsigned slot, uint8_t* data[4], int linesize[4]) = 0;

    // The producer has finished writing the slot; makes the bytes readable
    // by the GPU consumer. May block until the slot's previous consumer is
    // done with it. Called by publish() before the frame becomes visible;
    // false aborts the publish.
    virtual bool commit(unsigned slot) = 0;
};

// Valid from claim() (producer) or poll() (consumer) until publish()/
// abortClaim()/release(). `data` points into ring-or-backing-owned buffers.
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

    // Allocates slotCount NV12 frames at width x height in ordinary memory.
    // Returns false with *error on bad parameters or allocation failure.
    bool create(unsigned slotCount, int width, int height, std::string* error);

    // Full form (task 1.5i): pick the two-plane format and optionally let a
    // FrameBacking own the slot pixels instead of the ring allocating them.
    bool create(unsigned slotCount, int width, int height, FrameFormat format,
                FrameBacking* backing, std::string* error);
    void destroy();

    unsigned slotCount() const;
    int width() const;
    int height() const;
    FrameFormat format() const;

    // --- producer (decode thread) ---

    // Waits for a slot whose previous frame the consumer released. Fills
    // dimensions and plane pointers; ptsSeconds is the caller's to set
    // before publish(). Returns false if stop() was requested while
    // waiting (or had been before).
    bool claim(VideoFrame* out);

    // Completes the claim: with a backing attached, commits the slot first
    // (visible means GPU-readable), then stamps pts, assigns the sequence
    // number and makes the frame visible to poll(). Takes the frame
    // returned by claim(). Returns false when the commit failed — the
    // claim is aborted and nothing became visible.
    bool publish(const VideoFrame& frame);

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

    // True after stop(): lets a producer tell a shutdown-aborted publish
    // from a failed one.
    bool stopped() const;

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
