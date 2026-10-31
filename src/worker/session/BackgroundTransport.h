/*
 * BackgroundTransport: the one owner of the audience-side media (task
 * 6.18b, review R6/R10/R15), extracted from OutputSession the way 6.18a/
 * 6.18a2 moved the scene truth and the operator's picture: what used to be
 * transport state smeared across the session — the background pipeline and
 * its audio feeder, the standalone track and its swap slot, the video seam
 * submissions and their leases, the newest-wins intent atomics, the test
 * schedule, the typed fault table and the whole media status mirror — is
 * now one object with one set of rules.
 *
 * The rules this class owns, in its own words:
 *
 *  - The render thread IS the transport thread. AvPlayer's fan-out walks
 *    the attached-pipeline list, and only the thread that swaps transports
 *    touches it: every command from another thread lands in a newest-wins
 *    intent slot here and is applied by the loop's next pass
 *    (applyTransportOps). This is why a refusal cannot answer the call it
 *    arrived on; it surfaces as the typed fault (R19).
 *  - Quiesce before mutate (R6). submitVideo() reads the presenter pointer
 *    outside the lock, so the presenter, the clock and the seam slots may
 *    change only behind a requestStop()+join() generation boundary: after
 *    the join the render thread owns the seam until the next start(), which
 *    publishes its presenter before spawning its pacing thread. The same
 *    boundary orders teardown, the failed-start self-clean, and nothing
 *    else.
 *  - Identity means untouched. A scene change whose background carries the
 *    same asset id, path and kind leaves the transport exactly as it is —
 *    position, clock and sink keep rolling through a lyric change (the M3
 *    continuity rule). Anything else tears down before it opens: the
 *    replacement never starts beside its predecessor.
 *  - The seam is bounded: at most one pending submission and one held
 *    (drawn) one. A submission replaced before ever being drawn goes back
 *    through onDropped with its sequence as proof of ownership; the held
 *    frame keeps its slot leased until onReplaced names it (6.6a's lease).
 *  - Status is the transport's own words. The media mirror (playing, lane,
 *    position, the sticky per-class fault, the standalone block) lives
 *    here; status() reads it. The frame refresh is the normal heartbeat,
 *    the half-second housekeep catches the world while frames stand still,
 *    and an applied op refreshes immediately (R19: a pause that still
 *    reads "playing" is a swallowed op).
 *
 * Threading: every method is safe from any thread through its own mutex,
 * with two documented exemptions. The presenter pointer follows the quiesce
 * rule above (writer: render thread at a generation boundary; reader:
 * submitVideo's thread, ordered by thread creation/join). And the swap's
 * join() never runs under this lock — teardown reaches the players with it
 * released, because a dying transport reports through these same methods.
 * The session nests this mutex inside its own (session -> transport, never
 * the reverse) and nothing here calls back into the session.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <vulkan/vulkan.h>

#include "include/core/SkImage.h"
#include "include/gpu/ganesh/GrDirectContext.h"

#include "openshow/scene/scene.h"

#include "playback/AvPlayer.h"
#include "playback/PlaybackPipeline.h"
#include "playback/VideoSeam.h"

namespace openshow::worker {

// Device handles a bench needs beside the session (task 1.5e): the YUV
// upload ring lives outside the session but shares its device, and the seam
// to build it is this narrow view rather than the objects. The context may
// only be touched from the render thread (no thread-safety guarantee at
// this Skia revision); the Vulkan handles are safe to use from an upload
// thread. Lives beside the transport since 6.18b because the transport's
// bring-up is what it feeds; OutputSession::gpuHandles() still reads the
// session's own device.
struct GpuHandles {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    uint32_t graphicsQueueIndex = 0;
    uint32_t uploadQueueIndex = 0;
    VkQueue uploadQueue = VK_NULL_HANDLE;
    GrDirectContext* context = nullptr;
};

// Background media visibility (task 5.5d). playing says the session has a
// live transport for the on-air background; error carries the last failure
// for that asset and STAYS set until the same background opens successfully
// — the frontend polls, and an operator must be able to see what broke
// after the fact, not only in the moment. Every fresh failure additionally
// rides the media.error notification (immediacy), so status and push
// together close the gap between polls. Owned by BackgroundTransport since
// task 6.18b; the session's Status carries a copy of this reading.
struct MediaStatus {
    bool playing = false;
    std::string path;
    std::string error;
    // The audio lane (task 6.7c, review R18): "none", "silent",
    // "no-output", "degraded", "paused" or "live" — why the audience
    // hears what it hears. Refreshed with the transport's other state
    // each frame, so a sink that activates (or dies) after the present
    // is visible without another present.
    std::string audio = "none";
    // Loop-continuous media position in seconds, refreshed by the
    // render thread each frame while a video background plays; -1
    // otherwise. It is the proof of the continuity rule: a lyric
    // change over the same background shows position climbing
    // THROUGH the change — a transport that reopened would restart
    // near zero.
    double position = -1.0;
    // Media transport (task 6.15a, review R15): what the transport is
    // supposed to do and what it holds, so an operator command is
    // confirmed by status the way scenes and layers already are.
    // loop is the live rewind intent (true until EOF decides);
    // duration is the probe's clip length (-1 when no transport or
    // the container does not say); sources lists the REGISTERED
    // audio identities with their mixer echo — a source this
    // transport never had is absent rather than falsely listed.
    bool loop = false;
    double duration = -1.0;
    // True between a successful transport bring-up and its teardown.
    // The controls need this rather than the path: "path" stays set
    // with a sticky error so message and asset stay paired (5.5d's
    // rule), which would leave a dead transport looking commandable.
    bool live = false;
    struct Source {
        std::string name;  // "video" | "standalone"
        double gain = 1.0;
        bool muted = false;
    };
    std::vector<Source> sources;
    // The INDEPENDENT production track (task 6.15b, review R15). Its
    // own block rather than fields folded into the background's,
    // because it has its own lifetime: the background's fields above
    // keep describing the on-air video transport (and stay idle when
    // there is none), and this describes the track the operator
    // selected with media.standalone — which keeps playing through
    // every scene change. live is the commandability mirror again
    // (player up); error is sticky and names this block's own path,
    // so the 5.5d pairing rule holds inside the block without the
    // two transports ever crossing each other's reports.
    struct Standalone {
        bool live = false;
        std::string path;
        bool playing = false;
        double position = -1.0;
        double duration = -1.0;
        std::string error;
        std::string audio = "none";  // the lane names, as above
        // How many times the source has rewound at its end. Not
        // decoration: this transport loops by design, and the sink
        // clock keeps pacing whether or not the feeder is still
        // feeding — a bed that stopped at its EOF would keep reporting
        // "live" while the audience heard silence. The wrap count is
        // what distinguishes a bed that rewound from one that ran out.
        std::uint64_t loops = 0;
    } standalone;
};

// One recorded background-media failure, carried by the media.error
// notification (task 5.5d).
struct MediaError {
    std::string path;
    std::string message;
};

// The two audio identities the operator addresses (task 6.15a, review
// R15): the background's embedded audio, and the standalone production
// track. Each maps to one AvPlayer, and the mixer's table resolves the
// actual input index.
enum class AudioSource { kVideo, kStandalone };

// Task 6.7c (review R18) types the failure. Every report names the class
// of fault it is (a still that will not decode, the video transport, the
// audible lane), and recovery clears only the fault of its own class and
// asset — the audio returning must not hide an image that is still
// missing.
//
// kSync (task 6.7d, review R19) is a transport whose parts disagree: an
// operator seek the audio refused leaves the picture at the target and the
// sound where it was. The lane cannot say this — the audio is healthy,
// just elsewhere — so the lane reconcile must not clear it either, which
// is why it is its own class.
enum class MediaFault { kImage, kVideo, kAudio, kSync };

/*
 * The transport itself. Construction is cheap and side-effect free; the
 * players come up only through applyBackground() and the standalone swap,
 * and shutdown() is the single way down (idempotent, safe from the
 * destructor). OutputSession holds exactly one and delegates the whole
 * media surface to it; the --socket production loop and every bench seam
 * (setVideoPresenter/setVideoClock/submitVideo) drive this same object —
 * production and bench exercise one runtime, which is what R10 asked for.
 */
class BackgroundTransport {
public:
    BackgroundTransport() = default;
    ~BackgroundTransport();
    BackgroundTransport(const BackgroundTransport&) = delete;
    BackgroundTransport& operator=(const BackgroundTransport&) = delete;

    // --- the video seam (tasks 1.5e, 5.5; the bench seam too) ---

    // Before run(), or behind a retirement boundary (the 6.21c2 re-reading
    // of R6): the render thread is the only writer, the pointer is atomic,
    // and every pipeline it once named outlives its own join — so a pacing
    // thread's unsynchronised read finds either the pipeline it belongs to
    // or the successor, and the generation filter below decides which
    // submissions the seam still accepts.
    void setVideoPresenter(VideoPresenter* presenter);
    // Clock in seconds that pts is expressed against; defaults to wall
    // time since run() began (noteRunStart).
    void setVideoClock(std::function<double()> clock);
    // Any thread (the uploader): offer the newest GPU slot. Replaces the
    // pending submission (dropping it through the presenter) and wakes the
    // render thread.
    void submitVideo(VideoSubmission submission);
    // The pacing sink's entry (task 6.21c2, review F3): identical, plus the
    // GENERATION filter the retirement needs. A submission produced by a
    // pipeline the seam no longer names is retired work: dropped silently,
    // because the producing pipeline is stopping and its close() releases
    // every slot it held — including this frame's.
    void submitVideo(VideoSubmission submission, PlaybackPipeline* producer);
    // Render thread: the presenter submissions were made to (see the
    // retirement rule for who may hold this and when).
    VideoPresenter* presenter() const { return fPresenter.load(std::memory_order_relaxed); }

    // The run loop's wake question (task 6.7d, review R19): is a submitted
    // frame due against its DEADLINE in the clock's domain, and if not, how
    // long may the loop sleep before it is? Never a spin, never the media
    // pts (the two disagree by a pause, a seek or a loop wrap).
    struct VideoWake {
        bool pending = false;
        bool due = false;
        double sleepSeconds = 0.0;
    };
    VideoWake submissionWake(std::chrono::steady_clock::time_point now) const;

    // The frame's decision (tasks 1.5e, 5.5): draw the pending slot if the
    // clock has reached it (it becomes the held frame; the frame it
    // replaced is returned for onReplaced), otherwise the caller draws the
    // held frame unchanged. The presenter calls themselves (onReplaced,
    // acquireImage) belong to the caller, on the render thread, outside
    // this object — bounded to the two slots this seam carries.
    struct VideoTurnover {
        std::optional<VideoSubmission> toDraw;
        std::optional<VideoSubmission> replaced;
    };
    VideoTurnover takeDueVideo(std::chrono::steady_clock::time_point now);
    // The frame the audience is currently shown (drawn under the lease
    // until replaced) — the static hold a dirty redraw draws.
    std::optional<VideoSubmission> heldVideo() const;
    // The held submission yielded no image for its generation: stop
    // sampling it (the caller has already relinquished it through
    // onReplaced).
    void dropHeldVideo(unsigned token);
    // The preview branch (task 6.6a): sample the held frame again for a
    // publish without an audience present. Render thread, under the
    // graphics-queue lock, like frame()'s own acquire.
    SkImage* heldVideoImage() const;
    // The fallback clock's origin (the session's run() start): submissions
    // with no installed clock pace against wall time since this instant.
    void noteRunStart(std::chrono::steady_clock::time_point now);

    // --- status and fault reporting ---

    // The transport's reading of itself (both blocks: background and
    // standalone). OutputSession::status() copies it into the wire shape.
    MediaStatus mediaStatus() const;
    // Any thread (the RPC drain): take the next failure to push as
    // media.error.
    bool takeMediaError(MediaError* out);
    // A failure of one class for one asset (deduplicated on the message;
    // the socket must not spam). Public because the session's preload
    // reports through it (a file the probe cannot read is NAMED before the
    // audience ever sees the colour fill).
    void mediaFailed(MediaFault fault, const std::string& path, const std::string& message);
    // Clears the sticky error when (and only when) it is this class of
    // fault for this asset: the honest shape of "recovered".
    void mediaFaultCleared(MediaFault fault, const std::string& path);
    // The frame-time reconciliation of the transport's report with what
    // status claims (task 6.7c): position, audio lane, a decode that died
    // after opening, and an end-of-file the transport should not have
    // reached. Called by the frame commit and the half-second housekeep.
    void refreshMediaState();

    // --- lifecycle (render thread, OUTSIDE the frame's graphics-queue
    // lock: the swap's join() waits on pipeline threads that may be blocked
    // acquiring it) ---

    // Follow the scene's background (task 5.5). Same identity — asset id,
    // path and kind — changes nothing (the M3 continuity rule). Anything
    // else tears the transport down before opening the replacement through
    // the same PlaybackPipeline + AvPlayer pair the bench drives.
    // imageResident answers "is the still for this path in the decoded
    // cache" (a lookup the session's renderer owns, consulted under NO
    // lock); a picture that is not is a degradation the operator must see
    // (task 6.7c, review R18). Returns true when a video transport started
    // (the loop owes a present for its head frame). Since task 6.22d a call
    // that arrives while a retirement is in flight STARTS nothing and
    // returns false: the wanted transition becomes the single pending ask
    // (latest wins) and applyTransportOps() retries it when the lane
    // reports in — the realtime thread never waits on a slow join, and the
    // output keeps drawing the background still on screen meanwhile.
    bool applyBackground(const openshow::scene::Background& wanted, const GpuHandles& handles,
                         const std::function<bool(const std::string&)>& imageResident);
    // Prepare-time transport prime (task 6.9b, review R10). Runs a video
    // transport's FILE half — openFile: container, streams, decoder — on the
    // CALLER's thread (the prepare executor's), and parks the result as the
    // one primed transport. The next applyBackground that wants THIS path
    // attaches the GPU half on the render (owner) thread instead of opening
    // from disk, so a slow open cannot stall the frame boundary; a want for
    // any other path closes the stale prime (here, off the loop) and opens
    // fresh. The primed transport is bound to ONE: preparing a second video
    // retires the first's prime on the calling thread, never in a tick, and
    // the memory the file half holds (decoder + demuxer) is the whole cost.
    // The GPU handle stays untouched, so Ganesh's owner rule holds: only
    // applyBackground touches the device, always on the render thread.
    bool primeVideo(const std::string& path, const std::atomic<bool>* cancelled,
                    std::string* error);
    // Prepare-time audio prime (task 6.21c1, review F3). Runs on the
    // CALLER's thread (the prepare executor) like primeVideo: probes the
    // clip's audio story, starts an AvPlayer over it with
    // Config::startHeld — sink connection and negotiation, feeders, mixer,
    // pre-roll all paid HERE — and parks the held player beside the file
    // prime of the same path. The next applyBackground for that path adopts
    // the player (microseconds) instead of paying up to 1.5 s of negotiation
    // and 2 s of pre-roll on the render thread. A prime whose file half
    // never parked stages nothing; a superseded staged player is stopped on
    // the calling thread, never in a frame tick. The player touches no GPU,
    // so staging it keeps every GPU rule untouched.
    bool primeAudio(const std::string& path, const std::atomic<bool>* cancelled,
                    std::string* error);
    // End everything by name: the background transport (R6 order — quiesce,
    // then the slots, then the players) and the standalone track, which is
    // deliberately NOT part of the background's lifetime (task 6.15b) and
    // so must be ended by name. Idempotent; safe from the destructor.
    void shutdown();

    // --- transport control (task 6.7d review R19, 6.15a review R15) ---
    //
    // Any thread: the intent lands in a newest-wins slot and the render
    // loop applies it — AvPlayer's fan-out walks the attached-pipeline
    // list, and only the thread that swaps transports may touch it. A
    // refusal (the audio could not jump) is not answerable at this call;
    // it surfaces as the typed media fault, and the position the transport
    // keeps reporting is the one the audience actually has.
    void requestMediaSeek(double seconds);
    void requestMediaPause();
    void requestMediaPlay();
    // loop — live; the value AT the end-of-file decides rewind versus the
    // final-frame hold. gain / mute — per registered source, through the
    // mixer's clickless ramp; these answer up front: naming a source this
    // transport never registered is a refusal the caller hears immediately.
    void requestMediaLoop(bool enabled);
    bool requestMediaGain(AudioSource source, double gain, std::string* error = nullptr);
    bool requestMediaMute(AudioSource source, bool muted, std::string* error = nullptr);
    // Any thread: a background video transport is up (between a successful
    // bring-up and the teardown).
    bool mediaTransportLive() const { return fMediaTransportLive.load(); }
    // Whether this transport registered that audio identity (embedded
    // audio present and decodable), independent of whether the sink is
    // audible — the gain belongs to the mixer either way.
    bool mediaSourceLive(AudioSource source) const;

    // --- standalone production track (task 6.15b, review R15) ---
    //
    // The independent audio track's selection seam: the path is PROBED
    // here (the same probe scene.prepare runs, file I/O under no lock), so
    // "nothing readable at that path" is a refused request that changes
    // nothing — the old track, if any, keeps playing untouched. Accepted
    // paths ride to the render thread's swap (newest selection wins, like
    // every pending slot here) and output.status.media.standalone answers.
    // "" is a removal, not a probe candidate: it always accepts, and the
    // swap takes the track down. Since 6.21c2 the swap itself is cheap on
    // the render thread (claim, stage, publish) — the bring-up's waits
    // belong to a lane.
    bool requestStandaloneAudio(const std::string& path, std::string* error);
    // Any thread: the standalone transport is up (between a swap that
    // started it and one that stopped it, or teardown).
    bool standaloneLive() const { return fStandaloneLive.load(); }

    // The render loop's transport pass (task 6.7d): the pending intent and
    // any due scheduled op, then the mode/mix slots, then the standalone
    // swap, then a status refresh if anything was applied. Called by the
    // loop ahead of its present decision. Since task 6.22d it also reaps a
    // completed retirement and retries any deferred background transition
    // (PendingTransition); returns true when that retry STARTED a transport
    // — the loop then owes a present for its head frame, exactly as if the
    // scene had driven applyBackground itself.
    bool applyTransportOps();

    // Parses the test schedule (see fScheduledOps): "seek@900:0.6,
    // pause@2400,play@3200", milliseconds after the transport opened.
    // Public so the grammar itself is testable (task 6.18b); reports and
    // skips malformed entries — a fixture that quietly mis-parsed would
    // assert on an op that never ran.
    enum TransportOp { kOpNone, kOpSeek, kOpPause, kOpPlay };
    struct ScheduledOp {
        int atMs = 0;
        int kind = kOpNone;
        double seconds = 0.0;
    };
    static std::vector<ScheduledOp> parseScheduledOps(const std::string& spec);

    // Bench seam (task 6.22c, review A3): the RETIRED audio's delivery
    // truth while the retirement lane still holds it, empty when no
    // retirement is in flight. A3's contract is that the outgoing audio
    // goes inaudible at the hand-off, not when the lane finishes its join
    // — and the status mirror cannot show the difference, because the live
    // seam has already reported the fresh silent instance. Only the
    // in-flight retirement itself can: paused (the state the hand-off
    // established), sinkActive (whether the server still consumes the old
    // stream), and deliveredFrames (what it actually consumed). Safe from
    // any thread.
    struct RetiringAudioDelivery {
        bool paused = false;
        bool sinkActive = false;
        uint64_t deliveredFrames = 0;
    };
    std::optional<RetiringAudioDelivery> retiringAudioDelivery() const;

private:
    // Bring the background transport down in the R6 order (quiesce, then
    // the seam slots, then the players) and forget the applied identity.
    // It does NOT touch the standalone track — that outlives every
    // background (task 6.15b) and is ended only by shutdown(). Never
    // called with fMutex held: the join() must not wait behind a dying
    // transport's own mediaFailed report.
    void teardownBackgroundTransport();
    // Close a COMPLETED retirement on the render thread (the GPU half is a
    // Ganesh release; the close strictly follows the lane's join) and reap
    // its thread. Returns without touching anything while the lane still
    // waits — the lane's wakeRenderThread() brings the loop back for it.
    // Called from the loop's transport pass, applyBackground's head, and
    // (after a bounded wait for the lane) shutdown().
    void reapRetirement();
    // Task 6.21c1 (review F3): swap the current AvPlayer instance out for a
    // fresh silent one under fMutex, then stop the outgoing instance outside
    // the lock. Every death of an audio instance runs through here, so the
    // render thread's player is never null and a status reader is never
    // inside a dying player.
    void retireAudioInstance();

    // The lane to publish: the transport's own, unless this asset's audio
    // loss is something only the transport knows (a stream no decoder can
    // handle never becomes a source, so AvPlayer's lane says "none").
    AvPlayer::AudioLane mediaLaneLocked() const;

    void mediaPlaying(const openshow::scene::Background& background);
    void mediaStopped();
    void mediaFailedLocked(MediaFault fault, const std::string& path,
                           const std::string& message);
    void mediaFaultClearedLocked(MediaFault fault, const std::string& path);
    void refreshMediaStateLocked();

    // Standalone track machinery (task 6.15b; the swap re-cut by 6.21c2,
    // review F3). applyStandaloneOps() runs on the render thread ahead of
    // the ops, so a selection and a gain in the same pass land in order:
    // it publishes a lane-staged player when one is done (swap the instance
    // in under fMutex, release its hold, move live/mask/status with it),
    // takes a pending intent when the lane is free — claim the current
    // instance under fMutex, stop it outside the lock ("" ends there; a
    // path spawns the staging lane, Config::startHeld paying negotiation
    // and pre-roll there) — and never holds fMutex across either wait.
    // What the old swapStandalonePendingLocked did under the lock (stop +
    // start inline) held every RPC status reader for the seconds a bring-up
    // can take. reapStandaloneStage() drains the lane for shutdown (not a
    // frame tick; the bounded start wait is paid there).
    // refreshStandaloneMediaLocked samples the player into media.standalone
    // (position, lane, the lane-fault sticky) and rebuilds the
    // cross-transport sources list. standaloneFailedLocked names the
    // track's own path in the sticky error and the pushed notification —
    // deduplicated on the message, exactly the background's rule. Those
    // two keep their caller-holds-fMutex contract.
    void applyStandaloneOps();
    void reapStandaloneStage();
    void refreshStandaloneMediaLocked();
    void standaloneFailedLocked(const std::string& message);
    // Rebuilds media.sources from BOTH transports' registrations: "video"
    // when the background transport registered its embedded lane,
    // "standalone" when the standalone player is up. The mixer's answer
    // spans lifetimes — tearing the background down keeps the track's row
    // (and its echoed gain/mute), which is what the operator sees.
    void rebuildMediaSourcesLocked();

    // Seconds against the installed video clock; caller holds fMutex (or
    // is the seam reader behind the quiesce rule).
    double videoClockSecondsLocked(std::chrono::steady_clock::time_point now) const;

    // Wakes the session's render loop. Safe from any thread (GLFW owns the
    // socket this needs; posting an empty event is its portable equivalent
    // of writing an eventfd).
    void wakeRenderThread();

    // Background media transport (task 5.5, render thread; the R6 quiesce
    // rule says when the seam fields may change). The applied scene copy is
    // the identity in service: the session reads the seam, this decides
    // whether a scene change is even a change.
    //
    // Task 6.21c1 (review F3) made the audio half a swap-in INSTANCE, not a
    // fixed member: a staged, held player built on the prepare executor can
    // be adopted whole at the land. The instance is only ever commanded
    // from the render thread; the cross-thread readers (status under
    // fMutex) see whichever instance is current, and every replacement
    // happens under fMutex through retireAudioInstance(), so a reader is
    // never inside a dying player. The clock function a pacing thread holds
    // belongs to the instance that adopted it, and the R6 order (join, then
    // clock down, then retire) still guarantees the instance outlives its
    // last clock reader.
    std::unique_ptr<AvPlayer> fAudio = std::make_unique<AvPlayer>();
    std::unique_ptr<PlaybackPipeline> fBackground;
    openshow::scene::Background fAppliedBackground;    // identity in service

    // The deferred background transition (task 6.22d, review A4): when
    // applyBackground is asked for a new background while the ONE retirement
    // slot is still occupied, the transition is stored here — latest wins,
    // a burst of operator changes coalesces into the newest ask — and the
    // call returns without waiting. applyTransportOps() retries it on the
    // pass whose reap emptied the slot (the lane's wake makes that pass
    // prompt). Render-thread-only, like everything beside fAppliedBackground; a re-present of the background still in service cancels
    // it (the operator outranked it), and shutdown simply drops it (the
    // session is going away with its scene). No unbounded queue: capacity
    // overflow is this one slot plus the store's own newest-wins truth.
    struct PendingTransition {
        openshow::scene::Background wanted;
        GpuHandles handles;
        std::function<bool(const std::string&)> imageResident;
        std::chrono::steady_clock::time_point requestedAt;
    };
    std::optional<PendingTransition> fPendingTransition;

    // The retirement lane (task 6.21c2, review F3). The retired transport's
    // waits — join() and the player's stop — move onto this lane's thread;
    // the transport keeps the objects the retiring producer can still
    // REACH alive until the lane reports done and reapRetirement() closes
    // the GPU half on the render thread. One at a time: since task 6.22d a
    // second transition while the lane waits is DEFERRED by its caller
    // (PendingTransition below) rather than blocking this thread, so a
    // teardown reached with the slot occupied is a logged invariant
    // violation, not an overflow policy. The render thread owns the
    // slot; the lane holds a shared reference for the duration, so the
    // state (and its last release, which runs the post-close destructor)
    // stays ordered behind reapRetirement()'s join.
    struct Retirement {
        std::unique_ptr<PlaybackPipeline> pipeline;  // requestStop() already done
        std::unique_ptr<AvPlayer> audio;             // its player; stops on the lane
        std::atomic<bool> laneDone{false};           // join and audio stop finished
        std::thread worker;                          // the lane itself
    };
    std::shared_ptr<Retirement> fRetirement;         // render thread owns
                                                     // the hand-off; the
                                                     // pointer itself rides
                                                     // fMutex since 6.22c
    // The in-flight retirement, taken under fMutex (task 6.22c): every
    // touch of the shared_ptr itself is locked, because the retiring-audio
    // seam reads it from bench threads. The objects inside keep their own
    // rules — laneDone is atomic, and the lane moves `audio` out under
    // this same lock before stopping it outside.
    std::shared_ptr<Retirement> inFlightRetirement() const {
        const std::lock_guard<std::mutex> lock(fMutex);
        return fRetirement;
    }

    // Prepare-time prime (task 6.9b, review R10): a video transport whose
    // FILE half (container + decoder) is open but whose GPU half is not,
    // parked here for the render loop to attach. Bound to ONE — primeVideo
    // supersedes the previous prime on its own (executor) thread — and every
    // access is under fPrimeMutex, a leaf: primeVideo runs on the prepare
    // executor, applyBackground consumes it on the render thread, and neither
    // holds fMutex across the swap. A primed transport has touched no GPU,
    // so retiring it never runs a Ganesh call off its owner thread.
    //
    // Task 6.21c1 adds the audio half to the same lane, bound to the same
    // path: primeAudio stages a HELD AvPlayer (sink connected, feeders open,
    // pre-roll paid, clock parked — Config::startHeld) that
    // applyBackground adopts instead of probing and starting on the frame
    // boundary. The pair retires as a pair: a prime for a new path stops the
    // superseded audio on the executor thread, never in a tick.
    std::mutex fPrimeMutex;
    std::unique_ptr<PlaybackPipeline> fPrimed;
    std::unique_ptr<AvPlayer> fPrimedAudio;
    bool fPrimedAudioWanted = false;      // the prime's probe found a decodable stream
    std::string fPrimedAudioLoss;         // and what the audience would miss, if anything
    std::string fPrimedPath;

    // Standalone production audio (task 6.15b, review R15). A SEPARATE
    // AvPlayer, not the background's second mixer input: the background
    // transport is torn down and rebuilt every time the scene's background
    // identity changes, and a track riding it would restart at exactly the
    // moment R15 says it must not. The player lives between a swap that
    // started it and the next swap (or teardown); its sink is its own
    // stream and PipeWire mixes the two audience-side, which is the honest
    // shape — there is no business folding two lifetimes into one graph
    // just because one component could. The pending swap is the newest
    // selection the requesting thread probed; the render thread applies it
    // on the transport's own thread, where the background swap also runs.
    // Since 6.21c2 (review F3) the player is an INSTANCE — a staged, held
    // bring-up swaps in whole under fMutex, exactly like fAudio in 6.21c1 —
    // and the seconds of server negotiation and pre-roll a bring-up used to
    // spend under fMutex (blocking every RPC status reader with it) are
    // paid on the staging lane below. One stage at a time: a new intent
    // while a stage runs stays pending rather than racing a second
    // bring-up, and reapStandaloneStage() drains the lane before the
    // transport dies.
    std::unique_ptr<AvPlayer> fStandalonePlayer = std::make_unique<AvPlayer>();
    struct StandaloneStage {
        std::unique_ptr<AvPlayer> player;  // staged held player when done; null on failure
        std::string error;                 // bring-up failure (empty = ok)
        std::string path;                  // the request this stage answers
        std::atomic<bool> done{false};
        std::thread worker;
    };
    std::shared_ptr<StandaloneStage> fStandaloneStage;  // render thread
    std::mutex fStandaloneSwapMutex;                   // guards fStandaloneSwap
    std::optional<std::string> fStandaloneSwap;        // path to apply ("" = remove)
    std::atomic<bool> fStandaloneLive{false};          // any thread (the swap writes)
    std::string fStandalonePath;                       // transport thread; "" = none

    // The video seam (task 1.5e). fPresenter (task 6.6b, review R6, kept by
    // task 6.21c2): the render thread is the only writer, and it writes at
    // exactly three kinds of moment — before run(), around a start() whose
    // failure means no thread was ever spawned, and at a retirement, where
    // the pointer is ATOMIC so a pacing thread's unsynchronised read is
    // well-defined, and everything the read can name (the pipeline it was
    // stored from) stays alive in the retirement slot until that thread has
    // been joined. submitVideo() is the only reader off the render thread;
    // the producer-generation filter there is what lets the seam be
    // republished BEFORE the old producer's join instead of behind it —
    // the wait moved to the lane, the ordering stayed.
    std::atomic<VideoPresenter*> fPresenter{nullptr};
    std::function<double()> fVideoClock;               // fMutex
    std::optional<VideoSubmission> fPendingVideo;      // fMutex
    std::optional<VideoSubmission> fDrawnVideo;        // fMutex

    // Transport control handed to the render loop (task 6.7d). Newest-wins:
    // one intent at a time, applied by the loop's next pass, so a burst of
    // operator requests cannot queue behind each other and move the audience
    // somewhere nobody asked for.
    std::atomic<int> fPendingTransportOp{kOpNone};     // any thread
    std::atomic<double> fPendingSeekSeconds{0.0};      // any thread
    // Mode and per-source control intents beside the momentary ops above
    // (task 6.15a, review R15): each a newest-wins slot, applied by the
    // same render-thread pass. Loop matters AT the end of the clip, so
    // it is a stored mode, not an instant; gain and mute address
    // independent sources and ride beside a pending seek without
    // consuming it.
    std::atomic<int> fPendingLoop{-1};                 // -1 none, 0 off, 1 on; any thread
    std::atomic<int> fPendingGainSource{-1};           // identity or -1; any thread
    std::atomic<double> fPendingGain{0.0};             // meaningful while source >= 0
    std::atomic<int> fPendingMuteSource{-1};           // identity or -1; any thread
    std::atomic<int> fPendingMute{0};                  // 0/1 while source >= 0
    // What the RPC thread may ask for, mirrored at bring-up and teardown
    // (fBackground itself stays transport-thread-only).
    std::atomic<bool> fMediaTransportLive{false};      // any thread
    std::atomic<int> fMediaSourceMask{0};              // bit0 video, bit1 standalone
    // Test-only schedule (see OPENSHOW_TEST_TRANSPORT_OPS at the transport's
    // open): a timed script applied by the same render-thread seam, so the
    // scenarios R19 names run through the real transport rather than a
    // stand-in. Empty in production, where the variable is unset.
    std::vector<ScheduledOp> fScheduledOps;            // transport thread
    size_t fScheduledCursor = 0;                       // transport thread
    std::chrono::steady_clock::time_point fScheduledStart{};
    // One op on the transport (transport thread): pause, play, or a seek
    // whose refusal becomes the typed sync fault.
    void applyTransportOp(int op, double seconds);

    // The media mirror (task 5.5d onward): what status() will say about the
    // transport. The session copies it; it never mirrors it (R4's rule,
    // applied to media).
    MediaStatus fMedia;
    std::optional<MediaError> fPendingMediaError;      // fMutex; media.error push
    // Which fault the sticky fMedia.error describes, and the asset it
    // describes (task 6.7c, review R18): recovery has to know which report
    // it is allowed to erase.
    std::optional<std::pair<MediaFault, std::string>> fMediaFault;
    // Why the ON-AIR background's audio cannot be heard, when the transport
    // itself has no idea (task 6.7c, review R18). Set for the identity it
    // describes, cleared by mediaStopped().
    std::string fExpectedAudioLoss;
    // Fallback clock origin (the session's run() start) for a seam with no
    // installed clock; transport thread.
    std::chrono::steady_clock::time_point fRunStart{};

    // Guards everything above that is not atomic, EXCEPT the presenter
    // pointer (the quiesce rule) and the swap slot (its own lock, held only
    // for the handoff). The session may nest this mutex inside its own;
    // nothing here ever takes a session lock, and no join happens while
    // it is held.
    mutable std::mutex fMutex;
};

}  // namespace openshow::worker
