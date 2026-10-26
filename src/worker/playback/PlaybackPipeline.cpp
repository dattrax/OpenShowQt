#include "playback/PlaybackPipeline.h"

#include <algorithm>
#include <cstdio>

namespace openshow::worker {

namespace {

// The stream's probe hint decides the conversion the sampling of uploaded
// YUV applies (a Vulkan ycbcr conversion since task 1.5i). Unknown colour
// spaces follow the height convention (720 rows and up are usually
// Rec.709); this Skia revision has no Rec.601 full-range entry, where
// JPEG's full-range mapping is the matching one. (Moved from the bench.)
SkYUVColorSpace pickYuvColorSpace(const media::VideoStreamInfo& info) {
    const bool fullRange =
        info.colorRange == "jpeg" || info.colorRange == "pc" || info.colorRange == "full";
    bool bt709 = info.colorSpace == "bt709";
    const bool bt601 = info.colorSpace == "bt601" || info.colorSpace == "smpte240m";
    if (!bt709 && !bt601) {
        bt709 = info.height >= 720;
    }
    if (bt709) {
        return fullRange ? kRec709_Full_SkYUVColorSpace : kRec709_Limited_SkYUVColorSpace;
    }
    return fullRange ? kJPEG_Full_SkYUVColorSpace : kRec601_Limited_SkYUVColorSpace;
}

}  // namespace

PlaybackPipeline::~PlaybackPipeline() {
    close();
}

bool PlaybackPipeline::openCommon(const Options& options, std::string* error) {
    if (fMode != Mode::kClosed) {
        if (error) *error = "PlaybackPipeline is already open";
        return false;
    }
    if (options.path.empty()) {
        if (error) *error = "PlaybackPipeline needs a media path";
        return false;
    }
    // The container's duration gives looped presentations their monotonic
    // pts (the seam compares pts against one running clock); a container
    // that does not say simply keeps pts as decoded.
    media::MediaInfo info;
    fClipSeconds = media::probeMedia(options.path, &info, nullptr) == media::ProbeStatus::kOk
                       ? info.durationSeconds
                       : 0.0;
    fLoop = options.loop;
    return true;
}

bool PlaybackPipeline::openGpu(const Gpu& gpu, const Options& options, std::string* error) {
    if (!openCommon(options, error)) {
        return false;
    }
    fDepth = options.ringDepth != 0 ? options.ringDepth : unsigned(GpuYuvUploader::kSlots);
    if (fDepth > unsigned(GpuYuvUploader::kSlots)) {
        if (error) {
            *error = "GPU ring depth exceeds the uploader's " +
                     std::to_string(GpuYuvUploader::kSlots) + " presentation slots";
        }
        fMode = Mode::kClosed;
        return false;
    }
    fPtsByToken.assign(fDepth, 0.0);
    fLeaseSeq.assign(fDepth, 0);  // no generation leases anything yet
    // Direct decode is the plan; the copying compatibility path is the
    // explicitly permitted fallback (task 1.5i). No presentation has
    // started between attempts, so the first can release all decoder
    // references before the backing is rebuilt and the file reopened.
    for (int attempt = 0; attempt < (options.allowVideoCopy ? 2 : 1); ++attempt) {
        fSource.setDirectDecodeEnabled(attempt == 0);
        fSource.setHardwareDecodeEnabled(options.hardwareDecode);
        if (fSource.open(options.path, error) &&
            fUploader.create(gpu.device, gpu.physicalDevice, gpu.graphicsQueueIndex,
                             gpu.uploadQueueIndex, gpu.uploadQueue, gpu.context,
                             fSource.frameWidth(), fSource.frameHeight(), fSource.ringFormat(),
                             pickYuvColorSpace(fSource.streamInfo()), error) &&
            fSource.createRing(fDepth, &fUploader, error)) {
            fMode = Mode::kGpu;
            return true;
        }
        fSource.close();
        fUploader.destroy();
        if (attempt == 0 && options.allowVideoCopy) {
            std::fprintf(stderr, "openshow-worker: playback: copying video fallback: %s\n",
                         error ? error->c_str() : "unknown");
        }
    }
    fMode = Mode::kClosed;
    return false;
}

bool PlaybackPipeline::openCpu(const Options& options, std::string* error) {
    if (!openCommon(options, error)) {
        return false;
    }
    fDepth = options.ringDepth != 0 ? options.ringDepth : 4;
    fPtsByToken.assign(fDepth, 0.0);
    fLeaseSeq.assign(fDepth, 0);
    if (!fSource.open(options.path, fDepth, error)) {  // copying storage, plain ring
        fMode = Mode::kClosed;
        return false;
    }
    fMode = Mode::kCpu;
    return true;
}

bool PlaybackPipeline::start(SubmitFn submit, std::function<double()> clock,
                             std::string* error) {
    if (fMode == Mode::kClosed) {
        if (error) *error = "PlaybackPipeline is not open";
        return false;
    }
    if (fDecodeStarted || fPacing.joinable()) {
        if (error) *error = "PlaybackPipeline is already started";
        return false;
    }
    if (!submit) {
        if (error) *error = "PlaybackPipeline needs a submit sink";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(fStatsMutex);
        fClock = std::move(clock);
    }
    fSubmit = std::move(submit);
    fStop.store(false);
    // Anchor the default clock before either thread can read it.
    fClockStart = std::chrono::steady_clock::now();
    if (!fSource.startDecode(fLoop, error)) {
        return false;
    }
    fDecodeStarted = true;
    fPacing = std::thread([this] { runPacing(); });
    return true;
}

double PlaybackPipeline::clockSecondsLocked(
    const std::chrono::steady_clock::time_point now) const {
    if (fClock) {
        return fClock();
    }
    return std::chrono::duration<double>(now - fClockStart).count();
}

void PlaybackPipeline::runPacing() {
    media::VideoFrame frame;
    // pts continuity across decoder loops: the container pts restart at 0
    // while the presentation clock runs on; keep a monotonic effective pts
    // so drift against the clock means what M5 needs it to mean.
    double lastPts = -1.0;
    double loopBase = 0.0;
    // Clock anchoring (task 5.2): pause and seek both break the "pts 0 at
    // clock 0" assumption, so deadlines run against pts + clockOffset.
    // Resume and a landed seek each anchor the first following frame due
    // *now* — correct for a wall clock (which ran while paused) and a
    // no-op for an audio clock that paused or jumped with the media.
    double clockOffset = 0.0;
    bool anchorNext = false;
    bool wasPaused = false;
    // Seek flush: frames polled across a queued or landed seek are stale;
    // the decode thread purges the ring and skips to the target, this
    // thread drains what it already holds and re-anchors. A drained frame
    // that could be the target frame itself costs at most one present.
    uint64_t seekGen = fSource.seekGeneration();
    // Supply pacing: submit no further ahead of the clock than the session
    // can hold. The seam keeps one pending frame and drops the replaced
    // one, so the lookahead must stay under a frame interval — otherwise
    // every deadline is superseded before it arrives and nothing presents.
    const double frameRate = fSource.streamInfo().frameRate;
    const double lookahead =
        frameRate > 0.0 ? std::min(0.05, std::max(0.004, 0.4 / frameRate)) : 0.015;
    while (!fStop.load()) {
        if (fPaused.load()) {
            // Frozen: stop polling, so the ring fills and the producer
            // parks on it — the queue's own bound is the pause parking
            // spot, and the media position holds exactly where it was.
            wasPaused = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        if (wasPaused) {
            wasPaused = false;
            anchorNext = true;
        }
        if (!fSource.ring().poll(&frame, std::chrono::milliseconds(100))) {
            // A dead producer never publishes again: end rather than spin.
            // (The decode thread has already recorded the error.)
            if (!fSource.stats().fatalError.empty() || fSource.ring().stopped() ||
                fStop.load()) {
                break;
            }
            continue;
        }
        if (fStop.load()) {
            fSource.ring().release(frame.slot);
            break;
        }
        const uint64_t seekWanted = fSource.seekRequested();
        const uint64_t seekNow = fSource.seekGeneration();
        if (seekNow != seekGen) {  // a seek landed, maybe during the poll
            seekGen = seekNow;
            lastPts = -1.0;  // the position jumped; the loop heuristic restarts
            anchorNext = true;
            fSource.ring().release(frame.slot);
            continue;
        }
        if (seekWanted != seekNow) {  // one is queued, not yet applied
            fSource.ring().release(frame.slot);
            continue;
        }
        if (lastPts >= 0.0 && frame.ptsSeconds + 0.5 < lastPts && fClipSeconds > 0.0) {
            loopBase += fClipSeconds;  // the decoder rewound; pts restarts
        }
        lastPts = frame.ptsSeconds;
        const double pts = loopBase + frame.ptsSeconds;
        if (anchorNext) {
            anchorNext = false;
            std::lock_guard<std::mutex> lock(fStatsMutex);
            clockOffset = clockSecondsLocked(std::chrono::steady_clock::now()) - pts;
        }
        const double deadline = pts + clockOffset;
        // Hold the frame near its presentation time before offering it
        // (small sleeps; the stop flag cuts them short). Holding *here* —
        // one frame in flight, the rest parked in the bounded ring — is
        // what keeps the queue from growing when the clock stalls.
        bool abandoned = false;
        while (!fStop.load()) {
            if (fPaused.load() || fSource.seekRequested() != fSource.seekGeneration()) {
                abandoned = true;  // a control op arrived; this frame is history
                break;
            }
            std::chrono::steady_clock::time_point now;
            double clockSeconds;
            {
                std::lock_guard<std::mutex> lock(fStatsMutex);
                now = std::chrono::steady_clock::now();
                clockSeconds = clockSecondsLocked(now);
            }
            if (clockSeconds >= deadline - lookahead) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (fStop.load()) {
            fSource.ring().release(frame.slot);
            break;
        }
        if (!abandoned) {
            {
                std::lock_guard<std::mutex> lock(fStatsMutex);
                if (frame.slot < fPtsByToken.size()) {
                    fPtsByToken[frame.slot] = deadline;  // drift against the clock
                }
                // The lease begins exactly here (task 6.6a): stamped with
                // this frame's sequence immediately before the session is
                // handed the submission, so the token is protected from
                // producer reuse for its whole life in the session —
                // pending, drawn, and every redraw after.
                if (frame.slot < fLeaseSeq.size()) {
                    fLeaseSeq[frame.slot] = frame.sequence;
                }
                fPositionPts = pts;  // media position, frozen while paused
                ++fSubmitted;
            }
            VideoSubmission submission;
            submission.token = frame.slot;
            submission.ptsSeconds = pts;
            submission.sequence = frame.sequence;
            fSubmit(submission);
        } else {
            // Re-check like a fresh poll would: a seek that landed while
            // holding must still flush and re-anchor.
            const uint64_t seekAfter = fSource.seekGeneration();
            if (seekAfter != seekGen) {
                seekGen = seekAfter;
                lastPts = -1.0;
                anchorNext = true;
            }
        }
        // Return the ring slot immediately: the presentation token lives
        // on through the GPU seam. In GPU mode the producer may rewrite
        // the slot's LINEAR twin while its OPTIMAL image waits for its
        // draw, because the next commit gates on that draw's completion
        // semaphore before touching OPTIMAL again.
        fSource.ring().release(frame.slot);
    }
}

void PlaybackPipeline::requestStop() {
    fStop.store(true);
    // A pacing thread parked in poll(), or holding a frame while waiting
    // on a clock that will never advance again, ends on the stopped ring.
    fSource.ring().stop();
    // The decode thread unwinds, waking a claim-blocked producer; a
    // producer parked in a slot commit — waiting on a presentation that
    // will never be scheduled now — fails fast on the uploader shutdown.
    fSource.requestStop();
    if (fMode == Mode::kGpu) {
        fUploader.requestShutdown();
    }
}

void PlaybackPipeline::join() {
    if (fPacing.joinable()) {
        fPacing.join();
    }
    fSource.join();
}

void PlaybackPipeline::close() {
    requestStop();
    join();
    fSubmit = nullptr;
    {
        std::lock_guard<std::mutex> lock(fStatsMutex);
        fClock = nullptr;
    }
    // Codec-held reference images are freed before the owner's Vulkan
    // teardown (the bench's closing order).
    fSource.close();
    if (fMode == Mode::kGpu) {
        fUploader.destroy();
    }
    fPtsByToken.clear();
    fLeaseSeq.clear();
    fMode = Mode::kClosed;
}

SkImage* PlaybackPipeline::acquireImage(unsigned token, uint64_t sequence) {
    if (fMode != Mode::kGpu || token >= fDepth) {
        return nullptr;
    }
    {
        // Generation check (task 6.6a, review R6): the image is only the
        // frame this submission carries while that generation holds the
        // lease. A stale submission — from before the slot was released
        // and rewritten, or across a pipeline that no longer owns it —
        // must never sample the slot's later content.
        std::lock_guard<std::mutex> lock(fStatsMutex);
        if (token >= fLeaseSeq.size() || fLeaseSeq[token] != sequence) {
            ++fStaleAcquires;
            return nullptr;
        }
    }
    // Borrowed: the uploader holds the wrapper's own reference until it is
    // destroyed, so the raw pointer outlives every draw. The lease keeps
    // the producer from committing over it for as long as it is held, so
    // the same (token, sequence) acquires the same image every redraw.
    return fUploader.imageForSlot(token, nullptr).get();
}

void PlaybackPipeline::onPresented(unsigned token, VkSemaphore doneVk) {
    {
        std::lock_guard<std::mutex> lock(fStatsMutex);
        double driftMs = 0.0;
        if (token < fPtsByToken.size()) {
            driftMs = (clockSecondsLocked(std::chrono::steady_clock::now()) - fPtsByToken[token]) *
                      1000.0;
        }
        ++fPresents;
        fDriftSumMs += driftMs;
        if (driftMs > fDriftMaxMs) {
            fDriftMaxMs = driftMs;
        }
        if (driftMs > fMissedWindowMs) {
            ++fMissed;
        }
    }
    if (fMode == Mode::kGpu) {
        // A present is an OBLIGATION, not a release (task 6.6a): this
        // draw's doneVk joins the slot's pendingDone, and the slot stays
        // leased — the session will sample this very image again on the
        // next dirty redraw, then hand back another doneVk. The lease
        // ends only at onReplaced/onDropped.
        fUploader.addSamplingDone(token, doneVk);
    }
}

void PlaybackPipeline::onReplaced(unsigned token, uint64_t sequence) {
    bool ownsLease = false;
    {
        std::lock_guard<std::mutex> lock(fStatsMutex);
        if (token < fLeaseSeq.size() && fLeaseSeq[token] == sequence) {
            fLeaseSeq[token] = 0;
            ownsLease = true;
        } else {
            ++fStaleReleases;  // the generation named does not lease the token
        }
    }
    if (ownsLease && fMode == Mode::kGpu) {
        // Every sampling of the replaced frame registered its doneVk at
        // its own iteration; the next commit waits on all of them before
        // overwriting, so the frame is safe to hand to the producer now.
        fUploader.release(token, VK_NULL_HANDLE);
    }
}

void PlaybackPipeline::onDropped(unsigned token, uint64_t sequence) {
    {
        std::lock_guard<std::mutex> lock(fStatsMutex);
        ++fDropped;
    }
    onReplaced(token, sequence);  // same proof; never sampled, so no obligation
}

void PlaybackPipeline::setMissedWindowMs(double ms) {
    std::lock_guard<std::mutex> lock(fStatsMutex);
    fMissedWindowMs = ms;
}

// --- playback control (task 5.2) ---

void PlaybackPipeline::play() {
    fPaused.store(false);
}

void PlaybackPipeline::pause() {
    fPaused.store(true);
}

void PlaybackPipeline::seekTo(double seconds) {
    fSource.requestSeek(seconds < 0.0 ? 0.0 : seconds);
}

void PlaybackPipeline::setLoop(bool enabled) {
    fLoop.store(enabled);
    fSource.setLoopEnabled(enabled);
}

double PlaybackPipeline::position() const {
    std::lock_guard<std::mutex> lock(fStatsMutex);
    return fPositionPts;
}

PlaybackPipeline::Stats PlaybackPipeline::stats() const {
    Stats out;
    out.ringDepth = fDepth;
    const auto ring = fSource.ring().stats();
    out.ringPeakFilled = ring.peakFilled;
    out.ringPublished = ring.published;
    out.ringConsumed = ring.consumed;
    out.ringDiscarded = ring.discarded;
    const auto decoded = fSource.stats();
    out.decoded = decoded.framesProduced;
    out.decodeErrors = decoded.decodeErrors;
    out.loops = decoded.loops;
    out.seeks = decoded.seeks;
    out.seekDiscards = decoded.seekDiscards;
    out.directFrames = decoded.directFrames;
    out.downloadedFrames = decoded.downloadedFrames;
    out.copiedFrames = decoded.copiedFrames;
    out.convertedFrames = decoded.convertedFrames;
    out.fatalError = decoded.fatalError;
    out.eof = decoded.eof;
    if (fMode == Mode::kGpu) {
        const auto& gpu = fUploader.stats();
        out.gpuUploads = gpu.uploads;
        out.gpuSlotsCreated = gpu.slotsCreated;
        out.gpuWraps = gpu.wraps;
        out.gpuWaitsForFreeSlot = gpu.waitsForFreeSlot;
        out.gpuDirectCopies = gpu.directCopies;
        out.gpuDecodeImagesCreated = gpu.decodeImagesCreated;
        out.gpuCachedDecodeImages = gpu.cachedDecodeImages;
        out.gpuMsPerUpload = gpu.uploads != 0 ? gpu.totalGpuMs / double(gpu.uploads) : 0.0;
    }
    std::lock_guard<std::mutex> lock(fStatsMutex);
    out.submitted = fSubmitted;
    out.presents = fPresents;
    out.missed = fMissed;
    out.dropped = fDropped;
    out.staleAcquires = fStaleAcquires;
    out.staleReleases = fStaleReleases;
    out.driftMeanMs = fPresents != 0 ? fDriftSumMs / double(fPresents) : 0.0;
    out.driftMaxMs = fDriftMaxMs;
    return out;
}

}  // namespace openshow::worker
