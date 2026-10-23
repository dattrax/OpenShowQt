/*
 * OutputSession implementation: the task 1.1 render loop with its window,
 * device, context and swapchain ownership moved in from main.cpp unchanged
 * in behaviour — resize handling, the periodic extentOutOfDate() recreate,
 * the FIFO present path and the frame-30 readback probe (now behind
 * Options::diagnostics). Layer enable changes from other threads are
 * applied at the top of each frame before anything is recorded.
 */

#include "session/OutputSession.h"

#include <cstdio>
#include <utility>
#include <vector>

#include <unistd.h>

#include <GLFW/glfw3.h>
#include <vulkan/vulkan.h>

#include "include/core/SkColor.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/vk/GrVkDirectContext.h"

#include "output/OutputWindow.h"
#include "output/Swapchain.h"
#include "output/VulkanContext.h"
#include "preview/PreviewStream.h"
#include "render/TestScene.h"

namespace openshow::worker {

namespace {

constexpr uint32_t layerMaskOf(const TestScene& scene) {
    uint32_t mask = 0;
    for (uint32_t i = 0; i < static_cast<uint32_t>(LayerId::kCount); ++i) {
        const LayerId id = static_cast<LayerId>(i);
        if (scene.layerEnabled(id)) {
            mask |= layerBit(id);
        }
    }
    return mask;
}

}  // namespace

OutputSession::~OutputSession() {
    close();
}

bool OutputSession::open(const Options& options, std::string* error) {
    // Resolve the auto destination while GLFW has no window yet, so the
    // window maps straight onto its final panel (map-then-fullscreen, once).
    int monitor = options.monitor;
    if (monitor == kAutoSecondDisplay) {
        if (OutputWindow::initGlfw()) {
            int monitorCount = 0;
            glfwGetMonitors(&monitorCount);
            monitor = monitorCount >= 2 ? 1 : -1;
        } else {
            monitor = -1;
        }
    }

    fWindow = std::make_unique<OutputWindow>();
    OutputWindow::Options windowOptions;
    windowOptions.title = options.title;
    windowOptions.monitor = monitor;
    if (!fWindow->open(windowOptions)) {
        if (error) {
            *error = "could not open the output window (is a Wayland/X11 session "
                     "available?)";
        }
        fWindow.reset();
        return false;
    }

    fVk = std::make_unique<VulkanContext>();
    if (!fVk->create(fWindow->handle())) {
        if (error) {
            *error = "could not create the Vulkan device";
        }
        close();
        return false;
    }

    fContext = GrDirectContexts::MakeVulkan(fVk->backendContext());
    if (!fContext) {
        if (error) {
            *error = "GrDirectContexts::MakeVulkan failed";
        }
        close();
        return false;
    }
    fContext->setResourceCacheLimit(64 * 1024 * 1024);

    fScene = std::make_unique<TestScene>();
    if (!fScene->create(error)) {
        close();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(fMutex);
        fDesiredMask = layerMaskOf(*fScene);
        fAppliedMask = fDesiredMask;
        fStatus.layerMask = fDesiredMask;
        fStatus.revision = fRevision;
        fStatus.deviceName = fVk->deviceName();
        fDesiredMonitor = monitor;
        fAppliedMonitor = monitor;
        fStatus.monitor = monitor;
    }
    refreshDisplaySnapshot();
    if (monitor >= 0) {
        std::lock_guard<std::mutex> lock(fMutex);
        if (monitor < static_cast<int>(fDisplays.size())) {
            fStatus.monitorName = fDisplays[monitor].name;
        }
    }
    fDiagnostics = options.diagnostics;

    fSwapchain = std::make_unique<Swapchain>();
    if (!createSwapchain()) {
        if (error) {
            *error = "could not create the swapchain";
        }
        close();
        return false;
    }

    fStartTime = std::chrono::steady_clock::now();
    fStop = false;
    return true;
}

void OutputSession::close() {
    if (fSwapchain) {
        fSwapchain->destroy();
        fSwapchain.reset();
    }
    fPreviewSurface.reset();
    {
        std::lock_guard<std::mutex> lock(fMutex);
        fPreviewStream.reset();  // unlinks the preview segment (unless a release call still holds it)
    }
    fContext.reset();
    if (fVk) {
        fVk->destroy();
        fVk.reset();
    }
    fScene.reset();
    if (fWindow) {
        fWindow->close();
        fWindow.reset();
    }
}

bool OutputSession::createSwapchain() {
    Swapchain::CreateInfo info;
    info.device = fVk->device();
    info.physicalDevice = fVk->physicalDevice();
    info.surface = fVk->surface();
    info.queue = fVk->queue();
    info.presentQueueIndex = fVk->queueIndex();
    info.context = fContext.get();
    // Provisional size for the frame(s) before the compositor configures
    // the Wayland surface; the resize and extentOutOfDate() paths pick up
    // the real extent once configure lands.
    fWindow->framebufferSize(&info.width, &info.height);
    return fSwapchain->create(info);
}

bool OutputSession::setLayerEnabled(std::string_view name, bool enabled,
                                    uint64_t* revisionOut) {
    LayerId id;
    if (!layerIdFromName(name, &id)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(fMutex);
    if (enabled) {
        fDesiredMask |= layerBit(id);
    } else {
        fDesiredMask &= ~layerBit(id);
    }
    ++fRevision;
    if (revisionOut) {
        *revisionOut = fRevision;
    }
    return true;
}

bool OutputSession::selectMonitor(int index, uint64_t* revisionOut) {
    // -1 is the windowed destination and always valid; a panel index must
    // exist in the snapshot (which GLFW refreshed on the render thread).
    if (index < -1 || index == kAutoSecondDisplay) {
        return false;
    }
    std::lock_guard<std::mutex> lock(fMutex);
    if (index >= static_cast<int>(fDisplays.size())) {
        return false;
    }
    if (index != fDesiredMonitor) {
        fDesiredMonitor = index;
        ++fRevision;
    }
    if (revisionOut) {
        *revisionOut = fRevision;
    }
    return true;
}

std::vector<OutputSession::DisplayInfo> OutputSession::displays() const {
    std::lock_guard<std::mutex> lock(fMutex);
    return fDisplays;
}

bool OutputSession::startPreview(uint32_t width, uint32_t height, uint32_t fps,
                                 PreviewInfo* info, std::string* error) {
    // Geometry limits: a preview, not a second output.
    if (width < 64 || width > 1920 || height < 48 || height > 1080 || fps < 1 ||
        fps > 60) {
        if (error) {
            *error = "preview wants width 64..1920, height 48..1080, fps 1..60";
        }
        return false;
    }

    std::lock_guard<std::mutex> lock(fMutex);
    ++fPreviewRequestCounter;
    PreviewDesired desired;
    desired.enabled = true;
    desired.width = width;
    desired.height = height;
    desired.fps = fps;
    desired.name = "/openshow-preview-" + std::to_string(static_cast<long>(::getpid())) +
                   "-" + std::to_string(1000u + fPreviewRequestCounter);
    desired.generation = fDesiredPreview.generation + 1;
    fDesiredPreview = std::move(desired);

    // Fresh counters for the new stream; running flips true when the
    // render thread has actually built the segment and surface.
    fStatus.preview = PreviewStatus{};
    fStatus.preview.name = fDesiredPreview.name;
    fStatus.preview.width = width;
    fStatus.preview.height = height;
    fStatus.preview.fps = fps;
    for (auto& pending : fPendingReady) {
        pending.reset();  // slots belong to the stream being replaced
    }

    if (info) {
        info->name = fDesiredPreview.name;
        info->width = width;
        info->height = height;
        info->slotCount = protocol::kPreviewSlotCount;
        info->bytesPerSlot =
            static_cast<uint32_t>(protocol::previewSlotStrideBytes(width, height));
    }
    return true;
}

void OutputSession::stopPreview() {
    std::lock_guard<std::mutex> lock(fMutex);
    if (!fDesiredPreview.enabled) {
        return;
    }
    fDesiredPreview.enabled = false;
    ++fDesiredPreview.generation;
    for (auto& pending : fPendingReady) {
        pending.reset();
    }
}

void OutputSession::releasePreviewSlot(uint32_t slot, uint64_t seq) {
    std::shared_ptr<PreviewStream> stream;
    {
        std::lock_guard<std::mutex> lock(fMutex);
        stream = fPreviewStream;
    }
    if (stream) {
        stream->release(slot, seq);
    }
}

bool OutputSession::takePreviewReady(PreviewReady* out) {
    std::lock_guard<std::mutex> lock(fMutex);
    for (auto& pending : fPendingReady) {
        if (pending) {
            if (out) {
                *out = *pending;
            }
            pending.reset();
            return true;
        }
    }
    return false;
}

void OutputSession::refreshDisplaySnapshot() {
    int count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    std::vector<DisplayInfo> snapshot;
    if (monitors) {
        for (int i = 0; i < count; ++i) {
            DisplayInfo info;
            info.index = i;
            const char* name = glfwGetMonitorName(monitors[i]);
            info.name = name ? name : "monitor";
            const GLFWvidmode* mode = glfwGetVideoMode(monitors[i]);
            if (mode) {
                info.width = mode->width;
                info.height = mode->height;
                info.refreshHz = mode->refreshRate;
            }
            snapshot.push_back(std::move(info));
        }
    }
    std::lock_guard<std::mutex> lock(fMutex);
    fDisplays = std::move(snapshot);
}

OutputSession::Status OutputSession::status() const {
    std::lock_guard<std::mutex> lock(fMutex);
    return fStatus;
}

void OutputSession::requestStop() {
    fStop = true;
}

bool OutputSession::running() const {
    return !fStop && fWindow && !fWindow->shouldClose();
}

void OutputSession::applyPendingLayerChanges() {
    uint32_t desired = 0;
    {
        std::lock_guard<std::mutex> lock(fMutex);
        desired = fDesiredMask;
    }
    if (desired == fAppliedMask) {
        return;
    }
    for (uint32_t i = 0; i < static_cast<uint32_t>(LayerId::kCount); ++i) {
        const LayerId id = static_cast<LayerId>(i);
        const bool want = (desired & layerBit(id)) != 0;
        if (want != fScene->layerEnabled(id)) {
            fScene->setLayerEnabled(id, want);
        }
    }
    fAppliedMask = desired;
    std::lock_guard<std::mutex> lock(fMutex);
    fStatus.layerMask = fAppliedMask;
    fStatus.revision = fRevision;
}

void OutputSession::applyPendingDisplayChange() {
    int desired = fAppliedMonitor;
    {
        std::lock_guard<std::mutex> lock(fMutex);
        desired = fDesiredMonitor;
    }
    if (desired == fAppliedMonitor) {
        return;
    }
    if (!fWindow->setMonitor(desired, 300)) {
        // The index vanished between validation and apply (hotplug): drop
        // the request rather than retry a doomed one every frame.
        std::lock_guard<std::mutex> lock(fMutex);
        fDesiredMonitor = fAppliedMonitor;
        return;
    }
    fAppliedMonitor = desired;
    refreshDisplaySnapshot();

    // Rebuild the swapchain at the new panel; if the compositor's configure
    // is still lagging, the periodic extentOutOfDate() check recreates
    // again at the real extent, same as at open().
    fSwapchain->destroy();
    if (!createSwapchain()) {
        return;  // the resize path retries on the next frame
    }
    fWindow->resetFramebufferResize();  // setMonitor's resize is done here

    std::lock_guard<std::mutex> lock(fMutex);
    fStatus.monitor = fAppliedMonitor;
    fStatus.monitorName = fAppliedMonitor >= 0 &&
                                 fAppliedMonitor < static_cast<int>(fDisplays.size())
                             ? fDisplays[fAppliedMonitor].name
                             : "";
    fStatus.revision = fRevision;
}

void OutputSession::applyPreviewState() {
    PreviewDesired desired;
    std::shared_ptr<PreviewStream> retire;
    {
        std::lock_guard<std::mutex> lock(fMutex);
        if (fDesiredPreview.generation == fAppliedPreviewGeneration) {
            return;
        }
        desired = fDesiredPreview;
        retire = std::move(fPreviewStream);
        fAppliedPreviewGeneration = desired.generation;
    }
    // Outside the lock: an RPC thread answering a release may still hold a
    // reference, and the unlink then happens on its last drop.
    retire.reset();
    fPreviewSurface.reset();

    if (!desired.enabled) {
        std::lock_guard<std::mutex> lock(fMutex);
        fStatus.preview = PreviewStatus{};
        return;
    }

    auto stream = std::make_shared<PreviewStream>();
    std::string error;
    const bool ok = stream->create(desired.name, desired.width, desired.height, &error);
    std::lock_guard<std::mutex> lock(fMutex);
    fStatus.preview.running = ok;
    fStatus.preview.name = desired.name;
    fStatus.preview.width = desired.width;
    fStatus.preview.height = desired.height;
    fStatus.preview.fps = desired.fps;
    fStatus.preview.error = ok ? std::string() : error;
    if (ok) {
        fPreviewStream = std::move(stream);
        fLastPreview = std::chrono::steady_clock::time_point{};  // tick immediately
    }
}

void OutputSession::publishPreviewFrame(double elapsed) {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps = 0;
    uint32_t revision = 0;
    std::shared_ptr<PreviewStream> stream;
    {
        std::lock_guard<std::mutex> lock(fMutex);
        if (!fStatus.preview.running) {
            return;
        }
        width = fStatus.preview.width;
        height = fStatus.preview.height;
        fps = fStatus.preview.fps;
        stream = fPreviewStream;
        revision = static_cast<uint32_t>(fRevision);
    }
    if (!stream) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now - fLastPreview < std::chrono::duration<double>(1.0 / fps)) {
        return;
    }
    fLastPreview = now;

    const int slot = stream->claim();
    if (slot < 0) {
        // Every slot is still owned by the consumer: drop the preview
        // frame, never block the output for it (PLAN.md "Preview").
        std::lock_guard<std::mutex> lock(fMutex);
        ++fStatus.preview.dropped;
        return;
    }

    if (!fPreviewSurface || fPreviewSurface->width() != static_cast<int>(width) ||
        fPreviewSurface->height() != static_cast<int>(height)) {
        const SkImageInfo info =
            SkImageInfo::Make(static_cast<int>(width), static_cast<int>(height),
                              kRGBA_8888_SkColorType, kPremul_SkAlphaType);
        fPreviewSurface =
            SkSurfaces::RenderTarget(fContext.get(), skgpu::Budgeted::kNo, info);
        if (!fPreviewSurface) {
            stream->abort(slot);
            std::lock_guard<std::mutex> lock(fMutex);
            ++fStatus.preview.dropped;
            fStatus.preview.running = false;
            fStatus.preview.error = "could not create the preview surface";
            return;
        }
    }

    // The same scene through the same draw path as the audience output,
    // only smaller; the diagnostics status line stays empty so it does
    // not crowd the preview.
    fScene->draw(fPreviewSurface->getCanvas(), static_cast<int>(width),
                 static_cast<int>(height), elapsed, std::string());

    const SkImageInfo dst = SkImageInfo::Make(static_cast<int>(width),
                                              static_cast<int>(height),
                                              kRGBA_8888_SkColorType, kPremul_SkAlphaType);
    if (!fPreviewSurface->readPixels(dst, stream->slotPixels(slot), stream->rowBytes(), 0,
                                     0)) {
        stream->abort(slot);
        std::lock_guard<std::mutex> lock(fMutex);
        ++fStatus.preview.dropped;
        fStatus.preview.error = "preview readback failed";
        return;
    }

    const uint64_t publishedNs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch())
            .count());
    const uint64_t seq = stream->publish(slot, revision, publishedNs);

    PreviewReady ready;
    ready.slot = static_cast<uint32_t>(slot);
    ready.seq = seq;
    ready.width = width;
    ready.height = height;
    ready.rowBytes = stream->rowBytes();
    ready.revision = revision;
    ready.publishedNs = publishedNs;
    std::lock_guard<std::mutex> lock(fMutex);
    fPendingReady[slot] = ready;
    ++fStatus.preview.published;
}

void OutputSession::frame() {
    applyPendingLayerChanges();
    applyPendingDisplayChange();
    applyPreviewState();
    glfwPollEvents();

    int width = 0;
    int height = 0;
    const bool sizeChanged = fWindow->framebufferResized();
    if (fWindow->framebufferSize(&width, &height) &&
        (sizeChanged || width != fSwapchain->width() || height != fSwapchain->height())) {
        fSwapchain->destroy();
        if (!createSwapchain()) {
            return;  // retried next frame; the loop never blocks here
        }
        fWindow->resetFramebufferResize();
    }

    uint32_t imageIndex = 0;
    VkResult result = VK_SUCCESS;
    if (!fSwapchain->acquire(&imageIndex, &result)) {
        if (result == VK_TIMEOUT) {
            return;
        }
        fSwapchain->destroy();
        createSwapchain();
        return;
    }

    SkSurface* surface = fSwapchain->surface(imageIndex);
    if (surface) {
        char status[160];
        std::snprintf(status, sizeof(status), "live: %s \xc2\xb7 %dx%d \xc2\xb7 frame %llu",
                      fVk->deviceName().c_str(), fSwapchain->width(), fSwapchain->height(),
                      fFrames);
        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - fStartTime)
                .count();
        fScene->draw(surface->getCanvas(), fSwapchain->width(), fSwapchain->height(), elapsed,
                     status);
        ++fFrames;

        // Diagnostic (task 1.1): read the wrapped GPU image back on the CPU
        // once, to separate "Skia never wrote the image" from "present shows
        // the wrong thing".
        if (fDiagnostics && fFrames == 30) {
            const SkImageInfo info =
                SkImageInfo::MakeN32Premul(fSwapchain->width(), fSwapchain->height());
            std::vector<SkColor> pixels(size_t(info.width()) * size_t(info.height()));
            if (surface->readPixels(info, pixels.data(), info.minRowBytes(), 0, 0)) {
                size_t nonBlack = 0;
                SkColor firstBright = 0;
                for (SkColor c : pixels) {
                    if (c != SK_ColorBLACK) {
                        ++nonBlack;
                        if (!firstBright) {
                            firstBright = c;
                        }
                    }
                }
                std::fprintf(stderr,
                             "openshow-worker: readback frame 30: %zu/%zu non-black"
                             " (first bright %08x)\n",
                             nonBlack, pixels.size(), firstBright);
            } else {
                std::fprintf(stderr, "openshow-worker: readback at frame 30 failed\n");
            }
        }

        if (!fSwapchain->flushAndPresent(surface, imageIndex, &result)) {
            fSwapchain->destroy();
            createSwapchain();
            return;
        }
    }

    // Preview (task 1.4) runs after the audience frame is presented: the
    // extra pass never delays the swapchain work it follows, and its
    // throttling and drop policy live entirely in publishPreviewFrame.
    if (fFrames > 0) {
        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - fStartTime)
                .count();
        publishPreviewFrame(elapsed);
    }

    // Wayland may configure the surface after the swapchain was created
    // (or the swapchain fell back to the provisional size), and present
    // does not always report that. Check the surface extent periodically
    // and recreate at the real panel size when it differs.
    if (fFrames % 30 == 0) {
        int surfaceWidth = 0;
        int surfaceHeight = 0;
        if (fSwapchain->extentOutOfDate(&surfaceWidth, &surfaceHeight)) {
            std::fprintf(stderr,
                         "openshow-worker: compositor configured %dx%d, recreating "
                         "swapchain from %dx%d\n",
                         surfaceWidth, surfaceHeight, fSwapchain->width(),
                         fSwapchain->height());
            fSwapchain->destroy();
            createSwapchain();
        }
    }

    // Cheap re-enumeration so a hotplug reaches output.displays without
    // GLFW monitor callbacks (the dedicated hotplug story is M6.1).
    if (fFrames % 60 == 0) {
        refreshDisplaySnapshot();
    }

    {
        std::lock_guard<std::mutex> lock(fMutex);
        fStatus.frames = fFrames;
        fStatus.width = fSwapchain->width();
        fStatus.height = fSwapchain->height();
    }
}

unsigned long long OutputSession::run(double seconds, const std::atomic<bool>* abort) {
    const auto startTime = std::chrono::steady_clock::now();
    while (running() && !(abort && *abort)) {
        frame();
        if (seconds > 0.0) {
            const double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime)
                    .count();
            if (elapsed >= seconds) {
                break;
            }
        }
    }
    return fFrames;
}

}  // namespace openshow::worker
