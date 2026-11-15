/*
 * BackgroundTransport implementation: the audience-side media, moved out of
 * OutputSession by task 6.18b with the behaviour and the thread rules intact
 * (the R6 quiesce boundary, the newest-wins intent slots, the cross-transport
 * sources list, the per-class sticky fault). Read session/BackgroundTransport.h
 * first: it states the rules and the locking; this file is those rules
 * running.
 */

#include "session/BackgroundTransport.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <utility>

#include <GLFW/glfw3.h>

#include "session/Faults.h"

#include "media/MediaProbe.h"

namespace openshow::worker {

namespace {

// The audible identity of a scene (DR.0/DR.3): the picture whose embedded
// stream is part of the mix, and the show's own layer. A visual-only change
// leaves both alone, so the player, its position and its clock survive it; a
// different picture-video or a different track is a different mix and starts
// a new player.
struct AudioIdentity {
    std::string embedded;  // the video path whose own audio is mixed ("" = none)
    std::string layer;     // the show's track ("" = none)
    bool operator==(const AudioIdentity& other) const = default;
};

AudioIdentity audioIdentityOf(const openshow::scene::Background& background,
                              const openshow::scene::AudioLayer& audio) {
    AudioIdentity identity;
    if (background.hasImage && background.isVideo && !background.isCamera) {
        identity.embedded = background.assetPath;
    }
    if (audio.present) {
        identity.layer = audio.assetPath;
    }
    return identity;
}

}  // namespace

BackgroundTransport::~BackgroundTransport() {
    shutdown();
}

void BackgroundTransport::shutdown() {
    // The prime (task 6.9b) goes first and dies here: the prepare executor
    // has already retired (OutputSession::close stops it before shutting the
    // transport down), so no priming thread can be reaching the slot, and a
    // file-only close has no GPU call and no thread to join. A session must
    // not leave an open decoder parked past its own end.
    {
        const std::lock_guard<std::mutex> lock(fPrimeMutex);
        fPrimed.reset();
        // The staged bundle dies with the session (review D4): its player is
        // stopped here, on this thread, like every other staged death.
        fStagedAudio.reset();
        fPrimedPath.clear();
    }
    // 6.21c2: a retirement may still be in flight; shutdown is not a frame
    // tick, so it pays the lane's wait here BEFORE the teardown (task 6.22d
    // moved the drain ahead): the session must not leave a pacing thread, a
    // player, or unreleased GPU leases outliving it, and teardownBackground-
    // Transport's hand-off requires an empty slot (6.22d's capacity rule).
    // reapRetirement() closes the GPU half on this thread (which it has been
    // since 6.9c's order: the transport dies before the GPU resources it
    // references).
    if (auto prior = inFlightRetirement()) {
        while (!prior->laneDone.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        reapRetirement();
    }
    // End everything by name (6.9c's order): the background transport first
    // — it decodes into frame slots on the session's device and context, so
    // it goes before every GPU resource it references (task 5.5). Every
    // part is idempotent: a never-started transport tears down as cleanly
    // as a running one, which is what makes the destructor safe.
    teardownBackgroundTransport();
    // And drain once more AFTER the teardown: shutdown's own teardown hands
    // the live transport to a fresh retirement, and nothing below is a
    // frame tick to reap it — leaving it would destroy a joinable
    // std::thread with this object.
    if (auto lingering = inFlightRetirement()) {
        while (!lingering->laneDone.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        reapRetirement();
    }
    // The standalone track is deliberately NOT part of the background
    // transport's lifetime (task 6.15b) — which is exactly why shutdown
    // must end it by name. Live first, teardown's rule: a command that
    // races this refuses, never lands on a stopping player; the sticky
    // error keeps its path. The bring-up lane is drained before the track
    // itself: a staged player that never published is stopped by the
    // reaper (6.21c2), and its thread reaped, before the last instance
    // goes down.
    reapStandaloneStage();
    // 6.22e3: the track's OWN retirement lane drains too — shutdown is not
    // a frame tick, and a lane thread must not outlive the object whose
    // member it is (a joinable std::thread with its last reference dies
    // with the process). The player it holds was silenced at the hand-off;
    // this pays only its joins.
    if (auto prior = inFlightStandaloneRetirement(); prior != nullptr) {
        while (!prior->laneDone.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        reapStandaloneRetirement();
    }
    fStandaloneLive.store(false, std::memory_order_release);
    fMediaSourceMask.store(fMediaSourceMask.load(std::memory_order_relaxed) & ~2,
                           std::memory_order_release);
    fStandalonePlayer->stop();  // idempotent
    {
        std::lock_guard<std::mutex> lock(fStandaloneSwapMutex);
        fStandaloneSwap.reset();  // nobody left to apply it
    }
    {
        // The seam's slots go with the session, however the transport went
        // down: teardown cleared them behind its quiesce boundary when a
        // video transport was up, and a session that only ever drove the
        // bench seam (a presenter set, submissions pending, no transport)
        // has none of that — either way, a closed session must not sit on
        // leased frames of a presenter that is about to die. teardown has
        // already run, so fBackground is null here and nothing can submit
        // again.
        std::lock_guard<std::mutex> lock(fMutex);
        fPendingVideo.reset();
        fDrawnVideo.reset();
    }
    {
        std::lock_guard<std::mutex> lock(fMutex);
        fMedia.standalone.live = false;
        fMedia.standalone.playing = false;
        fMedia.standalone.position = -1.0;
        fMedia.standalone.duration = -1.0;
        fMedia.standalone.audio = "none";
        fMedia.standalone.loops = 0;
        rebuildMediaSourcesLocked();  // drops the track's row with its mask bit
    }
}

// --- the video seam (6.18b: the loop asks, the transport decides) ---

BackgroundTransport::VideoWake BackgroundTransport::submissionWake(
    std::chrono::steady_clock::time_point now) const {
    std::lock_guard<std::mutex> lock(fMutex);
    VideoWake wake;
    if (!fPendingVideo || !fPresenter) {
        return wake;
    }
    wake.pending = true;
    if (!fPresenter.load()->acceptsClockEpoch(fPendingVideo->clockEpoch)) {
        wake.due = true;  // wake once to return the obsolete undrawn lease
        return wake;
    }
    // The seam's own gate (task 6.7d, review R19): a frame is due against
    // its DEADLINE in the clock's domain, never against its media pts —
    // those two disagree by a pause, a seek offset or a loop wrap, and
    // comparing the wrong one either presents early or holds the picture
    // for seconds.
    const double clock = videoClockSecondsLocked(now);
    if (videoSubmissionDue(*fPendingVideo, clock)) {
        wake.due = true;
    } else {
        wake.sleepSeconds = fPendingVideo->deadlineSeconds - clock;
    }
    return wake;
}

BackgroundTransport::VideoTurnover BackgroundTransport::takeDueVideo(
    std::chrono::steady_clock::time_point now) {
    std::lock_guard<std::mutex> lock(fMutex);
    VideoTurnover turn;
    if (fPendingVideo && fPresenter &&
        !fPresenter.load()->acceptsClockEpoch(fPendingVideo->clockEpoch)) {
        turn.dropped = std::move(fPendingVideo);
        fPendingVideo.reset();
    }
    if (fPendingVideo && fPresenter &&
        videoSubmissionDue(*fPendingVideo, videoClockSecondsLocked(now))) {
        turn.replaced = fDrawnVideo;
        turn.toDraw = fPendingVideo;
        fDrawnVideo = turn.toDraw;
        fPendingVideo.reset();
    }
    return turn;
}

std::optional<VideoSubmission> BackgroundTransport::heldVideo() const {
    std::lock_guard<std::mutex> lock(fMutex);
    return fDrawnVideo;
}

void BackgroundTransport::dropHeldVideo(unsigned token) {
    std::lock_guard<std::mutex> lock(fMutex);
    if (fDrawnVideo && fDrawnVideo->token == token) {
        fDrawnVideo.reset();
    }
}

SkImage* BackgroundTransport::heldVideoImage() const {
    std::optional<VideoSubmission> held;
    {
        std::lock_guard<std::mutex> lock(fMutex);
        held = fDrawnVideo;
    }
    if (!held || !fPresenter) {
        return nullptr;
    }
    return fPresenter.load(std::memory_order_relaxed)
        ->acquireImage(held->token, held->sequence);
}

void BackgroundTransport::noteRunStart(std::chrono::steady_clock::time_point now) {
    fRunStart = now;
}

MediaStatus BackgroundTransport::mediaStatus() const {
    std::lock_guard<std::mutex> lock(fMutex);
    return fMedia;
}

void BackgroundTransport::refreshMediaState() {
    std::lock_guard<std::mutex> lock(fMutex);
    refreshMediaStateLocked();
}

void BackgroundTransport::wakeRenderThread() {
    // GLFW owns the display connection, so its portable cross-thread nudge
    // (an empty event waking glfwWaitEventsTimeout) is the eventfd of this
    // design — PLAN.md's "eventfd + display fd" intent without private
    // Wayland plumbing.
    glfwPostEmptyEvent();
}

bool BackgroundTransport::takeMediaError(MediaError* out) {
    std::lock_guard<std::mutex> lock(fMutex);
    if (!fPendingMediaError) {
        return false;
    }
    if (out) {
        *out = *fPendingMediaError;
    }
    fPendingMediaError.reset();
    return true;
}

void BackgroundTransport::mediaPlaying(const std::string& path) {
    std::lock_guard<std::mutex> lock(fMutex);
    // Task 6.7c (review R18): the transport is UP, which is not yet the
    // statement "the audience is watching frames". playing waits for the
    // first frame the transport actually hands over (the frame refresh
    // promotes it); position -1 and the lane say what is true meanwhile.
    // An audio-only cue has no frame to wait for; its refresh promotes
    // playback from the player's own pause state.
    fMedia.playing = false;
    fMedia.path = path;
    fMedia.error.clear();  // a successful open replaces any old failure
    fMediaFault.reset();
    fMedia.position = -1.0;  // until the transport presents its head
    fMedia.audio = AvPlayer::laneName(mediaLaneLocked());
    // The master mute is worker state: a successor transport comes up with
    // the operator's switch still in the position they left it.
    fMedia.muted = fMasterMute.load(std::memory_order_relaxed);
    // The transport's mode and mix answer from the first status (task
    // 6.15a): the frame refresh keeps them current, but the half-second
    // before the first frame should already read the truth — the mask is
    // stored before this call so the list starts complete.
    fMedia.live = true;
    fMedia.loop = fAudio->loopEnabled();
    rebuildMediaSourcesLocked();
}

AvPlayer::AudioLane BackgroundTransport::mediaLaneLocked() const {
    // The session's knowledge outranks the transport's ignorance (task
    // 6.7c, review R18): an asset whose audio never reached AvPlayer — a
    // stream no decoder can handle — leaves the transport honestly
    // reporting "none", while the audience is missing sound the file has.
    if (!fExpectedAudioLoss.empty()) {
        return AvPlayer::AudioLane::kDegraded;
    }
    return fAudio->audioLaneState();
}

void BackgroundTransport::mediaStopped() {
    std::lock_guard<std::mutex> lock(fMutex);
    fMedia.playing = false;
    fMedia.position = -1.0;
    fMedia.audio = "none";  // no transport, so no lane to report
    // The mode and the mix went out with the transport that held them
    // (task 6.15a): a successor recomputes loop, duration and sources at
    // its own bring-up, and until then there is nothing to acknowledge.
    // live is the point: the path above stays for the sticky error, but
    // a dead transport must not read as commandable.
    fMedia.live = false;
    fMedia.loop = false;
    fMedia.duration = -1.0;
    // The master mute outlives every transport; the per-source rows go with
    // the transport that registered them.
    fMedia.muted = fMasterMute.load(std::memory_order_relaxed);
    // Sources are cross-transport now (task 6.15b): the background's row
    // went with its mask bit, but a standalone row survives — the track
    // outlives the transport it happened to be heard beside.
    rebuildMediaSourcesLocked();
    // The on-air asset's known audio loss went out with the transport it
    // describes (task 6.7c): the next identity recomputes it.
    fExpectedAudioLoss.clear();
    // The last error stays visible (the operator still needs to read it);
    // path stays so the message and the asset that caused it stay paired.
}

void BackgroundTransport::mediaCameraFailed(const std::string& source,
                                            const std::string& message) {
    std::lock_guard<std::mutex> lock(fMutex);
    fMedia.playing = false;
    fMedia.position = -1.0;
    fMedia.audio = "none";
    fMedia.path = source;
    mediaFailedLocked(MediaFault::kVideo, source, message);
}

void BackgroundTransport::mediaCameraPlaying(const std::string& source) {
    std::lock_guard<std::mutex> lock(fMutex);
    // Only the camera the status already names may promote itself, and only
    // while its open is not carrying a fault. A camera requested but not yet
    // delivering frames stays "on air, not playing" — the colour fill is what
    // the audience is seeing.
    if (fMedia.live && fMedia.path == source && fMedia.error.empty()) {
        fMedia.playing = true;
    }
}

void BackgroundTransport::mediaFailed(MediaFault fault, const std::string& path,
                                const std::string& message) {
    std::lock_guard<std::mutex> lock(fMutex);
    mediaFailedLocked(fault, path, message);
}

void BackgroundTransport::mediaFailedLocked(MediaFault fault, const std::string& path,
                                      const std::string& message) {
    fMedia.path = path;
    // The same failure repeated (a re-prepare of a still-broken asset, or
    // the frame refresh re-reading one death every frame) updates the text
    // but notifies only when the message actually changes — the socket must
    // not spam, and this is now a per-frame path.
    const bool isNew = fMedia.error != message;
    fMedia.error = message;
    fMediaFault = std::make_pair(fault, path);
    if (isNew) {
        fPendingMediaError = MediaError{path, message};
    }
}

void BackgroundTransport::mediaFaultCleared(MediaFault fault, const std::string& path) {
    std::lock_guard<std::mutex> lock(fMutex);
    mediaFaultClearedLocked(fault, path);
}

void BackgroundTransport::mediaFaultClearedLocked(MediaFault fault, const std::string& path) {
    // Recovery is typed (task 6.7c): it clears only the fault it is the
    // recovery OF. Anything else on file — another class, another asset —
    // stays exactly where it is, because nothing about this event speaks
    // for it.
    if (!fMediaFault || fMediaFault->first != fault || fMediaFault->second != path) {
        return;
    }
    fMediaFault.reset();
    fMedia.error.clear();
}

void BackgroundTransport::refreshMediaStateLocked() {
    // The standalone track refreshes on the same heartbeat and the same
    // frame path (task 6.15b) — and BEFORE the background's early return,
    // because the track is exactly the case where the background is gone:
    // an audio-only session has no frames, and the half-second housekeep
    // is what keeps its position and lane honest.
    refreshStandaloneMediaLocked();
    if (fBackground == nullptr) {
        // The mix echo spans lifetimes (task 6.15b). Below, the sources
        // list is rebuilt as part of the background's own refresh; with no
        // background there are no frames to carry it, and a gain the
        // operator set on the track would be applied to the mixer and
        // never RE-PUBLISHED — the row would sit at whatever the swap
        // wrote. Cheap enough to rebuild every pass regardless: it reads
        // atomics from mixers that already exist.
        rebuildMediaSourcesLocked();
        if (!fMediaTransportLive.load(std::memory_order_relaxed)) {
            return;
        }
        // An audio-only cue (a show audio layer with no video pipeline, or
        // a track beside a live camera): there is no frame to promote, so
        // the player's own pause state and clock ARE the transport's
        // truth. The path stays whatever applyBackground named — the camera
        // source when there is one, otherwise the track.
        const AvPlayer::AudioLane lane = mediaLaneLocked();
        fMedia.audio = AvPlayer::laneName(lane);
        fMedia.position = fAudio->clockSeconds();
        fMedia.loop = fAudio->loopEnabled();
        fMedia.duration =
            fAppliedAudioDuration > 0.0 ? fAppliedAudioDuration : -1.0;
        fMedia.playing = !fAudio->paused();
        const bool laneFault = lane == AvPlayer::AudioLane::kDegraded ||
                               lane == AvPlayer::AudioLane::kNoOutput;
        const std::string& path = fAppliedAudio.present ? fAppliedAudio.assetPath
                                                        : fMedia.path;
        const bool audioFaultOnFile =
            fMediaFault && fMediaFault->first == MediaFault::kAudio &&
            fMediaFault->second == path;
        if (!laneFault) {
            mediaFaultClearedLocked(MediaFault::kAudio, path);
        } else if (!audioFaultOnFile) {
            mediaFailedLocked(
                MediaFault::kAudio, path,
                !fExpectedAudioLoss.empty()
                    ? fExpectedAudioLoss
                    : (lane == AvPlayer::AudioLane::kNoOutput
                           ? "media audio has no output (no audio server connection)"
                           : "media audio is not audible (no source could be opened, "
                             "or the audio server refused the stream)"));
        }
        return;
    }
    const PlaybackPipeline::Stats transport = fBackground->stats();
    const std::string& path = fAppliedBackground.assetPath;
    fMedia.position = fBackground->position();
    const AvPlayer::AudioLane lane = mediaLaneLocked();
    fMedia.audio = AvPlayer::laneName(lane);
    // Transport mode and mix (task 6.15a, review R15): the loop intent
    // EOF will honour, the clip's probed length, and the registered
    // sources' mixer echo. All read on the transport's own thread; a
    // source this transport never registered is absent rather than
    // falsely listed.
    fMedia.loop = fAudio->loopEnabled();
    const double clipSeconds = fBackground->clipSeconds();
    fMedia.duration = clipSeconds > 0.0 ? clipSeconds : -1.0;
    rebuildMediaSourcesLocked();

    if (!transport.fatalError.empty()) {
        // A decode that died after a successful open (review R18): no
        // further frame can ever arrive, so the picture is frozen and
        // "playing" would be a lie. The transport's own reason is the
        // media's story, and it holds the shared error text — the lane
        // above still tells the audio truth.
        fMedia.playing = false;
        mediaFailedLocked(MediaFault::kVideo, path,
                          "background decode failed: " + transport.fatalError);
        return;
    }
    if (transport.eof) {
        fMedia.playing = false;
        if (fBackground->looping()) {
            // A looping transport must never reach an end: the rewind
            // itself failed (or the media stopped yielding frames), which
            // is a fault, not a finished clip.
            mediaFailedLocked(MediaFault::kVideo, path,
                              "background ended while looping (the media cannot be read again)");
            return;
        }
        // The clip simply finished: accurate status, no fault — and this
        // asset's earlier video fault no longer describes anything.
        mediaFaultClearedLocked(MediaFault::kVideo, path);
    } else if (fAudio->paused()) {
        // Held, not playing (task 6.7d): the audience sees a still frame at
        // the position the hold latched, and the lane beside it says
        // "paused". Nothing is broken, so no fault is implied.
        fMedia.playing = false;
    } else if (transport.submitted > 0) {
        // Frames are flowing: the point at which status may claim playback.
        fMedia.playing = true;
    }

    // The audible lane (review R18): a fault lane is a fault the operator
    // must see; a healthy lane is the recovery that clears it. A fault
    // already on file keeps its text — the transport's start path can name
    // a reason more specific than a lane ever states, and rewriting it each
    // frame would notify the same degradation over and over.
    const bool laneFault = lane == AvPlayer::AudioLane::kDegraded ||
                           lane == AvPlayer::AudioLane::kNoOutput;
    const bool audioFaultOnFile = fMediaFault && fMediaFault->first == MediaFault::kAudio &&
                                  fMediaFault->second == path;
    if (!laneFault) {
        mediaFaultClearedLocked(MediaFault::kAudio, path);
    } else if (!audioFaultOnFile) {
        mediaFailedLocked(
            MediaFault::kAudio, path,
            !fExpectedAudioLoss.empty()
                ? fExpectedAudioLoss
                : (lane == AvPlayer::AudioLane::kNoOutput
                       ? "background audio has no output (no audio server connection)"
                       : "background audio is not audible (no source could be opened, or "
                         "the audio server refused the stream)"));
    }
}

void BackgroundTransport::setVideoPresenter(VideoPresenter* presenter) {
    // Before run(); the render thread reads it without synchronisation
    // because it is only ever written while the loop is not running.
    fPresenter = presenter;
}

void BackgroundTransport::setVideoClock(std::function<double()> clock) {
    std::lock_guard<std::mutex> lock(fMutex);
    fVideoClock = std::move(clock);
}

void BackgroundTransport::submitVideo(VideoSubmission submission) {
    submitVideo(std::move(submission), nullptr);
}

void BackgroundTransport::submitVideo(VideoSubmission submission,
                                      PlaybackPipeline* producer) {
    std::optional<VideoSubmission> dropped;
    VideoPresenter* rejected = nullptr;
    {
        std::lock_guard<std::mutex> lock(fMutex);
        // The retirement generation filter (task 6.21c2, review F3). With
        // the producer's pipeline named, the seam answers "is this the
        // transport in service?" — and a submission from one the seam no
        // longer names is retired work: dropped, because that pipeline is
        // stopping and its close() releases every slot it held, this
        // frame's included. Clock-epoch validation also applies to the
        // bench seam, whose presenter owns the same deadline contract.
        if (producer != nullptr &&
            fPresenter.load(std::memory_order_relaxed) != producer) {
            return;
        }
        auto* presenter = fPresenter.load(std::memory_order_relaxed);
        if (presenter && !presenter->acceptsClockEpoch(submission.clockEpoch)) {
            rejected = presenter;
        } else {
            if (fPendingVideo) dropped = fPendingVideo;
            fPendingVideo = submission;
        }
    }
    if (rejected) {
        rejected->onDropped(submission.token, submission.sequence);
        return;
    }
    // A slot replaced before ever being drawn goes back now, with no GPU
    // semaphore: nothing ever sampled it. The release carries the
    // replaced submission's sequence as its proof of ownership (6.6a).
    // With a producer named, the replaced submission is ITS OWN (the seam
    // was cleared at the last retirement), so the release walks the
    // pipeline that owns the slot rather than a pointer read off the seam.
    if (dropped && dropped->token != submission.token) {
        if (producer != nullptr) {
            producer->onDropped(dropped->token, dropped->sequence);
        } else if (VideoPresenter* presenter = fPresenter.load(std::memory_order_relaxed)) {
            presenter->onDropped(dropped->token, dropped->sequence);
        }
    }
    wakeRenderThread();
}

// --- background transport control (task 6.7d, review R19) ---

void BackgroundTransport::requestMediaSeek(double seconds) {
    fPendingSeekSeconds.store(seconds > 0.0 ? seconds : 0.0, std::memory_order_relaxed);
    fPendingTransportOp.store(kOpSeek, std::memory_order_release);
    wakeRenderThread();
}

void BackgroundTransport::requestMediaPause() {
    fPendingTransportOp.store(kOpPause, std::memory_order_release);
    wakeRenderThread();
}

void BackgroundTransport::requestMediaPlay() {
    fPendingTransportOp.store(kOpPlay, std::memory_order_release);
    wakeRenderThread();
}

void BackgroundTransport::requestMediaLoop(bool enabled) {
    fPendingLoop.store(enabled ? 1 : 0, std::memory_order_release);
    wakeRenderThread();
}

bool BackgroundTransport::mediaSourceLive(AudioSource source) const {
    const int mask = fMediaSourceMask.load(std::memory_order_acquire);
    switch (source) {
        case AudioSource::kVideo:
            return (mask & 1) != 0;
        case AudioSource::kAudio:
            return (mask & 4) != 0;
        case AudioSource::kStandalone:
            return (mask & 2) != 0;
        case AudioSource::kAll:
            break;
    }
    return false;
}

bool BackgroundTransport::requestMediaGain(AudioSource source, double gain,
                                          std::string* error) {
    // Gain names a real source. The master identity is a MUTE target only:
    // "gain 0.5 on everything" would be a second volume system, and the
    // per-source sliders are the one the mixer actually holds.
    if (source == AudioSource::kAll) {
        if (error != nullptr) {
            *error = "media.gain needs one source ('video' or 'audio'), not 'all'";
        }
        return false;
    }
    if (source == AudioSource::kStandalone) {
        if (!fStandaloneLive.load()) {
            if (error != nullptr) *error = "no standalone track is playing";
            return false;
        }
        fPendingGain.store(gain, std::memory_order_relaxed);
        fPendingGainSource.store(2 /* standalone player */, std::memory_order_release);
        wakeRenderThread();
        return true;
    }
    if (!fMediaTransportLive.load()) {
        if (error != nullptr) {
            *error = source == AudioSource::kVideo ? "no background media is playing"
                                                   : "the show audio layer is not playing";
        }
        return false;
    }
    if (!mediaSourceLive(source)) {
        if (error != nullptr) {
            *error = source == AudioSource::kVideo
                         ? "this background has no video audio source"
                         : "this transport has no show audio layer";
        }
        return false;
    }
    fPendingGain.store(gain, std::memory_order_relaxed);
    fPendingGainSource.store(source == AudioSource::kVideo ? AvPlayer::kVideoInput
                                                           : AvPlayer::kSecondInput,
                             std::memory_order_release);
    wakeRenderThread();
    return true;
}

bool BackgroundTransport::requestMediaMute(AudioSource source, bool muted,
                                           std::string* error) {
    if (source == AudioSource::kAll) {
        // The master switch is worker state, not transport state: it may be
        // set before any cue is on air and it survives every transport the
        // worker builds. Both players follow it now; every later instance
        // adopts the flag at bring-up.
        fMasterMute.store(muted, std::memory_order_relaxed);
        fPendingMasterMute.store(muted ? 1 : 0, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(fMutex);
            fMedia.muted = muted;
        }
        wakeRenderThread();
        return true;
    }
    if (source == AudioSource::kStandalone) {
        if (!fStandaloneLive.load()) {
            if (error != nullptr) *error = "no standalone track is playing";
            return false;
        }
        fPendingMute.store(muted ? 1 : 0, std::memory_order_relaxed);
        fPendingMuteSource.store(2 /* standalone player */, std::memory_order_release);
        wakeRenderThread();
        return true;
    }
    if (!fMediaTransportLive.load()) {
        if (error != nullptr) {
            *error = source == AudioSource::kVideo ? "no background media is playing"
                                                   : "the show audio layer is not playing";
        }
        return false;
    }
    // A live transport owns the identity table: a source it never registered
    // is a refusal naming what the operator asked for.
    if (!mediaSourceLive(source)) {
        if (error != nullptr) {
            *error = source == AudioSource::kVideo
                         ? "this background has no video audio source"
                         : "this transport has no show audio layer";
        }
        return false;
    }
    fPendingMute.store(muted ? 1 : 0, std::memory_order_relaxed);
    fPendingMuteSource.store(source == AudioSource::kVideo ? AvPlayer::kVideoInput
                                                           : AvPlayer::kSecondInput,
                             std::memory_order_release);
    wakeRenderThread();
    return true;
}

// --- standalone production track (task 6.15b, review R15) ---

bool BackgroundTransport::validateStandaloneTrack(const std::string& path,
                                                  std::string* error) const {
    // The same probe scene.prepare runs, under no lock: a file that is not
    // decodable audio at that path is a REFUSED request that changes
    // nothing — the old track, if any, keeps playing, and the operator
    // hears the reason rather than waiting after an accepted bring-up for
    // news no one asked for. (A file that vanishes between here and the
    // swap is the bring-up's bad news, named through the block's own
    // error.) Task 6.22e2 moved the WAITING on this probe off the RPC
    // thread; the verdict's content is exactly what it always was.
    const Faults& f = faults();
    if (f.delaysStandaloneProbe(path)) {
        std::fprintf(stderr, "openshow-worker: scripted slow standalone probe: %d ms for %s\n",
                     f.standaloneProbeDelayMs, path.c_str());
        for (int elapsed = 0; elapsed < f.standaloneProbeDelayMs; elapsed += 20) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(std::min(20, f.standaloneProbeDelayMs - elapsed)));
        }
    }
    openshow::media::MediaInfo info;
    std::string probeError;
    switch (openshow::media::probeMedia(path, &info, &probeError)) {
        case openshow::media::ProbeStatus::kOk:
            break;
        case openshow::media::ProbeStatus::kOpenFailed:
            if (error != nullptr) {
                *error = probeError.empty() ? "standalone track: '" + path + "' cannot be opened"
                                            : probeError;
            }
            return false;
        case openshow::media::ProbeStatus::kNoUsableStreams:
            if (error != nullptr) {
                *error = "standalone track: '" + path + "' carries no usable stream";
            }
            return false;
    }
    if (info.audioStreams.empty()) {
        if (error != nullptr) {
            *error = "standalone track: '" + path + "' carries no audio stream";
        }
        return false;
    }
    if (!info.audioStreams.front().decoderAvailable) {
        if (error != nullptr) {
            *error = "standalone track: '" + path + "' has audio no decoder can handle (" +
                     info.audioStreams.front().codec + ")";
        }
        return false;
    }
    return true;
}

BackgroundTransport::StandaloneIntent BackgroundTransport::beginStandaloneIntent() {
    std::lock_guard<std::mutex> lock(fStandaloneSwapMutex);
    fStandaloneSwap.reset();
    return ++fStandaloneIntent;
}

bool BackgroundTransport::armStandaloneTrack(const std::string& path, StandaloneIntent intent) {
    {
        std::lock_guard<std::mutex> lock(fStandaloneSwapMutex);
        if (intent != fStandaloneIntent) return false;
        fStandaloneSwap = StandaloneSelection{path, intent};
    }
    wakeRenderThread();
    return true;
}

void BackgroundTransport::armStandaloneTrack(const std::string& path) {
    const auto intent = beginStandaloneIntent();
    armStandaloneTrack(path, intent);
}

bool BackgroundTransport::requestStandaloneAudio(const std::string& path, std::string* error) {
    const auto intent = beginStandaloneIntent();
    // A removal always accepts, even with a broken track on file: taking
    // the track down is the operator's last resort, and the sticky error
    // stays paired with the path it describes (5.5d's rule, read inside
    // this block).
    if (path.empty()) {
        armStandaloneTrack(path, intent);
        return true;
    }
    if (!validateStandaloneTrack(path, error)) {
        return false;
    }
    armStandaloneTrack(path, intent);
    return true;
}

void BackgroundTransport::standaloneFailedLocked(const std::string& message) {
    // The track's own sticky error, deduplicated on the message exactly
    // like the background's: the lane reconcile runs every half second,
    // and the socket must not spam.
    const bool isNew = fMedia.standalone.error != message;
    fMedia.standalone.error = message;
    if (isNew) {
        fPendingMediaError = MediaError{fMedia.standalone.path, message};
    }
}

void BackgroundTransport::rebuildMediaSourcesLocked() {
    fMedia.sources.clear();
    const int sourceMask = fMediaSourceMask.load(std::memory_order_relaxed);
    if ((sourceMask & 1) != 0) {
        fMedia.sources.push_back({"video", fAudio->volume(AvPlayer::kVideoInput),
                                         fAudio->muted(AvPlayer::kVideoInput)});
    }
    if ((sourceMask & 4) != 0) {
        // The show's audio layer is the background player's SECOND input,
        // mixed beside the video's embedded audio so both share one clock.
        fMedia.sources.push_back({"audio",
                                  fAudio->volume(AvPlayer::kSecondInput),
                                  fAudio->muted(AvPlayer::kSecondInput)});
    }
    if ((sourceMask & 2) != 0) {
        // The echo comes from the TRACK's player: identity kSecondInput,
        // which AvPlayer's own table resolves — the standalone-only
        // transport hands that identity whichever mixer index it got (6.7b).
        fMedia.sources.push_back({"standalone",
                                         fStandalonePlayer->volume(AvPlayer::kSecondInput),
                                         fStandalonePlayer->muted(AvPlayer::kSecondInput)});
    }
}

// The standalone swap, re-cut by 6.21c2 (review F3). The old function ran
// stop()+start() — a server negotiation and a pre-roll, up to seconds —
// INSIDE fMutex, and every RPC status reader queued behind the transport's
// own lock for the whole of it. The waits are simply not the lock's, nor
// the frame tick's, business now:
//
//   * begin: claim the current instance (swap for a fresh silent one under
//     fMutex) and stop it outside the lock — milliseconds, and no reader
//     ever sees a dying player; a removal ends here;
//   * stage: a lane thread starts the new clip with Config::startHeld —
//     connection, negotiation, feeders, mixer, pre-roll ALL paid there,
//     nothing audible — and the finished player waits for the next pass;
//   * publish: swap the staged instance in under fMutex, release its hold
//     (microseconds), and move live/mask/status with it, exactly the old
//     swap's bookkeeping in the same order.
// One stage at a time: while a stage runs, a new intent stays PENDING (the
// old track keeps playing — the honest newest-wins wait, not a queue of
// bring-ups), and the stage's result for a superseded request dies on its
// own thread. The pending gain/mute drop keeps the teardown rule — the
// intent addresses the mixer that is going away — and the publish-then-ops
// order of applyTransportOps still lands a same-pass gain on the NEW
// player's mixer.
double BackgroundTransport::probeStandaloneDuration(const std::string& path) {
    // The seam sits with the file read itself (task 6.22e1), so a fixture
    // measures WHERE this probe is paid: on the bring-up lane it delays the
    // publish by nothing the loop can feel; anywhere under fMutex it stops
    // audience frames and status readers for the whole wait.
    const Faults& f = faults();
    if (f.delaysStandaloneProbe(path)) {
        std::fprintf(stderr, "openshow-worker: scripted slow standalone probe: %d ms for %s\n",
                     f.standaloneProbeDelayMs, path.c_str());
        for (int elapsed = 0; elapsed < f.standaloneProbeDelayMs; elapsed += 20) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(std::min(20, f.standaloneProbeDelayMs - elapsed)));
        }
    }
    openshow::media::MediaInfo info;
    std::string error;
    if (openshow::media::probeMedia(path, &info, &error) ==
            openshow::media::ProbeStatus::kOk &&
        info.durationSeconds > 0.0) {
        return info.durationSeconds;
    }
    return -1.0;
}

void BackgroundTransport::applyStandaloneOps() {
    // Reap first (6.22e3): a retirement whose lane finished must not read
    // as busy to the capacity checks below — the lane's wake is what
    // brought this pass home for it.
    reapStandaloneRetirement();
    // Publish first, if a stage has finished: same-pass gains land on it.
    if (fStandaloneStage && fStandaloneStage->done.load(std::memory_order_acquire)) {
        // Capacity check BEFORE the stage is consumed (task 6.22e3, review
        // A5): a superseded bring-up's held player goes to the retirement
        // lane, and a busy lane means this pass can retire nothing. The
        // stage therefore stays parked — its player started HELD, so the
        // audience hears nothing while it waits — and the lane's wake
        // brings the retry. Abandoning inline (the old way) destroyed the
        // player right here: feeder, mixer and sink joins on the frame
        // tick, uncontracted by anything.
        // done publishes the result before the worker's final wake. Reap
        // outside the intent lock; a parked stage may already be joined.
        if (fStandaloneStage->worker.joinable()) fStandaloneStage->worker.join();
        if (fStandaloneStageClaimHook) {
            fStandaloneStageClaimHook();
        }
        bool superseded = false;
        std::shared_ptr<StandaloneRetirement> reservation;
        std::shared_ptr<StandaloneStage> stage;
        // Keep request receipt ordered against publication as well as the
        // reservation. No later removal can be acknowledged and then have
        // this older player released from its hold.
        std::unique_lock<std::mutex> swapLock(fStandaloneSwapMutex);
        {
            superseded = fStandaloneStage->intent != fStandaloneIntent;
            if (superseded && fStandaloneStage->player) {
                reservation = reserveStandaloneRetirement();
                if (!reservation) return;  // keep the held player and its lane owned
            }
            stage = std::move(fStandaloneStage);
        }
        if (superseded) {
            // Supersession is a terminal outcome, including an obsolete
            // failed start. Ownership moves must never turn it into failure
            // of the current intent or produce a media.error notification.
            if (stage->player)
                retireStandaloneInstance(std::move(stage->player), reservation);
        } else if (!stage->player) {
            // A failed bring-up: the block's story, told under fMutex as
            // the old swap told it (sticky error, path pairing, dedupe).
            std::lock_guard<std::mutex> lock(fMutex);
            fMedia.standalone.live = false;
            fMedia.standalone.path = stage->path;
            standaloneFailedLocked("standalone track: " +
                                   (stage->error.empty() ? "the audio transport refused it"
                                                         : stage->error));
            std::fprintf(stderr, "openshow-worker: standalone track %s: %s\n",
                         stage->path.c_str(), stage->error.c_str());
        } else {
            std::lock_guard<std::mutex> lock(fMutex);
            fStandalonePlayer = std::move(stage->player);
            // Release the hold: the transport comes up exactly where the
            // old swap's start() left it — position 0, playing, lane
            // honest — and the refresh promotes the block's fields as
            // before. play() touches only this player's own sink and
            // clock; no reader can observe it mid-call.
            fStandalonePlayer->play();
            fStandaloneLive.store(true, std::memory_order_release);
            fMediaSourceMask.store(fMediaSourceMask.load(std::memory_order_relaxed) | 2,
                                   std::memory_order_release);
            // A successful bring-up replaces any earlier failure
            // (mediaPlaying's rule), and the block's path names what the
            // block's error describes.
            fMedia.standalone.error.clear();
            fMedia.standalone.live = true;
            fMedia.standalone.path = stage->path;
            fMedia.standalone.playing = false;  // the refresh promotes it, lane first
            fMedia.standalone.position = -1.0;
            // The lane seeded from the player that just came up: `live`
            // and the lane are one statement about the same transport, so
            // the swap that sets the first owes the second an honest value
            // — leaving the last block's "none" in place would let status
            // say "live" beside "none" until the next refresh (a real
            // window: the selection's own wait returns on live, and reads
            // the lane right there).
            fMedia.standalone.audio = AvPlayer::laneName(fStandalonePlayer->audioLaneState());
            // A fresh transport has not wrapped; the count is THIS file's story.
            fMedia.standalone.loops = 0;
            // The duration travels with the bring-up's result (task 6.22e1,
            // review A5): the file was read for it on the lane, so nothing
            // under this lock touches the disk. -1 is the container not
            // saying — the same value the old inline probe left behind.
            fMedia.standalone.duration = stage->duration;
            rebuildMediaSourcesLocked();
            std::fprintf(stderr, "openshow-worker: standalone track %s (%s clock)\n",
                         stage->path.c_str(),
                         fStandalonePlayer->audioLive() ? "audio" : "monotonic");
        }
    }
    if (fStandaloneStage) {
        return;  // one bring-up at a time; the lane wakes this loop when done
    }
    std::optional<StandaloneSelection> swap;
    {
        std::lock_guard<std::mutex> lock(fStandaloneSwapMutex);
        swap.swap(fStandaloneSwap);
    }
    if (!swap) {
        return;
    }
    // Capacity rule (task 6.22e3, review A5): the claim below hands a LIVE
    // instance to the retirement lane, and a busy lane means this pass can
    // hand nothing off — the swap is DEFERRED, 6.22d's policy for the same
    // shape of wait. The latest intent wins: the slot is refilled only if
    // nothing newer arrived in the meantime (a fresher selection outranks
    // this deferred one, exactly as in the session's pending scene),
    // and the lane's wake brings the retry. While deferred, the live track
    // keeps playing and the mirror keeps describing it — the honest
    // requested-vs-presented pair. A claim of a SILENT placeholder does
    // not occupy the lane at all, so a swap that has nothing audible to
    // retire never defers.
    const bool needsRetirement = fStandaloneLive.load(std::memory_order_acquire);
    const auto reservation = needsRetirement ? reserveStandaloneRetirement() : nullptr;
    if (needsRetirement && !reservation) {
        std::lock_guard<std::mutex> lock(fStandaloneSwapMutex);
        if (!fStandaloneSwap && swap->intent == fStandaloneIntent) {
            fStandaloneSwap = swap;
        }
        std::fprintf(stderr,
                     "openshow-worker: standalone swap deferred, retirement in flight: %s\n",
                     swap->path.empty() ? "<remove>" : swap->path.c_str());
        return;
    }
    // A queued intent for THIS identity addresses the mixer of the track
    // that is going away: dropped, never delayed onto a successor that
    // was never asked (teardown's rule, per identity).
    if (fPendingGainSource.load(std::memory_order_relaxed) == AvPlayer::kSecondInput) {
        fPendingGainSource.store(-1, std::memory_order_release);
    }
    if (fPendingMuteSource.load(std::memory_order_relaxed) == AvPlayer::kSecondInput) {
        fPendingMuteSource.store(-1, std::memory_order_release);
    }
    // Claim the current instance under fMutex, take it down OUTSIDE: the
    // 6.21c1 rule. Task 6.22e3 split the taking-down by what can actually
    // wait: a live instance — one that started, whose feeder, mixer and
    // sink threads the destructor joins — goes to the retirement lane,
    // silenced at this hand-off; the fresh never-started placeholder is
    // free to destroy right here (its stop has no threads to reap).
    std::unique_ptr<AvPlayer> retiring;
    bool hadLive = false;
    {
        std::lock_guard<std::mutex> lock(fMutex);
        retiring = std::move(fStandalonePlayer);
        fStandalonePlayer = std::make_unique<AvPlayer>();
        hadLive = fStandaloneLive.exchange(false, std::memory_order_acq_rel);
        fMediaSourceMask.store(fMediaSourceMask.load(std::memory_order_relaxed) & ~2,
                               std::memory_order_release);
        rebuildMediaSourcesLocked();
        fStandalonePath = swap->path;
        // Ownership, command gate and mirror move together (task 6.22f,
        // review A6). From this moment no player is up — for a removal OR
        // behind a successor's slow bring-up alike — and the block says
        // so: live/playing/position/duration/lane described the instance
        // that just left, the refresh cannot fix them while live is false
        // (it early-returns; the swap owns the block), and reporting a
        // removed player as current is a missing transition, not a
        // sampling delay.
        fMedia.standalone.live = false;
        fMedia.standalone.playing = false;
        fMedia.standalone.position = -1.0;
        fMedia.standalone.duration = -1.0;
        fMedia.standalone.audio = "none";
        fMedia.standalone.loops = 0;
        if (!swap->path.empty()) {
            // A replacement: the block names the NEW selection with
            // live=false — requested-not-yet-presented, the pair the
            // operator's other controls already agree with (the gain/mute
            // gate refuses exactly what this mirror shows). The sticky
            // error goes with the instance that produced it: beside a new
            // path it would tell a story this selection never had, and a
            // fresh failure writes its own.
            fMedia.standalone.path = swap->path;
            fMedia.standalone.error.clear();
        }
        // A removal keeps the path and the error together: what broke, and
        // where, is the story the operator still needs to read, and nothing
        // new is being asked of the block.
    }
    if (hadLive) {
        retireStandaloneInstance(std::move(retiring), reservation);  // joins on the lane
    } else {
        retiring.reset();  // never started: no threads, no wait, no lane
    }
    if (swap->path.empty()) {
        std::fprintf(stderr, "openshow-worker: standalone track removed\n");
        return;
    }
    // The production bed loops, the background transport's own default
    // (FreeShow's rule): the removal is the stop, and a walk-in track
    // that falls silent at its EOF mid-service is its own surprise.
    auto stage = std::make_shared<StandaloneStage>();
    stage->path = swap->path;
    stage->intent = swap->intent;
    AvPlayer::Config config;
    config.secondAudioPath = swap->path;
    config.loop = true;
    config.startHeld = true;  // silent while the wait runs (6.21c1's hold)
    stage->worker = std::thread([this, stage, config] {
        // The metadata half of the bring-up's result (task 6.22e1): read on
        // this thread, published with the player, so the render thread's
        // publish consumes a staged value instead of opening the container
        // under fMutex. A probe that fails or says nothing leaves the -1
        // the old inline probe left — the file's audibility is the
        // bring-up's story, the duration is colour.
        stage->duration = probeStandaloneDuration(stage->path);
        auto player = std::make_unique<AvPlayer>();
        if (!player->start(config, &stage->error)) {
            stage->player.reset();  // a failed start has self-cleaned (6.7a)
        } else {
            stage->player = std::move(player);
        }
        stage->done.store(true, std::memory_order_release);
        // The publish prompt: like the retirement lane, the stage wakes
        // the loop even when an idle slide would otherwise sleep through
        // the bring-up finishing.
        wakeRenderThread();
    });
    fStandaloneStage = std::move(stage);
}

void BackgroundTransport::reapStandaloneStage() {
    // Shutdown only (6.21c2): the stage's bring-up is bounded (start()
    // waits on negotiation and pre-roll, seconds at worst) and shutdown is
    // not a frame tick — the session must not die with a lane thread
    // staging a player into it. The staged player, published or not, dies
    // here (its stop runs on this thread; no GPU is anywhere near it).
    if (!fStandaloneStage) {
        return;
    }
    while (!fStandaloneStage->done.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (fStandaloneStage->worker.joinable()) fStandaloneStage->worker.join();
    fStandaloneStage->player.reset();
    fStandaloneStage.reset();
}

std::shared_ptr<BackgroundTransport::StandaloneRetirement>
BackgroundTransport::reserveStandaloneRetirement() {
    const std::lock_guard<std::mutex> lock(fMutex);
    if (fStandaloneRetirement) return nullptr;
    fStandaloneRetirement = std::make_shared<StandaloneRetirement>();
    return fStandaloneRetirement;
}

void BackgroundTransport::retireStandaloneInstance(
    std::unique_ptr<AvPlayer> player,
    const std::shared_ptr<StandaloneRetirement>& retirement) {
    // Silence at the hand-off (6.22c's rule, re-expressed for the track):
    // pause() deactivates the sink stream and latches the clock, and it
    // joins no thread. The retired bed must not stay AUDIBLE for as long
    // as its own joins take — what the lane holds is the object's
    // lifetime, not a licence to keep playing. A superseded staged player
    // was never audible (Config::startHeld); pausing it states the same
    // fact twice, harmlessly.
    player->pause();
    {
        const std::lock_guard<std::mutex> lock(fMutex);
        retirement->player = std::move(player);
    }
    retirement->worker = std::thread([this, r = retirement] {
        // Unlike the background lane, there is no producer join that keeps
        // this slot occupied while the player dies — so the player stays
        // IN the slot while stop() runs and the delivery seam can watch
        // the scripted (or real) hang it is exposed to. That is safe by
        // construction: the object is not destroyed until the reap, and
        // the seam's three reads — paused, sinkActive, deliveredFrames —
        // are all atomics, the same fields the status mirror samples off
        // a live player under the same lock.
        AvPlayer* dying = nullptr;
        {
            const std::lock_guard<std::mutex> lock(fMutex);
            dying = r->player.get();
        }
        if (dying != nullptr) {
            dying->stop();  // THE waits this task moved off the frame tick
        }
        r->laneDone.store(true, std::memory_order_release);
        // The reap rides the next pass; the wake brings even an idle loop
        // back for it, so nothing from the old track outlives that pass.
        // The object's destruction is the reap's (a second, thread-free
        // stop() under the lock: quick by construction, and the fault seam
        // has paid its one sleep).
        wakeRenderThread();
    });
}

void BackgroundTransport::reapStandaloneRetirement() {
    // Render thread only (the loop's transport pass) and shutdown's drain.
    // There is no GPU half to close — an AvPlayer touches no device — so
    // the reap is the thread and the slot.
    const std::shared_ptr<StandaloneRetirement> retirement = inFlightStandaloneRetirement();
    if (!retirement || !retirement->laneDone.load(std::memory_order_acquire)) {
        return;  // the lane wakes this thread when it is done
    }
    retirement->worker.join();
    const std::lock_guard<std::mutex> lock(fMutex);
    fStandaloneRetirement.reset();
}

std::optional<BackgroundTransport::RetiringAudioDelivery>
BackgroundTransport::retiringStandaloneDelivery() const {
    // fMutex is the whole surface, exactly as in retiringAudioDelivery():
    // the lane moves the player out under it and the reap clears the slot
    // under it; the sink's counters are atomics by construction.
    const std::lock_guard<std::mutex> lock(fMutex);
    if (!fStandaloneRetirement || !fStandaloneRetirement->player) {
        return std::nullopt;
    }
    const AvPlayer& player = *fStandaloneRetirement->player;
    return RetiringAudioDelivery{player.paused(), player.sinkActive(),
                                 player.sinkStats().deliveredFrames};
}

void BackgroundTransport::refreshStandaloneMediaLocked() {
    MediaStatus::Standalone& standalone = fMedia.standalone;
    if (!fStandaloneLive.load(std::memory_order_relaxed)) {
        // While no player is up the swap owns the block's fields — and
        // since 6.22f (review A6) it owns them COMPLETELY: the claim
        // publishes the pending-selection state (new path, live=false) for
        // a replacement as well as a removal, so this early return can
        // never preserve a removed player's live/playing/position as if it
        // were still current.
        return;
    }
    const AvPlayer::AudioLane lane = fStandalonePlayer->audioLaneState();
    standalone.audio = AvPlayer::laneName(lane);
    standalone.position = fStandalonePlayer->clockSeconds();
    // The track's own feeder answers the one question the lane cannot: with
    // the loop on, a starved feed is SILENCE the sink paces as usual. The
    // rewind counter is the difference between "still walking" and "ran
    // out", and the fixture watches it rather than trusting the lane.
    standalone.loops = fStandalonePlayer->feederStats(true).loops;
    // "Playing" means the transport is advancing: held freezes it, and a
    // transport that opened NO source has nothing to advance (its position
    // is a clock, not music). A no-output machine still runs — the same
    // monotonic-clock transport the video uses when the server is absent —
    // so it plays while its lane names why nothing is heard.
    standalone.playing = !fStandalonePlayer->paused() && lane != AvPlayer::AudioLane::kDegraded;
    // The lane fault is the track's own story (R18's rule, per block): a
    // fault lane is NAMED, a healthy one is the recovery that clears it.
    // An error set while the player was down (a bring-up that failed) is
    // never reached here — live is false, and only a successful swap
    // clears it, so this reconcile cannot erase a story it does not
    // describe.
    if (lane == AvPlayer::AudioLane::kNoOutput) {
        standaloneFailedLocked("standalone audio has no output (no audio server connection)");
    } else if (lane == AvPlayer::AudioLane::kDegraded) {
        standaloneFailedLocked("standalone audio is not audible (no source could be opened, or "
                               "the audio server refused the stream)");
    } else {
        standalone.error.clear();
    }
}

std::vector<BackgroundTransport::ScheduledOp> BackgroundTransport::parseScheduledOps(
    const std::string& spec) {
    // "seek@900:0.6,seek@1100:0.2,pause@2400,play@3200" — milliseconds after
    // the transport opened. Anything malformed is reported and skipped: a
    // fixture that quietly mis-parsed would assert on an op that never ran.
    std::vector<ScheduledOp> ops;
    size_t start = 0;
    while (start < spec.size()) {
        const size_t comma = spec.find(',', start);
        const std::string entry =
            spec.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        start = comma == std::string::npos ? spec.size() : comma + 1;
        if (entry.empty()) {
            continue;
        }
        const size_t at = entry.find('@');
        const size_t colon = entry.find(':', at == std::string::npos ? 0 : at);
        ScheduledOp op;
        const std::string kind = at == std::string::npos ? entry : entry.substr(0, at);
        if (kind == "seek") {
            op.kind = kOpSeek;
        } else if (kind == "pause") {
            op.kind = kOpPause;
        } else if (kind == "play") {
            op.kind = kOpPlay;
        } else {
            std::fprintf(stderr, "openshow-worker: test transport op '%s' is unknown\n",
                         entry.c_str());
            continue;
        }
        if (at == std::string::npos) {
            std::fprintf(stderr, "openshow-worker: test transport op '%s' has no time\n",
                         entry.c_str());
            continue;
        }
        try {
            op.atMs = std::stoi(entry.substr(at + 1, colon == std::string::npos
                                                          ? std::string::npos
                                                          : colon - at - 1));
            if (op.kind == kOpSeek) {
                if (colon == std::string::npos) {
                    std::fprintf(stderr,
                                 "openshow-worker: test seek '%s' has no target\n", entry.c_str());
                    continue;
                }
                op.seconds = std::stod(entry.substr(colon + 1));
            }
        } catch (const std::exception&) {
            std::fprintf(stderr, "openshow-worker: test transport op '%s' does not parse\n",
                         entry.c_str());
            continue;
        }
        ops.push_back(op);
    }
    return ops;
}

void BackgroundTransport::applyTransportOps() {
    // The transport's thread: AvPlayer's fan-out walks the attached-pipeline
    // list, which only the thread swapping transports may touch (6.6b's
    // rule). Since task 6.22e4 (review A5) a seek does NOT block this pass:
    // the op fans the jump out and hands the wait to a lane worker, and the
    // pass whose wake carries the completion runs the finish half (sink
    // flush, clock rebase) and the fault report here, on the transport's
    // own thread. While a flight runs, the momentary-op slot is not
    // consumed — pause, play and a newer seek hold their turn behind the
    // seek — so the ORDER the synchronous code enforced survives the split.
    bool applied = consumeSeekCompletion();
    const int op = fSeekFlight == nullptr
                       ? fPendingTransportOp.exchange(kOpNone, std::memory_order_acq_rel)
                       : kOpNone;
    if (op != kOpNone) {
        applyTransportOp(op, fPendingSeekSeconds.load(std::memory_order_relaxed));
        applied = true;
    } else {
        // The test schedule rides the same seam, so the scenarios drive the
        // transport exactly as 6.15a's RPC will. Ops fire in schedule order;
        // everything whose time has passed goes out on this pass (the last
        // seek wins, exactly as a burst of operator clicks would), and the
        // cursor only advances past what was applied — which since 6.22e4
        // also means it stops at a seek whose flight is still waiting.
        const double elapsedMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                      fScheduledStart)
                .count();
        while (fSeekFlight == nullptr && fScheduledCursor < fScheduledOps.size() &&
               elapsedMs >= fScheduledOps[fScheduledCursor].atMs) {
            applyTransportOp(fScheduledOps[fScheduledCursor].kind,
                             fScheduledOps[fScheduledCursor].seconds);
            ++fScheduledCursor;
            applied = true;
        }
    }
    // The mode and mix slots (task 6.15a, review R15). Applied here, on
    // the transport's own thread: setLoop walks the attached-pipeline list
    // (6.6b's rule), and the mixer identity table belongs to the transport
    // this loop owns. Each slot is independent of the momentary op above —
    // muting a source must not swallow a pending seek, and a loop toggle
    // is meaningless as an instant, it is the value EOF will honour.
    // Reap first (6.21c2): a retirement the lane finished gets its GPU half
    // closed here, on the device's owner thread, before anything else the
    // pass decides. The lane's wake brought this pass home for it.
    reapRetirement();
    // The standalone selection first (task 6.15b): a pass that both swaps
    // the track and carries a gain for it must land the gain on the NEW
    // player's mixer, not the one being stopped. The status fields it
    // touches are the same ones the refresh below samples. Since 6.21c2
    // the selection publishes or stages (see applyStandaloneOps) and takes
    // its own locks; the waits it used to hold under fMutex are gone.
    applyStandaloneOps();
    const int loop = fPendingLoop.exchange(-1, std::memory_order_acq_rel);
    if (loop >= 0 && fMediaTransportLive.load(std::memory_order_relaxed)) {
        fAudio->setLoop(loop == 1);
        std::fprintf(stderr, "openshow-worker: transport loop %s\n",
                     loop == 1 ? "on" : "off");
        applied = true;
    }
    // The gain and mute slots carry a target: the background player's
    // kVideoInput (its embedded audio), its kSecondInput (the show audio
    // layer), or kStandaloneTarget — a marker for the separate track's
    // player, which also uses kSecondInput. The per-player guard keeps an
    // intent that raced a teardown from reaching a successor (the exchange
    // drops it: a dropped command, never a late one).
    constexpr int kStandaloneTarget = 2;
    const int gainSource = fPendingGainSource.exchange(-1, std::memory_order_acq_rel);
    if (gainSource >= 0) {
        const bool forTrack = gainSource == kStandaloneTarget;
        AvPlayer& player = forTrack ? *fStandalonePlayer : *fAudio;
        const int identity = forTrack ? AvPlayer::kSecondInput : gainSource;
        if (forTrack ? fStandaloneLive.load(std::memory_order_relaxed)
                     : fMediaTransportLive.load(std::memory_order_relaxed)) {
            player.setVolume(identity,
                             static_cast<float>(fPendingGain.load(std::memory_order_relaxed)));
            applied = true;
        }
    }
    // The master mute goes FIRST: an operator who muted media audio must not
    // hear one more block because a per-source command was queued beside it.
    const int masterMute = fPendingMasterMute.exchange(-1, std::memory_order_acq_rel);
    if (masterMute >= 0) {
        const bool muted = masterMute == 1;
        fAudio->setMasterMute(muted);
        fStandalonePlayer->setMasterMute(muted);
        applied = true;
    }
    const int muteSource = fPendingMuteSource.exchange(-1, std::memory_order_acq_rel);
    if (muteSource >= 0) {
        const bool forTrack = muteSource == kStandaloneTarget;
        AvPlayer& player = forTrack ? *fStandalonePlayer : *fAudio;
        const int identity = forTrack ? AvPlayer::kSecondInput : muteSource;
        if (forTrack ? fStandaloneLive.load(std::memory_order_relaxed)
                     : fMediaTransportLive.load(std::memory_order_relaxed)) {
            player.setMute(identity, fPendingMute.load(std::memory_order_relaxed) == 1);
            applied = true;
        }
    }
    if (!applied) {
        return;
    }
    // An op the operator issued is an op status must report on this pass. A
    // held transport presents nothing after the hold — and the per-frame
    // refresh is what normally carries "playing" — so without this the fact
    // would sit stale until the half-second heartbeat noticed it (task 6.7d,
    // review R19: a pause that still reads "playing" is the swallowed op
    // wearing a different hat).
    std::lock_guard<std::mutex> lock(fMutex);
    refreshMediaStateLocked();
    return;
}

void BackgroundTransport::applyTransportOp(int op, double seconds) {
    // An audio-only transport (a show audio layer with no video picture) is
    // a transport like any other: play, hold and seek reach its player even
    // though there is no pipeline to pace.
    if (!fMediaTransportLive.load(std::memory_order_relaxed)) {
        std::fprintf(stderr, "openshow-worker: transport op ignored, no media is open\n");
        return;
    }
    if (op == kOpPause) {
        fAudio->pause();
        std::fprintf(stderr, "openshow-worker: transport held at %.3f s\n",
                     fAudio->clockSeconds());
        return;
    }
    if (op == kOpPlay) {
        fAudio->play();
        std::fprintf(stderr, "openshow-worker: transport resumed\n");
        return;
    }
    // The jump STARTS here and COMPLETES on a later pass (task 6.22e4,
    // review A5): beginSeekFlight fans the seek out on this thread — the
    // feeders' request queues and the pipelines' asynchronous seeks accept
    // without waiting — and the wait half, the sink flush, the clock
    // rebase and the fault report ride the flight's completion below
    // (consumeSeekCompletion). What applyTransportOps guarantees across
    // the split: no momentary op overtakes an unfinished seek, so the
    // order the synchronous code enforced is the order the operator sees.
    beginSeekFlight(seconds);
}

void BackgroundTransport::beginSeekFlight(double seconds) {
    // Preconditions (both enforced by the pass above): a background is
    // open, and no flight is in progress. The thread spawn is the whole
    // cost of this pass — seeks arrive at operator speed, and one flight
    // at a time keeps the lane trivially bounded.
    fAudio->beginSeekTo(seconds);
    auto flight = std::make_shared<SeekFlight>();
    flight->target = seconds;
    flight->assetPath = fAppliedBackground.assetPath.empty()
                            ? fAppliedAudio.assetPath
                            : fAppliedBackground.assetPath;
    flight->player = fAudio.get();
    AvPlayer* player = flight->player;
    flight->worker = std::thread([this, flight, player] {
        // THE wait this task moved off the frame tick: up to 500 ms per
        // feeder is a poll deadline, not a promise from the container.
        // The player OUTLIVES this thread by ownership: consuming a flight
        // is the render thread's alone, and every path that ends a player
        // (teardown's hand-off, retireAudioInstance) takes the flight with
        // it — the lane joins this thread before a retired player stops.
        flight->applied = player->waitSeekTo(500, &flight->abort, &flight->error);
        flight->done.store(true, std::memory_order_release);
        // The completing pass rides this wake, so even a loop parked on an
        // idle slide lands the finish half and the fault report promptly.
        wakeRenderThread();
    });
    fSeekFlight = std::move(flight);
}

bool BackgroundTransport::consumeSeekCompletion() {
    if (fSeekFlight == nullptr || !fSeekFlight->done.load(std::memory_order_acquire)) {
        return false;  // the worker's wake brings the pass that consumes it
    }
    std::shared_ptr<SeekFlight> flight = std::move(fSeekFlight);
    flight->worker.join();  // returned with its store; this only reaps
    // The finish half, on the transport's own thread (6.22e4): the sink
    // queue, the clock and the fallback position move on the wait's
    // verdict exactly as the synchronous seekTo did — one pass later.
    flight->player->finishSeekTo(flight->target, flight->applied);
    if (flight->applied) {
        std::fprintf(stderr, "openshow-worker: transport seek landed at %.3f s\n",
                     flight->target);
        // A seek that landed is the recovery for a previous refusal on this
        // asset (typed exactly as such): the parts agree again.
        mediaFaultCleared(MediaFault::kSync, flight->assetPath);
        return true;
    }
    // The audio refused the jump. The picture moved (the video seek applies
    // on its own thread), so the audience now hears one position and sees
    // another — that is what the operator has to be told, and the reported
    // position stays whatever the transport actually presents.
    mediaFailed(MediaFault::kSync, flight->assetPath,
                "background seek to " + std::to_string(flight->target) +
                    " s was refused: " +
                    (flight->error.empty() ? "no reason given" : flight->error));
    return true;
}

void BackgroundTransport::drainSeekFlight() {
    // The paths that are not a frame tick (retireAudioInstance's family):
    // the abort ends the feeder waits inside one poll, the join releases
    // the worker, and the completion is DISCARDED with the transport that
    // started it — a jump whose asset is out of service has no story left
    // to tell, and a dropped command is a dropped command, never a late
    // one fired against a successor.
    if (fSeekFlight == nullptr) {
        return;
    }
    fSeekFlight->abort.store(true, std::memory_order_release);
    fSeekFlight->worker.join();
    fSeekFlight.reset();
}

bool BackgroundTransport::primeVideo(const std::string& path,
                                     const std::atomic<bool>* cancelled, std::string* error) {
    // Runs on the CALLER's thread — the prepare executor's, never the render
    // loop. The scripted slow open (Faults.h) belongs here, not in
    // applyBackground: the point of the task is that a slow open is paid on
    // this thread, so the chunked sleep checks the same cooperative flag the
    // executor hands every prepare and a retired prime stops between chunks
    // rather than after the whole wait.
    const auto stopped = [cancelled] {
        return cancelled != nullptr && cancelled->load();
    };
    if (path.empty() || stopped()) {
        return false;
    }
    if (faults().delaysVideoOpen(path)) {
        const int milliseconds = faults().videoOpenDelayMs;
        std::fprintf(stderr, "openshow-worker: scripted slow video prime: %d ms for %s\n",
                     milliseconds, path.c_str());
        for (int elapsed = 0; elapsed < milliseconds && !stopped(); elapsed += 20) {
            std::this_thread::sleep_for(std::chrono::milliseconds(
                std::min(20, milliseconds - elapsed)));
        }
        if (stopped()) {
            return false;
        }
    }
    auto pipeline = std::make_unique<PlaybackPipeline>();
    PlaybackPipeline::Options options;
    options.path = path;
    options.loop = true;
    options.allowVideoCopy = true;
    if (!pipeline->openFile(options, error)) {
        return false;  // nothing parked; the present reopens cold and reports
    }
    // One prime, superseded here. The transport the previous prime held
    // dies on THIS thread (a file-only close: no GPU call, no thread to
    // join), so a stream of prepared videos never grows the primed set and
    // the retirement is never in a frame tick — R10's "superseded primed
    // transports are bounded". The AUDIO half is its own bundle now (review
    // D3/D4) and is not tied to this path, so a new file prime leaves it
    // alone.
    {
        const std::lock_guard<std::mutex> lock(fPrimeMutex);
        fPrimed = std::move(pipeline);
        fPrimedPath = path;
    }
    return true;
}

bool BackgroundTransport::primeAudio(const openshow::scene::Background& wanted,
                                     const openshow::scene::AudioLayer& audio,
                                     const std::atomic<bool>* cancelled,
                                     std::string* error) {
    // Runs on the CALLER's thread — the prepare executor's, never the render
    // loop (task 6.21c1, review F3). An audio bring-up holds real waits:
    // the server connection and negotiation, the feeder opens, and the
    // pre-roll window (up to seconds at a cold daemon). They belong HERE;
    // the frame boundary keeps only the release of the hold, which is
    // microseconds.
    //
    // Review D3: this is NOT tied to a video prime any more. An audio-only
    // cue, a still and a camera with a show layer all reach the frame thread
    // with a bundle ready, and the same video with a new layer stages the new
    // mix rather than cold-opening. Review D4: the player and every flag that
    // describes it are parked as ONE bundle, taken whole.
    const auto stopped = [cancelled] {
        return cancelled != nullptr && cancelled->load();
    };
    const bool wantsVideo = wanted.hasImage && wanted.isVideo && !wanted.isCamera;
    StagedAudioBundle bundle;
    bundle.videoPath = wantsVideo ? wanted.assetPath : std::string();
    bundle.layerPath = audio.present ? audio.assetPath : std::string();
    if ((bundle.videoPath.empty() && bundle.layerPath.empty()) || stopped()) {
        return false;  // a silent scene has nothing to stage
    }
    // Publish "a prime is in flight" for the whole bring-up (review D3): a
    // scene that needs audio and finds no bundle is deferred once instead of
    // opening the file on the frame thread. The flag falls and the loop is
    // woken on every exit — success, failure and cancellation alike — so a
    // deferred scene always gets its retry.
    struct StagingFlag {
        BackgroundTransport* self;
        ~StagingFlag() {
            self->fStagingAudio.store(false, std::memory_order_release);
            self->wakeRenderThread();
        }
    } staging{this};
    fStagingAudio.store(true, std::memory_order_release);

    AvPlayer::Config config;
    config.loop = true;
    config.startHeld = true;
    bool embeddedAudio = false;
    bool layerAudio = false;
    double layerDuration = -1.0;
    std::string loss;
    // The same probes applyBackground runs, on the thread that can afford
    // them: which stream the picture carries and whether the layer is
    // playable. The answers ride the bundle, so the adopt side neither
    // re-probes nor loses the "wanted and lost" story (R18) by going staged.
    if (!bundle.videoPath.empty()) {
        openshow::media::MediaInfo info;
        std::string probeError;
        if (openshow::media::probeMedia(bundle.videoPath, &info, &probeError) ==
            openshow::media::ProbeStatus::kOk) {
            if (!info.audioStreams.empty()) {
                if (info.audioStreams.front().decoderAvailable) {
                    config.videoAudioPath = bundle.videoPath;
                    embeddedAudio = true;
                } else {
                    loss = "background video has an audio stream no decoder can handle; "
                           "playing muted";
                }
            }
        }
    }
    if (!bundle.layerPath.empty()) {
        // The show's audio layer rides the same held player as the video's
        // own stream, so the adopt side gets one clock for both.
        openshow::media::MediaInfo layerInfo;
        std::string layerError;
        if (openshow::media::probeMedia(bundle.layerPath, &layerInfo, &layerError) ==
            openshow::media::ProbeStatus::kOk) {
            layerDuration = layerInfo.durationSeconds;
            if (layerInfo.audioStreams.empty()) {
                loss = "show audio layer '" + bundle.layerPath +
                       "' carries no audio stream; playing silent";
            } else if (!layerInfo.audioStreams.front().decoderAvailable) {
                loss = "show audio layer '" + bundle.layerPath +
                       "' has audio no decoder can handle (" +
                       layerInfo.audioStreams.front().codec + "); playing silent";
            } else {
                config.secondAudioPath = bundle.layerPath;
                layerAudio = true;
            }
        } else {
            loss = "show audio layer '" + bundle.layerPath +
                   "' cannot be opened: " + layerError;
        }
    }
    bundle.embeddedAudio = embeddedAudio;
    bundle.layerAudio = layerAudio;
    bundle.wantsAudio = embeddedAudio || layerAudio;
    bundle.loss = std::move(loss);
    bundle.layerDuration = layerDuration;
    // The player is started even when nothing is playable, exactly as the cold
    // path does: a video with no audio stream still needs a clock and a loop
    // answer, and a scene staged beside it must adopt the same shape rather
    // than leaving a never-started placeholder that reports loop off.
    {
        auto player = std::make_unique<AvPlayer>();
        player->setMasterMute(fMasterMute.load(std::memory_order_relaxed));
        std::string startError;
        if (!player->start(config, &startError)) {
            // A failed start has self-cleaned (6.7a). The failure RIDES the
            // bundle (review D3): the adopt side reports this sentence and
            // plays without audio instead of re-opening the file on the frame
            // thread to discover the same thing.
            bundle.error = "background media audio failed to start: " + startError +
                           "; playing muted";
            bundle.wantsAudio = false;
            bundle.embeddedAudio = false;
            bundle.layerAudio = false;
        } else if (stopped()) {
            return false;  // ~player stops the staged transport on this thread
        } else {
            bundle.player = std::move(player);
        }
    }
    // One bundle, superseded here. The previous one's player dies on THIS
    // thread (~AvPlayer joins feeder, mixer and sink; no GPU is anywhere near
    // it), so a stream of prepares never grows the staged set.
    std::optional<StagedAudioBundle> superseded;
    {
        const std::lock_guard<std::mutex> lock(fPrimeMutex);
        superseded = std::move(fStagedAudio);
        fStagedAudio = std::move(bundle);
    }
    return true;
}

bool BackgroundTransport::backgroundOnAir(const std::string& assetId,
                                          const std::string& path,
                                          const openshow::scene::AudioLayer& audio) const {
    if (path.empty() || !fMediaTransportLive.load(std::memory_order_acquire)) {
        return false;
    }
    const std::lock_guard<std::mutex> lock(fOnAirMutex);
    return fOnAirVideoAssetId == assetId && fOnAirVideoPath == path &&
           fOnAirAudioPath == (audio.present ? audio.assetPath : std::string());
}

bool BackgroundTransport::backgroundChangeBlocked(
    const openshow::scene::Background& wanted,
    const openshow::scene::AudioLayer& audio) const {
    // The audio half is identity too: a scene whose picture is unchanged but
    // whose show gained or lost its track is a new transport, and it must
    // wait for the retirement slot like any other media change.
    const bool same = fAppliedBackground.hasImage == wanted.hasImage &&
                      fAppliedBackground.isVideo == wanted.isVideo &&
                      fAppliedBackground.isCamera == wanted.isCamera &&
                      fAppliedBackground.assetId == wanted.assetId &&
                      fAppliedBackground.assetPath == wanted.assetPath &&
                      fAppliedAudio == audio;
    return fMediaTransportLive.load() && !same && inFlightRetirement() != nullptr;
}

BackgroundTransport::BackgroundApply BackgroundTransport::applyBackground(
    const openshow::scene::Background& wanted,
    const openshow::scene::AudioLayer& audio,
    const GpuHandles& handles,
    const std::function<bool(const std::string&)>& imageResident) {
    // The identity rule (M3 continuity): same asset id, same path, same
    // kind — and the same show audio layer — the transport stays exactly as
    // it is. A lyric change over the same background video therefore
    // disturbs nothing: decode position, the audio clock and the sink all
    // keep rolling; and a song chart stepping its words over one track does
    // not restart the music.
    const bool sameIdentity =
        fMediaTransportLive.load() &&
        fAppliedBackground.hasImage == wanted.hasImage &&
        fAppliedBackground.isVideo == wanted.isVideo &&
        // ND.6: a live camera is its own kind of background. A still or a
        // video sharing an id/path with it is a different picture on air, so
        // the flag and the source both join the identity — otherwise a
        // camera-to-file switch would keep the old transport rolling.
        fAppliedBackground.isCamera == wanted.isCamera &&
        fAppliedBackground.cameraSource == wanted.cameraSource &&
        fAppliedBackground.assetId == wanted.assetId &&
        fAppliedBackground.assetPath == wanted.assetPath &&
        fAppliedAudio == audio;
    if (sameIdentity) {
        // Returning to the current identity needs no retirement capacity.
        // The session can cancel an older pending scene immediately.
        return BackgroundApply::kApplied;
    }
    bool wantsVideo = wanted.hasImage && wanted.isVideo && !wanted.isCamera;
    // Only this thread changes transports. The session retains the whole
    // scene when the bounded retirement slot is occupied, and retries it
    // after the lane's wake; no media-only transition can overtake it.
    reapRetirement();
    // The continuity rule (DR.0): a visual-only change — a different still,
    // a colour, a gradient, a camera — leaves the audible mix exactly as it
    // is, so the player keeps its position, clock, sink, gain and mute, and
    // only the picture is rebuilt. A lyric cue that gains a picture must not
    // restart the music. (A change that alters the mix — a different video's
    // embedded stream or a different track — takes the normal path below.)
    if (fMediaTransportLive.load() && audio.present && !wantsVideo &&
        audioIdentityOf(fAppliedBackground, fAppliedAudio) == audioIdentityOf(wanted, audio)) {
        if (inFlightRetirement() != nullptr) {
            return BackgroundApply::kDeferred;  // the picture's lane is still busy
        }
        return applyPictureOnlyChange(wanted, audio, imageResident);
    }
    if (backgroundChangeBlocked(wanted, audio)) return BackgroundApply::kDeferred;
    // The staged bundle for THIS identity (review D3/D4): taken WHOLE under
    // one lock — the held player and every flag that describes it — so a
    // prepare that publishes the next bundle in the same window can never
    // hand its source flags to this player. A bundle that does not match is
    // left for its own scene; a scene with none cold-opens below (the bench
    // path, and the honest recovery when a prime was superseded).
    std::optional<StagedAudioBundle> stagedBundle;
    {
        const std::lock_guard<std::mutex> lock(fPrimeMutex);
        if (fStagedAudio.has_value() &&
            fStagedAudio->videoPath == (wantsVideo ? wanted.assetPath : std::string()) &&
            fStagedAudio->layerPath == (audio.present ? audio.assetPath : std::string())) {
            stagedBundle = std::move(fStagedAudio);
            fStagedAudio.reset();
        }
    }
    if (stagedBundle.has_value() && fStagedBundleTakeHook) {
        // Test seam (review D4): the window between "taken" and "adopted".
        fStagedBundleTakeHook();
    }
    if (!stagedBundle.has_value() && (wantsVideo || audio.present) &&
        fStagingAudio.load(std::memory_order_acquire)) {
        // A prime is in flight for this scene's media (review D3): the bundle
        // is moments away, so DEFER the scene rather than open the file on the
        // frame thread. The prime always ends — success, failure or
        // cancellation — and always wakes this loop, so the retry is bounded
        // and latest-wins: a newer scene supersedes this one in the store.
        return BackgroundApply::kDeferred;
    }

    teardownBackgroundTransport();
    fAppliedBackground = wanted;
    fAppliedAudio = audio;
    fAppliedAudioDuration = -1.0;

    const bool wantsCamera = wanted.isCamera;
    const bool wantsLayer = audio.present;
    const int videoBit = 1;
    const int leanBit = 4;  // matches rebuildMediaSourcesLocked's "audio" row

    if (wantsCamera) {
        // The live hub is the session's, but the media status is this
        // transport's mirror: a camera is on air, has no clock, seek, loop
        // or video, and is never commandable as a file. A show's audio
        // layer may still ride beside it, so the status names the camera
        // and the audio refresh below keeps the audible half honest.
        std::lock_guard<std::mutex> lock(fMutex);
        // Opening a source waits for its first frame, so "on air" is not yet
        // "playing": the render thread promotes playing when it actually draws
        // a frame (mediaCameraPlaying). Until then the colour fill is what the
        // audience sees, and the status says so honestly.
        fMedia.playing = false;
        fMedia.path = wanted.cameraSource;
        fMedia.error.clear();
        fMediaFault.reset();
        fMedia.position = -1.0;
        fMedia.audio = "none";
        fMedia.live = true;
        fMedia.loop = false;
        fMedia.duration = -1.0;
        fMedia.muted = fMasterMute.load(std::memory_order_relaxed);
        fMediaTransportLive.store(false, std::memory_order_release);
        fMediaSourceMask.store(fMediaSourceMask.load(std::memory_order_relaxed) &
                                   ~(videoBit | leanBit),
                               std::memory_order_release);
        rebuildMediaSourcesLocked();
        if (!wantsLayer) {
            return BackgroundApply::kApplied;
        }
    } else if (!wantsVideo) {
        // Colour fill or still image: no video transport. The picture the
        // audience should see is named even when it is a broken still (a
        // degradation that must be reported); the show's audio layer, if
        // any, starts below and makes this an audio-only transport.
        //
        // The cache is consulted BEFORE fMutex, so even the pure lookup of
        // 6.10 keeps this probe out of the lock that status and layer
        // commands wait on. Task 6.10 made it a lookup with no disk access:
        // the pin taken at prepare guarantees the preload's answer is
        // still resident, so a miss here genuinely means "no picture for
        // this asset", which is exactly what the report below says.
        const bool brokenStill =
            wanted.hasImage && !wanted.isVideo && !imageResident(wanted.assetPath);
        {
            std::lock_guard<std::mutex> lock(fMutex);
            fMedia.playing = false;
            fMedia.position = -1.0;
            fMedia.audio = "none";
            fMedia.path = wantsLayer ? audio.assetPath
                                     : (wanted.hasImage ? wanted.assetPath : std::string());
            fMedia.muted = fMasterMute.load(std::memory_order_relaxed);
            if (wantsLayer) {
                fMedia.live = true;
            } else {
                fMedia.live = false;
                fMedia.loop = false;
                fMedia.duration = -1.0;
            }
            if (brokenStill) {
                const bool alreadyReported =
                    fMediaFault && fMediaFault->first == MediaFault::kImage &&
                    fMediaFault->second == wanted.assetPath;
                if (!alreadyReported) {
                    mediaFailedLocked(MediaFault::kImage, wanted.assetPath,
                                      "background image: '" + wanted.assetPath +
                                          "' is not in the decoded cache; the slide is drawing "
                                          "its colour fill");
                }
            } else {
                fMedia.error.clear();
                fMediaFault.reset();
            }
        }
        if (!wantsLayer) {
            std::lock_guard<std::mutex> lock(fMutex);
            rebuildMediaSourcesLocked();
            return brokenStill ? BackgroundApply::kFailed : BackgroundApply::kApplied;
        }
    }

    std::string error;
    std::unique_ptr<PlaybackPipeline> pipeline;
    if (wantsVideo) {
        PlaybackPipeline::Gpu gpu;
        gpu.device = handles.device;
        gpu.physicalDevice = handles.physicalDevice;
        gpu.graphicsQueueIndex = handles.graphicsQueueIndex;
        gpu.uploadQueueIndex = handles.uploadQueueIndex;
        gpu.uploadQueue = handles.uploadQueue;
        gpu.context = handles.context;

        PlaybackPipeline::Options options;
        options.path = wanted.assetPath;
        // A background loops: the lyric service expects the visual to keep
        // running (FreeShow's loop-by-default rule; a per-slide setting can
        // come with the editor UI). The 1.5i copying-compatibility reopen is
        // allowed — losing a background to a decoder gap is worse than a
        // frame-copy.
        options.loop = true;
        options.allowVideoCopy = true;

        // Task 6.9b (review R10): the file prime, if one covers this picture.
        // The AUDIO half is a separate bundle (review D3/D4) and is taken
        // below, keyed by the complete media identity.
        {
            const std::lock_guard<std::mutex> lock(fPrimeMutex);
            if (fPrimed && fPrimedPath == wanted.assetPath) {
                pipeline = std::move(fPrimed);
                fPrimedPath.clear();
            }
        }
        if (pipeline != nullptr && !pipeline->attachGpu(gpu, &error)) {
            // The GPU half failed on a transport whose file half was sound;
            // retire it (a close of a not-yet-started pipeline: no threads to
            // join) and let the cold open below report the bad news once.
            std::fprintf(stderr,
                         "openshow-worker: primed background %s cannot attach (%s); "
                         "reopening cold\n",
                         wanted.assetPath.c_str(), error.c_str());
            pipeline.reset();
        }
        if (pipeline == nullptr) {
            pipeline = std::make_unique<PlaybackPipeline>();
            if (!pipeline->openGpu(gpu, options, &error)) {
                std::fprintf(stderr, "openshow-worker: background video %s: %s\n",
                             wanted.assetPath.c_str(), error.c_str());
                mediaFailed(MediaFault::kVideo, wanted.assetPath,
                            "background video: " + error);
                // A broken picture must not take a working audio layer down
                // with it; an audio-only transport still starts below.
                if (!wantsLayer) {
                    std::lock_guard<std::mutex> lock(fMutex);
                    rebuildMediaSourcesLocked();
                    return BackgroundApply::kFailed;
                }
                // The layer still plays, and this is now an audio-only
                // transport: nothing below may treat the broken picture as
                // the transport's path or claim its audio.
                pipeline.reset();
                wantsVideo = false;
            }
        }
    }

    // Embedded audio (the background video's own stream), when it can be
    // decoded, becomes one half of the audible mix; the show's audio layer is
    // the other. One AvPlayer owns both, so the clock that paces the video is
    // the clock that paces the track — the two cannot drift apart.
    bool wantsAudio = false;
    bool embeddedAudio = false;
    bool layerAudio = false;
    std::string audioLoss;
    double layerDuration = -1.0;
    if (stagedBundle.has_value()) {
        // A bundle covers this identity: its player (when it has one) is
        // adopted, and its flags, loss and duration are the ones that were
        // measured for it — the pairing review D4 requires.
        embeddedAudio = stagedBundle->embeddedAudio;
        layerAudio = stagedBundle->layerAudio;
        wantsAudio = stagedBundle->wantsAudio;
        audioLoss = stagedBundle->loss;
        layerDuration = stagedBundle->layerDuration;
        if (!stagedBundle->error.empty()) {
            // The staged bring-up failed for THIS identity. Report that
            // sentence and play without audio; re-opening the file on the
            // frame thread would only discover the same thing more slowly
            // (review D3).
            audioLoss = stagedBundle->error;
        }
        if (stagedBundle->player != nullptr) {
            // The instance swap is under fMutex (the 6.21c1 ownership rule): a
            // status reader on another thread sees the outgoing silent
            // placeholder or the staged player, never a player being destroyed.
            std::unique_ptr<AvPlayer> placeholder;
            {
                const std::lock_guard<std::mutex> lock(fMutex);
                placeholder = std::move(fAudio);
                fAudio = std::move(stagedBundle->player);
            }
            fAudio->setMasterMute(fMasterMute.load(std::memory_order_relaxed));
            // Release the hold BEFORE anything else: no pipeline is attached
            // to this player yet, so the release touches only its own sink and
            // clock — and the first audible millisecond belongs to the scene
            // this transport was staged for.
            fAudio->play();
        }
    } else {
        AvPlayer::Config config;
        config.loop = true;
        if (wantsVideo) {
            openshow::media::MediaInfo info;
            if (openshow::media::probeMedia(wanted.assetPath, &info, &error) ==
                openshow::media::ProbeStatus::kOk) {
                if (!info.audioStreams.empty()) {
                    if (info.audioStreams.front().decoderAvailable) {
                        config.videoAudioPath = wanted.assetPath;
                        embeddedAudio = true;
                    } else {
                        audioLoss =
                            "background video has an audio stream no decoder can handle; "
                            "playing muted";
                    }
                }
            } else {
                // The container opened a moment ago, so a second probe failing
                // is a curiosity rather than a story; it stays in the log.
                std::fprintf(stderr, "openshow-worker: background audio probe %s: %s\n",
                             wanted.assetPath.c_str(), error.c_str());
            }
        }
        if (wantsLayer) {
            openshow::media::MediaInfo info;
            std::string layerError;
            if (openshow::media::probeMedia(audio.assetPath, &info, &layerError) ==
                openshow::media::ProbeStatus::kOk) {
                layerDuration = info.durationSeconds;
                if (info.audioStreams.empty()) {
                    audioLoss = "show audio layer '" + audio.assetPath +
                                "' carries no audio stream; playing silent";
                } else if (!info.audioStreams.front().decoderAvailable) {
                    audioLoss = "show audio layer '" + audio.assetPath +
                                "' has audio no decoder can handle (" +
                                info.audioStreams.front().codec + "); playing silent";
                } else {
                    config.secondAudioPath = audio.assetPath;
                    layerAudio = true;
                }
            } else {
                audioLoss = "show audio layer '" + audio.assetPath +
                            "' cannot be opened: " + layerError;
            }
        }
        wantsAudio = embeddedAudio || layerAudio;
        if (!fAudio->start(config, &error)) {
            std::fprintf(stderr,
                         "openshow-worker: background audio: %s (the picture continues, "
                         "paced by the monotonic clock)\n",
                         error.c_str());
            if (wantsAudio) {
                audioLoss = "background media audio failed to start: " + error +
                            "; playing muted";
            }
            embeddedAudio = false;
            layerAudio = false;
        }
        fAudio->setMasterMute(fMasterMute.load(std::memory_order_relaxed));
    }
    if (pipeline != nullptr) {
        fAudio->attachPipeline(pipeline.get());
    }

    fAppliedAudioDuration = layerDuration;

    if (pipeline != nullptr) {
        // The seam swaps before frames can flow: presenter and clock
        // together, so a submission is never paced by the previous
        // background's clock.
        fBackground = std::move(pipeline);
        setVideoClock(fAudio->clockFn());
        fPresenter = fBackground.get();
        // The sink names its producer (task 6.21c2): once a teardown hands
        // this pipeline to the retirement lane, the seam's generation filter
        // drops anything still in flight from it, so a slow exit can never
        // present a frame past its transport. The captured raw pointer names
        // an object the retirement slot keeps alive until after the pacing
        // thread's join.
        if (!fBackground->start(
                [this, producer = fBackground.get()](const VideoSubmission& submission) {
                    submitVideo(submission, producer);
                },
                fAudio->clockFn(), &error)) {
            std::fprintf(stderr, "openshow-worker: background decode %s: %s\n",
                         wanted.assetPath.c_str(), error.c_str());
            mediaFailed(MediaFault::kVideo, wanted.assetPath,
                        "background decode: " + error);
            // No quiesce boundary is needed here (6.6b): a false start() means
            // no pacing or decode thread was ever spawned, so nothing can read
            // the presenter or the clock after this point. The audio player
            // started for this transport goes down with it (idempotent).
            setVideoClock(nullptr);
            fPresenter = nullptr;
            fAudio->detachPipeline(fBackground.get());
            retireAudioInstance();  // 6.21c1: the adopted player goes out of service
            fBackground->close();
            fBackground.reset();
            std::lock_guard<std::mutex> lock(fMutex);
            rebuildMediaSourcesLocked();
            return BackgroundApply::kFailed;
        }
    } else {
        setVideoClock(fAudio->clockFn());
        fPresenter = nullptr;
    }

    // The identity table mirrors what THIS transport handed AvPlayer. The
    // video's embedded audio is bit1, the show's audio layer bit2 (value 4);
    // the standalone track's own bit (value 2) is preserved — the background
    // is not that track's owner. Stored BEFORE mediaPlaying so the first
    // status it publishes already lists the sources.
    fMediaSourceMask.store(
        (fMediaSourceMask.load(std::memory_order_relaxed) & ~(videoBit | leanBit)) |
            (embeddedAudio ? videoBit : 0) | (layerAudio && wantsLayer ? leanBit : 0),
        std::memory_order_release);
    {
        // Publish the identity BEFORE the live flag: a prepare that reads
        // live=true then takes fOnAirMutex must see the path it is asking
        // about, or it re-primes the clip already on air (harmless, but the
        // head start it pays for is the one this avoids).
        const std::lock_guard<std::mutex> lock(fOnAirMutex);
        fOnAirVideoAssetId = wantsVideo ? wanted.assetId : std::string();
        fOnAirVideoPath = wantsVideo ? wanted.assetPath : std::string();
    }
    fMediaTransportLive.store(true, std::memory_order_release);
    mediaPlaying(wantsCamera ? wanted.cameraSource
                             : (wantsVideo ? wanted.assetPath
                                           : (wantsLayer ? audio.assetPath
                                                         : std::string())));
    if (!audioLoss.empty()) {
        {
            std::lock_guard<std::mutex> lock(fMutex);
            fExpectedAudioLoss = audioLoss;
        }
        mediaFailed(MediaFault::kAudio, wantsLayer ? audio.assetPath : wanted.assetPath,
                    audioLoss);
    }
    // The M5 clock rule, visible at transport start: one line naming which
    // clock now paces the transport.
    std::fprintf(stderr, "openshow-worker: background transport %s (%s clock)\n",
                 (wantsVideo ? wanted.assetPath
                             : (wantsCamera ? wanted.cameraSource : audio.assetPath))
                     .c_str(),
                 fAudio->audioLive() ? "audio" : "monotonic");
    // Test-only transport script (task 6.7d, review R19), armed per transport
    // and applied by the render loop through the same seam 6.15a's RPC will
    // use. Production leaves the variable unset and this stays empty.
    fScheduledCursor = 0;
    fScheduledStart = std::chrono::steady_clock::now();
    const char* const opSpec = std::getenv("OPENSHOW_TEST_TRANSPORT_OPS");
    fScheduledOps = opSpec != nullptr ? parseScheduledOps(opSpec) : std::vector<ScheduledOp>{};
    if (!fScheduledOps.empty()) {
        std::fprintf(stderr, "openshow-worker: %zu test transport ops armed for %s\n",
                     fScheduledOps.size(), wanted.assetPath.c_str());
    }
    return fBackground != nullptr ? BackgroundApply::kVideoStarted
                                  : BackgroundApply::kApplied;
}

BackgroundTransport::BackgroundApply BackgroundTransport::applyPictureOnlyChange(
    const openshow::scene::Background& wanted, const openshow::scene::AudioLayer& audio,
    const std::function<bool(const std::string&)>& imageResident) {
    // The player stays: same audible identity, so position, clock, sink,
    // gain and mute are exactly what the audience already has (DR.0). Only
    // the PICTURE is retired, through the same lane a full teardown uses —
    // the pipeline's join belongs off this thread like every other.
    if (fBackground != nullptr) {
        fBackground->requestStop();
        fPresenter.store(nullptr, std::memory_order_relaxed);
        setVideoClock(nullptr);
        // Detaching the clock's owner relation is seam work and belongs to
        // the thread that swaps transports (6.6b), before the hand-off.
        fAudio->detachPipeline(fBackground.get());
        auto retirement = std::make_shared<Retirement>();
        retirement->pipeline = std::move(fBackground);
        retirement->worker = std::thread([this, r = retirement] {
            r->pipeline->join();
            r->laneDone.store(true, std::memory_order_release);
            wakeRenderThread();  // the reap's prompt, as every lane's is
        });
        {
            const std::lock_guard<std::mutex> lock(fMutex);
            fPendingVideo.reset();
            fDrawnVideo.reset();
            fRetirement = std::move(retirement);
        }
    }
    constexpr int kVideoBit = 1;
    constexpr int kLeanBit = 4;  // the "audio" row in rebuildMediaSourcesLocked
    const bool brokenStill =
        wanted.hasImage && !wanted.isVideo && !wanted.isCamera &&
        !imageResident(wanted.assetPath);
    {
        std::lock_guard<std::mutex> lock(fMutex);
        // The picture's own reading. The audible half keeps what it has: the
        // player is playing, the layer's duration still stands, and the mute
        // is the operator's standing answer.
        fMedia.playing = false;
        fMedia.position = -1.0;
        fMedia.path = wanted.isCamera ? wanted.cameraSource : audio.assetPath;
        fMedia.live = true;
        fMedia.loop = false;
        fMedia.duration = fAppliedAudioDuration;
        fMedia.muted = fMasterMute.load(std::memory_order_relaxed);
        fMediaSourceMask.store(
            (fMediaSourceMask.load(std::memory_order_relaxed) & ~kVideoBit) | kLeanBit,
            std::memory_order_release);
        if (brokenStill) {
            const bool alreadyReported =
                fMediaFault && fMediaFault->first == MediaFault::kImage &&
                fMediaFault->second == wanted.assetPath;
            if (!alreadyReported) {
                mediaFailedLocked(MediaFault::kImage, wanted.assetPath,
                                  "background image: '" + wanted.assetPath +
                                      "' is not in the decoded cache; the slide is drawing "
                                      "its colour fill");
            }
        } else {
            fMedia.error.clear();
            fMediaFault.reset();
        }
        rebuildMediaSourcesLocked();
    }
    {
        // The published identity follows the picture, so a prepare that asks
        // after this change sees the new one.
        const std::lock_guard<std::mutex> lock(fOnAirMutex);
        fOnAirVideoAssetId.clear();
        fOnAirVideoPath.clear();
        fOnAirAudioPath = audio.present ? audio.assetPath : std::string();
    }
    fAppliedBackground = wanted;
    fAppliedAudio = audio;
    return brokenStill ? BackgroundApply::kFailed : BackgroundApply::kApplied;
}

void BackgroundTransport::teardownBackgroundTransport() {
    reapRetirement();  // a completed retirement closes here (render thread)
    mediaStopped();
    // The transport this script was armed for is going away (task 6.7d): a
    // pending intent and any later scheduled ops must not fire against a
    // successor that was never asked for them.
    fScheduledOps.clear();
    fScheduledCursor = 0;
    fPendingTransportOp.store(kOpNone, std::memory_order_release);
    // Same rule for the 6.15a slots — and live goes first: an RPC that
    // checked the flag a breath ago has its armed intent cleared here,
    // so a dropped command is a dropped command, never a late one fired
    // against a successor transport that was never asked. The standalone
    // track's bits and slots are NOT touched (task 6.15b): the track
    // outlives this transport, and its pending gain belongs to its own
    // player.
    fMediaTransportLive.store(false, std::memory_order_release);
    {
        // Clear after the flag: a reader that sees live=false never has to
        // compare this identity, and one that already passed the check is
        // answering a question about the transport that is being retired.
        const std::lock_guard<std::mutex> lock(fOnAirMutex);
        fOnAirVideoAssetId.clear();
        fOnAirVideoPath.clear();
    }
    fMediaSourceMask.store(fMediaSourceMask.load(std::memory_order_relaxed) & ~(1 | 4),
                           std::memory_order_release);
    fPendingLoop.store(-1, std::memory_order_release);
    const int pendingGain = fPendingGainSource.load(std::memory_order_relaxed);
    if (pendingGain == AvPlayer::kVideoInput || pendingGain == AvPlayer::kSecondInput) {
        fPendingGainSource.store(-1, std::memory_order_release);
    }
    const int pendingMute = fPendingMuteSource.load(std::memory_order_relaxed);
    if (pendingMute == AvPlayer::kVideoInput || pendingMute == AvPlayer::kSecondInput) {
        fPendingMuteSource.store(-1, std::memory_order_release);
    }
    if (!fBackground) {
        retireAudioInstance();  // idempotent: a never-started player tears down clean
        fAppliedBackground = openshow::scene::Background{};
        fAppliedAudio = openshow::scene::AudioLayer{};
        fAppliedAudioDuration = -1.0;
        return;
    }
    // Quiesce, then hand off (task 6.21c2, review F3, re-expressing R6).
    // The old order — requestStop, join, close, all here — put the
    // producer's exit wait on the frame tick: a blocking read inside the
    // pacing thread held the audience's frames for its whole timeout. The
    // generation rule replaces that wait:
    //
    //   * requestStop() wakes every wait the pacing thread can park in
    //     (clock sleep, ring poll, uploader commit). It does not wait; it
    //     only opens the exit;
    //   * the seam closes to the old producer before its successor
    //     arrives: the atomic presenter goes null (an in-flight submit
    //     that took fMutex before this point still lands in the pre-
    //     retirement seam; any submit after it sees the filter and is
    //     retired work), and the seam's slots — pending, drawn, clock —
    //     forget this transport;
    //   * everything the producer can still REACH — the pipeline it paces,
    //     the player whose clock function it carries — now lives in the
    //     retirement slot, so the thread touches nothing but live memory
    //     until it returns;
    //   * the lane joins it, stops the player, and the RENDER thread then
    //     closes the GPU half (Ganesh keeps its owner, and the close
    //     strictly follows the join).
    //
    // Capacity rule (task 6.22d, review A4): the ONE retirement slot must be
    // empty here. It used to sleep for the prior lane's join — an untimed
    // wait on the realtime thread before the current transport's even got
    // its stop request, so two ordinary operator changes plus one slow exit
    // froze the audience. The wait cannot belong to this function's ordinary
    // path any more: applyBackground DEFERS before reaching here, and
    // shutdown drains before calling here. Reaching this line with the slot
    // occupied is a routing bug — and draining is the only total response
    // (overwriting the slot would destroy a joinable std::thread with the
    // lane's own last reference), so the loop below is a logged invariant
    // violation, not the overflow policy it used to be.
    if (auto prior = inFlightRetirement(); prior != nullptr) {
        std::fprintf(stderr, "openshow-worker: BUG: retirement hand-off over a "
                             "busy slot; the caller bypassed 6.22d's capacity check\n");
        while (!prior->laneDone.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        reapRetirement();
    }
    fBackground->requestStop();
    fPresenter.store(nullptr, std::memory_order_relaxed);
    setVideoClock(nullptr);
    // Detaching the clock's owner relation is seam work, and AvPlayer's
    // attach list belongs to the thread that swaps transports (6.6b) — so
    // it happens here, before the hand-off, never on the lane.
    fAudio->detachPipeline(fBackground.get());
    // Task 6.22c (review A3): the outgoing audio goes SILENT here, at the
    // hand-off — what the retirement holds is the objects' lifetime, not a
    // licence to keep playing. pause() deactivates the sink stream (server
    // consumption stops inside one slice) and latches the clock, and it
    // joins no thread at all: the feeder/mixer/sink shutdown stays the
    // lane's audio.reset() work below. Before this, the retired clip stayed
    // AUDIBLE for as long as its producer's join took — a slow exit
    // stretched the lifetime, and a successor that released its staged
    // audio did so beside it. The fan-out touches no pipeline: detach just
    // emptied the attach list, so this is the audio half alone.
    fAudio->pause();
    auto retirement = std::make_shared<Retirement>();
    retirement->pipeline = std::move(fBackground);
    // Task 6.22e4 (review A5): a seek still waiting is waiting on THIS
    // player's feeders, so the flight retires with the player. Abort first
    // — the feeder waits end inside one poll — and the lane joins the
    // worker before the player's stop below; without that order the worker
    // would be waiting on feeders a dead AvPlayer owned. The completion is
    // discarded: the transport it described is out of service (teardown
    // already dropped its identity and its pending ops above), so the jump
    // reports to nobody.
    retirement->pendingSeek = std::move(fSeekFlight);
    if (retirement->pendingSeek != nullptr) {
        retirement->pendingSeek->abort.store(true, std::memory_order_release);
    }
    {
        std::lock_guard<std::mutex> lock(fMutex);
        fPendingVideo.reset();
        fDrawnVideo.reset();
        // The audio half moves out with its pipeline (6.21c1's instance
        // ownership): the stop is the lane's work after the join, and until
        // it runs the retired player still answers its producer's final
        // clock reads. The seam gets the fresh silent instance at once, so
        // no status reader ever sees the outgoing one being taken down.
        retirement->audio = std::move(fAudio);
        fAudio = std::make_unique<AvPlayer>();
    }
    retirement->worker = std::thread([this, r = retirement] {
        if (r->pendingSeek != nullptr) {
            // 6.22e4: the seek worker waits on the player's feeders, so it
            // goes down BEFORE the player does. The abort was set at the
            // hand-off, so this join costs a poll, not the 500 ms deadline
            // the worker was under.
            r->pendingSeek->worker.join();
        }
        r->pipeline->join();  // THE wait this task moved off the frame tick
        // The player's DESTRUCTION is lane work (~AvPlayer joins its
        // feeder, mixer and sink threads), but its availability rides
        // fMutex (task 6.22c): move it out under the lock so the retiring
        // audio's delivery seam sees the player or sees nothing, never a
        // dying AvPlayer — then stop it outside the lock, as every other
        // player death does.
        std::unique_ptr<AvPlayer> audio;
        {
            const std::lock_guard<std::mutex> lock(fMutex);
            audio = std::move(r->audio);
        }
        audio.reset();
        r->laneDone.store(true, std::memory_order_release);
        // The render thread owes the GPU half a close. This wake is the
        // reap's prompt, so even a loop sleeping an idle slide comes back
        // for it: nothing from the old transport outlives the next pass.
        wakeRenderThread();
    });
    {
        // The slot itself rides fMutex since 6.22c — the seam reads it from
        // bench threads, and a shared_ptr is not its own synchronisation.
        const std::lock_guard<std::mutex> lock(fMutex);
        fRetirement = std::move(retirement);
    }
    fAppliedBackground = openshow::scene::Background{};
}

void BackgroundTransport::reapRetirement() {
    // Render thread only (the loop's pass, applyBackground's head, and the
    // shutdown drain): the close is a Ganesh release and must keep the
    // device's owner thread.
    const std::shared_ptr<Retirement> retirement = inFlightRetirement();
    if (!retirement) {
        return;
    }
    if (!retirement->laneDone.load(std::memory_order_acquire)) {
        return;  // the lane wakes this thread when it is done
    }
    // Close follows the join (R6's order kept): no producer can be uploading
    // into a slot this releases. The unique_ptr itself goes with the slot's
    // last reference — which is here, after the worker is reaped, so the
    // (post-close, GPU-quiet) destructor runs on this thread.
    if (retirement->pipeline) {
        retirement->pipeline->close();
    }
    retirement->worker.join();
    const std::lock_guard<std::mutex> lock(fMutex);
    fRetirement.reset();
}

std::optional<BackgroundTransport::RetiringAudioDelivery>
BackgroundTransport::retiringAudioDelivery() const {
    // fMutex is the whole synchronisation surface: the lane moves the
    // player out under it, the reap resets the slot under it, and the
    // sink's counters are atomics by construction — the status mirror
    // reads a LIVE player's the same way, from this same lock.
    const std::lock_guard<std::mutex> lock(fMutex);
    if (!fRetirement || !fRetirement->audio) {
        return std::nullopt;
    }
    const AvPlayer& audio = *fRetirement->audio;
    return RetiringAudioDelivery{audio.paused(), audio.sinkActive(),
                                 audio.sinkStats().deliveredFrames};
}

void BackgroundTransport::retireAudioInstance() {
    // One place an AvPlayer instance goes out of service. A seek flight
    // still waiting on THIS instance's feeders cannot outlive it (6.22e4):
    // drain the flight first — abort, join, discard — before the swap.
    drainSeekFlight();
    // The swap happens
    // under fMutex so the status readers that take the lane on another
    // thread see either the outgoing instance or the fresh silent one —
    // never a player mid-destruction. The stop then runs outside the lock,
    // exactly where the old `fAudio.stop()` ran: joins of feeder, mixer and
    // sink threads never hold up a status read.
    std::unique_ptr<AvPlayer> retiring;
    {
        const std::lock_guard<std::mutex> lock(fMutex);
        retiring = std::move(fAudio);
        fAudio = std::make_unique<AvPlayer>();
        // The master mute is worker state, not instance state: a transport
        // rebuilt while the operator has media audio switched off must come
        // up silent, not surprise the audience.
        fAudio->setMasterMute(fMasterMute.load(std::memory_order_relaxed));
    }
    retiring.reset();  // ~AvPlayer stops; a never-started instance dies clean
}

double BackgroundTransport::videoClockSecondsLocked(
    std::chrono::steady_clock::time_point now) const {
    if (fVideoClock) {
        return fVideoClock();
    }
    return std::chrono::duration<double>(now - fRunStart).count();
}

}  // namespace openshow::worker
