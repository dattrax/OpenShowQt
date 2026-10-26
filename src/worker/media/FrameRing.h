/*
 * Bounded producer/consumer presentation queue. In direct mode slots contain
 * only tokens: publish copies a retained decoder allocation through the GPU
 * backing before making its token visible. Decoder images are independent of
 * these tokens and may remain referenced by FFmpeg after presentation.
 *
 * Compatibility/CPU mode owns fixed native plane buffers or obtains them
 * from the backing. All rows honor linesize; unused planes are null.
 * claim/publish run on the producer, poll/release on one consumer. stop is
 * idempotent and wakes waiters, including before initialization/after teardown.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "media/FrameStorage.h"

namespace openshow::media {

// GPU backing for direct allocations/copies and compatibility slot buffers.
class FrameBacking {
public:
    virtual ~FrameBacking() = default;

    // Fills native plane pointers and linesizes for a compatibility slot
    // (unused planes are null). Called once per slot by
    // FrameRing::create(); the memory must stay valid until destroy().
    virtual bool slotPlanes(unsigned slot, uint8_t* data[4], int linesize[4]) = 0;

    // The producer has finished writing the slot; makes the bytes readable
    // by the GPU consumer. May block until the slot's previous consumer is
    // done with it. Called by publish() before the frame becomes visible;
    // false aborts the publish.
    virtual bool commit(unsigned slot) = 0;

    // A committed frame is being destroyed without ever reaching a consumer
    // (a seek flush purges the ring). Whatever the commit reserved for the
    // slot must come back: the presenter's release is the only other one,
    // and it can never arrive for a frame that was never offered. Runs on
    // the producer thread, with no ring lock held (task 6.7d).
    virtual void discardCommitted(unsigned slot) { (void)slot; }

    // Direct decoding: allocation may run on a codec worker thread. It must
    // fail rather than wait for decoder-held reference frames to be released.
    virtual std::shared_ptr<FrameStorage> allocateDecodeFrame(const FrameAllocation&,
                                                            std::string* error) {
        if (error) *error = "backing does not support direct decoder allocation";
        return {};
    }
    virtual bool commitFrame(unsigned, const std::shared_ptr<FrameStorage>&) { return false; }
    virtual void requestShutdown() {}

    // A producer parked in commit() wakes and unwinds while this flag reads
    // true (task 6.7d). The pipeline needs it because a held slot is the
    // NORMAL state with a seek outstanding: the pacer offers nothing new
    // while one is queued, so the frame on air is never replaced and the
    // release the parked commit waits for can never come. Without the abort
    // the decode thread is stuck behind the very frame it must flush, and the
    // transport holds its last picture with the clock running on. Owners wire
    // this once, before the first frame; nullptr (the default) means a parked
    // commit waits for the slot, which is right for a consumer that will come
    // back to it (a pause).
    void setProducerAbort(const std::atomic<bool>* abort) { fProducerAbort = abort; }

protected:
    // True when the frame a parked producer holds is history: the commit
    // refuses, publish aborts the claim, and the producer goes looking for
    // the work that made the frame stale.
    bool producerAbandoned() const {
        return fProducerAbort != nullptr &&
               fProducerAbort->load(std::memory_order_acquire);
    }

private:
    const std::atomic<bool>* fProducerAbort = nullptr;
};

// Valid from claim() (producer) or poll() (consumer) until publish()/
// abortClaim()/release(). Compatibility `data` points into owned buffers;
// direct producers attach decoder storage, which the consumer does not need.
struct VideoFrame {
    uint32_t slot = 0;
    int width = 0;
    int height = 0;
    uint8_t* data[4] = {};
    int linesize[4] = {};
    double ptsSeconds = 0.0;
    uint64_t sequence = 0;  // 1, 2, 3… in production order; never reset,
                            // not even across loops
    std::shared_ptr<FrameStorage> storage; // producer-only, released after the copy fence
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

    // Select native plane storage, or direct presentation tokens without
    // per-slot pixel buffers (requires a backing and publish(frame.storage)).
    bool create(unsigned slotCount, int width, int height, FrameFormat format,
                FrameBacking* backing, std::string* error, bool direct = false);
    void destroy();

    unsigned slotCount() const;
    int width() const;
    int height() const;
    FrameFormat format() const;

    // --- producer (decode thread) ---

    // Waits for a slot whose previous frame the consumer released. Fills
    // dimensions and plane pointers; ptsSeconds is the caller's to set
    // before publish(). Returns false if stop() was requested while
    // waiting (or had been before). Slot choice is round-robin: with the
    // cursor's help, a consumer that holds one frame still lets writes
    // spread across every slot instead of collapsing onto the lowest one.
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

    // Playback control (task 5.2): discards every currently visible frame,
    // returning its slot to the free pool, and returns how many. A frame
    // the consumer already polled is untouched — it knows to drop it from
    // the seek generation instead. A consumer parked in poll() keeps
    // waiting: after a purge the queue legitimately holds nothing yet.
    unsigned discardReady();

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
        uint64_t discarded = 0;   // frames purged by discardReady()
        uint64_t peakFilled = 0;  // high-water of visible-but-unconsumed
    };
    Stats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> fImpl;
};

}  // namespace openshow::media
