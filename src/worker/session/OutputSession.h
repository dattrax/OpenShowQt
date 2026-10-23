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
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "include/core/SkRefCnt.h"
#include "include/core/SkSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"

#include "openshow/protocol/preview_wire.h"

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

    Status status() const;
    void requestStop();
    bool running() const;

private:
    bool createSwapchain();
    void frame();
    void applyPendingLayerChanges();
    void applyPendingDisplayChange();
    void applyPreviewState();
    void publishPreviewFrame(double elapsed);
    void refreshDisplaySnapshot();

    std::unique_ptr<OutputWindow> fWindow;
    std::unique_ptr<VulkanContext> fVk;
    sk_sp<GrDirectContext> fContext;
    std::unique_ptr<Swapchain> fSwapchain;
    std::unique_ptr<TestScene> fScene;

    std::atomic<bool> fStop{false};
    bool fDiagnostics = false;
    unsigned long long fFrames = 0;
    uint32_t fAppliedMask = 0;
    std::chrono::steady_clock::time_point fStartTime;

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
};

}  // namespace openshow::worker
