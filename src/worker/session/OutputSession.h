/*
 * OutputSession: the worker's runtime seam (PLAN.md "Worker architecture
 * graduation"), carved out of task 1.1's runRenderTest loop.
 *
 * The session owns the output window, Vulkan device, Skia Ganesh context,
 * the swapchain and the production compositor (SceneRenderer, task 6.1b),
 * and composites frames from the thread that calls open()/run(). This is
 * the object RPC commands act on: handler
 * threads may only call the thread-safe operations below. Layer toggles
 * land on the next frame (never mid-frame), bump an output revision the
 * acknowledgement reports, and apply on the render thread from the guarded
 * desired mask — so nothing render-owned is touched off-thread.
 *
 * It deliberately exposes no window, device or swapchain handles: if a
 * command cannot be expressed as a session operation, the seam grows
 * first (PLAN.md review rule).
 *
 * What the session does NOT own (tasks 6.18a/6.18a2): the scene truth and
 * the operator's picture. The prepared scenes, the output revision, the
 * desired mask/destination/scene, what the current frame has staged and what
 * the newest presented frame proved all belong to SceneStore
 * (session/SceneStore.h); the preview stream's lifecycle, the three targets'
 * cadence and the audience's timing budget, the editor's selection state, the
 * thumbnail queue and its decode policy, and the ready notifications all
 * belong to PreviewProducer (session/PreviewProducer.h), whose headers state
 * the rules and the thread contracts. This object coordinates the loop with
 * them: ask the store what is owed and for a snapshot, draw exactly that,
 * tell the store whether the panel took the frame, and hand the producer the
 * frame it drew (frames in, slots out). Two explicit thread rules make that
 * safe — the store's decide/land/commit trio is called only from the
 * thread running run(), and its command methods only from a handler thread
 * holding the session mutex, so the lock order is session -> store and the
 * store never calls back. The producer follows the same order and is called
 * under the same lock, with the documented exceptions its own header names.
 * There is exactly one mutable scene and one preview owner, and this file is
 * no longer allowed a copy of any of their fields.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <vulkan/vulkan.h>

#include "include/core/SkRefCnt.h"
#include "include/core/SkSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"

#include "openshow/scene/scene.h"

#include "output/OutputWindow.h"
#include "output/Swapchain.h"
#include "output/VulkanContext.h"
#include "playback/VideoSeam.h"
#include "render/Layer.h"
#include "render/SceneRenderer.h"
#include "session/BackgroundTransport.h"
#include "session/PreviewProducer.h"
#include "session/PrepareExecutor.h"
#include "session/SceneStore.h"

namespace openshow::worker {

class OutputSession {
public:
    // The preview types the RPC seam answers with (task 6.18a2): they are
    // PreviewProducer's now. The aliases keep the session as the object a
    // caller asks, without the session holding any of the state.
    using PreviewInfo = PreviewProducer::PreviewInfo;
    using PreviewReady = PreviewProducer::PreviewReady;
    using PreviewStatus = PreviewProducer::PreviewStatus;
    using ThumbnailEntry = PreviewProducer::ThumbnailEntry;
    using ThumbnailAck = PreviewProducer::ThumbnailAck;

    // Options::monitor values. -1 stays windowed, >=0 is that monitor;
    // kAutoSecondDisplay is the FreeShow autoPosition rule: land on the
    // second panel when the machine has one, so the operator's screen
    // stays free for the frontend.
    static constexpr int kAutoSecondDisplay = -2;

    struct Options {
        const char* title = "OpenShowQt output";
        // -1 keeps the borderless windowed mode; >=0 fullscreens on that
        // monitor; kAutoSecondDisplay picks index 1 when one exists.
        int monitor = -1;
        // Read the frame-30 image back on the CPU (the task 1.1 diagnostic).
        bool diagnostics = false;
        // Task 1.5g: VK_LAYER_KHRONOS_validation + debug messenger on the
        // Vulkan instance (dev tool, off by default).
        bool vulkanValidation = false;
        // Task 5.5: drive a presented scene's video background through the
        // production transport (PlaybackPipeline + AvPlayer). --socket mode
        // turns it on; benches and checks that never present scenes leave
        // it off, and the dev video-quad seam stays exactly as built.
        bool backgroundMedia = false;
        // Task 6.1b (review R1): what the compositor is composed OF.
        // Production is the default: create() with no builder draws black
        // with nothing presented (no demo gradient/photo/title), the clock
        // and aux layers start off, and the session suppresses the
        // diagnostics footer — so the startup and clear-all frames are
        // uniformly black including the footer region, and output and
        // preview carry the identical scene. A development driver passes
        // demoBuilder() (render/TestScene.h) to get the 1.x picture with
        // the live-frame clock; --socket leaves this empty.
        SceneRenderer::DemoBuilder demo;
    };

    // One physical panel, as the frontend sees it. index is the value
    // output.select takes; -1 (windowed) is not in this list.
    struct DisplayInfo {
        int index = 0;
        std::string name;
        int width = 0;
        int height = 0;
        int refreshHz = 0;
    };

    // Live preview state (task 1.4; the owner since 6.18a2 is
    // PreviewProducer — see PreviewStatus above). The Status snapshot below
    // embeds the producer's answer; the session keeps no mirror of it.

    // Live scene-store state (task 2.4b). Prepared scenes are kept by
    // the store revision prepareScene returns; revision 0 is never
    // handed out. The fields keep the lifecycle states distinct, per the
    // publication rule on Status (task 6.4, review R4):
    //   queued         pending names the newest accepted scene.present
    //                  whose scene has not reached a presented frame
    //                  yet (0 = nothing queued).
    //   transitioning active carries the revision the compositor is
    //                  committed to on air and transitioning says the
    //                  presented frames still blend the previous scene
    //                  in — the switch has landed but is not visually
    //                  settled.
    //   confirmed      active == the revision a scene.present was
    //                  acknowledged with, pending == 0 and
    //                  revision >= that command's revision.
    struct SceneStatus {
        uint64_t active = 0;        // scene carried by the newest presented frame
        uint64_t pending = 0;       // accepted present not yet presented (0 = none)
        bool transitioning = false; // that presented frame was mid-crossfade
        uint64_t prepared = 0;      // how many scenes the store holds
        // Task 6.9a: prepares whose file work the executor still owes —
        // readiness a status poller can watch when the ack itself matters.
        uint64_t preparing = 0;
        // The decoded-still cache (task 6.10, review R12), mirrored in at
        // every pin sync and frame commit: an operator sizing a show must
        // be able to ask how much CPU picture memory the worker holds,
        // what of it is pinned content, and whether the budget has begun
        // evicting. entries counts cached paths including remembered
        // failures; decodedBytes is pixel bytes only.
        struct Images {
            uint64_t decodedBytes = 0;
            uint64_t entries = 0;
            uint64_t pins = 0;
            uint64_t evictions = 0;
            // Lifetime preload hits (task 6.9a): times a prepare found the
            // still already decoded. With the ack deferred to the executor,
            // wall-clock timing cannot tell a re-decode from a hit — this
            // counter is the worker's own word that the disk was skipped.
            uint64_t preloadHits = 0;
            // The draw-boundary misses (task 6.9b, review R10): decodes and
            // blur builds the DRAW paid because the prepare did not cover
            // them — plus the frame loop's own text-layout builds. A ready
            // show moves none of these three.
            uint64_t drawDecodes = 0;
            uint64_t drawBackdrops = 0;
            uint64_t layoutBuilds = 0;
        } images;
        // Per-item foreground-picture failures (task 6.20d, review R18).
        // The media lane is background-only by recorded design: its error
        // names what the audience's TRANSPORT is showing, and a stack
        // picture's decode error must not evict that truth with a file
        // nobody is playing. What the finding named was the other half: an
        // authored picture that cannot decode was honest ON SCREEN (its box
        // shows what is under it) but invisible to anything ASKING — it
        // existed only in the worker's stderr. This is the asking half: one
        // entry per asset path whose most recent prepare attempt failed,
        // carrying that attempt's reason, so "which picture is missing" is
        // a question output.status answers. Entries clear when a later
        // prepare decodes the same path again — the same repair edge the
        // media lane gives its own assets (refreshFor re-reads a remembered
        // failure), and for the same reason: the operator's fix must be
        // able to undo the report without a restart. The LIST is capped
        // (a show with more broken paths than this is reporting an
        // incident, not a list); foregroundFailureEvents is the lifetime
        // count of failed attempts — monotonic, like presentFailures — so a
        // capped list beside it says "more happened than is shown" without
        // a second set to keep in step.
        struct ForegroundFailure {
            std::string path;
            std::string error;
        };
        static constexpr size_t kMaxReportedForegroundFailures = 8;
        std::vector<ForegroundFailure> foregroundFailures;
        uint64_t foregroundFailureEvents = 0;
    };

    // Background media visibility (task 5.5d) and the media.error
    // notification payload (task 5.5d): the wire shapes. Since task 6.18b
    // they are BackgroundTransport's types (session/BackgroundTransport.h
    // documents every field and the rules that fill them); the aliases keep
    // the session as the object a caller asks, without the session holding
    // any of the state.
    using MediaStatus = openshow::worker::MediaStatus;
    using MediaError = openshow::worker::MediaError;
    // The two audio identities the operator addresses (task 6.15a): the
    // transport's type, aliased for the same reason as the two above.
    using AudioSource = openshow::worker::AudioSource;

    // Snapshot of output state, safe to take from any thread.
    //
    // Publication rule (task 6.4, review R4): the applied fields —
    // revision, layerMask, monitor and scene.active — describe the
    // newest SUCCESSFULLY PRESENTED frame, never the moment the render
    // thread picked a command up. The run loop stages one immutable
    // FrameSnapshot of the desired state, the frame draws exactly that
    // snapshot, and only a successful submit/present publishes it. An
    // acquire or present failure therefore publishes nothing: active
    // keeps naming the scene the audience actually has, pending keeps
    // the command alive, and presentFailures honestly counts the failed
    // submits. So waiting for status().revision >= the revision a
    // command was acknowledged with now means "a frame carrying it
    // reached the compositor", not "the worker noticed".
    //
    // Since task 6.18a those fields are not stored here at all: they are
    // SceneStore's published set, copied into the returned snapshot by
    // status(). A Status is therefore always derived from the one scene
    // truth, and there is no mirror in the session that could be updated a
    // frame early — which is precisely how R4 happened.
    struct Status {
        uint32_t layerMask = 0;
        uint64_t revision = 0;
        unsigned long long frames = 0;
        // Failed acquires/presents since start: the honest evidence that
        // a confirmation took the long way around (task 6.4).
        unsigned long long presentFailures = 0;
        int width = 0;
        int height = 0;
        std::string deviceName;
        // Where the output currently is: -1 windowed, >=0 that display's
        // index, with its name empty when windowed.
        int monitor = -1;
        std::string monitorName;
        PreviewStatus preview;
        SceneStatus scene;
        MediaStatus media;
    };


    OutputSession() = default;
    ~OutputSession();

    OutputSession(const OutputSession&) = delete;
    OutputSession& operator=(const OutputSession&) = delete;

    // Opens the window, device, context and first swapchain, then draws
    // nothing until run() is called. Fills error on failure.
    bool open(const Options& options, std::string* error);
    void close();

    // Composites frames until the window closes, requestStop() is called,
    // seconds elapse (<=0 means no limit) or *abort turns true. Returns
    // frames presented.
    unsigned long long run(double seconds, const std::atomic<bool>* abort = nullptr);

    // --- thread-safe operations (RPC handlers) ---

    // Queues a layer toggle by name ("background", "images", ...). Returns
    // false for unknown names; otherwise *revisionOut (when given) receives
    // the revision this change is included in.
    bool setLayerEnabled(std::string_view name, bool enabled, uint64_t* revisionOut);

    // The panels GLFW reported, refreshed on the render thread at open,
    // after each destination change and periodically after that (so a
    // hotplug shows up without any event plumbing yet, M6.1).
    std::vector<DisplayInfo> displays() const;

    // Moves the audience output: -1 windowed, >=0 that display's index
    // (validated against the last snapshot). The GLFW call and the
    // swapchain rebuild happen on the render thread at the top of a frame,
    // exactly like layer toggles; *revisionOut reports the revision the
    // change is included in.
    bool selectMonitor(int index, uint64_t* revisionOut);

    // --- preview (task 1.4; owner since 6.18a2 is PreviewProducer) ---
    //
    // The session keeps this seam because it is the object the RPC handlers
    // act on and the object that wakes the frame loop; the lifecycle,
    // generation gates, cadence, budget, queue and counters all answer to
    // the producer, whose header states each rule. A preview command that
    // lands (and a pump that settles anything) wakes the render thread.

    // Enables the preview pipeline (geometry limits keep a preview a
    // preview, not a second output); the segment is built by the render
    // thread on its next frame. Calling it again replaces the stream under
    // a new name.
    bool startPreview(uint32_t width, uint32_t height, uint32_t fps,
                      PreviewInfo* info, std::string* error);
    void stopPreview();

    // The editor's off-air render (task 6.16a, review R16): draws a
    // prepared scene into the stream without touching the audience's scene,
    // playback or crossfades. Revision 0 stands the target down; `request`
    // must come strictly after the accepted generation.
    bool selectPreviewScene(uint64_t revision, std::uint32_t request,
                            std::string* error);

    // --- thumbnails (task 6.16b, review R16) ---

    // Replaces the pending thumbnail batch at `width` x `height`;
    // replacement is the coalescing and cancellation rule at once, and the
    // refusals (stream, extent, batch cap, key rules, generation gate) are
    // the producer's. An empty batch with a new request stands the target
    // down.
    bool requestThumbnails(const std::vector<ThumbnailEntry>& batch, uint32_t width,
                           uint32_t height, std::uint32_t request, ThumbnailAck* ack,
                           std::string* error);

    // Settles at most one thumbnail decode on the CALLER's thread (the RPC
    // loop) — the draw never opens a file (task 6.10's rule). Returns true
    // when it settled at least one job; the caller's wake follows.
    bool pumpThumbnailDecode();

    // The consumer finished copying slot@seq of the named stream; releases
    // for a retired or unknown stream, an unknown slot or a sequence that
    // is not exactly the slot's published one are ignored (task 6.5, R5).
    void releasePreviewSlot(uint32_t slot, uint64_t seq, const std::string& stream);

    // Pops one queued preview.ready notification (RPC thread); stale-
    // stream notifications are dropped, never sent (task 6.5).
    bool takePreviewReady(PreviewReady* out);

    // --- background media (task 5.5d) ---

    // Pops the pending media.error notification (RPC thread, from
    // RpcServer::flushNotifications). Latest failure wins and the slot is
    // a single optional: two failures between polls would still leave the
    // newest text in status.media.error, so a fresh overwrite never hides
    // what the operator needs. This is the immediacy path; the polled
    // status.media.error is the durability path.
    bool takeMediaError(MediaError* out);

    // --- scenes (task 2.4b, store extracted in 6.18a) ---

    // How many prepared scenes the store keeps: beyond this the oldest revision
    // that can no longer be put on air is evicted — the exemptions are the
    // desired scene (its command is accepted, its frame has not run) and the
    // landed one (the renderer is drawing it, a fade may still blend from it).
    // The rule and the number belong to SceneStore; the session only names
    // them, because a caller sizing a show against the store's capacity asks
    // this object.
    static constexpr std::size_t kMaxPreparedScenes = SceneStore::kMaxPrepared;

    // Keeps a compiled scene and reports the store revision that
    // presentScene accepts. Preparing changes nothing on screen, so no
    // output revision moves and no frame is provoked; the count visible
    // in status().scene.prepared does.
    //
    // Task 6.9a (review R10): the store + pin answer is synchronous — the
    // revision is immediate truth — but the file work it schedules (still
    // decodes, the video probe) runs on the prepare executor, so a cold
    // file never parks the RPC thread. onReady fires exactly once per
    // accepted prepare: from the executor thread with the revision, or
    // cancelled=true when the work was retired (stop) — the job's reply
    // belongs to its caller either way. Returns false — with NO onReady to
    // follow — when the store refused, or the executor was stopping or at
    // capacity; the caller answers that request explicitly. jobId is the
    // RPC request id, opaque here, so a superseded job can be retired by
    // name. Without background media (benches) there is nothing to
    // schedule: the completion fires inline before this returns.
    bool prepareScene(const openshow::scene::Scene& scene, uint64_t jobId,
                      std::function<void(uint64_t revision, bool cancelled)> onReady,
                      uint64_t* revisionOut);
    // Task 5.5 preload, off the render thread always and since 6.9a off the
    // RPC thread too: decodes the background still into the image cache
    // and probes a background video, so the file's cost and its failures
    // surface while the OLD scene is still on air. `cancelled` is the
    // executor's cooperative flag (nullptr = run to completion); it is
    // checked between files, the natural unit.
    void preloadScene(const openshow::scene::Scene& scene,
                      const std::atomic<bool>* cancelled);

    // Task 6.20d (review R18): the per-item foreground lane behind
    // SceneStatus::foregroundFailures — one upsert per failed prepare
    // attempt (deduped by path, message updated), one removal per same-path
    // success. The executor thread runs every prepare, so both take fMutex
    // for the length of a vector scan; the prepare's own file work is
    // orders of magnitude longer, and status() reads the same lock it
    // already reads for every other field.
    void foregroundImageFailed(const std::string& path, const std::string& message);
    void foregroundImageRecovered(const std::string& path);
    // Task 6.9b: the text and blur half of a prepare — the costs the
    // frame loop used to pay AT the switch. Shapes the scene into the
    // production renderer's OWN layout cache and builds its blur backdrop
    // if it has one, on the CALLER's thread (the prepare executor's). The
    // caches' own rules (LRU slots, byte budget) are unchanged; only the
    // moment of payment moves off the frame boundary (p95 100 ms, R10).
    void prewarmScene(const std::shared_ptr<const openshow::scene::Scene>& scene);

    // Names a prepared revision as the output's scene. The switch lands
    // on the render thread's next frame, like layer toggles, and
    // *revisionOut reports the output revision it is included in;
    // presenting the revision already desired is a success without a
    // new revision. Unknown revisions (and 0) are refused — to go black
    // the frontend prepares the cleared scene, which is an ordinary
    // scene. The applied scene is what the production layers draw
    // (task 3.1): the background fills its colour (or its cover-fitted
    // image, task 3.2) and the words layer lays out its text; with
    // nothing presented the demo picture renders. `kind` decides how the
    // switch is revealed: a fade crossfades the outgoing scene (FreeShow's
    // default, task 3.2), a cut swaps in one frame; the first
    // presentation always pops in — there is nothing to fade from.
    bool presentScene(uint64_t prepared, uint64_t* revisionOut,
                      SceneTransitionKind kind = SceneTransitionKind::kFade);

    // --- video (task 1.5e) ---

    // The seam a bench (later: the media layer) uses to present GPU slots:
    // the session decides *when* a slot is drawn (against the video clock),
    // the presenter owns *what* the slot is and its GPU lifetime. Task 5.1
    // moved the types to playback/VideoSeam.h so the production pipeline
    // implements them without the session's window machinery; the names
    // stay here for every existing caller.
    using VideoPresenter = ::openshow::worker::VideoPresenter;
    using VideoSubmission = ::openshow::worker::VideoSubmission;

    // Both set before run(); presenter may be null (no video at all).
    void setVideoPresenter(VideoPresenter* presenter);
    // Clock in seconds that pts is expressed against; defaults to wall
    // time since run() began.
    void setVideoClock(std::function<double()> clock);

    // Any thread (the uploader): offer the newest GPU slot. Replaces the
    // pending submission (dropping it through the presenter) and wakes the
    // render thread.
    void submitVideo(VideoSubmission submission);

    // --- background transport control (task 6.7d review R19, 6.15a/b
    // review R15; BackgroundTransport is the owner since 6.18b) ---
    //
    // These forward the operator's intent to the transport's newest-wins
    // slots; the render loop applies it, because AvPlayer's fan-out walks
    // the attached-pipeline list and only the thread that swaps transports
    // may touch it. A refusal (the audio could not jump) is not answerable
    // at this call — it surfaces as the typed media fault, and the position
    // the transport keeps reporting is the one the audience actually has.
    // gain and mute DO answer up front: naming a source this transport
    // never registered is a refusal the caller hears immediately.
    // mediaTransportLive() guards the commands that need a transport at
    // all; the rules are stated in full in session/BackgroundTransport.h.
    void requestMediaSeek(double seconds);
    void requestMediaPause();
    void requestMediaPlay();
    void requestMediaLoop(bool enabled);
    bool requestMediaGain(AudioSource source, double gain, std::string* error = nullptr);
    bool requestMediaMute(AudioSource source, bool muted, std::string* error = nullptr);
    // Any thread: a background video transport is up (between a
    // successful bring-up and the teardown).
    bool mediaTransportLive() const { return fTransport.mediaTransportLive(); }
    // Whether this transport registered that audio identity (embedded
    // audio present and decodable), independent of whether the sink is
    // audible — the gain belongs to the mixer either way.
    bool mediaSourceLive(AudioSource source) const;

    // --- standalone production track (task 6.15b, review R15) ---
    //
    // The independent audio track's selection seam, forwarded to the
    // transport: the path is PROBED before the request is accepted (file
    // I/O under no lock), so "nothing readable at that path" changes
    // nothing — the old track, if any, keeps playing. "" is a removal and
    // always accepts. Accepted paths ride to the render thread's swap and
    // output.status.media.standalone answers.
    bool requestStandaloneAudio(const std::string& path, std::string* error);
    // Any thread: the standalone transport is up (between a swap that
    // started it and one that stopped it, or teardown).
    bool standaloneLive() const { return fTransport.standaloneLive(); }

    // Device handles a bench needs beside the session (task 1.5e): the YUV
    // upload ring lives outside the session but shares its device, and the
    // seam to build it is this narrow view rather than the objects (the
    // struct is BackgroundTransport's since 6.18b, which is where the
    // bring-up consumes it). The context may only be touched from the
    // render thread (no thread-safety guarantee at this Skia revision); the
    // Vulkan handles are safe to use from an upload thread. Valid between
    // open() and close().
    GpuHandles gpuHandles() const;

    Status status() const;
    void requestStop();
    bool running() const;

private:
    // One immutable capture of the desired output state (task 6.4, the
    // snapshot stage of the publication rule, owned by SceneStore since
    // 6.18a): taken under the store's lock at the run loop's present
    // decision, drawn exactly by frame(), and published by that frame's
    // successful present. Commands arriving after the capture belong to the
    // next snapshot — they can neither ride into this frame's pixels
    // unnoticed nor borrow its status credit.
    using FrameSnapshot = SceneStore::Snapshot;

    bool createSwapchain();
    // Presents exactly one frame, drawn from the staged snapshot. The
    // snapshot's revision is what a successful present credits: a command
    // landing after the capture re-dirties the loop instead of being
    // swallowed or borrowed (task 1.5e's invariant, given the honest
    // publication of task 6.4).
    void frame(const FrameSnapshot& snapshot);
    void housekeep();
    void applyPendingLayerChanges(const FrameSnapshot& snapshot);
    void applyPendingDisplayChange(const FrameSnapshot& snapshot);
    void applyPendingSceneChange(const FrameSnapshot& snapshot);
    // The one preview handoff the loop makes (task 6.18a2): wraps the frame
    // this pass put on the panel (or the last presented one when it presented
    // nothing) with the video the audience's transport is showing, the
    // session's frame count, its clock and its on-air scene, and hands them
    // to PreviewProducer::drawTargets. Lifecycle, target choice, cadence,
    // budget and publication all live there.
    PreviewProducer::DrawInput previewHandoff(const SceneFrame& frame, SkImage* video) const;
    void refreshDisplaySnapshot();

    // Wakes the thread blocked in run()'s glfwWaitEventsTimeout. Safe from
    // any thread (GLFW owns the socket this needs; posting an empty event
    // is its portable equivalent of writing an eventfd).
    void wakeRenderThread();

    // Task 6.10 (review R12): the pins that make the image cache's byte
    // budget safe. Every still background the session could still put on
    // air — any scene in the bounded store, plus the desired/applied
    // revisions and the render thread's on-air copy — pins its path, so
    // eviction only ever forgets scenes the store itself has let go. Re-
    // derives the whole pin set (at most 17 paths) and mirrors the cache
    // stats into status; called on the RPC thread at prepare/present and
    // on the render thread at the frame commit, all under fMutex, none of
    // it touching the disk.
    void syncScenePins();
    // Copies the cache's counters into fStatus.scene.images (on-lock).
    void mirrorImageStatsLocked();
    // The panel a published destination names, read from the display snapshot
    // GLFW refreshed on the render thread (task 6.18a: status() derives the
    // name from the store's published monitor, so the name can never describe
    // an index a later frame moved past). Empty while windowed — and also
    // empty when a hotplug removed the panel that index named, which is the
    // honest answer to "where is the output". On-lock (reads fDisplays).
    std::string monitorNameLocked(int monitor) const;

    // Background media fault reporting (task 5.5d, typed in 6.7c review
    // R18) — thin forwarders to BackgroundTransport, the media's owner
    // (6.18b): the preload's probe reports through them (a file it cannot
    // read is NAMED before the audience ever sees the colour fill, and a
    // file put back is the recovery that clears the error). The fault
    // classes and the deduplication rules are BackgroundTransport's.
    void mediaFailed(MediaFault fault, const std::string& path, const std::string& message);
    void mediaFaultCleared(MediaFault fault, const std::string& path);

    std::unique_ptr<OutputWindow> fWindow;
    std::unique_ptr<VulkanContext> fVk;
    sk_sp<GrDirectContext> fContext;
    std::unique_ptr<Swapchain> fSwapchain;
    std::unique_ptr<SceneRenderer> fScene;

    std::atomic<bool> fStop{false};
    bool fDiagnostics = false;
    unsigned long long fFrames = 0;
    bool fLoggedVideoQuad = false;
    std::chrono::steady_clock::time_point fStartTime;

    // Dirty tracking for the event-driven loop (task 1.5e): the run loop
    // presents only when the store owes a frame, a new video slot is due, or
    // the window itself asks (close, resize). Since task 6.18a the first half
    // of that question is SceneStore's own (its published revision against the
    // commands it has accepted), so the loop keeps only the reason that is
    // about this object's GPU state.
    bool fNeedsPresent = false;           // render thread (swapchain recreated)

    // The frame this loop is drawing, and the last one the panel took
    // (task 6.11a, review R13). Every target of a frame — the audience
    // swapchain and the operator's preview — draws fComposedFrame, and a
    // preview-only wake redraws fPresentedFrame; neither composes a frame of
    // its own, so a preview cannot advance a crossfade, spend the audience's
    // clock, or publish pixels under a revision it does not carry. Both are
    // render-thread only, like the loop that writes them.
    SceneFrame fComposedFrame;            // render thread
    SceneFrame fPresentedFrame;           // render thread

    ActiveScene fRenderScene;                              // = the composition's scene
    bool fBackgroundMedia = false;                         // from Options

    // Guards the session-side state below and orders the calls the render
    // thread makes into the components this session coordinates. It may NEST
    // the store's lock (task 6.18a's rule: session -> store, never the
    // reverse), and the store calls nothing back — which is what lets a command
    // thread hold it briefly and keep the expensive part of its work (a decode,
    // a media probe) outside both locks.
    mutable std::mutex fMutex;
    // The output's scene truth (task 6.18a): the bounded prepared store, the
    // output revision, the desired mask/destination/scene, what the current
    // frame has staged, and what the newest presented frame proved. Command
    // threads change desired state; the render thread stages and commits. The
    // display snapshot is refreshed on the render thread too, so GLFW is only
    // ever queried there.
    SceneStore fStore;
    std::vector<DisplayInfo> fDisplays;
    Status fStatus;

    // The prepare-time file work (task 6.9a, review R10): still decodes and
    // the video probe leave the RPC thread for this bounded, cancellable
    // worker. It is started by open() only in socket mode (fBackgroundMedia)
    // and stopped at the very top of close(), so when stop() returns no
    // completion can fire again and nothing this work touches — the image
    // cache, the transport's fault lane — is torn down under it.
    PrepareExecutor fPrepares;

    // The operator's picture (task 1.4; owner since 6.18a2 is
    // PreviewProducer): the stream's lifecycle and generation rule, the
    // three targets' cadence and the audience's timing budget, the editor's
    // selection, the thumbnail queue and its decode policy, the slot
    // ownership proof and the ready notifications. The session wakes it
    // into existence with applyStreamState() at the top of a frame and
    // hands it the frame the audience drew with drawTargets() after the
    // present — frames in, slots out; the session holds no preview state
    // of its own to fall out of step with. Its header owns the thread
    // contract; the lock order is session -> producer, never the reverse.
    // Declared after the GPU members so destruction releases its surfaces
    // before the context they live on (close() releases them explicitly
    // first).
    PreviewProducer fPreview;

    // The audience-side media (tasks 5.5–6.15b; owner since task 6.18b is
    // BackgroundTransport): the background transport and its audio feeder,
    // the standalone track, the video seam's slots and their leases, the
    // intent slots, the typed fault table and the whole media status mirror.
    // The loop drives it on its own thread (the ops pass, the scene-change
    // swap ahead of the queue lock, the frame-commit and housekeep
    // refreshes), the command verbs forward straight to it, and status()
    // copies its mirror — the session holds no media state of its own to
    // fall out of step with. The lock order is session -> transport, never
    // the reverse, and nothing in it calls back. Declared after the GPU
    // members, and close() (and its destructor) shut it down first, so the
    // pipeline's device objects are released before the context they live
    // on — the same rule that ordered it inside the session before.
    BackgroundTransport fTransport;
};

}  // namespace openshow::worker
