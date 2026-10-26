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

#include "openshow/protocol/preview_wire.h"
#include "openshow/scene/scene.h"

#include "output/OutputWindow.h"
#include "output/Swapchain.h"
#include "output/VulkanContext.h"
#include "playback/AvPlayer.h"
#include "playback/PlaybackPipeline.h"
#include "playback/VideoSeam.h"
#include "render/Layer.h"
#include "render/SceneRenderer.h"

namespace openshow::worker {

class PreviewStream;

class OutputSession {
public:
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

    // Live preview state as the render thread applies it. running only
    // turns true once the shm segment exists; error explains a failed
    // start (and then running stays false). published/dropped let the
    // frontend show the load behaviour honestly.
    struct PreviewStatus {
        bool running = false;
        std::string name;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t fps = 0;
        uint64_t published = 0;
        uint64_t dropped = 0;
        std::string error;
    };

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
    };

    // Background media visibility (task 5.5d). playing says the session
    // has a live transport for the on-air background; error carries the
    // last failure for that asset and STAYS set until the same background
    // opens successfully — the frontend polls, and an operator must be
    // able to see what broke after the fact, not only in the moment.
    // Every fresh failure additionally rides the media.error
    // notification (immediacy), so status and push together close the
    // gap between polls.
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
    };

    // One recorded background-media failure, carried by the media.error
    // notification (task 5.5d).
    struct MediaError {
        std::string path;
        std::string message;
    };

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

    // --- preview (task 1.4) ---

    // What preview.start reports: where the frames will live and how the
    // segment is laid out (openshow/protocol/preview_wire.h). The segment
    // itself is created by the render thread on its next frame, so wait
    // for the first preview.ready notification (or status().preview
    // .running) before mapping.
    struct PreviewInfo {
        std::string name;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t slotCount = 0;
        uint32_t bytesPerSlot = 0;
    };

    // One queued preview.ready notification (drained by the RPC thread).
    struct PreviewReady {
        uint32_t slot = 0;
        uint64_t seq = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t rowBytes = 0;
        uint32_t revision = 0;
        uint64_t publishedNs = 0;
        std::string stream;  // segment name this frame belongs to (task 6.5)
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

    // Enables the preview pipeline (task 1.4): the same scene, drawn to
    // a GPU offscreen surface at the requested size, is read back into
    // rotating shm slots at the requested cadence. Callable from any
    // thread; the segment and the surface are built by the render thread
    // on its next frame. Calling it again replaces the stream under a new
    // name. Geometry limits keep a preview a preview, not a second output.
    bool startPreview(uint32_t width, uint32_t height, uint32_t fps,
                      PreviewInfo* info, std::string* error);
    void stopPreview();

    // The consumer finished copying slot@seq of the named stream; the
    // slot becomes claimable again. Proof of ownership (task 6.5, review
    // R5): the release must name the stream that is currently live —
    // releases for a retired or unknown stream, an unknown slot or a
    // sequence that is not exactly the slot's published one are ignored.
    void releasePreviewSlot(uint32_t slot, uint64_t seq, const std::string& stream);

    // Pops one queued preview.ready notification (RPC thread). At most
    // one is queued per slot: if the RPC side lags, the newest frame for
    // that slot is all that matters. Ready notifications left queued for
    // a retired stream are dropped, never sent: the socket must not
    // describe slots of a segment the consumer no longer has (task 6.5).
    bool takePreviewReady(PreviewReady* out);

    // --- background media (task 5.5d) ---

    // Pops the pending media.error notification (RPC thread, from
    // RpcServer::flushNotifications). Latest failure wins and the slot is
    // a single optional: two failures between polls would still leave the
    // newest text in status.media.error, so a fresh overwrite never hides
    // what the operator needs. This is the immediacy path; the polled
    // status.media.error is the durability path.
    bool takeMediaError(MediaError* out);

    // --- scenes (task 2.4b) ---

    // How many prepared scenes the store keeps: the oldest revision that
    // is neither presented nor pending is evicted beyond this.
    static constexpr std::size_t kMaxPreparedScenes = 16;

    // Keeps a compiled scene and reports the store revision that
    // presentScene accepts. Preparing changes nothing on screen, so no
    // output revision moves and no frame is provoked; the count visible
    // in status().scene.prepared does.
    bool prepareScene(const openshow::scene::Scene& scene, uint64_t* revisionOut);
    // Task 5.5 preload, run from prepareScene's RPC thread (never the
    // render thread): decodes the background still into the image cache
    // and probes a background video, so the file's cost and its failures
    // surface while the OLD scene is still on air.
    void preloadScene(const openshow::scene::Scene& scene);

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

    // Device handles a bench needs beside the session (task 1.5e): the YUV
    // upload ring lives outside the session but shares its device, and the
    // seam to build it is this narrow view rather than the objects. The
    // context may only be touched from the render thread (no thread-safety
    // guarantee at this Skia revision); the Vulkan handles are safe to use
    // from an upload thread. Valid between open() and close().
    struct GpuHandles {
        VkDevice device = VK_NULL_HANDLE;
        VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
        uint32_t graphicsQueueIndex = 0;
        uint32_t uploadQueueIndex = 0;
        VkQueue uploadQueue = VK_NULL_HANDLE;
        GrDirectContext* context = nullptr;
    };
    GpuHandles gpuHandles() const;

    Status status() const;
    void requestStop();
    bool running() const;

private:
    // One immutable capture of the desired output state (task 6.4, the
    // snapshot stage of the publication rule): taken under fMutex at the
    // run loop's present decision, drawn exactly by frame(), and published
    // by that frame's successful present. Commands arriving after the
    // capture belong to the next snapshot — they can neither ride into
    // this frame's pixels unnoticed nor borrow its status credit.
    struct FrameSnapshot {
        uint64_t revision = 0;
        uint32_t mask = 0;
        uint64_t scene = 0;  // scene-store id, 0 = nothing presented
        SceneTransitionKind kind = SceneTransitionKind::kFade;
        int monitor = -1;
        // The scene's content, pinned at capture (one copy per scene id,
        // cached in fCapturedScene). Without the pin a scene prepared
        // after this capture could evict the snapshot's store entry before
        // the frame applied it — the store race R4 hid behind the old
        // apply-time read of fDesiredScene.
        ActiveScene content;
    };

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
    void applyPreviewState();
    // Tags the published frame with the revision of the snapshot it drew
    // (frame()), or of the last presented frame when the run loop
    // republishes without presenting (task 6.4: tags name drawn content,
    // never merely desired state).
    void publishPreviewFrame(double elapsed, SkImage* video, uint64_t snapshotRevision);
    void refreshDisplaySnapshot();

    // Wakes the thread blocked in run()'s glfwWaitEventsTimeout. Safe from
    // any thread (GLFW owns the socket this needs; posting an empty event
    // is its portable equivalent of writing an eventfd).
    void wakeRenderThread();

    // Background media transport (task 5.5). Both run on the render thread
    // only, and OUTSIDE frame()'s graphics queue lock (the run loop applies
    // scene changes before the lock is taken): the swap's join() waits on
    // pipeline threads that may be blocked acquiring that lock. Apply
    // compares the scene's background against the identity the transport
    // currently serves — unchanged means untouched (the M3 continuity rule:
    // a lyric change must not disturb a playing background video) — and
    // anything else tears the transport down before opening the replacement
    // through the same PlaybackPipeline + AvPlayer pair the bench drives.
    void applyBackgroundTransport(const openshow::scene::Background& wanted);
    void teardownBackgroundTransport();

    // Background media visibility events (task 5.5d). Each updates
    // fStatus.media under fMutex; mediaFailed additionally fills the
    // pending notification slot the RPC thread sends as media.error.
    // mediaPlaying is the success path: it clears the sticky error.
    //
    // Task 6.7c (review R18) types the failure. Every report names the
    // class of fault it is (a still that will not decode, the video
    // transport, the audible lane), and recovery clears only the fault of
    // its own class and asset — the audio returning must not hide an image
    // that is still missing. The *Locked variants are for callers that
    // already hold fMutex (the frame's status refresh reconciles the
    // transport's own report there, every frame).
    enum class MediaFault { kImage, kVideo, kAudio };

    void mediaPlaying(const openshow::scene::Background& background);
    void mediaStopped();
    void mediaFailed(MediaFault fault, const std::string& path, const std::string& message);
    void mediaFailedLocked(MediaFault fault, const std::string& path, const std::string& message);
    // Clears the sticky error when (and only when) it is this class of
    // fault for this asset: the honest shape of "recovered".
    void mediaFaultCleared(MediaFault fault, const std::string& path);
    void mediaFaultClearedLocked(MediaFault fault, const std::string& path);
    // The frame-time reconciliation of the transport's report with what
    // status claims (task 6.7c): position, audio lane, a decode that died
    // after opening, and an end-of-file the transport should not have
    // reached. Caller holds fMutex.
    void refreshMediaStateLocked();

    // The lane to publish: the transport's own, unless this asset's audio
    // loss is something only the session knows (a stream no decoder can
    // handle never becomes a source, so AvPlayer's lane says "none").
    AvPlayer::AudioLane mediaLaneLocked() const;

    // Seconds against the installed video clock; caller holds fMutex.
    double videoClockSecondsLocked(std::chrono::steady_clock::time_point now) const;

    std::unique_ptr<OutputWindow> fWindow;
    std::unique_ptr<VulkanContext> fVk;
    sk_sp<GrDirectContext> fContext;
    std::unique_ptr<Swapchain> fSwapchain;
    std::unique_ptr<SceneRenderer> fScene;

    std::atomic<bool> fStop{false};
    bool fDiagnostics = false;
    unsigned long long fFrames = 0;
    bool fLoggedVideoQuad = false;
    uint32_t fAppliedMask = 0;
    std::chrono::steady_clock::time_point fStartTime;

    // Dirty tracking for the event-driven loop (task 1.5e): presented* are
    // render-thread copies compared against the guarded desired state; the
    // run loop presents only when they diverge, a new video slot is due, or
    // the window itself asks (close, resize).
    uint64_t fPresentedRevision = 0;      // render thread
    bool fNeedsPresent = false;           // render thread (swapchain recreated)

    // Video seam state (task 1.5e).
    // fVideoPresenter (task 6.6b, review R6): the render thread writes it
    // only when no pacing thread can read it — before run(), after a
    // teardown's requestStop+join, or around a start() whose failure means
    // no thread was ever spawned. submitVideo() (the producer's thread)
    // reads it unsynchronised, and thread creation/join are what order
    // those accesses; nothing else may change it while a transport runs.
    VideoPresenter* fVideoPresenter = nullptr;
    std::function<double()> fVideoClock;                   // fMutex
    std::optional<VideoSubmission> fPendingVideo;          // fMutex
    std::optional<VideoSubmission> fDrawnVideo;            // render thread
    std::chrono::steady_clock::time_point fRunStart;       // render thread

    // Background media transport (task 5.5, render thread; declared after
    // fContext/fVk so their destruction order frees the pipeline's device
    // objects before the context they live on). The applied scene copy is
    // what the draw path consults to decide whether the offered video
    // frame belongs to the background layer.
    AvPlayer fAudio;
    std::unique_ptr<PlaybackPipeline> fBackground;
    openshow::scene::Background fAppliedBackground;        // identity in service
    ActiveScene fRenderScene;                              // = the composition's scene
    bool fBackgroundMedia = false;                         // from Options
    std::optional<MediaError> fPendingMediaError;          // fMutex; media.error push
    // Which fault the sticky fStatus.media.error describes, and the asset it
    // describes (task 6.7c, review R18): recovery has to know which report
    // it is allowed to erase. fMutex.
    std::optional<std::pair<MediaFault, std::string>> fMediaFault;
    // Why the ON-AIR background's audio cannot be heard, when the transport
    // itself has no idea (task 6.7c, review R18). Set for the identity it
    // describes, cleared by mediaStopped(). fMutex.
    std::string fExpectedAudioLoss;

    // Guards fDesiredMask, fRevision and the fields of fStatus that the
    // render thread refreshes (frames, extent, applied mask, revision).
    mutable std::mutex fMutex;
    uint32_t fDesiredMask = 0;
    uint64_t fRevision = 0;
    // Destination: desired is written by selectMonitor (any thread),
    // applied by the render thread; the display snapshot is refreshed
    // there too so GLFW is only ever queried from the render thread.
    int fDesiredMonitor = -1;
    int fAppliedMonitor = -1;
    std::vector<DisplayInfo> fDisplays;
    Status fStatus;

    // Scene store (task 2.4b). Prepared scenes live here keyed by a
    // monotonic revision counter; desired is written by presentScene
    // (RPC thread) and applied by the render thread. All guarded by
    // fMutex so status() can read them alongside the rest.
    std::map<uint64_t, openshow::scene::Scene> fScenes;  // fMutex
    uint64_t fNextSceneRevision = 1;                     // fMutex (0 never handed out)
    uint64_t fDesiredScene = 0;                          // fMutex; 0 = nothing presented
    SceneTransitionKind fDesiredTransition = SceneTransitionKind::kFade;  // fMutex (task 3.2)
    uint64_t fAppliedScene = 0;                          // render thread, published under fMutex
    // Snapshot capture cache (render thread, run() only): store entries are
    // immutable per id and revisions are never reused, so the content copy
    // a snapshot pins can be shared across every capture of one scene id.
    uint64_t fCapturedSceneId = 0;
    ActiveScene fCapturedScene;

    // Preview pipeline (task 1.4). Desired state is set from the RPC
    // thread and a generation counter drives creation/retirement on the
    // render thread. The stream is shared_ptr'd because the RPC thread
    // may hold it while answering a release exactly as the render thread
    // retires it: destruction then defers to the last reference.
    struct PreviewDesired {
        bool enabled = false;
        uint32_t width = 640;
        uint32_t height = 360;
        uint32_t fps = 15;
        std::string name;  // shm object name when enabled
        uint64_t generation = 0;
    };
    PreviewDesired fDesiredPreview;            // fMutex
    uint32_t fPreviewRequestCounter = 0;       // fMutex, names the segments
    uint64_t fAppliedPreviewGeneration = 0;    // render thread
    std::shared_ptr<PreviewStream> fPreviewStream;  // fMutex
    sk_sp<SkSurface> fPreviewSurface;          // render thread only
    std::optional<PreviewReady> fPendingReady[protocol::kPreviewSlotCount];  // fMutex
    std::chrono::steady_clock::time_point fLastPreview;  // render thread
    // Content gate (task 1.5e): the preview republishes only when the
    // presented frame changed since the last publish.
    unsigned long long fPreviewPublishedFrames = 0;  // render thread
};

}  // namespace openshow::worker
