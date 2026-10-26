/*
 * PlaybackPipeline (task 5.1): the production decode→present chain for one
 * video file, graduated from the 1.5e bench's improvised glue (PLAN's
 * worker-architecture rule: a fixed dev loop is proof, not a destination).
 * It owns the demux/decode thread (VideoFrameSource), the GPU presentation
 * slots (GpuYuvUploader serving as the ring's FrameBacking), the pacing
 * thread that offers near-due frames through the session's video seam
 * (playback/VideoSeam.h), and the presenter role that hands slots back
 * after the GPU sampled them.
 *
 * The boundedness contract — the task's acceptance sentence — is that the
 * queue never grows: memory is fixed at open (ring-depth slots, plus in
 * GPU mode the uploader's fixed presentation slots and bounded decoder
 * pool), and a consumer that stops taking frames parks the decoder on a
 * held slot or a full ring instead of buffering ahead. stats()'s ring
 * high-water pins that against the configured depth; playback_queue_tests
 * proves it with a frozen clock and a stuck presenter.
 *
 * Clock and sink are injected: with a session, the sink calls submitVideo
 * and the clock is the audio stream time (wall time from start() is the
 * default). CPU mode runs the plain-memory ring with no GPU at all — the
 * same pacing and bounds, so the queue contract is testable headless.
 *
 * Thread discipline: open/close on the owning thread while nothing runs;
 * the sink and clock run on the pacing thread; the presenter callbacks run
 * on the session's render thread (and onDropped also on any thread that
 * replaced a pending submission), so counters live under one mutex.
 * requestStop() + join() unwind in the order the bench proved: stop flags
 * first, then the ring and decode thread, then the GPU waits — no thread
 * can outlive the uploader or codec context.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <vulkan/vulkan.h>

#include "include/core/SkImage.h"

#include "media/FrameRing.h"
#include "media/MediaProbe.h"
#include "media/VideoFrameSource.h"
#include "playback/VideoSeam.h"
#include "render/YuvGpu.h"

class GrDirectContext;

namespace openshow::worker {

class PlaybackPipeline final : public VideoPresenter {
public:
    // Device handles the GPU ring shares with its owner: from
    // OutputSession::gpuHandles() in production, or a headless
    // VulkanContext's equivalents in the GPU tests.
    struct Gpu {
        VkDevice device = VK_NULL_HANDLE;
        VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
        uint32_t graphicsQueueIndex = 0;
        uint32_t uploadQueueIndex = 0;
        VkQueue uploadQueue = VK_NULL_HANDLE;
        GrDirectContext* context = nullptr;
    };

    struct Options {
        std::string path;
        // Ring slots, fixed at open. 0 selects the default: the uploader's
        // presentation-slot count in GPU mode (tokens are slot indices),
        // four plain slots in CPU mode.
        unsigned ringDepth = 0;
        bool loop = false;
        // GPU mode: direct decode is the plan; allowVideoCopy adds the
        // explicitly permitted copying-compatibility reopen (task 1.5i).
        bool allowVideoCopy = false;
        // Compatibility path only: VA-API when available (task 1.5h).
        bool hardwareDecode = true;
    };

    PlaybackPipeline() = default;
    ~PlaybackPipeline() override;

    PlaybackPipeline(const PlaybackPipeline&) = delete;
    PlaybackPipeline& operator=(const PlaybackPipeline&) = delete;

    // Opens the container and the presentation ring; nothing flows until
    // start(). GPU mode probes the stream (pts continuity across loops),
    // builds the uploader at the decoded size and validates the first
    // frame before accepting, exactly as the bench did.
    bool openGpu(const Gpu& gpu, const Options& options, std::string* error);
    // CPU mode: plain-memory ring, copying decode. The presenter role is
    // inert (acquireImage returns null); this is the bounded queue the
    // headless tests drive without Vulkan.
    bool openCpu(const Options& options, std::string* error);

    // Stops producers, joins both threads, frees the decoder and ring
    // resources (codec-held images go before the owner's Vulkan teardown).
    // Idempotent; the destructor calls it.
    void close();

    // Starts decode and the pacing thread: exactly one start per open.
    // The sink runs on the pacing thread once per frame whose pts the
    // clock has caught up to (within the supply lookahead); with a
    // session, wire it to submitVideo. The clock reports seconds the pts
    // is expressed against (audio stream time in production); null means
    // wall time from start().
    using SubmitFn = std::function<void(const VideoSubmission&)>;
    bool start(SubmitFn submit, std::function<double()> clock, std::string* error);

    // Unwinding pair (idempotent, safe before start): requestStop wakes
    // every parked producer and waiter; join waits out both threads.
    void requestStop();
    void join();

    // --- the session's video seam (VideoPresenter) ---
    SkImage* acquireImage(unsigned token, uint64_t sequence) override;
    void onPresented(unsigned token, VkSemaphore doneVk) override;
    void onReplaced(unsigned token, uint64_t sequence) override;
    void onDropped(unsigned token, uint64_t sequence) override;

    // A present whose drift exceeds this counts as missed — the M5 metric.
    // 25 ms (twice the fastest frame interval) keeps wake-up jitter out of
    // the count while real stalls count, as the bench's threshold did.
    void setMissedWindowMs(double ms);

    // --- playback control (task 5.2) ---
    //
    // All thread-safe after open. Pause stops polling: the ring fills, the
    // producer parks on it (the queue stays the bound), and on resume the
    // pacing re-anchors media to the clock — a gap-less gap for a paused
    // audio clock, the paused duration for a wall clock. seekTo lands on
    // the decode thread: the demuxer jumps, the decoder flushes, the ring
    // purges, and frames short of the target are skipped before claiming.
    // The pacing thread drains anything polled across those steps and
    // re-anchors at the seek target, so a seek never presents a stale
    // frame and never trips the loop-wrap heuristic. Loop toggles live;
    // the value at the next EOF decides rewind or finish.
    void play();
    void pause();
    bool paused() const { return fPaused.load(); }
    void seekTo(double seconds);
    void setLoop(bool enabled);
    // Whether this transport is supposed to never end (task 6.7c: an EOF
    // that arrives anyway is then a fault rather than a finished clip).
    bool looping() const { return fLoop.load(); }
    // Media position presented so far (loop-continuous pts); -1 before the
    // first submission, frozen while paused.
    double position() const;

    unsigned ringDepth() const { return fDepth; }
    const media::VideoStreamInfo& streamInfo() const { return fSource.streamInfo(); }
    const std::string& decodeDescription() const { return fSource.decodeDescription(); }

    struct Stats {
        // The boundedness contract: filled slots never exceeded ringDepth.
        unsigned ringDepth = 0;
        uint64_t ringPeakFilled = 0;
        uint64_t ringPublished = 0;
        uint64_t ringConsumed = 0;
        // Pacing and the session seam.
        uint64_t submitted = 0;
        uint64_t presents = 0;
        uint64_t missed = 0;
        uint64_t dropped = 0;  // replaced before ever drawn
        // Lease proof refusals (task 6.6a, review R6): an acquire or a
        // release naming a generation other than the one leasing the token.
        uint64_t staleAcquires = 0;
        uint64_t staleReleases = 0;
        double driftMeanMs = 0.0;
        double driftMaxMs = 0.0;
        // Decode (VideoFrameSource).
        uint64_t decoded = 0;
        uint64_t decodeErrors = 0;
        uint64_t loops = 0;
        // Playback control (task 5.2).
        uint64_t seeks = 0;          // applied by the decode thread
        uint64_t seekDiscards = 0;   // decoded frames skipped before target
        uint64_t ringDiscarded = 0;  // published frames purged by a seek
        uint64_t directFrames = 0;
        uint64_t downloadedFrames = 0;
        uint64_t copiedFrames = 0;
        uint64_t convertedFrames = 0;
        // GPU ring (zero in CPU mode; task 1.5j counters).
        uint64_t gpuUploads = 0;
        uint64_t gpuSlotsCreated = 0;
        uint64_t gpuWraps = 0;
        uint64_t gpuWaitsForFreeSlot = 0;
        uint64_t gpuDirectCopies = 0;
        uint64_t gpuDecodeImagesCreated = 0;
        uint64_t gpuCachedDecodeImages = 0;
        double gpuMsPerUpload = 0.0;  // copy submit + fence per commit
        // Outcome.
        std::string fatalError;
        bool eof = false;
    };
    // Safe from any thread while open; counters move while running.
    Stats stats() const;

private:
    enum class Mode { kClosed, kCpu, kGpu };

    void runPacing();
    // Seconds against the injected clock; callers hold fStatsMutex.
    double clockSecondsLocked(std::chrono::steady_clock::time_point now) const;
    // Returns a polled frame to the ring, and — for a frame the audience is
    // never offered — to the uploader as well. See runPacing's drop sites.
    void returnFrame(const media::VideoFrame& frame, bool submitted);
    bool openCommon(const Options& options, std::string* error);

    Mode fMode = Mode::kClosed;
    media::VideoFrameSource fSource;
    GpuYuvUploader fUploader;
    unsigned fDepth = 0;
    std::atomic<bool> fLoop{false};  // start value and live toggle
    double fClipSeconds = 0.0;       // probe duration; 0 = container does not say

    std::function<double()> fClock;                  // fStatsMutex
    std::chrono::steady_clock::time_point fClockStart;  // default clock anchor
    SubmitFn fSubmit;                                // set before start, pacing thread

    std::thread fPacing;
    std::atomic<bool> fStop{false};
    std::atomic<bool> fPaused{false};  // task 5.2 control
    bool fDecodeStarted = false;
    double fPositionPts = -1.0;  // fStatsMutex: last submitted media pts

    // pts by presentation token (the slot index), stamped before the
    // submission so the render thread can never observe a frame without
    // its pts; an entry is only rewritten after the presenter released
    // that slot, which reads it first.
    std::vector<double> fPtsByToken;  // fStatsMutex

    // The lease ledger (task 6.6a, review R6): the decoder sequence of
    // the generation currently leasing each token, stamped by the pacing
    // thread immediately before its submit and cleared by a generation-
    // proved onReplaced/onDropped. Zero means no lease. Sequences are
    // never reset and grow, so equality with a submission's sequence is
    // the release/acquire proof; the producer cannot stamp a new lease
    // over an unreleased one because its commit parks in waitForFree
    // until the lease ends.
    std::vector<uint64_t> fLeaseSeq;  // fStatsMutex

    mutable std::mutex fStatsMutex;
    double fMissedWindowMs = 25.0;  // fStatsMutex
    uint64_t fSubmitted = 0;        // fStatsMutex
    uint64_t fPresents = 0;         // fStatsMutex
    uint64_t fMissed = 0;           // fStatsMutex
    uint64_t fDropped = 0;          // fStatsMutex
    uint64_t fStaleAcquires = 0;    // fStatsMutex
    uint64_t fStaleReleases = 0;    // fStatsMutex
    double fDriftSumMs = 0.0;       // fStatsMutex
    double fDriftMaxMs = 0.0;       // fStatsMutex
};

}  // namespace openshow::worker
