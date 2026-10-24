/*
 * OutputSession: the worker's runtime seam (PLAN.md "Worker architecture
 * graduation"), carved out of task 1.1's runRenderTest loop.
 *
 * The session owns the output window, Vulkan device, Skia Ganesh context,
 * swapchain and the demo scene, and composites frames from the thread that
 * calls open()/run(). This is the object RPC commands act on: handler
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
#include "render/Layer.h"
#include "render/TestScene.h"

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
    // the store revision prepareScene returns; active reports the
    // revision the render thread has taken on (0 = nothing presented,
    // the black start), pending the presentation in flight. Revision 0
    // is never handed out, and once status().revision reaches the
    // revision a scene.present was acknowledged with, active equals
    // what that command named.
    struct SceneStatus {
        uint64_t active = 0;
        uint64_t pending = 0;
        uint64_t prepared = 0;  // how many scenes the store holds
    };

    // Snapshot of output state, safe to take from any thread. layerMask
    // reports what has actually been applied on the render thread, so a
    // caller can wait for status().revision >= the revision its command
    // was acknowledged with.
    struct Status {
        uint32_t layerMask = 0;
        uint64_t revision = 0;
        unsigned long long frames = 0;
        int width = 0;
        int height = 0;
        std::string deviceName;
        // Where the output currently is: -1 windowed, >=0 that display's
        // index, with its name empty when windowed.
        int monitor = -1;
        std::string monitorName;
        PreviewStatus preview;
        SceneStatus scene;
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

    // The consumer finished copying slot@seq; the slot becomes claimable
    // again. Releases for an unknown slot or stale sequence are ignored.
    void releasePreviewSlot(uint32_t slot, uint64_t seq);

    // Pops one queued preview.ready notification (RPC thread). At most
    // one is queued per slot: if the RPC side lags, the newest frame for
    // that slot is all that matters.
    bool takePreviewReady(PreviewReady* out);

    // --- scenes (task 2.4b) ---

    // How many prepared scenes the store keeps: the oldest revision that
    // is neither presented nor pending is evicted beyond this.
    static constexpr std::size_t kMaxPreparedScenes = 16;

    // Keeps a compiled scene and reports the store revision that
    // presentScene accepts. Preparing changes nothing on screen, so no
    // output revision moves and no frame is provoked; the count visible
    // in status().scene.prepared does.
    bool prepareScene(const openshow::scene::Scene& scene, uint64_t* revisionOut);

    // Names a prepared revision as the output's scene. The switch lands
    // on the render thread's next frame, like layer toggles, and
    // *revisionOut reports the output revision it is included in;
    // presenting the revision already desired is a success without a
    // new revision. Unknown revisions (and 0) are refused — to go black
    // the frontend prepares the cleared scene, which is an ordinary
    // scene. The applied scene is what the production layers draw
    // (task 3.1): the background fills its colour and the words layer
    // lays out its text; with nothing presented the demo picture renders.
    bool presentScene(uint64_t prepared, uint64_t* revisionOut);

    // --- video (task 1.5e) ---

    // The seam a bench (later: the media layer) uses to present GPU slots:
    // the session decides *when* a slot is drawn (against the video clock),
    // the presenter owns *what* the slot is and its GPU lifetime.
    class VideoPresenter {
    public:
        virtual ~VideoPresenter() = default;
        // Render thread: the image to sample for this token. Wrapping
        // belongs here — every Skia call stays on the render thread.
        // Returning null drops the submission.
        virtual SkImage* acquireImage(unsigned token) = 0;
        // Render thread, once the frame has been submitted and any preview
        // readback completed: doneVk (freshly created here, ownership now
        // the presenter's) signals when the GPU finished sampling.
        virtual void onPresented(unsigned token, VkSemaphore doneVk) = 0;
        // Render thread: the submission was replaced before ever being drawn.
        virtual void onDropped(unsigned token) = 0;
    };

    struct VideoSubmission {
        unsigned token = 0;       // presenter-side slot identity
        double ptsSeconds = 0.0;  // in video-clock seconds
        uint64_t sequence = 0;
    };

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
    bool createSwapchain();
    void frame();
    void housekeep();
    void applyPendingLayerChanges();
    void applyPendingDisplayChange();
    void applyPendingSceneChange();
    void applyPreviewState();
    void publishPreviewFrame(double elapsed, SkImage* video);
    void refreshDisplaySnapshot();

    // Wakes the thread blocked in run()'s glfwWaitEventsTimeout. Safe from
    // any thread (GLFW owns the socket this needs; posting an empty event
    // is its portable equivalent of writing an eventfd).
    void wakeRenderThread();

    // Seconds against the installed video clock; caller holds fMutex.
    double videoClockSecondsLocked(std::chrono::steady_clock::time_point now) const;

    std::unique_ptr<OutputWindow> fWindow;
    std::unique_ptr<VulkanContext> fVk;
    sk_sp<GrDirectContext> fContext;
    std::unique_ptr<Swapchain> fSwapchain;
    std::unique_ptr<TestScene> fScene;

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
    VideoPresenter* fVideoPresenter = nullptr;             // set before run()
    std::function<double()> fVideoClock;                   // fMutex
    std::optional<VideoSubmission> fPendingVideo;          // fMutex
    std::optional<VideoSubmission> fDrawnVideo;            // render thread
    std::chrono::steady_clock::time_point fRunStart;       // render thread

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
    uint64_t fAppliedScene = 0;                          // render thread, published under fMutex

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
