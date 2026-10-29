/*
 * BackgroundTransport implementation: the audience-side media, moved out of
 * OutputSession by task 6.18b with the behaviour and the thread rules intact
 * (the R6 quiesce boundary, the newest-wins intent slots, the cross-transport
 * sources list, the per-class sticky fault). Read session/BackgroundTransport.h
 * first: it states the rules and the locking; this file is those rules
 * running.
 */

#include "session/BackgroundTransport.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <utility>

#include <GLFW/glfw3.h>

#include "media/MediaProbe.h"

namespace openshow::worker {

BackgroundTransport::~BackgroundTransport() {
    shutdown();
}

void BackgroundTransport::shutdown() {
    // End everything by name (6.9c's order): the background transport first
    // — it decodes into frame slots on the session's device and context, so
    // it goes before every GPU resource it references (task 5.5). Every
    // part is idempotent: a never-started transport tears down as cleanly
    // as a running one, which is what makes the destructor safe.
    teardownBackgroundTransport();
    // The standalone track is deliberately NOT part of the background
    // transport's lifetime (task 6.15b) — which is exactly why shutdown
    // must end it by name. Live first, teardown's rule: a command that
    // races this refuses, never lands on a stopping player; the sticky
    // error keeps its path.
    fStandaloneLive.store(false, std::memory_order_release);
    fMediaSourceMask.store(fMediaSourceMask.load(std::memory_order_relaxed) & ~2,
                           std::memory_order_release);
    fStandalonePlayer.stop();  // idempotent
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
    return fPresenter->acquireImage(held->token, held->sequence);
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

void BackgroundTransport::mediaPlaying(const openshow::scene::Background& background) {
    std::lock_guard<std::mutex> lock(fMutex);
    // Task 6.7c (review R18): the transport is UP, which is not yet the
    // statement "the audience is watching frames". playing waits for the
    // first frame the transport actually hands over (the frame refresh
    // promotes it); position -1 and the lane say what is true meanwhile.
    fMedia.playing = false;
    fMedia.path = background.assetPath;
    fMedia.error.clear();  // a successful open replaces any old failure
    fMediaFault.reset();
    fMedia.position = -1.0;  // until the transport presents its head
    fMedia.audio = AvPlayer::laneName(mediaLaneLocked());
    // The transport's mode and mix answer from the first status (task
    // 6.15a): the frame refresh keeps them current, but the half-second
    // before the first frame should already read the truth — the mask is
    // stored before this call so the list starts complete.
    fMedia.live = true;
    fMedia.loop = fAudio.loopEnabled();
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
    return fAudio.audioLaneState();
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
    fMedia.loop = fAudio.loopEnabled();
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
    } else if (fAudio.paused()) {
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
    std::optional<VideoSubmission> dropped;
    {
        std::lock_guard<std::mutex> lock(fMutex);
        if (fPendingVideo) {
            dropped = fPendingVideo;
        }
        fPendingVideo = submission;
    }
    // A slot replaced before ever being drawn goes back now, with no GPU
    // semaphore: nothing ever sampled it. The release carries the
    // replaced submission's sequence as its proof of ownership (6.6a).
    if (dropped && fPresenter && dropped->token != submission.token) {
        fPresenter->onDropped(dropped->token, dropped->sequence);
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
    return (mask & (source == AudioSource::kVideo ? 1 : 2)) != 0;
}

bool BackgroundTransport::requestMediaGain(AudioSource source, double gain, std::string* error) {
    if (source == AudioSource::kVideo ? !fMediaTransportLive.load() : !fStandaloneLive.load()) {
        if (error != nullptr) {
            *error = source == AudioSource::kVideo ? "no background media is playing"
                                                   : "no standalone track is playing";
        }
        return false;
    }
    // See requestMediaMute: for the track, live IS registration; the
    // video leg still needs its own gate for the live-but-silent clip.
    if (source == AudioSource::kVideo && !mediaSourceLive(source)) {
        if (error != nullptr) {
            *error = "this background has no video audio source";
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

bool BackgroundTransport::requestMediaMute(AudioSource source, bool muted, std::string* error) {
    if (source == AudioSource::kVideo ? !fMediaTransportLive.load() : !fStandaloneLive.load()) {
        if (error != nullptr) {
            *error = source == AudioSource::kVideo ? "no background media is playing"
                                                   : "no standalone track is playing";
        }
        return false;
    }
    // The standalone gate above already IS the registration: bit1 of the
    // mask is stored by the swap that started the player and cleared by
    // the one that stopped it, so a live track has its mixer slot — there
    // is no second kind of "absent" to refuse. The video leg keeps its
    // own gate because a playing silent clip is live without one.
    if (source == AudioSource::kVideo && !mediaSourceLive(source)) {
        if (error != nullptr) {
            *error = "this background has no video audio source";
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

bool BackgroundTransport::requestStandaloneAudio(const std::string& path, std::string* error) {
    // A removal always accepts, even with a broken track on file: taking
    // the track down is the operator's last resort, and the sticky error
    // stays paired with the path it describes (5.5d's rule, read inside
    // this block).
    if (path.empty()) {
        {
            std::lock_guard<std::mutex> lock(fStandaloneSwapMutex);
            fStandaloneSwap = path;
        }
        wakeRenderThread();
        return true;
    }
    // The same probe scene.prepare runs, on the caller's thread and under
    // no lock: a file that is not decodable audio at that path is a
    // REFUSED request that changes nothing — the old track, if any, keeps
    // playing, and the operator hears the reason now rather than after an
    // accepted bring-up. (A file that vanishes between here and the swap
    // is the bring-up's bad news, named through the block's own error.)
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
    {
        std::lock_guard<std::mutex> lock(fStandaloneSwapMutex);
        fStandaloneSwap = path;
    }
    wakeRenderThread();
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
        fMedia.sources.push_back({"video", fAudio.volume(AvPlayer::kVideoInput),
                                         fAudio.muted(AvPlayer::kVideoInput)});
    }
    if ((sourceMask & 2) != 0) {
        // The echo comes from the TRACK's player: identity kSecondInput,
        // which AvPlayer's own table resolves — the standalone-only
        // transport hands that identity whichever mixer index it got (6.7b).
        fMedia.sources.push_back({"standalone",
                                         fStandalonePlayer.volume(AvPlayer::kSecondInput),
                                         fStandalonePlayer.muted(AvPlayer::kSecondInput)});
    }
}

void BackgroundTransport::swapStandalonePendingLocked() {
    std::optional<std::string> swap;
    {
        std::lock_guard<std::mutex> lock(fStandaloneSwapMutex);
        swap.swap(fStandaloneSwap);
    }
    if (!swap) {
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
    fStandalonePlayer.stop();  // idempotent: a never-started player tears down clean
    fStandaloneLive.store(false, std::memory_order_release);
    fMediaSourceMask.store(fMediaSourceMask.load(std::memory_order_relaxed) & ~2,
                           std::memory_order_release);
    rebuildMediaSourcesLocked();
    fStandalonePath = *swap;
    if (swap->empty()) {
        // A removal: the block goes idle — except the error, which stays
        // sticky beside the path it describes (the operator still needs
        // to read what broke).
        fMedia.standalone.live = false;
        fMedia.standalone.playing = false;
        fMedia.standalone.position = -1.0;
        fMedia.standalone.duration = -1.0;
        fMedia.standalone.audio = "none";
        fMedia.standalone.loops = 0;
        std::fprintf(stderr, "openshow-worker: standalone track removed\n");
        return;
    }
    AvPlayer::Config config;
    config.secondAudioPath = *swap;
    // The production bed loops, the background transport's own default
    // (FreeShow's rule): the removal is the stop, and a walk-in track
    // that falls silent at its EOF mid-service is its own surprise.
    config.loop = true;
    std::string startError;
    if (!fStandalonePlayer.start(config, &startError)) {
        fMedia.standalone.live = false;
        fMedia.standalone.path = *swap;
        standaloneFailedLocked("standalone track: " +
                               (startError.empty() ? "the audio transport refused it"
                                                   : startError));
        std::fprintf(stderr, "openshow-worker: standalone track %s: %s\n", swap->c_str(),
                     startError.c_str());
        return;
    }
    fStandaloneLive.store(true, std::memory_order_release);
    fMediaSourceMask.store(fMediaSourceMask.load(std::memory_order_relaxed) | 2,
                           std::memory_order_release);
    // A successful bring-up replaces any earlier failure (mediaPlaying's
    // rule), and the block's path names what the block's error describes.
    fMedia.standalone.error.clear();
    fMedia.standalone.live = true;
    fMedia.standalone.path = *swap;
    fMedia.standalone.playing = false;  // the refresh promotes it, lane first
    fMedia.standalone.position = -1.0;
    // The lane seeded from the player that just came up: `live` and the
    // lane are one statement about the same transport, so the swap that
    // sets the first owes the second an honest value — leaving the last
    // block's "none" in place would let status say "live" beside "none"
    // until the next refresh (a real window: the selection's own wait
    // returns on live, and reads the lane right there).
    fMedia.standalone.audio = AvPlayer::laneName(fStandalonePlayer.audioLaneState());
    // A fresh transport has not wrapped; the count is THIS file's story.
    fMedia.standalone.loops = 0;
    double duration = -1.0;
    openshow::media::MediaInfo info;
    std::string probeError;
    if (openshow::media::probeMedia(*swap, &info, &probeError) ==
            openshow::media::ProbeStatus::kOk &&
        info.durationSeconds > 0.0) {
        duration = info.durationSeconds;
    }
    fMedia.standalone.duration = duration;
    rebuildMediaSourcesLocked();
    std::fprintf(stderr, "openshow-worker: standalone track %s (%s clock)\n",
                 swap->c_str(), fStandalonePlayer.audioLive() ? "audio" : "monotonic");
}

void BackgroundTransport::refreshStandaloneMediaLocked() {
    MediaStatus::Standalone& standalone = fMedia.standalone;
    if (!fStandaloneLive.load(std::memory_order_relaxed)) {
        return;  // while no player is up, the swap owns the block's fields
    }
    const AvPlayer::AudioLane lane = fStandalonePlayer.audioLaneState();
    standalone.audio = AvPlayer::laneName(lane);
    standalone.position = fStandalonePlayer.clockSeconds();
    // The track's own feeder answers the one question the lane cannot: with
    // the loop on, a starved feed is SILENCE the sink paces as usual. The
    // rewind counter is the difference between "still walking" and "ran
    // out", and the fixture watches it rather than trusting the lane.
    standalone.loops = fStandalonePlayer.feederStats(true).loops;
    // "Playing" means the transport is advancing: held freezes it, and a
    // transport that opened NO source has nothing to advance (its position
    // is a clock, not music). A no-output machine still runs — the same
    // monotonic-clock transport the video uses when the server is absent —
    // so it plays while its lane names why nothing is heard.
    standalone.playing = !fStandalonePlayer.paused() && lane != AvPlayer::AudioLane::kDegraded;
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
    // rule). A seek also blocks until the audio answers — normally a few
    // milliseconds, bounded at 500 — because the clock must not be re-anchored
    // on a jump the media did not make; 6.9b moves that wait off this loop.
    const int op = fPendingTransportOp.exchange(kOpNone, std::memory_order_acq_rel);
    bool applied = false;
    if (op != kOpNone) {
        applyTransportOp(op, fPendingSeekSeconds.load(std::memory_order_relaxed));
        applied = true;
    } else {
        // The test schedule rides the same seam, so the scenarios drive the
        // transport exactly as 6.15a's RPC will. Ops fire in schedule order;
        // everything whose time has passed goes out on this pass (the last
        // seek wins, exactly as a burst of operator clicks would), and the
        // cursor only advances past what was applied.
        const double elapsedMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                      fScheduledStart)
                .count();
        while (fScheduledCursor < fScheduledOps.size() &&
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
    // The standalone selection first (task 6.15b): a pass that both swaps
    // the track and carries a gain for it must land the gain on the NEW
    // player's mixer, not the one being stopped. The status fields it
    // touches are the same ones the refresh below samples.
    {
        std::lock_guard<std::mutex> lock(fMutex);
        swapStandalonePendingLocked();
    }
    const int loop = fPendingLoop.exchange(-1, std::memory_order_acq_rel);
    if (loop >= 0 && fBackground != nullptr) {
        fAudio.setLoop(loop == 1);
        std::fprintf(stderr, "openshow-worker: transport loop %s\n",
                     loop == 1 ? "on" : "off");
        applied = true;
    }
    // The gain and mute slots carry a SOURCE IDENTITY, which in production
    // selects the player one-to-one: the background transport never
    // registers a second mixer input, and the track's player has no video
    // identity — kSecondInput is the track, kVideoInput is the background,
    // and each player's own table resolves the index. The per-player guard
    // keeps an intent that raced a teardown from reaching a successor
    // (the exchange drops it: a dropped command, never a late one).
    const int gainSource = fPendingGainSource.exchange(-1, std::memory_order_acq_rel);
    if (gainSource >= 0) {
        const bool forTrack = gainSource == AvPlayer::kSecondInput;
        AvPlayer& player = forTrack ? fStandalonePlayer : fAudio;
        if (forTrack ? fStandaloneLive.load(std::memory_order_relaxed) : fBackground != nullptr) {
            player.setVolume(gainSource,
                             static_cast<float>(fPendingGain.load(std::memory_order_relaxed)));
            applied = true;
        }
    }
    const int muteSource = fPendingMuteSource.exchange(-1, std::memory_order_acq_rel);
    if (muteSource >= 0) {
        const bool forTrack = muteSource == AvPlayer::kSecondInput;
        AvPlayer& player = forTrack ? fStandalonePlayer : fAudio;
        if (forTrack ? fStandaloneLive.load(std::memory_order_relaxed) : fBackground != nullptr) {
            player.setMute(muteSource, fPendingMute.load(std::memory_order_relaxed) == 1);
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
}

void BackgroundTransport::applyTransportOp(int op, double seconds) {
    if (fBackground == nullptr) {
        std::fprintf(stderr, "openshow-worker: transport op ignored, no background is open\n");
        return;
    }
    if (op == kOpPause) {
        fAudio.pause();
        std::fprintf(stderr, "openshow-worker: transport held at %.3f s\n",
                     fBackground->position());
        return;
    }
    if (op == kOpPlay) {
        fAudio.play();
        std::fprintf(stderr, "openshow-worker: transport resumed\n");
        return;
    }
    std::string error;
    if (fAudio.seekTo(seconds, &error)) {
        std::fprintf(stderr, "openshow-worker: transport seek landed at %.3f s\n", seconds);
        // A seek that landed is the recovery for a previous refusal on this
        // asset (typed exactly as such): the parts agree again.
        mediaFaultCleared(MediaFault::kSync, fAppliedBackground.assetPath);
        return;
    }
    // The audio refused the jump. The picture moved (the video seek applies
    // on its own thread), so the audience now hears one position and sees
    // another — that is what the operator has to be told, and the reported
    // position stays whatever the transport actually presents.
    mediaFailed(MediaFault::kSync, fAppliedBackground.assetPath,
                "background seek to " + std::to_string(seconds) +
                    " s was refused: " + (error.empty() ? "no reason given" : error));
}

bool BackgroundTransport::applyBackground(
    const openshow::scene::Background& wanted, const GpuHandles& handles,
    const std::function<bool(const std::string&)>& imageResident) {
    // The identity rule (M3 continuity): same asset id, same path, same
    // kind — the transport stays exactly as it is. A lyric change over the
    // same background video therefore disturbs nothing: decode position,
    // the audio clock and the sink all keep rolling.
    const bool sameIdentity =
        fBackground != nullptr && fAppliedBackground.hasImage == wanted.hasImage &&
        fAppliedBackground.isVideo == wanted.isVideo &&
        fAppliedBackground.assetId == wanted.assetId &&
        fAppliedBackground.assetPath == wanted.assetPath;
    if (sameIdentity) {
        return false;
    }
    teardownBackgroundTransport();
    fAppliedBackground = wanted;
    if (!wanted.hasImage || !wanted.isVideo) {
        // Colour fill or still image: no video transport, so the media
        // status goes idle (a stale video error is not the story of the
        // scene now on air). A still is different: the picture it could not
        // decode IS the story, and the colour fill is a degradation rather
        // than a success (task 6.7c, review R18). So the asset the audience
        // is looking at is always NAMED, and the error is set only when
        // that asset is what cannot be shown.
        //
        // The cache is consulted BEFORE fMutex, so even the pure lookup of
        // 6.10 keeps this probe out of the lock that status and layer
        // commands wait on. Task 6.10 made it a lookup with no disk access:
        // the pin taken at prepare guarantees the preload's answer is
        // still resident, so a miss here genuinely means "no picture for
        // this asset", which is exactly what the report below says.
        const bool brokenStill =
            wanted.hasImage && !wanted.isVideo &&
            !imageResident(wanted.assetPath);
        std::lock_guard<std::mutex> lock(fMutex);
        fMedia.playing = false;
        fMedia.position = -1.0;
        fMedia.audio = "none";
        // The asset the audience is looking at, whether or not it works:
        // media.path always names what media.error describes, and a healthy
        // still is an asset on air like any other. (The identity short-cut
        // above means a re-present of the SAME still cannot rewrite this,
        // which is why the rule has to hold in both directions.)
        fMedia.path = wanted.hasImage ? wanted.assetPath : std::string();
        if (brokenStill) {
            // R18's rule read from the cache's side: the picture the
            // audience should be seeing is not here, and the colour fill
            // they get instead is a degradation, so it is NAMED. The
            // prepare's own preload already reports a file it could not
            // decode (with the better message — the codec's reason); this
            // half covers the picture that was there and is not any more,
            // which with the 6.10 pins should be a cache bug rather than
            // a show event, and must never pass as success. The apply
            // path runs on an identity change, not per frame, so this
            // cannot turn into notification noise.
            const bool alreadyReported = fMediaFault && fMediaFault->first == MediaFault::kImage &&
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
        return false;
    }

    std::string error;
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
    auto pipeline = std::make_unique<PlaybackPipeline>();
    if (!pipeline->openGpu(gpu, options, &error)) {
        std::fprintf(stderr, "openshow-worker: background video %s: %s\n",
                     wanted.assetPath.c_str(), error.c_str());
        mediaFailed(MediaFault::kVideo, wanted.assetPath, "background video: " + error);
        return false;  // the colour fill stands; the scene stays legible
    }

    // Embedded audio, when it can be decoded, becomes the audible mix and
    // the clock video paces against (the M5 checkpoint rule); a silent
    // file plays silent on the player's monotonic clock. Either way the
    // player is up: transport commands have one owner.
    openshow::media::MediaInfo info;
    AvPlayer::Config config;
    config.loop = true;
    bool wantsAudio = false;
    // What the audience is missing, when the clip's audio cannot be heard
    // (task 6.7c, review R18). AvPlayer cannot know this: a stream no
    // decoder can handle never reaches it as a source at all, so its own
    // lane would honestly report "none" — silence by nature — while the
    // file plainly carries sound. The session knows, and says so.
    std::string audioLoss;
    if (openshow::media::probeMedia(wanted.assetPath, &info, &error) ==
        openshow::media::ProbeStatus::kOk) {
        if (!info.audioStreams.empty()) {
            if (info.audioStreams.front().decoderAvailable) {
                config.videoAudioPath = wanted.assetPath;
                wantsAudio = true;
            } else {
                audioLoss = "background video has an audio stream no decoder can handle; "
                            "playing muted";
            }
        }
    } else {
        // The container opened a moment ago, so a second probe failing is
        // a curiosity rather than a story; it stays in the log. (Nothing
        // was expected yet, so no audio is claimed lost.)
        std::fprintf(stderr, "openshow-worker: background audio probe %s: %s\n",
                     wanted.assetPath.c_str(), error.c_str());
    }
    if (!fAudio.start(config, &error)) {
        std::fprintf(stderr, "openshow-worker: background audio: %s (video continues, "
                             "paced by the monotonic clock)\n",
                     error.c_str());
        if (wantsAudio) {  // only a real loss when audio was expected
            audioLoss = "background video audio failed to start: " + error + "; playing muted";
        }
    }
    fAudio.attachPipeline(pipeline.get());

    // The seam swaps before frames can flow: presenter and clock together,
    // so a submission is never paced by the previous background's clock.
    fBackground = std::move(pipeline);
    setVideoClock(fAudio.clockFn());
    fPresenter = fBackground.get();
    if (!fBackground->start(
            [this](const VideoSubmission& submission) { submitVideo(submission); },
            fAudio.clockFn(), &error)) {
        std::fprintf(stderr, "openshow-worker: background decode %s: %s\n",
                     wanted.assetPath.c_str(), error.c_str());
        mediaFailed(MediaFault::kVideo, wanted.assetPath, "background decode: " + error);
        // No quiesce boundary is needed here (6.6b): a false start() means
        // no pacing or decode thread was ever spawned, so nothing can read
        // the presenter or the clock after this point. The audio player
        // started for this transport goes down with it (idempotent).
        setVideoClock(nullptr);
        fPresenter = nullptr;
        fAudio.detachPipeline(fBackground.get());
        fAudio.stop();
        fBackground->close();
        fBackground.reset();
        return false;
    }
    // The video transport is up. The audio's bad news (if any) is recorded
    // on top of the success, in that order: mediaPlaying clears the previous
    // asset's reports, and this asset's known audio loss then rides them.
    // The loss also drives the lane (a stream no decoder can handle never
    // reaches AvPlayer as a source, so its own lane would truthfully say
    // "none"), and it belongs to this identity: a new background recomputes
    // it from scratch.
    // The identity table mirrors what THIS transport handed AvPlayer:
    // embedded audio registered its mixer slot when the probe found the
    // stream (the slot exists even if the feeder's own open later failed
    // — the gain belongs to the slot). Bit1 belongs to the standalone
    // track's lifetime (task 6.15b) and is preserved: the background is
    // not this track's owner and never rebuilds its registration. Stored
    // BEFORE mediaPlaying so the first status it publishes already lists
    // the sources (task 6.15a).
    fMediaSourceMask.store((fMediaSourceMask.load(std::memory_order_relaxed) & ~1) |
                              (config.videoAudioPath.empty() ? 0 : 1),
                          std::memory_order_release);
    fMediaTransportLive.store(true, std::memory_order_release);
    mediaPlaying(wanted);
    if (!audioLoss.empty()) {
        {
            std::lock_guard<std::mutex> lock(fMutex);
            fExpectedAudioLoss = audioLoss;
        }
        mediaFailed(MediaFault::kAudio, wanted.assetPath, audioLoss);
    }
    // The M5 clock rule, visible at transport start (checkpoint rule:
    // "use the audio clock when audio is present and a monotonic clock
    // otherwise"): one line naming which clock now paces the video.
    std::fprintf(stderr, "openshow-worker: background transport %s (%s clock)\n",
                 wanted.assetPath.c_str(),
                 fAudio.audioLive() ? "audio" : "monotonic");
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
    return true;  // the head frame arrives soon; the loop owes a present
}

void BackgroundTransport::teardownBackgroundTransport() {
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
    fMediaSourceMask.store(fMediaSourceMask.load(std::memory_order_relaxed) & ~1,
                           std::memory_order_release);
    fPendingLoop.store(-1, std::memory_order_release);
    if (fPendingGainSource.load(std::memory_order_relaxed) == AvPlayer::kVideoInput) {
        fPendingGainSource.store(-1, std::memory_order_release);
    }
    if (fPendingMuteSource.load(std::memory_order_relaxed) == AvPlayer::kVideoInput) {
        fPendingMuteSource.store(-1, std::memory_order_release);
    }
    if (!fBackground) {
        fAudio.stop();  // idempotent: a never-started player tears down clean
        fAppliedBackground = openshow::scene::Background{};
        return;
    }
    // Quiesce the producer FIRST (task 6.6b, review R6). submitVideo()
    // reads the presenter pointer outside fMutex, so the only moment it is
    // safe to change anything the sink touches — the presenter, the clock,
    // the pending seam — is behind the stop/join generation boundary:
    // requestStop wakes every wait the pacing thread can park in (clock
    // sleep, ring poll, uploader commit) and join closes the window for
    // good. After it the render thread owns the seam exclusively until the
    // next start(), which creates the new pacing thread after its presenter
    // is already published (thread creation orders the two).
    fBackground->requestStop();
    fBackground->join();
    // Presenter and clock down next, then the seam forgets the slots:
    // their GPU lifetime belongs to the pipeline being closed. Outstanding
    // leases die with the pipeline — the joined producer parked on one was
    // woken by requestStop, and close() releases every slot (task 6.6a).
    fPresenter = nullptr;
    setVideoClock(nullptr);
    fAudio.detachPipeline(fBackground.get());
    {
        std::lock_guard<std::mutex> lock(fMutex);
        fPendingVideo.reset();
        fDrawnVideo.reset();
    }
    fBackground->close();
    fBackground.reset();
    // The audio player outlived no clock reader: its clock function was
    // only ever handed to the (now joined) pipeline, so stopping it here
    // cannot race a pacing thread either.
    fAudio.stop();
    fAppliedBackground = openshow::scene::Background{};
}

double BackgroundTransport::videoClockSecondsLocked(
    std::chrono::steady_clock::time_point now) const {
    if (fVideoClock) {
        return fVideoClock();
    }
    return std::chrono::duration<double>(now - fRunStart).count();
}

}  // namespace openshow::worker
