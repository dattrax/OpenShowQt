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
 * transport thread, attach/detach with it; the feeder seeked callback
 * runs on a decode thread and touches only atomics.
 */

#pragma once

#include <array>
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
    // Source IDENTITIES (task 6.7b, review R2): stable names for the two
    // sources, not mixer table positions. The player maps identity ->
    // mixer index when it registers inputs, so a standalone-only
    // transport (only kSecondInput wanted, the mixer hands it index 0)
    // addresses its feeder, flushes and volumes by the same names as a
    // transport that has both.
    static constexpr int kVideoInput = 0;
    static constexpr int kSecondInput = 1;

    // The AUDIBLE outcome of the transport as one typed lane (task 6.7c,
    // review R18). The question an operator asks is not "did start()
    // return" but "why is there no sound", and a boolean cannot answer it:
    // an expected source that failed to open, a machine with no audio
    // server, and an output deliberately switched off all look identical
    // from audioLive() alone while start() still reports success.
    enum class AudioLane {
        kNone,      // no transport, or a transport with no audio source
        kSilent,    // audio wanted, output switched off by configuration
        kNoOutput,  // audio wanted and an output wanted: no server connection
        kDegraded,  // audio and output armed, but nothing can be heard:
                    // every expected source failed to open, or the server
                    // refused the stream activation
        kPaused,    // the transport holds; the lane holds with it
        kLive,      // the mix is armed and the server consumes it
    };
    // The wire name of a lane (task 6.7c: status.media.audio).
    static const char* laneName(AudioLane lane);

    struct Config {
        // The file whose embedded audio drives the output ("" = none).
        std::string videoAudioPath;
        // An independent standalone track mixed alongside ("" = none).
        std::string secondAudioPath;
        bool loop = false;      // feeder EOF rewind (live via setLoop)
        uint32_t preferredRate = 48000;
        // Task 6.21c1 (review F3): STAGE the transport without rolling it.
        // Everything a normal start brings up comes up — sink connection
        // and rate negotiation, feeders open, mixer started, pre-roll paid
        // — but the sink is never resumed and the clock is anchored in its
        // hold, so nothing is audible and no position advances. This is
        // exactly the paused state a live transport reaches on an
        // operator's hold, entered at start: a later play() releases it
        // (sink resume, clock continue) and the first audible millisecond
        // belongs to whoever adopts the staged transport, not to the
        // thread that built it. Default false: today's start-rolls behaviour.
        bool startHeld = false;
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
    // Attaching is deduplicated, and a pipeline being destroyed must be
    // detached first (the background transport swaps pipelines as scene
    // backgrounds change: attach the replacement, detach the dying one
    // before closing it).
    void attachPipeline(PlaybackPipeline* pipeline);
    void detachPipeline(PlaybackPipeline* pipeline);

    // --- transport (single transport thread) ---
    void play();   // resume sink (no flush — the queue IS the continuation)
    void pause();  // freeze sink+clock; bounded rings park the feeders
    // Jumps every source and (only when the audio actually moved) the clock.
    // Returns whether the whole fan-out landed: false means at least one
    // source refused its jump, in which case NOTHING was re-anchored — no
    // sink flush, no clock rebase, no fallback position move — so the heard
    // position stays the truth while the caller reports what it could not
    // honour (task 6.7d, review R19). `error`, when given, carries the first
    // refusal reason. Video followers apply asynchronously and a video seek
    // that cannot be performed is fatal for that transport (it surfaces as
    // the pipeline's own error), so this answer is about the audio.
    // Since 6.22e4 this is the composition of the three halves below with
    // the original 500 ms per-feeder budget and no abort — for callers that
    // own their thread (the bench, the tests). The transport pass pays the
    // wait on a lane instead, through the halves.
    bool seekTo(double targetSeconds, std::string* error = nullptr);
    // The seek's three halves (task 6.22e4, review A5). beginSeekTo() fans
    // decoder requests out and gates video deadlines, without waiting.
    // Video retains its target until finishSeekTo completes the clock rebase.
    // waitSeekTo() is the whole cost: it blocks until each feeder settles its request, bounded per
    // feeder by timeoutMs, ended early when `abort` is set (the transport
    // retired around the waiter). finishSeekTo() moves the sink queue, the
    // clock and the fallback position, and belongs to the thread that owns
    // the transport pass — it may only run after the wait, and only ever
    // once per seek, with the wait's verdict. Nothing here is reentrant:
    // one seek in flight per player, as before.
    void beginSeekTo(double targetSeconds);
    bool waitSeekTo(int timeoutMs, const std::atomic<bool>* abort, std::string* error);
    void finishSeekTo(double targetSeconds, bool applied);
    void setLoop(bool enabled);
    bool paused() const { return fPaused.load(); }
    bool audioLive() const { return fAudioLive.load(); }
    bool sinkActive() const { return fSinkActive.load(); }

    // The audio lane for status (task 6.7c, review R18): derived only from
    // state this class already keeps honest (started, what was wanted, what
    // opened, sink readiness/activation, pause) so a paused transport reads
    // "paused" rather than a fault, a machine without an audio server reads
    // "no-output", and an expected source that never opened reads
    // "degraded" instead of a successful-looking silent mix.
    AudioLane audioLaneState() const;

    // The clock video paces against: the sink's audio clock while live,
    // one monotonic clock anchored at start() otherwise.
    std::function<double()> clockFn() const;

    // Mixer passthroughs (per-input volume/mute is the 5.3 contract).
    // The index names a SOURCE identity (kVideoInput/kSecondInput); an
    // identity the transport never registered is a no-op (setters) or
    // the mixer's invalid-slot answer (getters).
    void setVolume(int identity, float volume);
    void setMute(int identity, bool muted);
    float volume(int identity) const;
    bool muted(int identity) const;
    // The live loop state (mirrored to feeders and pipelines by setLoop).
    bool loopEnabled() const { return fLoop.load(); }

    // Stats passthroughs for reporting/tests.
    media::PipeWireSink::Stats sinkStats() const;
    media::AudioMixer::Stats mixerStats() const;
    media::AudioFileFeeder::Stats feederStats(bool second) const;
    // The heard media position: the sink's audio clock when audio is
    // live, else the fallback position the transport controls — start
    // anchors it at 0, pause holds it, play continues, seek moves it
    // (task 6.7b; raw elapsed time ignored all three).
    double clockSeconds() const;

private:
    struct FeederSlot {
        std::unique_ptr<media::AudioFileFeeder> feeder;
        // The MIXER INDEX this feeder writes (looked up from the source
        // identity in fInputOf; not necessarily 0/1).
        int input = media::AudioMixer::kNoInput;
    };

    media::PipeWireSink fSink;
    media::AudioMixer fMixer;
    std::vector<FeederSlot> fFeeders;  // only feeders that opened
    std::vector<PlaybackPipeline*> fPipelines;

    // Source identity (kVideoInput/kSecondInput) -> mixer index assigned
    // at registration, kNoInput while unregistered (task 6.7b). Written
    // by start()/stop(); control reads from any thread.
    std::array<std::atomic<int>, 2> fInputOf{
        {media::AudioMixer::kNoInput, media::AudioMixer::kNoInput}};

    std::atomic<bool> fAudioLive{false};
    std::atomic<bool> fSinkActive{false};
    // The fault matcher's copy of this transport's file names (task 6.22e3,
    // review A5): written by start() where the Config is, read by stop() —
    // possibly on another thread, but every hand-off of player ownership
    // (staging lane → render → retirement lane) passes through a mutex or
    // the stage's done flag, so a reader always sees the written string.
    std::string fFaultPaths;
    // The slow-stop seam pays exactly once (task 6.22e3): what it stands
    // in for — the joins — really do happen once. A second stop() of an
    // already-stopped player has nothing left to wait for, and the reap's
    // destructor path re-enters stop(), so the seam must not sleep twice.
    std::atomic_flag fStopFaultPaid = ATOMIC_FLAG_INIT;
    // What this transport EXPECTED (task 6.7c): an audio source at all,
    // and an output to play it through. start() writes them, stop() clears
    // the expectation, and a start that failed halfway leaves the
    // expectation set — that is exactly the state whose lane must read
    // "degraded" instead of the emptiness of "none".
    std::atomic<bool> fWantsAudio{false};
    std::atomic<bool> fSinkEnabled{true};
    std::atomic<bool> fSinkReady{false};
    // Sources this transport ACTUALLY opened (task 6.7c, review R18): an
    // expected file whose decoder never came up leaves this at zero, which
    // is the difference between a mix that is running and one that plays
    // nothing. Written by start(), cleared by stop().
    std::atomic<unsigned> fSourcesOpen{0};
    std::atomic<bool> fPaused{false};
    std::atomic<bool> fStarted{false};
    std::atomic<bool> fLoop{false};

    // The fallback (sinkless) clock: a transport-controlled media
    // position — base + (now - anchor) while playing, base while held
    // (paused). start() anchors 0/now, pause() latches the hold, play()
    // re-anchors, seekTo() moves the base (and the hold if parked).
    // Transport-thread writes, readers anywhere (AudioClock's rule).
    std::atomic<int64_t> fFallbackBaseNs{0};
    std::atomic<int64_t> fFallbackAnchorNs{0};
    std::atomic<bool> fFallbackHeld{false};

    int mixerIndexFor(int identity) const;
};

}  // namespace openshow::worker
