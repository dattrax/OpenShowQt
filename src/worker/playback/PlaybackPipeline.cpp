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
            // The ring's backing carries the source's stale-frame abort (see
            // VideoFrameSource::createRing): with a seek outstanding the pacer
            // offers nothing, so the frame on air is never replaced and a
            // producer parked on its slot would wait behind the very seek it
            // is on its way to perform (task 6.7d, review R19).
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
    // A seek leaves this thread owing the audience a picture, and until it
    // delivers one nothing downstream frees a presentation slot: the frame on
    // air is released only by the NEXT submission, which is this thread's job.
    // So the flag that tells a parked producer to drop its frame rather than
    // wait for that release (FrameBacking::setProducerAbort) is raised across
    // that whole window. It is measured against what was last OFFERED, not
    // against what the media already did: a seek can be requested, performed
    // and settled between two passes of this loop, and a transport that
    // noticed none of it still owes the audience the target frame (task 6.7d,
    // review R19).
    uint64_t offeredGen = seekGen;
    // Everything a landed seek costs this thread. Both places that notice one
    // — the head of the loop, and a hold broken mid-frame — have to do all of
    // it: resetting the wrap base here is the whole difference between the
    // audience seeing the target and seeing the target PLUS a clip.
    auto reconcileSeek = [&](uint64_t generation) {
        seekGen = generation;
        lastPts = -1.0;  // the position jumped; the loop heuristic restarts
        // A seek is an ABSOLUTE media position, so the wrap continuity this
        // transport built up does not carry across it (task 6.7d, review
        // R19): seeking back to 2 s of a 6 s clip that already looped once
        // used to report pts 8.0 and pace the frame that way — the audience's
        // picture then sat in the session's pending slot waiting on a clock
        // that would need six more seconds to catch up. From the target, pts
        // runs on its own again.
        loopBase = 0.0;
        anchorNext = true;
    };
    while (!fStop.load()) {
        const uint64_t seekWanted = fSource.seekRequested();
        const uint64_t seekNow = fSource.seekGeneration();
        if (seekNow != seekGen) {  // a seek landed since the last pass
            reconcileSeek(seekNow);
        }
        // What the audience is owed: a request the media has not performed,
        // or one it has performed without this thread offering the result.
        const bool seekOwed = seekWanted != seekNow || seekNow != offeredGen;
        fSource.setSeekAbandoned(seekOwed);
        // Held with a seek owed: the operator moved the position, so the parked
        // transport shows where the media is now rather than the frame it froze
        // on. The offer is also what frees the presentation slot a producer may
        // be waiting on, so the hold always has a way out (task 6.7d, R19).
        const bool scrub = fPaused.load() && seekOwed;
        if (fPaused.load() && !seekOwed) {
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
            returnFrame(frame, /*submitted=*/false);
            break;
        }
        // A seek may have been queued, or landed, while this frame was being
        // polled — either way the frame in hand is history, and the next pass
        // reconciles at the head of the loop (which is also where a landed
        // seek's re-anchor happens, so no frame is needed to notice one).
        if (fSource.seekGeneration() != seekGen ||
            fSource.seekRequested() != fSource.seekGeneration()) {
            returnFrame(frame, /*submitted=*/false);
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
        // A held transport presents the seek's frame as soon as it arrives
        // (scrub): the clock it would hold against is frozen, and changing what
        // the parked picture shows is the point of the seek.
        if (!scrub) {
            while (!fStop.load()) {
                // A hold, a queued seek, or a seek that landed since this
                // thread last reconciled: all three make the frame in hand
                // history, because its deadline belongs to a position the
                // audience no longer occupies (task 6.7d, review R19). Without
                // the landed check a frame polled just before a seek was held
                // against the OLD position's deadline — and a seek that moves
                // the clock (the sinkless fallback rebases to the target) then
                // left it waiting seconds for a deadline nobody owns.
                if (fPaused.load() || fSource.seekRequested() != fSource.seekGeneration() ||
                    fSource.seekGeneration() != seekGen) {
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
                returnFrame(frame, /*submitted=*/false);
                break;
            }
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
            submission.ptsSeconds = pts;        // the media position (R19)
            submission.deadlineSeconds = deadline;  // the clock's own domain
            submission.sequence = frame.sequence;
            fSubmit(submission);
            // The audience has the frame this seek asked for: the session now
            // releases the one it replaced, so a producer may wait for a slot
            // the ordinary way again (6.7d).
            offeredGen = fSource.seekGeneration();
            fSource.setSeekAbandoned(false);
        } else {
            // Re-check like a fresh poll would: a seek that landed while
            // holding must still flush and re-anchor — and it must forget the
            // wrap base too, which is exactly what this branch used to skip by
            // crediting the generation without reconciling it.
            const uint64_t seekAfter = fSource.seekGeneration();
            if (seekAfter != seekGen) {
                reconcileSeek(seekAfter);
            }
        }
        // Return the ring slot immediately: the presentation token lives
        // on through the GPU seam. In GPU mode the producer may rewrite
        // the slot's LINEAR twin while its OPTIMAL image waits for its
        // draw, because the next commit gates on that draw's completion
        // semaphore before touching OPTIMAL again.
        returnFrame(frame, !abandoned);
    }
}

void PlaybackPipeline::returnFrame(const media::VideoFrame& frame, bool submitted) {
    if (!submitted && fMode == Mode::kGpu) {
        // A frame the pacer drops — a seek queued or landed, a control op
        // that arrived while it held the frame — is never handed to the
        // session, so no onReplaced()/onDropped() will ever run for it, and
        // those are the only other releases of the uploader's slot. Every
        // drop therefore used to leave one presentation slot held for the
        // life of the transport: ring depth equals the uploader's slot
        // count, so the fourth drop parks the decode thread inside its own
        // publish() — in the loop whose next pass is the one that applies
        // the pending seek. A seek on a GPU background stopped the transport
        // dead (last frame held, clock running) rather than leaking memory
        // (task 6.7d, review R19; the session-level seek scenario is what
        // showed it, because no earlier GPU fixture ever sought).
        //
        // Releasing is safe exactly because the frame was never offered: it
        // carries no sampling obligation, and the lease table still names the
        // last generation the session was given, so a stale submission cannot
        // acquire this slot either way.
        fUploader.release(frame.slot, VK_NULL_HANDLE);
    }
    fSource.ring().release(frame.slot);
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
