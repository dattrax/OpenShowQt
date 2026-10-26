#include "playback/AvPlayer.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "playback/PlaybackPipeline.h"

namespace openshow::worker {

namespace {
// The pre-roll ceiling: a feeder that cannot pre-fill this much within
// the window (a pathological slow first packet) is tolerated — starting
// without it just re-exposes the head-starvation the pre-roll exists to
// prevent, and the stats show it.
constexpr int kPreRollMs = 2000;

int64_t steadyNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
}  // namespace

AvPlayer::AvPlayer() {
    // The fallback clock answers from the moment the player exists (task
    // 6.7b): av-bench --no-audio captures clockFn() without ever calling
    // start(), and an un-anchored position read as time since the
    // steady_clock epoch — the scheduler drifted by the machine's uptime.
    // start() re-anchors to 0 for a real transport.
    fFallbackAnchorNs.store(steadyNowNs(), std::memory_order_release);
}

AvPlayer::~AvPlayer() {
    stop();
}

bool AvPlayer::start(const Config& config, std::string* error) {
    if (fStarted.load()) {
        if (error) {
            *error = "AvPlayer already started";
        }
        return false;
    }
    fStarted.store(true);
    fLoop.store(config.loop);
    // The fallback clock starts at 0 from this instant (task 6.7b); it
    // only answers while audio is not live.
    fFallbackBaseNs.store(0, std::memory_order_relaxed);
    fFallbackAnchorNs.store(steadyNowNs(), std::memory_order_release);
    fFallbackHeld.store(false, std::memory_order_relaxed);

    const bool wantVideo = !config.videoAudioPath.empty();
    const bool wantSecond = !config.secondAudioPath.empty();

    // The sink first (inactive): the mixer needs its negotiated rate, and
    // a missing daemon must degrade to the monotonic clock, not fail.
    bool sinkUp = false;
    if (config.enableSink) {
        std::string sinkError;
        if (fSink.start(config.preferredRate, &sinkError, config.sinkCapacityFrames)) {
            sinkUp = true;
        } else {
            std::fprintf(stderr, "openshow-worker: AvPlayer: no audio output (%s), "
                                 "pacing on the monotonic clock\n",
                         sinkError.c_str());
        }
    }
    const uint32_t rate =
        sinkUp && fSink.negotiatedRate() != 0 ? fSink.negotiatedRate() : config.preferredRate;

    // Inputs and feeders run BEFORE the mixer: the pre-roll exists so the
    // first mixed block has real content from every feeder (a mixer that
    // starts against empty inputs emits starvation-padded head blocks —
    // the measured 2 s startup offset of task 5.3's shape).
    // Identity-first registration (task 6.7b, review R2): each wanted
    // source takes the mixer index it is handed, remembered by identity.
    // The old code demanded index 1 from a mixer that had never seen
    // input 0 — a standalone-only transport failed that identity check,
    // registered nothing it could use and silently started no feeder.
    bool inputsOk = true;
    for (int identity = 0; identity < 2; ++identity) {
        const bool wanted = (identity == kVideoInput && wantVideo) ||
                            (identity == kSecondInput && wantSecond);
        if (!wanted) {
            continue;
        }
        const int index = fMixer.addInput();
        if (index == media::AudioMixer::kNoInput) {
            inputsOk = false;
            break;
        }
        fInputOf[identity].store(index, std::memory_order_relaxed);
    }
    const char* const labels[] = {"video", "second"};
    const std::string paths[] = {config.videoAudioPath, config.secondAudioPath};
    media::AudioMixer* mixer = &fMixer;
    for (int identity = 0; inputsOk && identity < 2; ++identity) {
        if (paths[identity].empty()) {
            continue;
        }
        auto feeder = std::make_unique<media::AudioFileFeeder>();
        std::string feederError;
        if (!feeder->open(paths[identity], rate, &feederError)) {
            std::fprintf(stderr, "openshow-worker: AvPlayer: %s audio unavailable (%s)\n",
                         labels[identity], feederError.c_str());
            continue;
        }
        const int input = fInputOf[identity].load(std::memory_order_relaxed);
        // The race-free flush seam: the decode thread is quiet at the
        // callback, so this input's queue drops exactly at the jump.
        feeder->setSeekedCallback([this, mixer, input](double) {
            mixer->requestInputFlush(input);
            if (fSinkActive.load(std::memory_order_relaxed)) {
                fSink.requestFlush();
            }
        });
        // Input id by value (5.3 lesson): the decode thread outlives
        // this scope.
        if (!feeder->start(
                [mixer, input](const float* frames, size_t count) {
                    return mixer->write(input, frames, count);
                },
                config.loop, &feederError)) {
            std::fprintf(stderr, "openshow-worker: AvPlayer: %s audio feeder: %s\n",
                         labels[identity], feederError.c_str());
            continue;
        }
        fFeeders.push_back({std::move(feeder), input});
    }

    // Pre-roll: every feeder that took its slot pre-fills, so the mix
    // head is real audio from all of them.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kPreRollMs);
    while (std::chrono::steady_clock::now() < deadline) {
        bool ready = true;
        for (const FeederSlot& slot : fFeeders) {
            if (fMixer.stats().input[slot.input].framesWritten < config.preRollFrames) {
                ready = false;
                break;
            }
        }
        if (ready) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    // Mixer, then the server wakes: the clock anchors at resume, and the
    // queue below it (the pre-roll plus whatever the mix delivered in the
    // meantime) is latency, not skew — ring position 0 is real audio at
    // media ~0 because the pre-roll guaranteed it.
    if (!fFeeders.empty()) {
        std::string mixerError;
        // Test seam (task 6.7a): the env-gated fault knob makes the mixer
        // start itself fail — the shape no external configuration can
        // reach — so fixtures pin the failure path's self-clean without
        // a daemon. Same discipline as the 6.4 output fault knobs.
        const bool faultMixer = std::getenv("OPENSHOW_FAULT_AUDIO_MIXER_FAIL") != nullptr;
        const bool mixerUp =
            !faultMixer &&
            fMixer.start(
                [this, sinkUp](const float* frames, size_t count) {
                    return sinkUp ? fSink.feed(frames, count) : count;
                },
                rate, &mixerError);
        if (faultMixer) {
            mixerError = "injected mixer failure (OPENSHOW_FAULT_AUDIO_MIXER_FAIL)";
        }
        if (!mixerUp) {
            if (error) {
                *error = mixerError;
            }
            for (FeederSlot& slot : fFeeders) {
                slot.feeder->requestStop();
                slot.feeder->join();
            }
            fFeeders.clear();
            // Self-clean (task 6.7a): a failed start must leave the whole
            // transport re-armable, not just the player's own flag — the
            // sink goes down, the mixer returns to its pre-start table
            // (the inputs this attempt registered would otherwise shift
            // the retry's addInput() off its expected index and silently
            // degrade it), and fStarted re-arms, so a retry (or the next
            // background) faces no "already running" residue.
            fSink.stop();
            fMixer.reset();
            fInputOf[kVideoInput].store(media::AudioMixer::kNoInput,
                                        std::memory_order_relaxed);
            fInputOf[kSecondInput].store(media::AudioMixer::kNoInput,
                                         std::memory_order_relaxed);
            fAudioLive.store(false);
            fSinkActive.store(false);
            fStarted.store(false);
            return false;
        }
        fAudioLive.store(sinkUp);
        if (sinkUp) {
            fSink.resume();
            fSinkActive.store(fSink.active());
        }
    }
    return true;
}

void AvPlayer::stop() {
    for (FeederSlot& slot : fFeeders) {
        slot.feeder->requestStop();
    }
    for (FeederSlot& slot : fFeeders) {
        slot.feeder->join();
    }
    fFeeders.clear();
    fMixer.requestStop();
    fMixer.join();
    // Reset, not just re-flag (task 6.7a, review R2): the runtime probe
    // showed the next transport's addInput() failing with "AudioMixer is
    // already running" — join() reaps the thread but leaves fStarted, the
    // input table and the queues behind. The whole mixer returns to its
    // pre-start state so the next start registers fresh inputs over fresh
    // rings, and its stats describe its own run.
    fMixer.reset();
    fSink.stop();
    fSinkActive.store(false);
    fAudioLive.store(false);
    // The identity map follows the (reset) mixer table: nothing may
    // address a next transport's inputs through this run's indices.
    fInputOf[kVideoInput].store(media::AudioMixer::kNoInput, std::memory_order_relaxed);
    fInputOf[kSecondInput].store(media::AudioMixer::kNoInput, std::memory_order_relaxed);
    // Re-armable: the session's background transport stops and restarts
    // the player as backgrounds change (every component's stop is
    // idempotent, so a never-started player tears down cleanly too).
    fStarted.store(false);
}

void AvPlayer::attachPipeline(PlaybackPipeline* pipeline) {
    if (pipeline == nullptr) {
        return;
    }
    // Deduplicated: a transport that swaps files re-attaches after
    // detaching, and a double entry would double-fan transport commands.
    for (PlaybackPipeline* existing : fPipelines) {
        if (existing == pipeline) {
            return;
        }
    }
    fPipelines.push_back(pipeline);
}

void AvPlayer::detachPipeline(PlaybackPipeline* pipeline) {
    for (size_t i = 0; i < fPipelines.size(); ++i) {
        if (fPipelines[i] == pipeline) {
            fPipelines.erase(fPipelines.begin() + static_cast<ptrdiff_t>(i));
            return;
        }
    }
}

void AvPlayer::play() {
    // No flush: the queue holds the exact continuation of the media (the
    // rings map position continuously and nothing was dropped at pause),
    // and the clock continues from its hold — flushing here would punch
    // a hole of exactly the unheard queue.
    fPaused.store(false);
    if (fAudioLive.load()) {
        fSink.resume();
        fSinkActive.store(fSink.active());  // reflect the server's verdict
    } else if (fFallbackHeld.load(std::memory_order_relaxed)) {
        // Sinkless: the fallback position continues from its hold —
        // re-anchor first, then release, so no reader sees the base with
        // the pause-era anchor (task 6.7b).
        fFallbackAnchorNs.store(steadyNowNs(), std::memory_order_relaxed);
        fFallbackHeld.store(false, std::memory_order_release);
    }
    for (PlaybackPipeline* pipeline : fPipelines) {
        pipeline->play();
    }
}

void AvPlayer::pause() {
    fPaused.store(true);
    // Clock before stream: the hold must latch the position the server
    // has actually reached (PipeWireSink::pause orders this). Feeders
    // keep running until the bounded rings park them — the pile-up is
    // the seamless continuation, memory is the bound (the 5.1 contract).
    if (fAudioLive.load()) {
        fSink.pause();
        fSinkActive.store(fSink.active());
    } else if (!fFallbackHeld.load(std::memory_order_relaxed)) {
        // Sinkless: latch the current fallback position as the hold
        // (task 6.7b): base becomes the held position, then readers take
        // the hold branch.
        const int64_t base = fFallbackBaseNs.load(std::memory_order_relaxed);
        const int64_t anchor = fFallbackAnchorNs.load(std::memory_order_relaxed);
        fFallbackBaseNs.store(base + (steadyNowNs() - anchor), std::memory_order_relaxed);
        fFallbackHeld.store(true, std::memory_order_release);
    }
    for (PlaybackPipeline* pipeline : fPipelines) {
        pipeline->pause();
    }
}

void AvPlayer::seekTo(double targetSeconds) {
    // Fan out first (all seeks are asynchronous), then move the clock
    // once the audio actually jumped — video's deadline must never lead
    // the heard position to the target. Each feeder's seeked callback
    // dropped its input's queue at the instant it went quiet.
    for (FeederSlot& slot : fFeeders) {
        slot.feeder->requestSeek(targetSeconds);
    }
    for (PlaybackPipeline* pipeline : fPipelines) {
        pipeline->seekTo(targetSeconds);
    }
    bool applied = true;
    for (FeederSlot& slot : fFeeders) {
        applied = slot.feeder->waitSeekApplied(500) && applied;
    }
    if (!fFeeders.empty() && applied) {
        // The sink queue goes last: everything the ring still holds is
        // pre-jump content (the inputs were flushed at their jumps). The
        // drop is answered by the next process callback — while paused
        // there is none, and it lands on resume instead, which is exactly
        // right: nothing is heard while parked, and the first sound after
        // play() is at the target. The clock rebase lands the (held or
        // live) position on the target either way.
        fSink.requestFlush();
        if (fAudioLive.load()) {
            fSink.rebaseClock(targetSeconds);
        }
    }
    if (!fAudioLive.load() && (fFeeders.empty() || applied)) {
        // The sinkless fallback position obeys the seek too (task 6.7b):
        // playing, it continues from the target; parked, the hold now
        // reports the target. A failed feeder seek (timeout) leaves the
        // position where it was — the clock never claims a jump the
        // audio did not make.
        const int64_t targetNs = static_cast<int64_t>(targetSeconds * 1e9);
        fFallbackBaseNs.store(targetNs, std::memory_order_relaxed);
        fFallbackAnchorNs.store(steadyNowNs(), std::memory_order_relaxed);
    }
}

void AvPlayer::setLoop(bool enabled) {
    fLoop.store(enabled);
    for (FeederSlot& slot : fFeeders) {
        slot.feeder->setLoopEnabled(enabled);
    }
    for (PlaybackPipeline* pipeline : fPipelines) {
        pipeline->setLoop(enabled);
    }
}

std::function<double()> AvPlayer::clockFn() const {
    const media::PipeWireSink* sink = &fSink;
    const std::atomic<bool>* live = &fAudioLive;
    const std::atomic<int64_t>* baseNs = &fFallbackBaseNs;
    const std::atomic<int64_t>* anchorNs = &fFallbackAnchorNs;
    const std::atomic<bool>* held = &fFallbackHeld;
    return [sink, live, baseNs, anchorNs, held] {
        if (live->load(std::memory_order_acquire)) {
            return sink->clock().clockSeconds();
        }
        // Sinkless fallback (task 6.7b): the transport-controlled
        // position — frozen at the hold while paused, base + elapsed
        // otherwise. start() anchors it at 0; pause/play/seek keep it
        // honest, unlike the raw elapsed time R2 flagged.
        if (held->load(std::memory_order_acquire)) {
            return static_cast<double>(baseNs->load(std::memory_order_relaxed)) / 1e9;
        }
        const int64_t since = steadyNowNs() - anchorNs->load(std::memory_order_acquire);
        return static_cast<double>(baseNs->load(std::memory_order_relaxed) + since) / 1e9;
    };
}

int AvPlayer::mixerIndexFor(int identity) const {
    if (identity == kVideoInput || identity == kSecondInput) {
        return fInputOf[identity].load(std::memory_order_relaxed);
    }
    return identity;  // not an identity: the mixer's bounds check no-ops it
}

void AvPlayer::setVolume(int identity, float volume) {
    fMixer.setVolume(mixerIndexFor(identity), volume);
}

void AvPlayer::setMute(int identity, bool muted) {
    fMixer.setMute(mixerIndexFor(identity), muted);
}

float AvPlayer::volume(int identity) const {
    return fMixer.volume(mixerIndexFor(identity));
}

bool AvPlayer::muted(int identity) const {
    return fMixer.muted(mixerIndexFor(identity));
}

media::PipeWireSink::Stats AvPlayer::sinkStats() const {
    return fSink.stats();
}

media::AudioMixer::Stats AvPlayer::mixerStats() const {
    return fMixer.stats();
}

media::AudioFileFeeder::Stats AvPlayer::feederStats(bool second) const {
    // Look up by the mixer index registered for the source IDENTITY (a
    // feeder that failed to open never took its slot, and the survivor
    // must still report under its own name; identity keeps that true no
    // matter which other source exists — task 6.7b).
    const int wanted =
        fInputOf[second ? kSecondInput : kVideoInput].load(std::memory_order_relaxed);
    for (const FeederSlot& slot : fFeeders) {
        if (slot.input == wanted) {
            return slot.feeder->stats();
        }
    }
    return media::AudioFileFeeder::Stats{};
}

double AvPlayer::clockSeconds() const {
    return clockFn()();
}

}  // namespace openshow::worker
