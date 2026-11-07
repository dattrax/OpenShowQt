/*
 * The worker's live NDI source for the multimedia layer (ND.6).
 *
 * One hub owns the helper receiver, the newest UYVY frame, its NV12
 * conversion and the GPU uploader. It is deliberately not a video pipeline:
 * a live source has no clock, no seek and no audio, and newest-wins is the
 * whole pacing rule. The compositor therefore takes either this image or a
 * PlaybackPipeline's image for the one background layer, never both.
 *
 * Opening a source is BLOCKING — the helper answers `open` only once the
 * first frame has arrived (up to its 15 s timeout). The render thread must
 * never wait for that, so the hub starts on its own thread: beginStart()
 * returns immediately and pollStart() reports when pump() is safe. A hub is
 * single-use — cancel() it and create another for a different source.
 *
 * pump()/image()/presented() run on the render thread. The helper receiver
 * publishes into POSIX shared memory on its own thread, so reading the newest
 * frame never blocks the frame loop.
 */

#pragma once

#include <vulkan/vulkan.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "live/helper_backend.h"
#include "live/live_source.h"
#include "live/uyvy_to_nv12.h"
#include "media/FrameRing.h"
#include "render/YuvGpu.h"

class GrDirectContext;

namespace openshow::live {

class LiveSourceHub : public std::enable_shared_from_this<LiveSourceHub> {
public:
    // The Vulkan handles the uploader needs; the session's gpuHandles() fits
    // without either side owning the other's types.
    struct Gpu {
        VkDevice device = VK_NULL_HANDLE;
        VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
        uint32_t graphicsQueueIndex = 0;
        uint32_t uploadQueueIndex = 0;
        VkQueue uploadQueue = VK_NULL_HANDLE;
        GrDirectContext* context = nullptr;
    };

    // What pollStart() reports. kReady means the uploader exists and pump()
    // may be called; kFailed means the open failed and the reason was handed
    // back once (the hub is idle afterwards).
    enum class StartState { kIdle, kStarting, kReady, kFailed };

    LiveSourceHub() = default;
    ~LiveSourceHub();
    LiveSourceHub(const LiveSourceHub&) = delete;
    LiveSourceHub& operator=(const LiveSourceHub&) = delete;

    // ND.6: non-blocking start. Spawns the blocking helper launch/open on its
    // own thread and returns as soon as the thread is running. False means a
    // start is already in flight on this hub (single-use object).
    bool beginStart(const std::string& source, const Gpu& gpu,
                    HelperOptions options = {});
    // Render thread: polls the start thread. kStarting until it finishes;
    // kFailed consumes the reason into *error and leaves the hub idle.
    StartState pollStart(std::string* error);

    // Blocking start, for tests and callers that own the thread and can wait.
    bool start(const std::string& source, const Gpu& gpu, std::string* error,
               HelperOptions options = {});
    // Cancels a start in flight and tears the hub down when no thread owns it.
    // A start thread that is still blocked in open() finishes on its own and
    // tears itself down when it sees the cancel.
    void cancel();
    // Blocking teardown: joins a start in flight, then releases everything.
    // The session's close() uses this so the Vulkan device outlives its users.
    void stop();
    bool active() const { return fActive.load(std::memory_order_acquire); }
    const std::string& source() const { return fSource; }

    // Render thread: reads the newest frame, converts and uploads it, and
    // returns the slot to draw (-1 when nothing has ever been drawn). A lost
    // source sets active() false and explains itself in `error`.
    int pump(std::string* error);
    SkImage* image(int slot, std::string* error);
    // The draw that sampled `slot` has been submitted; register its
    // completion before the slot may be overwritten.
    void presented(int slot, VkSemaphore done);

private:
    bool startImpl(const std::string& source, const Gpu& gpu,
                   const HelperOptions& options, std::string* error);
    void teardown();

    std::unique_ptr<HelperBackend> fBackend;
    openshow::worker::GpuYuvUploader fUploader;
    Frame fFrame;
    Nv12Frame fNv12;
    OpenInfo fInfo;
    std::string fSource;
    std::uint64_t fLastSequence = 0;
    int fDrawnSlot = -1;
    int fPendingSlot = -1;

    std::atomic<bool> fActive{false};
    // Set when the owner no longer wants this source. The start thread checks
    // it after each blocking step and tears itself down rather than publishing
    // a receiver nobody will read.
    std::atomic<bool> fCancelStart{false};
    std::atomic<StartState> fStartState{StartState::kIdle};
    mutable std::mutex fStartMutex;
    std::string fStartError;
    std::thread fStartThread;
};

}  // namespace openshow::live
