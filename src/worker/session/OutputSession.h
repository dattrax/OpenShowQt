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
#include <string>
#include <string_view>
#include <vector>

#include "include/core/SkRefCnt.h"
#include "include/gpu/ganesh/GrDirectContext.h"

#include "output/OutputWindow.h"
#include "output/Swapchain.h"
#include "output/VulkanContext.h"
#include "render/Layer.h"
#include "render/TestScene.h"

namespace openshow::worker {

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

    Status status() const;
    void requestStop();
    bool running() const;

private:
    bool createSwapchain();
    void frame();
    void applyPendingLayerChanges();
    void applyPendingDisplayChange();
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
};

}  // namespace openshow::worker
