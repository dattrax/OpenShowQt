#include "playback/AvPlayer.h"

#include <chrono>
#include <cstdio>
#include <thread>

#include "playback/PlaybackPipeline.h"

namespace openshow::worker {

namespace {
// The pre-roll ceiling: a feeder that cannot pre-fill this much within
// the window (a pathological slow first packet) is tolerated — starting
// without it just re-exposes the head-starvation the pre-roll exists to
// prevent, and the stats show it.
constexpr int kPreRollMs = 2000;
}  // namespace

AvPlayer::AvPlayer() = default;

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
    fMonotonicStart = std::chrono::steady_clock::now();

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
    bool inputsOk = true;
    if (wantVideo) {
        inputsOk = fMixer.addInput() == kVideoInput;
    }
    if (wantSecond && inputsOk) {
        inputsOk = fMixer.addInput() == kSecondInput;
    }
    const int inputCount = inputsOk ? static_cast<int>(fMixer.inputCount()) : 0;
    const char* const labels[] = {"video", "second"};
    const std::string paths[] = {config.videoAudioPath, config.secondAudioPath};
    media::AudioMixer* mixer = &fMixer;
    for (int i = 0; i < 2 && i < inputCount; ++i) {
        if (paths[i].empty()) {
            continue;
        }
        auto feeder = std::make_unique<media::AudioFileFeeder>();
        std::string feederError;
        if (!feeder->open(paths[i], rate, &feederError)) {
            std::fprintf(stderr, "openshow-worker: AvPlayer: %s audio unavailable (%s)\n",
                         labels[i], feederError.c_str());
            continue;
        }
        const int input = i;
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
                         labels[i], feederError.c_str());
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
        if (!fMixer.start(
                [this, sinkUp](const float* frames, size_t count) {
                    return sinkUp ? fSink.feed(frames, count) : count;
                },
                rate, &mixerError)) {
            if (error) {
                *error = mixerError;
            }
            for (FeederSlot& slot : fFeeders) {
                slot.feeder->requestStop();
                slot.feeder->join();
            }
            fFeeders.clear();
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
    fSink.stop();
    fSinkActive.store(false);
    fAudioLive.store(false);
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
    const std::chrono::steady_clock::time_point monotonicStart = fMonotonicStart;
    const std::atomic<bool>* live = &fAudioLive;
    return [sink, monotonicStart, live] {
        if (live->load(std::memory_order_acquire)) {
            return sink->clock().clockSeconds();
        }
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - monotonicStart)
            .count();
    };
}

void AvPlayer::setVolume(int input, float volume) {
    fMixer.setVolume(input, volume);
}

void AvPlayer::setMute(int input, bool muted) {
    fMixer.setMute(input, muted);
}

float AvPlayer::volume(int input) const {
    return fMixer.volume(input);
}

bool AvPlayer::muted(int input) const {
    return fMixer.muted(input);
}

media::PipeWireSink::Stats AvPlayer::sinkStats() const {
    return fSink.stats();
}

media::AudioMixer::Stats AvPlayer::mixerStats() const {
    return fMixer.stats();
}

media::AudioFileFeeder::Stats AvPlayer::feederStats(bool second) const {
    // Look up by input id, not vector position: a feeder that failed to
    // open never took its slot, and the survivor must still report under
    // its own name.
    const int wanted = second ? kSecondInput : kVideoInput;
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
