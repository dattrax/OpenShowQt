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

#include <GLFW/glfw3.h>
#include <vulkan/vulkan.h>

#include "include/core/SkColor.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/vk/GrVkDirectContext.h"

#include "output/OutputWindow.h"
#include "output/Swapchain.h"
#include "output/VulkanContext.h"
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

void OutputSession::frame() {
    applyPendingLayerChanges();
    applyPendingDisplayChange();
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
