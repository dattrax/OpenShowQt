/*
 * AvPlayer (task 5.4): the transport orchestrator for one A/V setup —
 * PipeWireSink + AudioMixer + the AudioFileFeeders whose combined audio
 * IS the output, plus the PlaybackPipelines (video) it drives in step.
 *
 * The sync invariant it exists to hold: what you HEAR, what the CLOCK
 * reports, and what video treats as its present deadline are the same
 * media position. Three rules keep that true:
 *
 *  - Feeders pre-fill their bounded input rings BEFORE the mixer starts
 *    (so the mix never emits head blocks padded with one input's
 *    starvation silence), and the sink stream only resumes after that
 *    pre-roll — ring position 0 is then real mixed audio at media ~0,
 *    and the clock anchors at the same instant (measured in 5.3: mixing
 *    into an empty-input ring pre-filled the sink with ~2 s of silence
 *    the audience heard before media 0).
 *  - Queue depth is latency, never skew: the clock is the server's
 *    consumption position and the rings map ring-index to media
 *    position continuously, so pause/resume must NOT flush (a paused
 *    pile-up resumes as seamless, correctly positioned audio; flushing
 *    would punch a hole of exactly the unheard queue instead).
 *  - A seek is the only thing that re-anchors the mapping: feeders and
 *    pipelines fan out, each feeder's seeked callback (the instant it
 *    is quiet) drops its input's queue, and after every jump applied
 *    the sink queue is flushed and the clock rebases to the target.
 *    Flush-boundary fuzz is bounded by one mix block plus one server
 *    slice (~30 ms) and disclosed, not hidden.
 *
 * A/V offset budget after those rules: startup pre-roll head (~1 block),
 * underrun padding, seek-boundary fuzz, and the sink's pause/resume
 * activation lag (<= one slice) — every component is a counter or a
 * bound, none is assumed.
 *
 * Failure honesty (PLAN.md): with no PipeWire daemon audioLive() is
 * false, clockFn() falls back to a monotonic clock and the player is
 * fully usable — video still runs, just paced by the wall.
 *
 * Thread discipline: start()/stop() and the transport methods run on one
 * transport thread; attachPipeline() before start(); the feeder seeked
 * callback runs on a decode thread and touches only atomics.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "media/AudioFeeder.h"
#include "media/AudioMixer.h"
#include "media/PipeWireSink.h"

namespace openshow::worker {

class PlaybackPipeline;

class AvPlayer {
public:
    static constexpr int kVideoInput = 0;
    static constexpr int kSecondInput = 1;

    struct Config {
        // The file whose embedded audio drives the output ("" = none).
        std::string videoAudioPath;
        // An independent standalone track mixed alongside ("" = none).
        std::string secondAudioPath;
        bool loop = false;      // feeder EOF rewind (live via setLoop)
        uint32_t preferredRate = 48000;
        // Per-input pre-fill before the mixer starts: enough that the
        // first mixed block has real content from every feeder that
        // opened (default 100 ms). A slower starter delays the start by
        // its lag, honestly.
        size_t preRollFrames = 4800;
        // Unheard sink-queue capacity (frames). A half-second default:
        // deep enough that mixer scheduling never shows as an underrun,
        // shallow enough that transport actions bite quickly.
        size_t sinkCapacityFrames = 24000;
        // Connect the sink at all; false keeps the sink out and the
        // clock monotonic (tests drive mixer and feeders through the
        // same orchestrator without a daemon).
        bool enableSink = true;
    };

    AvPlayer();
    ~AvPlayer();

    AvPlayer(const AvPlayer&) = delete;
    AvPlayer& operator=(const AvPlayer&) = delete;

    // sink (inactive) -> feeder seeked callbacks -> feeders -> pre-roll
    // (every feeder pre-filled, <= 2 s) -> mixer -> sink resume (the
    // clock's anchor). Returns false only when the player is not usable
    // at all (mixer refused); no daemon is NOT a failure: audioLive()
    // says no and the clock is monotonic. Unfixable inputs (no decoder,
    // bad file) log to stderr and leave silence — same rule as the bench.
    bool start(const Config& config, std::string* error);
    // feeders -> mixer -> sink. Idempotent. Attached pipelines belong to
    // callers and must be stopped before the AvPlayer dies.
    void stop();

    // Video transport followers; pointers must outlive the player.
    void attachPipeline(PlaybackPipeline* pipeline);

    // --- transport (single transport thread) ---
    void play();   // resume sink (no flush — the queue IS the continuation)
    void pause();  // freeze sink+clock; bounded rings park the feeders
    void seekTo(double targetSeconds);
    void setLoop(bool enabled);
    bool paused() const { return fPaused.load(); }
    bool audioLive() const { return fAudioLive.load(); }
    bool sinkActive() const { return fSinkActive.load(); }

    // The clock video paces against: the sink's audio clock while live,
    // one monotonic clock anchored at start() otherwise.
    std::function<double()> clockFn() const;

    // Mixer passthroughs (per-input volume/mute is the 5.3 contract).
    void setVolume(int input, float volume);
    void setMute(int input, bool muted);
    float volume(int input) const;
    bool muted(int input) const;
    // The live loop state (mirrored to feeders and pipelines by setLoop).
    bool loopEnabled() const { return fLoop.load(); }

    // Stats passthroughs for reporting/tests.
    media::PipeWireSink::Stats sinkStats() const;
    media::AudioMixer::Stats mixerStats() const;
    media::AudioFileFeeder::Stats feederStats(bool second) const;
    // The sink's media position (0 when no sink) — the heard position.
    double clockSeconds() const;

private:
    struct FeederSlot {
        std::unique_ptr<media::AudioFileFeeder> feeder;
        int input = media::AudioMixer::kNoInput;
    };

    media::PipeWireSink fSink;
    media::AudioMixer fMixer;
    std::vector<FeederSlot> fFeeders;  // [0] = video audio, [1] = second
    std::vector<PlaybackPipeline*> fPipelines;

    std::atomic<bool> fAudioLive{false};
    std::atomic<bool> fSinkActive{false};
    std::atomic<bool> fPaused{false};
    std::atomic<bool> fStarted{false};
    std::atomic<bool> fLoop{false};
    std::chrono::steady_clock::time_point fMonotonicStart;
};

}  // namespace openshow::worker
