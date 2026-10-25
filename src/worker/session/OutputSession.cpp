/*
 * OutputSession implementation: the task 1.1 render loop with its window,
 * device, context and swapchain ownership moved in from main.cpp unchanged
 * in behaviour — resize handling, the periodic extentOutOfDate() recreate,
 * the FIFO present path and the frame-30 readback probe (now behind
 * Options::diagnostics). Layer enable changes from other threads are
 * applied at the top of each frame before anything is recorded.
 */

#include "session/OutputSession.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>
#include <vector>

#include <unistd.h>

#include <GLFW/glfw3.h>
#include <vulkan/vulkan.h>

#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/vk/GrVkDirectContext.h"

#include "output/OutputWindow.h"
#include "output/Swapchain.h"
#include "output/VulkanContext.h"
#include "output/VulkanQueueGuard.h"
#include "media/MediaProbe.h"
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
    if (!fVk->create(fWindow->handle(), options.vulkanValidation)) {
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
    fBackgroundMedia = options.backgroundMedia;

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
    // The background transport decodes into frame slots on this device
    // and context, so it goes before every GPU resource it references
    // (task 5.5). fAudio.stop() is idempotent: a never-started player
    // tears down as cleanly as a running one.
    teardownBackgroundTransport();
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
    int logicalWidth = 0;
    int logicalHeight = 0;
    glfwGetWindowSize(fWindow->handle(), &logicalWidth, &logicalHeight);
    std::fprintf(stderr, "openshow-worker: window %dx%d logical, framebuffer %dx%d\n",
                 logicalWidth, logicalHeight, info.width, info.height);
    return fSwapchain->create(info);
}

bool OutputSession::setLayerEnabled(std::string_view name, bool enabled,
                                    uint64_t* revisionOut) {
    LayerId id;
    if (!layerIdFromName(name, &id)) {
        return false;
    }
    {
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
    }
    // The event-driven loop (task 1.5e) sleeps between frames: nudge it so
    // the change lands on the next vsync rather than never.
    wakeRenderThread();
    return true;
}

bool OutputSession::selectMonitor(int index, uint64_t* revisionOut) {
    // -1 is the windowed destination and always valid; a panel index must
    // exist in the snapshot (which GLFW refreshed on the render thread).
    if (index < -1 || index == kAutoSecondDisplay) {
        return false;
    }
    {
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
    }
    wakeRenderThread();
    return true;
}

std::vector<OutputSession::DisplayInfo> OutputSession::displays() const {
    std::lock_guard<std::mutex> lock(fMutex);
    return fDisplays;
}

bool OutputSession::prepareScene(const openshow::scene::Scene& scene,
                                 uint64_t* revisionOut) {
    {
        std::lock_guard<std::mutex> lock(fMutex);
        const uint64_t revision = fNextSceneRevision++;
        fScenes[revision] = scene;
        // Bounded store: evict the oldest revision nobody can present
        // again — a frontend that lost the revision number has to re-prepare
        // anyway, and the audience's scene is never a victim.
        while (fScenes.size() > kMaxPreparedScenes) {
            const auto victim = std::find_if(fScenes.begin(), fScenes.end(),
                                             [&](const auto& entry) {
                                                 return entry.first != fDesiredScene &&
                                                        entry.first != fAppliedScene;
                                             });
            if (victim == fScenes.end()) {
                break;
            }
            fScenes.erase(victim);
        }
        fStatus.scene.prepared = fScenes.size();
        if (revisionOut != nullptr) {
            *revisionOut = revision;
        }
    }
    // Task 5.5 preload: outside the store lock (a decode must never hold
    // up status or layer commands) and on this RPC thread (the render
    // thread must never pay a cold-file cost at present time). Socket
    // mode only: benches do not prepare, and the dev paths stay exact.
    if (fBackgroundMedia) {
        preloadScene(scene);
    }
    return true;
}

void OutputSession::preloadScene(const openshow::scene::Scene& scene) {
    const openshow::scene::Background& background = scene.background;
    if (!background.hasImage || fScene == nullptr) {
        return;
    }
    if (!background.isVideo) {
        // Stills: decode now. The cache keeps failures too, so a bad
        // path is one attempt, not a retry on every frame.
        fScene->imageCache()->imageFor(background.assetPath);
        return;
    }
    // Videos: opening a transport belongs to the present (the audience
    // decides when it starts), but the cheap probe belongs to now — a
    // file that cannot even be read or demuxed is known while the old
    // scene is still on air, and reported (5.5d) instead of discovered
    // as a silent colour fill one frame after the change.
    openshow::media::MediaInfo info;
    std::string error;
    if (openshow::media::probeMedia(background.assetPath, &info, &error) !=
        openshow::media::ProbeStatus::kOk) {
        std::fprintf(stderr, "openshow-worker: background preload %s: %s\n",
                     background.assetPath.c_str(), error.c_str());
    }
}

bool OutputSession::presentScene(uint64_t prepared, uint64_t* revisionOut,
                                 SceneTransitionKind kind) {
    {
        std::lock_guard<std::mutex> lock(fMutex);
        if (prepared == 0 || fScenes.find(prepared) == fScenes.end()) {
            return false;
        }
        fStatus.scene.pending = prepared;
        if (prepared != fDesiredScene) {
            fDesiredScene = prepared;
            fDesiredTransition = kind;  // travels with the change to the render thread
            ++fRevision;
        }
        if (revisionOut != nullptr) {
            *revisionOut = fRevision;
        }
    }
    wakeRenderThread();
    return true;
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

    std::unique_lock<std::mutex> lock(fMutex);
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
    lock.unlock();
    wakeRenderThread();
    return true;
}

void OutputSession::stopPreview() {
    {
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
    wakeRenderThread();
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

void OutputSession::setVideoPresenter(VideoPresenter* presenter) {
    // Before run(); the render thread reads it without synchronisation
    // because it is only ever written while the loop is not running.
    fVideoPresenter = presenter;
}

void OutputSession::setVideoClock(std::function<double()> clock) {
    std::lock_guard<std::mutex> lock(fMutex);
    fVideoClock = std::move(clock);
}

void OutputSession::submitVideo(VideoSubmission submission) {
    std::optional<VideoSubmission> dropped;
    {
        std::lock_guard<std::mutex> lock(fMutex);
        if (fPendingVideo) {
            dropped = fPendingVideo;
        }
        fPendingVideo = submission;
    }
    // A slot replaced before ever being drawn goes back now, with no GPU
    // semaphore: nothing ever sampled it.
    if (dropped && fVideoPresenter && dropped->token != submission.token) {
        fVideoPresenter->onDropped(dropped->token);
    }
    wakeRenderThread();
}

OutputSession::GpuHandles OutputSession::gpuHandles() const {
    GpuHandles handles;
    if (fVk) {
        handles.device = fVk->device();
        handles.physicalDevice = fVk->physicalDevice();
        handles.graphicsQueueIndex = fVk->queueIndex();
        handles.uploadQueueIndex = fVk->uploadQueueIndex();
        handles.uploadQueue = fVk->uploadQueue();
    }
    handles.context = fContext.get();
    return handles;
}

void OutputSession::wakeRenderThread() {
    // GLFW owns the display connection, so its portable cross-thread nudge
    // (an empty event waking glfwWaitEventsTimeout) is the eventfd of this
    // design — PLAN.md's "eventfd + display fd" intent without private
    // Wayland plumbing.
    glfwPostEmptyEvent();
}

double OutputSession::videoClockSecondsLocked(
    std::chrono::steady_clock::time_point now) const {
    if (fVideoClock) {
        return fVideoClock();
    }
    return std::chrono::duration<double>(now - fRunStart).count();
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
    wakeRenderThread();
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

void OutputSession::applyPendingSceneChange() {
    uint64_t desired = 0;
    SceneTransitionKind kind = SceneTransitionKind::kFade;
    {
        std::lock_guard<std::mutex> lock(fMutex);
        desired = fDesiredScene;
        kind = fDesiredTransition;
    }
    if (desired == fAppliedScene) {
        return;
    }
    // Task 3.1: the applied scene is what the production layers draw. The
    // store may evict its entry later, so a copy shared with the
    // composition keeps the on-air scene alive as long as it is drawn.
    // Applying also records it as active for status and the acknowledged
    // revision.
    std::shared_ptr<const openshow::scene::Scene> applied;
    {
        std::lock_guard<std::mutex> lock(fMutex);
        const auto entry = fScenes.find(desired);
        if (entry != fScenes.end()) {
            applied = std::make_shared<const openshow::scene::Scene>(entry->second);
        }
        fAppliedScene = desired;
        fStatus.scene.active = fAppliedScene;
        fStatus.revision = fRevision;
    }
    // Task 5.5: the render thread keeps its own handle on the on-air scene
    // (the composition gets a copy) — the draw path and the background
    // transport both read the same thing from here.
    fRenderScene = applied;
    fScene->setActiveScene(applied, kind);
    if (fBackgroundMedia) {
        // The transport follows the scene in the same breath — and this
        // whole function runs ahead of frame()'s graphics queue lock
        // precisely because the swap's join() waits on threads that may
        // be blocked acquiring it (deadlock otherwise).
        applyBackgroundTransport(applied ? applied->background
                                         : openshow::scene::Background{});
    }
}

void OutputSession::applyBackgroundTransport(const openshow::scene::Background& wanted) {
    // The identity rule (M3 continuity): same asset id, same path, same
    // kind — the transport stays exactly as it is. A lyric change over the
    // same background video therefore disturbs nothing: decode position,
    // the audio clock and the sink all keep rolling.
    const bool sameIdentity =
        fBackground != nullptr && fAppliedBackground.hasImage == wanted.hasImage &&
        fAppliedBackground.isVideo == wanted.isVideo &&
        fAppliedBackground.assetId == wanted.assetId &&
        fAppliedBackground.assetPath == wanted.assetPath;
    if (sameIdentity) {
        return;
    }
    teardownBackgroundTransport();
    fAppliedBackground = wanted;
    if (!wanted.hasImage || !wanted.isVideo) {
        return;  // colour fill or still image: the scene draws itself, no media
    }

    std::string error;
    const GpuHandles handles = gpuHandles();
    PlaybackPipeline::Gpu gpu;
    gpu.device = handles.device;
    gpu.physicalDevice = handles.physicalDevice;
    gpu.graphicsQueueIndex = handles.graphicsQueueIndex;
    gpu.uploadQueueIndex = handles.uploadQueueIndex;
    gpu.uploadQueue = handles.uploadQueue;
    gpu.context = handles.context;

    PlaybackPipeline::Options options;
    options.path = wanted.assetPath;
    // A background loops: the lyric service expects the visual to keep
    // running (FreeShow's loop-by-default rule; a per-slide setting can
    // come with the editor UI). The 1.5i copying-compatibility reopen is
    // allowed — losing a background to a decoder gap is worse than a
    // frame-copy.
    options.loop = true;
    options.allowVideoCopy = true;
    auto pipeline = std::make_unique<PlaybackPipeline>();
    if (!pipeline->openGpu(gpu, options, &error)) {
        std::fprintf(stderr, "openshow-worker: background video %s: %s\n",
                     wanted.assetPath.c_str(), error.c_str());
        return;  // the colour fill stands; the scene stays legible
    }

    // Embedded audio, when it can be decoded, becomes the audible mix and
    // the clock video paces against (the M5 checkpoint rule); a silent
    // file plays silent on the player's monotonic clock. Either way the
    // player is up: transport commands have one owner.
    openshow::media::MediaInfo info;
    AvPlayer::Config config;
    config.loop = true;
    if (openshow::media::probeMedia(wanted.assetPath, &info, &error) ==
        openshow::media::ProbeStatus::kOk) {
        if (!info.audioStreams.empty() && info.audioStreams.front().decoderAvailable) {
            config.videoAudioPath = wanted.assetPath;
        }
    } else {
        std::fprintf(stderr, "openshow-worker: background audio probe %s: %s\n",
                     wanted.assetPath.c_str(), error.c_str());
    }
    if (!fAudio.start(config, &error)) {
        std::fprintf(stderr, "openshow-worker: background audio: %s (video continues, "
                             "paced by the monotonic clock)\n",
                     error.c_str());
    }
    fAudio.attachPipeline(pipeline.get());

    // The seam swaps before frames can flow: presenter and clock together,
    // so a submission is never paced by the previous background's clock.
    fBackground = std::move(pipeline);
    setVideoClock(fAudio.clockFn());
    fVideoPresenter = fBackground.get();
    if (!fBackground->start(
            [this](const VideoSubmission& submission) { submitVideo(submission); },
            fAudio.clockFn(), &error)) {
        std::fprintf(stderr, "openshow-worker: background decode %s: %s\n",
                     wanted.assetPath.c_str(), error.c_str());
        setVideoClock(nullptr);
        fVideoPresenter = nullptr;
        fAudio.detachPipeline(fBackground.get());
        fBackground->close();
        fBackground.reset();
        return;
    }
    fNeedsPresent = true;  // the head frame arrives soon; redraw when it does
}

void OutputSession::teardownBackgroundTransport() {
    fAudio.stop();
    if (!fBackground) {
        fAppliedBackground = openshow::scene::Background{};
        return;
    }
    // Presenter and clock down first, then the seam forgets the slots:
    // their GPU lifetime belongs to the pipeline being closed, and with no
    // presenter registered the render thread can neither acquire nor
    // release them (this all runs on the render thread, so no frame is
    // mid-draw against them).
    fVideoPresenter = nullptr;
    setVideoClock(nullptr);
    fAudio.detachPipeline(fBackground.get());
    {
        std::lock_guard<std::mutex> lock(fMutex);
        fPendingVideo.reset();
        fDrawnVideo.reset();
    }
    fBackground->requestStop();
    fBackground->join();
    fBackground->close();
    fBackground.reset();
    fAppliedBackground = openshow::scene::Background{};
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
        fPreviewPublishedFrames = 0;  // a fresh stream owes its first frame
    }
}

void OutputSession::publishPreviewFrame(double elapsed, SkImage* video) {
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

    // Content gate (task 1.5e): republish only when the presented frame
    // changed since the last publish — a slide that sits still must not
    // keep ticking preview frames.
    if (fFrames == fPreviewPublishedFrames) {
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
    // not crowd the preview. Video belongs to what the audience sees: a
    // background video (task 5.5) goes into the scene's background layer
    // through draw() itself, anything else (the 1.5e bench) stays the dev
    // overlay over the whole preview — the readPixels below is
    // CPU-synchronous, which keeps this sampling of a released slot
    // ordered ahead of any later reuse.
    const bool asSceneBackground =
        video != nullptr && fRenderScene && fRenderScene->background.isVideo;
    fScene->draw(fPreviewSurface->getCanvas(), static_cast<int>(width),
                 static_cast<int>(height), elapsed, std::string(),
                 asSceneBackground ? video : nullptr);
    if (video && !asSceneBackground) {
        fPreviewSurface->getCanvas()->drawImageRect(
            video, SkRect::MakeIWH(static_cast<int>(width), static_cast<int>(height)),
            SkSamplingOptions(SkFilterMode::kLinear));
    }

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
    fPreviewPublishedFrames = fFrames;  // the gate counts this publish

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
    std::lock_guard queueLock(graphicsQueueMutex());
    // The revision this frame represents: a command landing while the
    // frame is in flight must re-dirty the loop rather than be swallowed
    // (task 1.5e).
    uint64_t presentedFor = 0;
    {
        std::lock_guard<std::mutex> lock(fMutex);
        presentedFor = fRevision;
    }

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
    // Video hand-off state for this frame (task 1.5e).
    SkImage* videoImage = nullptr;
    unsigned videoToken = 0;
    VkSemaphore videoDone = VK_NULL_HANDLE;
    if (surface) {
        char status[160];
        std::snprintf(status, sizeof(status), "live: %s \xc2\xb7 %dx%d \xc2\xb7 frame %llu",
                      fVk->deviceName().c_str(), fSwapchain->width(), fSwapchain->height(),
                      fFrames);
        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - fStartTime)
                .count();

        // Video hand-off for this frame (tasks 1.5e, 5.5): the pending
        // slot is drawn once the clock has reached its pts; until then —
        // and on every dirty redraw between video frames — the last
        // presented frame is held, so a layer toggle in the middle of a
        // video never blanks it. The decision runs before the scene draw
        // because task 5.5 may hand the frame to the composition itself.
        std::optional<VideoSubmission> toDraw;
        {
            std::lock_guard<std::mutex> lock(fMutex);
            if (fPendingVideo && fVideoPresenter &&
                videoClockSecondsLocked(std::chrono::steady_clock::now()) >=
                    fPendingVideo->ptsSeconds) {
                toDraw = fPendingVideo;
                fDrawnVideo = toDraw;
                fPendingVideo.reset();
            }
        }
        if (!toDraw && fDrawnVideo) {
            toDraw = fDrawnVideo;
        }
        if (toDraw && fVideoPresenter) {
            videoImage = fVideoPresenter->acquireImage(toDraw->token);
            if (!videoImage) {
                fVideoPresenter->onDropped(toDraw->token);
                if (fDrawnVideo && fDrawnVideo->token == toDraw->token) {
                    fDrawnVideo.reset();
                }
            } else {
                videoToken = toDraw->token;
            }
        }

        // Where the frame belongs (task 5.5): with a video background on
        // air it is part of the composition — the background layer draws
        // it cover-fitted inside the scene, text over it. With no video
        // background the 1.5e dev placement stands unchanged: fit centred
        // inside the whole output, aspect preserved, the scene background
        // showing through as letterbox bars (real media layers bring their
        // own geometry; assumes square pixels, which the wrapped YUV slots
        // always are). NOTE: TestScene::draw leaves its output-rule
        // transform (the fit of the authored 1920x1080 canvas) on this
        // canvas — the dev video rect is in swapchain pixels, so it must
        // be drawn on the identity.
        const bool videoIsBackground =
            videoImage != nullptr && fRenderScene && fRenderScene->background.isVideo;
        fScene->draw(surface->getCanvas(), fSwapchain->width(), fSwapchain->height(), elapsed,
                     status, videoIsBackground ? videoImage : nullptr);
        ++fFrames;
        if (videoImage && !videoIsBackground) {
            surface->getCanvas()->save();
            surface->getCanvas()->resetMatrix();
            const SkISize srcSize = videoImage->dimensions();
            const float fitScale = std::min(
                static_cast<float>(fSwapchain->width()) / srcSize.width(),
                static_cast<float>(fSwapchain->height()) / srcSize.height());
            // Round to whole pixels: the scale is a float division and
            // multiplying it back leaves ~1e-5 px error on the axis that
            // decides the fit (a 972-height returning 972.00006), which
            // is noise but reads as a bug in the quad log.
            const float fitWidth = std::round(srcSize.width() * fitScale);
            const float fitHeight = std::round(srcSize.height() * fitScale);
            const SkRect fitRect = SkRect::MakeLTRB(
                (fSwapchain->width() - fitWidth) * 0.5f,
                (fSwapchain->height() - fitHeight) * 0.5f,
                (fSwapchain->width() + fitWidth) * 0.5f,
                (fSwapchain->height() + fitHeight) * 0.5f);
            if (!fLoggedVideoQuad) {
                fLoggedVideoQuad = true;
                std::fprintf(stderr,
                             "openshow-worker: video quad: image %dx%d -> (%g,%g,%g,%g) "
                             "on surface %dx%d\n",
                             srcSize.width(), srcSize.height(), fitRect.left(),
                             fitRect.top(), fitRect.right(), fitRect.bottom(),
                             surface->width(), surface->height());
            }
            surface->getCanvas()->drawImageRect(videoImage, fitRect,
                                                SkSamplingOptions(SkFilterMode::kLinear));
            surface->getCanvas()->restore();
        }

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

        // Every draw that samples video attaches a fresh done-semaphore to
        // its flush: the release protocol gives the uploader one semaphore
        // per sampling flush, and the next upload to the slot waits on all
        // of them before overwriting. (A single per-slot semaphore could
        // not express the static-hold redraws of the same texture.)
        if (videoImage) {
            VkSemaphoreCreateInfo semaphoreInfo = {};
            semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            if (vkCreateSemaphore(fVk->device(), &semaphoreInfo, nullptr, &videoDone) !=
                VK_SUCCESS) {
                videoDone = VK_NULL_HANDLE;
            }
        }

        const bool presented =
            fSwapchain->flushAndPresent(surface, imageIndex, &result, videoDone);
        if (!presented) {
            // flushAndPresent only fails after flushing, so an attached
            // semaphore will still be signalled: the hand-off stands.
            if (videoDone != VK_NULL_HANDLE && fVideoPresenter) {
                fVideoPresenter->onPresented(videoToken, videoDone);
            }
            fSwapchain->destroy();
            createSwapchain();
            return;
        }
    }

    // Preview (task 1.4) runs after the audience frame is presented: the
    // extra pass never delays the swapchain work it follows, and its
    // throttling, content gate and drop policy live in publishPreviewFrame.
    if (fFrames > 0) {
        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - fStartTime)
                .count();
        publishPreviewFrame(elapsed, videoImage);
    }

    // The video slot is handed back last: publishPreviewFrame's readback
    // above is CPU-synchronous, so every queued sampling of the slot has
    // finished by now, and once doneVk signals the uploader may write the
    // slot again with nothing in flight against it.
    if (videoDone != VK_NULL_HANDLE && fVideoPresenter) {
        fVideoPresenter->onPresented(videoToken, videoDone);
    }

    fPresentedRevision = presentedFor;
    fNeedsPresent = false;
    // The crossfade animates frame by frame, but this loop only presents
    // on a reason — and the M3 demo caught it sleeping the moment a fade
    // began: one frame at blend 0 (still the old picture) and the audience
    // never saw the new scene. While the blends run, the next frame is
    // due; the swap's vsync paces the chase.
    if (fScene && fScene->transitionRunning()) {
        fNeedsPresent = true;
    }

    {
        std::lock_guard<std::mutex> lock(fMutex);
        fStatus.frames = fFrames;
        fStatus.width = fSwapchain->width();
        fStatus.height = fSwapchain->height();
    }
}

void OutputSession::housekeep() {
    std::lock_guard queueLock(graphicsQueueMutex());
    // Time-based chores that must not depend on frames being presented:
    // the event-driven loop presents nothing while content sits still.

    // Wayland may configure the surface after the swapchain was created
    // (or the swapchain fell back to the provisional size), and present
    // does not always report that. Check the surface extent and recreate
    // at the real panel size when it differs; the fresh buffers then make
    // the loop present once.
    int surfaceWidth = 0;
    int surfaceHeight = 0;
    if (fSwapchain && fSwapchain->extentOutOfDate(&surfaceWidth, &surfaceHeight)) {
        std::fprintf(stderr,
                     "openshow-worker: compositor configured %dx%d, recreating "
                     "swapchain from %dx%d\n",
                     surfaceWidth, surfaceHeight, fSwapchain->width(),
                     fSwapchain->height());
        fSwapchain->destroy();
        if (createSwapchain()) {
            fNeedsPresent = true;
        }
    }

    // Cheap re-enumeration so a hotplug reaches output.displays without
    // GLFW monitor callbacks (the dedicated hotplug story is M6.1).
    refreshDisplaySnapshot();
}

unsigned long long OutputSession::run(double seconds, const std::atomic<bool>* abort) {
    const auto startTime = std::chrono::steady_clock::now();
    fRunStart = startTime;
    // Far-future sentinel: steady_clock::time_point::max() overflows when
    // durations are subtracted from it.
    const auto forever = startTime + std::chrono::hours(24 * 3650);
    const auto hardDeadline =
        seconds > 0.0
            ? startTime + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                  std::chrono::duration<double>(seconds))
            : forever;
    auto nextHousekeeping = startTime + std::chrono::milliseconds(500);

    // Event-driven frame loop (task 1.5e): render only on a reason — a
    // command that changed state, a window event, or a video slot due
    // against the clock. Otherwise sleep in glfwWaitEventsTimeout, which
    // wakes on display events and on wakeRenderThread() posts; the
    // periodic housekeeping is time-based and presents nothing, so an idle
    // slide keeps the present counter flat at ~0% CPU (PLAN.md 1.5e).
    while (running() && !(abort && *abort)) {
        glfwPollEvents();
        const auto now = std::chrono::steady_clock::now();

        bool presentNow = false;
        bool previewDue = false;
        auto wakeAt = hardDeadline;
        {
            std::lock_guard<std::mutex> lock(fMutex);
            presentNow = fRevision != fPresentedRevision ||
                         fDesiredMonitor != fAppliedMonitor ||
                         fDesiredPreview.generation != fAppliedPreviewGeneration ||
                         fNeedsPresent || fWindow->framebufferResized();
            if (!presentNow && fPendingVideo && fVideoPresenter) {
                const double clock = videoClockSecondsLocked(now);
                if (clock >= fPendingVideo->ptsSeconds) {
                    presentNow = true;  // frame() re-checks and consumes it
                } else {
                    wakeAt = std::min(
                        wakeAt,
                        now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                  std::chrono::duration<double>(
                                      fPendingVideo->ptsSeconds - clock)));
                }
            }
            if (!presentNow && fStatus.preview.running &&
                fFrames != fPreviewPublishedFrames) {
                // A change is owed to the preview but the cadence throttled
                // it: republish when allowed without a fresh output frame.
                const auto nextAllowed =
                    fLastPreview +
                    std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        std::chrono::duration<double>(
                            1.0 / std::max(1u, fStatus.preview.fps)));
                if (now >= nextAllowed) {
                    previewDue = true;
                } else {
                    wakeAt = std::min(wakeAt, nextAllowed);
                }
            }
        }

        if (now >= nextHousekeeping) {
            housekeep();
            nextHousekeeping =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            if (fNeedsPresent) {
                presentNow = true;
            }
        }

        // The deadline is checked ahead of the present branches: a due
        // video frame every 40 ms would otherwise keep re-entering frame()
        // and never let a timed run end (dev-only; seconds==0 runs forever).
        if (now >= hardDeadline) {
            break;
        }

        if (presentNow) {
            // The scene change is applied outside the frame's graphics
            // queue lock on purpose (task 5.5): a background-video swap
            // joins pipeline threads that may themselves be waiting on
            // that lock, so it must not run while frame() holds it.
            // Composition and transport still switch together, directly
            // ahead of the draw.
            applyPendingSceneChange();
            frame();
            continue;
        }
        if (previewDue) {
            const double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - fStartTime)
                    .count();
            SkImage* held = nullptr;
            if (fVideoPresenter && fDrawnVideo) {
                held = fVideoPresenter->acquireImage(fDrawnVideo->token);
            }
            publishPreviewFrame(elapsed, held);
            continue;
        }
        const auto waitUntil = std::min(nextHousekeeping, wakeAt);
        if (waitUntil > now) {
            glfwWaitEventsTimeout(std::chrono::duration<double>(waitUntil - now).count());
        }
    }
    return fFrames;
}

}  // namespace openshow::worker
