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
#include <cstdlib>
#include <ctime>
#include <thread>
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
#include "render/SceneRenderer.h"
#include "session/Faults.h"

namespace openshow::worker {

namespace {

constexpr uint32_t layerMaskOf(const SceneRenderer& scene) {
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

    // Task 6.1b (R1): the session drives the production compositor. With
    // no demo builder in Options — what --socket passes — it starts black
    // with nothing presented, clock and aux off, and open() reports those
    // defaults in the initial layer mask. A development driver hands in
    // demoBuilder() and gets the decorated 1.x picture instead.
    fScene = std::make_unique<SceneRenderer>();
    if (!fScene->create(error, options.demo)) {
        close();
        return false;
    }
    // The preview producer draws with this compositor and builds its
    // surfaces on this context (task 6.18a2); it never holds them past
    // close(), which detaches and shuts it down before the GPU goes.
    fPreview.attachRenderer(fContext.get(), fScene.get());

    {
        std::lock_guard<std::mutex> lock(fMutex);
        // The store starts knowing the renderer's own layer mask and the panel
        // the window was mapped onto: desired, staged and proved are the same
        // thing here, because nothing has been asked for yet (task 6.18a).
        fStore.seed(layerMaskOf(*fScene), monitor);
        fStatus.deviceName = fVk->deviceName();
    }
    refreshDisplaySnapshot();
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
    if (fBackgroundMedia && !fPrepares.start()) {
        // Task 6.9a: socket mode is the only place a prepare schedules
        // file work, so that is the only place the executor runs. A worker
        // that cannot start it would answer every prepare with a refusal;
        // failing open is the honest alternative.
        if (error) {
            *error = "could not start the prepare executor";
        }
        close();
        return false;
    }
    return true;
}

void OutputSession::close() {
    // Task 6.9a first: retire the prepare work BEFORE anything it touches.
    // stop() flags the running job, retires the waiting queue, joins, and
    // after it no completion can fire again — so the image cache, the
    // transport's fault lane and the GPU below are never torn down under
    // a decode. Idempotent, like this close() is.
    fPrepares.stop();
    // The audience-side media — background transport and standalone track —
    // goes down by name and in its owner's documented order (task 5.5's:
    // before every GPU resource it decodes into), ahead of the context
    // below. Owner since task 6.18b: session/BackgroundTransport.h states
    // the shutdown rules; it is idempotent, like this close() is.
    fTransport.shutdown();
    if (fSwapchain) {
        fSwapchain->destroy();
        fSwapchain.reset();
    }
    // The preview surfaces are allocations on this context and the shm
    // unlink belongs to the producer (task 6.18a2): shut it down while the
    // GPU it drew into still exists. The selection's content pin goes with
    // the stream it was drawn for (task 6.16a) — a closed session must not
    // sit on a scene the store would otherwise be free to evict, and no
    // later pass exists to draw it.
    fPreview.shutdown();
    fPreview.detachRenderer();
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
        fStore.setLayerBit(layerBit(id), enabled, revisionOut);
    }
    // The event-driven loop (task 1.5e) sleeps between frames: nudge it so
    // the change lands on the next vsync rather than never.
    wakeRenderThread();
    return true;
}

bool OutputSession::selectMonitor(int index, uint64_t* revisionOut) {
    // kAutoSecondDisplay is the --monitor option's encoding, never a
    // destination: open() resolves it into a panel before this seam exists, so
    // a command naming it gets the ordinary refusal.
    if (index == kAutoSecondDisplay) {
        return false;
    }
    bool accepted = false;
    {
        std::lock_guard<std::mutex> lock(fMutex);
        // The range rule — -1 windowed, a panel index that exists in the
        // snapshot GLFW refreshed — lives with the desired destination it
        // validates against (task 6.18a).
        accepted = fStore.selectMonitor(index, fDisplays.size(), revisionOut);
    }
    if (!accepted) {
        return false;  // a refusal is not a reason to wake the frame loop
    }
    wakeRenderThread();
    return true;
}

std::vector<OutputSession::DisplayInfo> OutputSession::displays() const {
    std::lock_guard<std::mutex> lock(fMutex);
    return fDisplays;
}

bool OutputSession::prepareScene(const openshow::scene::Scene& scene, uint64_t jobId,
                                 std::function<void(uint64_t, bool)> onReady,
                                 uint64_t* revisionOut) {
    uint64_t revision = 0;
    {
        std::lock_guard<std::mutex> lock(fMutex);
        // The store mints the id and bounds itself (task 6.18a): nothing here
        // touches the output, so no revision moves and no frame is provoked.
        revision = fStore.prepare(scene);
        if (revisionOut != nullptr) {
            *revisionOut = revision;
        }
        // Task 6.10 (review R12): the store's contents are the pin set.
        // Before the scheduled preload puts this scene's still in the
        // cache — and before any churn of other scenes can evict anything —
        // the whole held set is re-pinned, so a prepared scene's picture is
        // guaranteed at present time no matter what the budget does to
        // the scenes the store has let go.
        syncScenePins();
    }
    if (!fBackgroundMedia) {
        // Benches do not prepare, and the dev paths stay exact: with no
        // transport there is no preload to schedule, and an ack need not
        // wait on a thread that is not running.
        onReady(revision, false);
        return true;
    }
    // Task 6.9a (review R10): the file work leaves this thread. The
    // completion names the revision it answers, captured here rather than
    // read back from the output argument — the job may resolve on the
    // executor thread before this function has returned.
    auto ready = [revision, onReady = std::move(onReady)](bool cancelled) {
        onReady(revision, cancelled);
    };
    // The job prewarms the store's OWN instance (task 6.9b): the frame
    // loop's layout cache is keyed by that pointer, so the shape only
    // counts as a hit when it is built for the shared object. pin() shares
    // it out exactly as the draw path takes it; held by the job, the
    // content cannot be evicted beneath it or beneath the frame loop.
    ActiveScene content;
    fStore.pin(revision, &content);
    if (!fPrepares.submit(
            jobId,
            [this, scene, content](const std::atomic<bool>& cancelled) {
                preloadScene(scene, &cancelled);
                // Task 6.9b: the same job prewarms the shared caches the
                // frame boundary draws from — the scene's shaped text and
                // its blur backdrop — because a pin, a decode and a shape
                // for one prepared scene belong to one unit of file work.
                // The scene travels BY VALUE (the job may run after this
                // function returns); the shape is keyed on `content`, the
                // store's own instance, because that is the pointer the
                // draw path will ask with.
                prewarmScene(content);
            },
            std::move(ready))) {
        // Explicit refusal, two possible reasons, both honest: the worker
        // is closing, or kMaxQueued prepares already await the disk. The
        // store holds the scene either way (the pins synced above); no
        // onReady follows, so the caller answers its request itself.
        return false;
    }
    return true;
}

void OutputSession::preloadScene(const openshow::scene::Scene& scene,
                                 const std::atomic<bool>* cancelled) {
    auto stopped = [cancelled] {
        return cancelled != nullptr && cancelled->load();
    };
    if (fScene == nullptr || stopped()) {
        return;
    }
    // Task 6.9b (review R10): a prepare whose file work really costs time,
    // so the queue's bound is tested against a cost. The sleep checks the
    // cooperative flag the way real cancellable work does — a retired job
    // stops early, its ack comes from the retirement.
    if (faults().prepareDelayMs > 0) {
        const int milliseconds = faults().prepareDelayMs;
        for (int elapsed = 0; elapsed < milliseconds && !stopped(); elapsed += 20) {
            std::this_thread::sleep_for(std::chrono::milliseconds(
                std::min(20, milliseconds - elapsed)));
        }
        if (stopped()) {
            return;
        }
    }
    // Foreground pictures (task 6.17b): the same discipline as the
    // background's still — decoded NOW, on the prepare executor thread
    // (task 6.9a), through the REFRESHING entry point so a file that was
    // missing and has been put back comes back at the next prepare — and
    // every failure NAMED on
    // stderr while the old scene is still on air. The MEDIA LANE
    // deliberately stays background-only: status.media names what the
    // audience's transport is showing, and a stack picture's decode error
    // would evict that truth with a file nobody is playing. A missing
    // foreground picture is honest on screen — its box shows what is
    // under it, which is visibly not the slide the operator composed —
    // and per-item fault reporting is the R19 media lane's to grow.
    for (const openshow::scene::SceneItem& item : scene.items) {
        if (stopped()) {
            return;  // superseded or closing: one image is the unit (6.9a)
        }
        if (!item.isImage || item.assetPath.empty()) {
            continue;
        }
        std::string error;
        if (fScene->imageCache()->refreshFor(item.assetPath, &error) == nullptr) {
            std::fprintf(stderr, "openshow-worker: foreground image %s: %s\n",
                         item.assetPath.c_str(), error.c_str());
            // Task 6.20d (review R18): the log line above always named the
            // failure; this is the half that lets the FRONTEND name it —
            // status.scene.foregroundErrors, per asset path, until a later
            // prepare decodes that path again.
            foregroundImageFailed(item.assetPath, error);
        } else {
            foregroundImageRecovered(item.assetPath);
        }
    }
    const openshow::scene::Background& background = scene.background;
    if (!background.hasImage || stopped()) {
        return;
    }
    if (!background.isVideo) {
        // Stills: decode now, through the REFRESHING entry point (task
        // 6.7c, review R18). The per-frame draw path keeps a cached failure
        // — one attempt per path, not a retry at frame rate — but a prepare
        // is an operator action, and two things follow: a file that cannot
        // be decoded must be NAMED (media.error), not degraded into a silent
        // colour fill, and a file that was missing and has been put back
        // must come back on air and clear its error.
        std::string error;
        if (fScene->imageCache()->refreshFor(background.assetPath, &error) == nullptr) {
            std::fprintf(stderr, "openshow-worker: background image %s: %s\n",
                         background.assetPath.c_str(), error.c_str());
            fTransport.mediaFailed(MediaFault::kImage, background.assetPath,
                                "background image: " + error);
        } else {
            fTransport.mediaFaultCleared(MediaFault::kImage, background.assetPath);
            // Task 6.9b: the blur fit's companion rides this same unit of
            // file work — built NOW, on the executor thread, once the pixels
            // it derives from are resident — so the frame boundary that
            // switches to this scene pays only the map probe. Blur is a fit
            // (task 6.17c1), not a flag; a cover/contain still has no
            // backdrop to prewarm, and a blur over a VIDEO composites from
            // the presented frame (6.6), so this belongs to the still alone.
            if (background.fit == openshow::domain::BackgroundFit::Blur) {
                fScene->imageCache()->prewarmBackdrop(background.assetPath);
            }
        }
        return;
    }
    // Videos: opening a transport belongs to the present (the audience
    // decides when it starts), but the cheap probe AND the file-half prime
    // belong to now. The probe is first: a file that cannot even be read or
    // demuxed is known while the old scene is still on air, and reported
    // (5.5d) instead of discovered as a silent colour fill one frame after
    // the change. A file the probe could read is then PRIME-OPENED on THIS
    // thread — the prepare executor's — so the audience's boundary attaches
    // the GPU half instead of opening from disk (task 6.9b, review R10):
    // the slow open happens while the old scene is still on air, and the
    // prime is the one bounded transport R10 asks for (a superseded prime
    // retires on this thread). The prime changes nothing on screen — it is
    // readiness, and the present remains the decision.
    openshow::media::MediaInfo info;
    std::string error;
    if (openshow::media::probeMedia(background.assetPath, &info, &error) !=
        openshow::media::ProbeStatus::kOk) {
        std::fprintf(stderr, "openshow-worker: background preload %s: %s\n",
                     background.assetPath.c_str(), error.c_str());
        fTransport.mediaFailed(MediaFault::kVideo, background.assetPath,
                             "background preload: " + error);
    } else if (!fTransport.primeVideo(background.assetPath, cancelled, &error)) {
        // A prime failure is not a reportable media fault: the file reads
        // (the probe just said so), and the present will open cold and
        // report anything truly wrong. It is only the loss of the head
        // start, so it stays in the log.
        std::fprintf(stderr, "openshow-worker: background prime %s skipped: %s\n",
                     background.assetPath.c_str(), error.c_str());
    }
    // A successful probe deliberately clears nothing: the audience still
    // has the previous background until this one's transport opens, and the
    // report of what is on air belongs to that transport (mediaPlaying),
    // not to a probe of something not yet shown.
}

void OutputSession::foregroundImageFailed(const std::string& path, const std::string& message) {
    // Task 6.20d (review R18): the per-item lane behind
    // SceneStatus::foregroundFailures. The attempt count is every failure;
    // the LIST is the current set — an already-listed path keeps its slot
    // and takes the newest reason (a re-prepare of the same broken file
    // updates the story rather than adding a second entry), and once the
    // list is full a NEW path is counted but not shown, which
    // foregroundFailureEvents keeps visible rather than silent.
    std::lock_guard<std::mutex> lock(fMutex);
    ++fStatus.scene.foregroundFailureEvents;
    for (SceneStatus::ForegroundFailure& entry : fStatus.scene.foregroundFailures) {
        if (entry.path == path) {
            entry.error = message;
            return;
        }
    }
    if (fStatus.scene.foregroundFailures.size() <
        SceneStatus::kMaxReportedForegroundFailures) {
        fStatus.scene.foregroundFailures.push_back(SceneStatus::ForegroundFailure{path, message});
    }
}

void OutputSession::foregroundImageRecovered(const std::string& path) {
    // The repair edge, identical in kind to the media lane's: a file that
    // was missing and has been put back clears its report at the next
    // prepare that decodes it. No event is un-counted — the events are
    // history; this list is now.
    std::lock_guard<std::mutex> lock(fMutex);
    std::vector<SceneStatus::ForegroundFailure>& list = fStatus.scene.foregroundFailures;
    list.erase(std::remove_if(list.begin(), list.end(),
                              [&path](const SceneStatus::ForegroundFailure& entry) {
                                  return entry.path == path;
                              }),
               list.end());
}

void OutputSession::prewarmScene(
    const std::shared_ptr<const openshow::scene::Scene>& scene) {
    if (fScene == nullptr || !scene || scene->items.empty()) {
        return;
    }
    // Shaping is a scene's expensive half (task 6.9b): the frame loop's
    // first draw of a prepared scene must find the paragraphs already
    // built. The production renderer owns the cache its draw reads; this
    // call only moves the moment of payment, never the work's rules.
    fScene->prewarmLayouts(scene);
}

bool OutputSession::presentScene(uint64_t prepared, uint64_t* revisionOut,
                                 SceneTransitionKind kind) {
    {
        std::lock_guard<std::mutex> lock(fMutex);
        // One call, one truth (task 6.18a): the store refuses 0 and an id it
        // does not hold, queues the ask as pending until a presented frame
        // carries it, and says whether the desired scene moved. Re-presenting
        // exactly what is confirmed on air starts no frame, so it queues
        // nothing and re-pins nothing.
        bool changed = false;
        if (!fStore.present(prepared, kind, revisionOut, &changed)) {
            return false;
        }
        if (changed) {
            syncScenePins();  // task 6.10: the outgoing scene stays pinned while
                              // the fade draws it; the commit releases it
        }
    }
    wakeRenderThread();
    return true;
}

/*
 * Preview commands (task 1.4; owner since task 6.18a2 is PreviewProducer,
 * session/PreviewProducer.h). The rules each command enforces — geometry,
 * the generation gates, the batch caps, the pin-at-accept, the slot
 * identity, the notification dropping — live in the producer, and its
 * header states them. What stays here is the session's half of the two
 * contracts: cheap accepted commands are made under the session mutex so
 * the lock order stays session -> producer, the two commands whose
 * selection went changes the image-pin union (task 6.10), and a command or
 * pump that left the render thread owing something wakes it (task 1.5e's
 * discipline: the ask is a reason to wake; the producer never touches
 * GLFW itself).
 */

bool OutputSession::startPreview(uint32_t width, uint32_t height, uint32_t fps,
                                 PreviewInfo* info, std::string* error) {
    bool accepted;
    {
        std::lock_guard<std::mutex> lock(fMutex);
        accepted = fPreview.start(width, height, fps, info, error);
    }
    if (!accepted) {
        return false;  // a refusal is not a reason to wake the frame loop
    }
    wakeRenderThread();
    return true;
}

void OutputSession::stopPreview() {
    bool changed;
    {
        std::lock_guard<std::mutex> lock(fMutex);
        changed = fPreview.stop();
        if (changed) {
            syncScenePins();  // the selection's still is no longer held
        }
    }
    if (changed) {
        wakeRenderThread();
    }
}

bool OutputSession::selectPreviewScene(uint64_t revision, std::uint32_t request,
                                       std::string* error) {
    bool accepted;
    {
        std::lock_guard<std::mutex> lock(fMutex);
        // The store lookup rides as the producer's pin seam (the lock order
        // is session -> producer -> store, and the store calls nothing
        // back). The accepted scene's still joins the pin set NOW, not on
        // the render pass that draws it, so the frame loop never opens a
        // file (task 6.10's rule for a target that has no present to pin it
        // through).
        accepted =
            fPreview.select(revision, request,
                            [this](uint64_t rev, ActiveScene* out) {
                                return fStore.pin(rev, out);
                            },
                            error);
        if (accepted) {
            syncScenePins();
        }
    }
    // The ask is a reason to wake: an idle show with nothing else owed
    // sleeps in glfwWaitEventsTimeout, and an edit that waits for the next
    // housekeeping tick is an edit the operator watched not happen.
    if (accepted) {
        wakeRenderThread();
    }
    return accepted;
}

bool OutputSession::requestThumbnails(const std::vector<ThumbnailEntry>& batch,
                                      uint32_t width, uint32_t height,
                                      std::uint32_t request, ThumbnailAck* ack,
                                      std::string* error) {
    // No session lock around this call: the producer's state is its own,
    // and it must release ITS lock for the companion renderer's font setup
    // — the same reason R10 gives for the session lock, and taking
    // fMutex here would only re-add the block the extraction removed.
    // A band the operator is looking at is a reason to wake: an idle show
    // sleeps, and a list that fills in only at the next housekeeping tick
    // looks broken even though every frame of it is correct.
    const bool accepted =
        fPreview.requestThumbnails(batch, width, height, request, ack, error);
    if (accepted) {
        wakeRenderThread();
    }
    return accepted;
}

bool OutputSession::pumpThumbnailDecode() {
    // One decode per call happens on this (RPC) thread, outside both the
    // producer lock and the session lock. True means a job settled: a head
    // that just became ready is owed a frame.
    const bool settled = fPreview.pumpThumbnailDecode();
    if (settled) {
        wakeRenderThread();
    }
    return settled;
}

void OutputSession::releasePreviewSlot(uint32_t slot, uint64_t seq,
                                       const std::string& stream) {
    fPreview.releaseSlot(slot, seq, stream);
}

bool OutputSession::takePreviewReady(PreviewReady* out) {
    return fPreview.takeReady(out);
}

/*
 * The background-media seam (tasks 5.5–6.15b; BackgroundTransport is the
 * owner since 6.18b). Everything the audience-side media does lives behind
 * this surface: the session keeps the thread that drives it — the render
 * loop runs applyTransportOps, the scene change runs applyBackground, the
 * frame commit and the half-second housekeep run refreshMediaState — and
 * these calls forward the verbs a command thread issues. The rules for
 * each (the newest-wins intent slots, the probe-before-accept standalone
 * request, the R6 quiesce boundary, the typed faults) are Background-
 * Transport's, stated in its header; a wrapper here adds nothing to them.
 */

bool OutputSession::takeMediaError(MediaError* out) {
    return fTransport.takeMediaError(out);
}

void OutputSession::mediaFailed(MediaFault fault, const std::string& path,
                                const std::string& message) {
    fTransport.mediaFailed(fault, path, message);
}

void OutputSession::mediaFaultCleared(MediaFault fault, const std::string& path) {
    fTransport.mediaFaultCleared(fault, path);
}

void OutputSession::setVideoPresenter(VideoPresenter* presenter) {
    fTransport.setVideoPresenter(presenter);
}

void OutputSession::setVideoClock(std::function<double()> clock) {
    fTransport.setVideoClock(std::move(clock));
}

void OutputSession::submitVideo(VideoSubmission submission) {
    fTransport.submitVideo(std::move(submission));
}

void OutputSession::requestMediaSeek(double seconds) {
    fTransport.requestMediaSeek(seconds);
}

void OutputSession::requestMediaPause() {
    fTransport.requestMediaPause();
}

void OutputSession::requestMediaPlay() {
    fTransport.requestMediaPlay();
}

void OutputSession::requestMediaLoop(bool enabled) {
    fTransport.requestMediaLoop(enabled);
}

bool OutputSession::mediaSourceLive(AudioSource source) const {
    return fTransport.mediaSourceLive(source);
}

bool OutputSession::requestMediaGain(AudioSource source, double gain, std::string* error) {
    return fTransport.requestMediaGain(source, gain, error);
}

bool OutputSession::requestMediaMute(AudioSource source, bool muted, std::string* error) {
    return fTransport.requestMediaMute(source, muted, error);
}

bool OutputSession::requestStandaloneAudio(const std::string& path, std::string* error) {
    return fTransport.requestStandaloneAudio(path, error);
}

GpuHandles OutputSession::gpuHandles() const {
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
    Status snapshot = fStatus;
    // The media half (task 6.18b): BackgroundTransport's own mirror, copied
    // in for the same reason the store's published set is — Status is a
    // reading of its owner, never a session-side mirror that could be
    // advanced out of step.
    snapshot.media = fTransport.mediaStatus();
    // The store-owned fields (task 6.18a): revision, layerMask, monitor and
    // scene.active/pending/transitioning/prepared do not live in fStatus at
    // all — they are SceneStore's published set, copied in here so a caller's
    // Status is always a reading of the one scene truth rather than a mirror
    // that could be advanced a frame early. That mirror is what review R4
    // found, and the finding cannot come back while there is nothing to
    // publish into.
    const SceneStore::Published published = fStore.published();
    snapshot.revision = published.revision;
    snapshot.layerMask = published.mask;
    snapshot.monitor = published.monitor;
    snapshot.monitorName = monitorNameLocked(published.monitor);
    snapshot.scene.active = published.activeScene;
    snapshot.scene.pending = published.pendingScene;
    snapshot.scene.transitioning = published.transitioning;
    snapshot.scene.prepared = published.prepared;
    // Not a frame-truth field (R4 governs what the audience HAS; this is
    // what the worker still OWES): the executor's live count, read leaf-side
    // so a poller can watch a heavy prepare drain without racing an ack
    // (task 6.9a).
    snapshot.scene.preparing = static_cast<uint64_t>(fPrepares.queued());
    // The preview half (task 6.18a2): the producer's own mirror, copied in
    // here for the same reason the store's published set is — Status is a
    // reading of its owner, never a session-side mirror that could be
    // advanced out of step.
    snapshot.preview = fPreview.status();
    return snapshot;
}

std::string OutputSession::monitorNameLocked(int monitor) const {
    if (monitor < 0 || monitor >= static_cast<int>(fDisplays.size())) {
        return "";  // windowed, or a panel the last hotplug took away
    }
    return fDisplays[static_cast<std::size_t>(monitor)].name;
}

void OutputSession::requestStop() {
    fStop = true;
    wakeRenderThread();
}

bool OutputSession::running() const {
    return !fStop && fWindow && !fWindow->shouldClose();
}

void OutputSession::applyPendingLayerChanges(const FrameSnapshot& snapshot) {
    // The snapshot is captured and drawn by this same thread: read it
    // directly, no second opinion from the guarded desired state (that
    // read is what let a late command ride into an early frame). Staging the
    // landed mask here is bookkeeping for the frame's commit; the status
    // publication itself happens only after a successful present (task 6.4),
    // and it is the store that holds both halves (task 6.18a).
    const uint32_t desired = snapshot.mask;
    if (desired == fStore.landedMask()) {
        return;
    }
    for (uint32_t i = 0; i < static_cast<uint32_t>(LayerId::kCount); ++i) {
        const LayerId id = static_cast<LayerId>(i);
        const bool want = (desired & layerBit(id)) != 0;
        if (want != fScene->layerEnabled(id)) {
            fScene->setLayerEnabled(id, want);
        }
    }
    fStore.landMask(desired);
}

void OutputSession::applyPendingDisplayChange(const FrameSnapshot& snapshot) {
    const int desired = snapshot.monitor;
    if (desired == fStore.landedMonitor()) {
        return;
    }
    if (!fWindow->setMonitor(desired, 300)) {
        // The index vanished between validation and apply (hotplug): drop
        // the request rather than retry a doomed one every frame. The store
        // moves its desired destination back to where the presented frame
        // says the window really is; the published destination never moved,
        // because this frame never reached the panel.
        fStore.abandonMonitor();
        return;
    }
    fStore.landMonitor(desired);
    refreshDisplaySnapshot();

    // Rebuild the swapchain at the new panel; if the compositor's configure
    // is still lagging, the periodic extentOutOfDate() check recreates
    // again at the real extent, same as at open().
    fSwapchain->destroy();
    if (!createSwapchain()) {
        return;  // the resize path retries on the next frame
    }
    fWindow->resetFramebufferResize();  // setMonitor's resize is done here
    // monitor/monitorName join the other applied fields in the frame's
    // commit block: a moved window is on air when its frame presents.
}

void OutputSession::applyPendingSceneChange(const FrameSnapshot& snapshot) {
    const uint64_t desired = snapshot.scene;
    if (desired == fStore.landedScene()) {
        return;
    }
    // Fault injection (task 6.4): real applies take time — opening a
    // background transport really runs hundreds of ms. The fixture uses
    // this to land a command squarely inside a live apply instead of
    // racing a microsecond window. Arming belongs to the change too: the
    // scene's next presents are the ones that must fail, not the worker's
    // startup frames.
    if (faults().applyDelayMs > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(faults().applyDelayMs));
    }
    if (faults().scenePresentFails > 0) {
        faults().armedPresents = faults().scenePresentFails;
    }
    // Task 3.1: the landed scene is what the production layers draw. The
    // store may evict its entry later, so a copy shared with the
    // composition keeps the on-air scene alive as long as it is drawn —
    // here that copy is the snapshot's pin, so what is applied is exactly
    // what the run loop captured, whatever commands arrived since.
    fStore.landScene(desired);
    // Task 5.5: the render thread keeps its own handle on the on-air scene
    // (the composition gets a copy) — the draw path and the background
    // transport both read the same thing from here.
    fRenderScene = snapshot.content;
    fScene->setActiveScene(snapshot.content, snapshot.kind);
    if (fBackgroundMedia) {
        // The transport follows the scene in the same breath — and this
        // whole function runs ahead of frame()'s graphics queue lock
        // precisely because the swap's join() waits on threads that may be
        // blocked acquiring it (deadlock otherwise). Since task 6.18b the
        // identity rule, the quiesce order and the broken-still report all
        // belong to BackgroundTransport: the session hands it the device it
        // paints with and the cache answer it cannot read itself, and the
        // one fact the loop needs — a video transport started, so a present
        // is owed — comes back as the return value.
        const bool started = fTransport.applyBackground(
            snapshot.content ? snapshot.content->background : openshow::scene::Background{},
            gpuHandles(),
            [this](const std::string& path) {
                return fScene != nullptr && fScene->imageCache()->has(path);
            });
        if (started) {
            fNeedsPresent = true;  // the head frame arrives soon; redraw when it does
        }
    }
    // active/revision are NOT published here (task 6.4, review R4): the
    // apply only stages the scene. The commit after a successful present
    // is what tells callers the scene is on air.
}

void OutputSession::syncScenePins() {
    // fMutex held. The store's lock nests inside it (task 6.18a's order, and
    // the store calls nothing back). The cache lives in the renderer and its
    // pin operations are map arithmetic under the cache's own lock — no disk,
    // no waiting — so this is safe from both the RPC thread and the frame loop.
    if (fScene == nullptr) {
        return;
    }
    if (!fBackgroundMedia) {
        // No media mode (benches, --render-test): nothing prepares, so the
        // session owns no pins and must release none — a bench draws
        // through the same cache without ever declaring a held set.
        mirrorImageStatsLocked();
        return;
    }
    // The union of everything that can still be put on air, re-derived in
    // full every time: the bounded store, walked directly (task 6.18a), plus
    // the render thread's on-air copy — which covers a queued or landed
    // revision the store has since evicted, and the scene a fade came from
    // until the commit says the transition settled.
    // One reference per DISTINCT path — a shared background held by two
    // scenes is one asset, and the count that keeps it out of eviction is
    // one too. Deriving beats bookkeeping: an eviction in the store, a
    // present and a commit all land on the same truth without any of them
    // having to remember what the last one did.
    std::vector<std::string> held;
    const auto collect = [&held](const openshow::scene::Scene& scene) {
        const openshow::scene::Background& background = scene.background;
        if (background.hasImage && !background.isVideo && !background.assetPath.empty()) {
            held.push_back(background.assetPath);
        }
        // A foreground picture (task 6.17b) can be put on air exactly as
        // its background can — it rides the same scene — so the union
        // takes the same reference rule: one pin per distinct path,
        // shared pictures counted once.
        for (const openshow::scene::SceneItem& item : scene.items) {
            if (item.isImage && !item.assetPath.empty()) {
                held.push_back(item.assetPath);
            }
        }
    };
    // Every scene the store holds contributes (task 6.18a: the store itself is
    // walked, so no content copy is made here), and so does the render thread's
    // on-air copy.
    fStore.forEachPrepared(collect);
    const auto pinned = [&](uint64_t revision) {
        // A revision the store still holds was collected above; one it has let
        // go contributes through the on-air copy, which is the only reference
        // left to it.
        if (!fStore.holds(revision) && fRenderScene) {
            collect(*fRenderScene);  // store-evicted but still on air
        }
    };
    pinned(fStore.desiredScene());
    pinned(fStore.landedScene());
    if (fRenderScene) {
        collect(*fRenderScene);
    }
    // The editor's off-air target (task 6.16a, producer-owned since
    // 6.18a2): its still is drawn by the render thread like any other
    // background, so it is held for as long as the selection is registered
    // — even after the store evicts the revision the pin was taken from.
    // That is the whole point of pinning at accept: looking at an edit must
    // never leave a background image unpinned while something is still
    // drawing it.
    if (const ActiveScene selected = fPreview.selectedScene()) {
        collect(*selected);
    }
    std::sort(held.begin(), held.end());
    held.erase(std::unique(held.begin(), held.end()), held.end());
    fScene->imageCache()->syncPinSet(held);
    mirrorImageStatsLocked();
}

void OutputSession::mirrorImageStatsLocked() {
    if (fScene == nullptr) {
        fStatus.scene.images = {};
        return;
    }
    const SceneImageCache::Stats stats = fScene->imageCache()->stats();
    fStatus.scene.images.decodedBytes = stats.decodedBytes;
    fStatus.scene.images.entries = stats.entries;
    fStatus.scene.images.pins = stats.pins;
    fStatus.scene.images.evictions = stats.evictions;
    fStatus.scene.images.preloadHits = stats.preloadHits;
    // The draw-boundary trio (task 6.9b, review R10): what the frame loop
    // still had to build for itself — decodes, blur backdrops, and the text
    // layouts that missed this renderer's own cache. A companion's shaping
    // is deliberately absent: it is not the audience's cost (R16).
    fStatus.scene.images.drawDecodes = stats.drawDecodes;
    fStatus.scene.images.drawBackdrops = stats.drawBackdrops;
    fStatus.scene.images.layoutBuilds = fScene->textLayoutBuilds();
}

/*
 * The preview handoff (task 6.18a2). Everything the operator's picture
 * needs — stream lifecycle, target choice, cadence, the audience's budget,
 * the generation gates, the queue, the slots and the counters — is
 * PreviewProducer's (session/PreviewProducer.h states the rules). The loop
 * here only says WHICH frame the mirrors redraw: the one this pass put on
 * the panel, or the last one the panel took when the pass presented
 * nothing — never one the preview captured for itself, so a preview tick,
 * a slow consumer or a second target cannot move the show along (task
 * 6.11a, review R13), and the revision tag belongs to the pixels the
 * stream carries.
 */
PreviewProducer::DrawInput OutputSession::previewHandoff(const SceneFrame& frame,
                                                         SkImage* video) const {
    PreviewProducer::DrawInput input;
    input.audienceFrame = &frame;
    input.video = video;
    input.frames = fFrames;
    input.startTime = fStartTime;
    input.audienceScene = &fRenderScene;
    return input;
}

void OutputSession::frame(const FrameSnapshot& snapshot) {
    std::lock_guard queueLock(graphicsQueueMutex());
    // Everything this frame draws comes from the run loop's snapshot —
    // captured under fMutex at the present decision, ahead of every apply
    // below. A command that lands after the capture belongs to the next
    // snapshot: it re-dirties the loop, draws in the next frame, and is
    // credited by that frame's present (task 6.4, review R4).
    applyPendingLayerChanges(snapshot);
    applyPendingDisplayChange(snapshot);
    fPreview.applyStreamState();
    glfwPollEvents();

    // True only after flushAndPresent succeeds for this frame's drawn
    // content. It is the sole gate on publishing applied state: an acquire
    // or present failure publishes nothing, keeps pending alive, and is
    // honestly counted in presentFailures.
    bool presentedFrame = false;

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
            return;  // routine backpressure, not a failed submit
        }
        {
            std::lock_guard<std::mutex> lock(fMutex);
            ++fStatus.presentFailures;
        }
        fSwapchain->destroy();
        createSwapchain();
        return;
    }

    SkSurface* surface = fSwapchain->surface(imageIndex);
    if (!surface) {
        // An acquired index without a surface means the swapchain's images
        // are broken: rebuild rather than credit a frame that cannot be
        // drawn (the old code fell through and acknowledged the revision
        // with nothing presented — task 6.4).
        fSwapchain->destroy();
        createSwapchain();
        return;
    }
    // Video hand-off state for this frame (task 1.5e).
    SkImage* videoImage = nullptr;
    unsigned videoToken = 0;
    VkSemaphore videoDone = VK_NULL_HANDLE;
    // The seam's presenter (task 6.18b): render-thread only, and unchanged
    // while this frame samples it — the transport swaps presenters only
    // behind a quiesce boundary, which this loop is not inside.
    VideoPresenter* const videoPresenter = fTransport.presenter();
    {
        // The footer line (tasks 6.1a/6.1b): a development affordance
        // only. A production compositor gets the empty string — the same
        // one the preview already draws with — so output and preview
        // compose the identical scene. The clock layer starts off in
        // production anyway; if an operator toggles the audience clock up,
        // the audience gets the time, not this process's device and frame
        // numbers.
        std::string statusLine;
        if (!fScene->production()) {
            char status[160];
            std::snprintf(status, sizeof(status), "live: %s \xc2\xb7 %dx%d \xc2\xb7 frame %llu",
                          fVk->deviceName().c_str(), fSwapchain->width(), fSwapchain->height(),
                          fFrames);
            statusLine = status;
        }
        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - fStartTime)
                .count();

        // Video hand-off for this frame (tasks 1.5e, 5.5): the pending
        // slot is drawn once the clock has reached its pts; until then —
        // and on every dirty redraw between video frames — the last
        // presented frame is held, so a layer toggle in the middle of a
        // video never blanks it. The decision runs before the scene draw
        // because task 5.5 may hand the frame to the composition itself.
        //
        // The held frame's lease (task 6.6a, review R6): the drawn submission's
        // image is sampled by every redraw until this decision replaces
        // it, so the presenter keeps its slot out of the producer until
        // onReplaced names it. The replaced frame's last sampling already
        // handed back its doneVk (each frame's onPresented runs at the
        // end of its own iteration, before the next decision), which is
        // what makes releasing here safe for the producer.
        const BackgroundTransport::VideoTurnover turn =
            fTransport.takeDueVideo(std::chrono::steady_clock::now());
        if (turn.replaced && videoPresenter && turn.toDraw &&
            turn.replaced->token != turn.toDraw->token) {
            videoPresenter->onReplaced(turn.replaced->token, turn.replaced->sequence);
        }
        std::optional<VideoSubmission> toDraw = turn.toDraw;
        if (!toDraw) {
            toDraw = fTransport.heldVideo();
        }
        if (toDraw && videoPresenter) {
            videoImage = videoPresenter->acquireImage(toDraw->token, toDraw->sequence);
            if (!videoImage) {
                // The token no longer yields an image for its generation:
                // stop sampling it and give back exactly the lease this
                // submission owns (nothing if another generation already
                // holds the slot).
                videoPresenter->onReplaced(toDraw->token, toDraw->sequence);
                fTransport.dropHeldVideo(toDraw->token);
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
        // always are). NOTE: SceneRenderer::draw leaves its output-rule
        // transform (the fit of the authored 1920x1080 canvas) on this
        // canvas — the dev video rect is in swapchain pixels, so it must
        // be drawn on the identity.
        const bool videoIsBackground =
            videoImage != nullptr && fRenderScene && fRenderScene->background.isVideo;
        // One frame, every target (task 6.11a, review R13). The composition
        // is captured exactly once for this audience frame — on-air scene,
        // crossfade blends, clock and the revision the frame presents — and
        // both the panel and the preview below draw THAT. Capturing is what
        // advances a crossfade, so a preview tick, a slow consumer or a
        // second target cannot move the show along or leave the audience on
        // a blend nobody meant to finish; and because the revision travels
        // inside the frame, the preview's tag belongs to the pixels it
        // publishes.
        fComposedFrame = fScene->captureFrame(elapsed, snapshot.revision);
        fScene->draw(fComposedFrame, surface->getCanvas(), fSwapchain->width(),
                     fSwapchain->height(), statusLine,
                     videoIsBackground ? videoImage : nullptr);
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

        bool presented =
            fSwapchain->flushAndPresent(surface, imageIndex, &result, videoDone);
        if (presented && faults().armedPresents > 0) {
            // Fault injection (task 6.4): a submit the driver refused. The
            // fixture counts on the run loop retrying and on NOTHING being
            // published for this attempt, exactly as for a real failure.
            --faults().armedPresents;
            presented = false;
        }
        if (!presented) {
            // flushAndPresent only fails after flushing, so an attached
            // semaphore will still be signalled: the hand-off stands.
            if (videoDone != VK_NULL_HANDLE && videoPresenter) {
                videoPresenter->onPresented(videoToken, videoDone);
            }
            fSwapchain->destroy();
            createSwapchain();
            {
                std::lock_guard<std::mutex> lock(fMutex);
                ++fStatus.presentFailures;
            }
            if (faults().holdMs > 0) {
                // Give the fixture a stable window to read the honest
                // mid-failure status (pending still queued, active still
                // the old scene, presentFailures climbing).
                std::this_thread::sleep_for(std::chrono::milliseconds(faults().holdMs));
            }
            return;
        }
        presentedFrame = true;
        // The picture in fComposedFrame is on the panel now: credit it (the
        // only place a completed crossfade is retired) and remember it as
        // the frame every preview redraws until the audience is owed a
        // newer one (task 6.11a).
        fScene->commitFrame(fComposedFrame);
        fPresentedFrame = fComposedFrame;
    }

    // Preview (task 1.4) runs after the audience frame is presented: the
    // extra pass never delays the swapchain work it follows, and its
    // throttling, content gate and drop policy live in the producer
    // (task 6.18a2). It draws fComposedFrame — the snapshot this frame put
    // on the panel, revision included (task 6.11a, review R13). Before
    // that it asked the renderer for a composition of its own at a later
    // timestamp, which advanced the live crossfade a second time: the
    // preview ran the fade to a blend the audience had not been shown, on
    // a clock the audience's 800 ms does not own.
    fPreview.drawTargets(previewHandoff(fComposedFrame, videoImage));

    // The present's sampling obligation is handed back last:
    // The mirror's readback is CPU-synchronous, so by now
    // every sampling of the slot this frame queued — output and preview —
    // is complete, and doneVk's signal puts a GPU fence behind it. This
    // does NOT free the slot: the frame stays leased while the held
    // submission (the transport's slot) is sampled again (task 6.6a), and
    // the producer waits for the lease to end
    // AND for every registered doneVk before writing the slot again.
    if (videoDone != VK_NULL_HANDLE && videoPresenter) {
        videoPresenter->onPresented(videoToken, videoDone);
    }

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
        // Background media state rides the frame's status refresh (task
        // 5.5, typed in 6.7c): the render thread owns the transport, so
        // this is the one honest place to sample position, the audible
        // lane, and a decode that died or ended after opening — and to let
        // "playing" describe a frame that exists rather than an open() that
        // returned. (The transport's own reconcile since 6.18b; the session
        // mutex nests above the transport's, never the reverse.)
        fTransport.refreshMediaState();
        // The commit (task 6.4, review R4, handed to the store in 6.18a):
        // published state moves only from a frame that reached the compositor,
        // and only to describe what that frame drew — the landed fields this
        // frame's apply* calls staged, plus the snapshot's revision. Queued
        // state (pending) is recomputed against the newest accepted command, so
        // it settles only when the audience really has it; transitioning says
        // the committed switch has not visually settled yet. Nothing is written
        // into fStatus here: status() reads the store's answer, so the frame is
        // the only thing that can move what a caller sees.
        if (presentedFrame) {
            fStore.commit(snapshot.revision, fScene && fScene->transitionRunning());
            // Task 6.10: once the fade has settled, the scene it faded
            // FROM is no longer drawn and its still stops being sacred
            // (unless the store still holds it, which the sync re-derives
            // from the truth rather than from bookkeeping).
            syncScenePins();
        }
    }
}

void OutputSession::housekeep() {
    // Media state heartbeat (task 6.7c, review R18), taken and released
    // before the graphics queue is touched: a transport whose decode dies,
    // or which ends when it should have looped, changes what status must
    // say — and the event-driven loop presents NOTHING while the picture
    // sits still, so a status only refreshed by frames would go on claiming
    // playback after the last one. The per-frame refresh stays (immediate
    // while frames flow); this is the half-second catch for when they stop.
    fTransport.refreshMediaState();

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
    fTransport.noteRunStart(startTime);
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
        // Transport control first (task 6.7d): a seek or a hold the audience
        // asked for changes what this pass must decide, and the apply belongs
        // on this thread (see BackgroundTransport::applyTransportOps for the
        // discipline).
        fTransport.applyTransportOps();

        bool presentNow = false;
        bool previewDue = false;
        FrameSnapshot snapshot;
        auto wakeAt = hardDeadline;
        {
            std::lock_guard<std::mutex> lock(fMutex);
            // What the output owes (task 6.18a): the store answers both of its
            // questions — an accepted command newer than the newest presented
            // frame, and a destination the window is not on yet — and hands
            // back the snapshot that frame must draw in the same critical
            // section, so the decision and the capture are one instant.
            SceneStore::Decision decision;
            fStore.decide(&decision);
            presentNow = decision.commandsDue || decision.destinationDue ||
                         fPreview.streamChangePending() ||
                         fNeedsPresent || fWindow->framebufferResized();
            // The revision this frame represents, captured in the same
            // critical section as the decision and therefore ahead of
            // every apply the frame performs (see frame()'s header note).
            if (!presentNow) {
                // The seam's own gate (task 6.7d, review R19), answered by
                // the transport that owns the slots since 6.18b: a frame is
                // due against its DEADLINE in the clock's domain, never
                // against its media pts. A horizon in the future is a SLEEP
                // and never a spin.
                const BackgroundTransport::VideoWake video =
                    fTransport.submissionWake(now);
                if (video.pending) {
                    if (video.due) {
                        presentNow = true;  // frame() re-checks and consumes it
                    } else {
                        wakeAt = std::min(
                            wakeAt,
                            now + std::chrono::duration_cast<
                                      std::chrono::steady_clock::duration>(
                                      std::chrono::duration<double>(video.sleepSeconds)));
                    }
                }
            }
            // The capture (task 6.4, taken by the store in 6.18a): it came from
            // the same critical section as the decision above — unconditionally,
            // because housekeeping can add a present reason after the lock — so
            // this snapshot is everything owed as of now and nothing that
            // arrives later, its scene content pinned from the store's entry.
            // frame() draws exactly it and its successful present credits
            // exactly its revision.
            snapshot = std::move(decision.snapshot);
            if (!presentNow) {
                // Either preview target can owe a frame: the MIRROR when the
                // audience's last presented frame has not reached the stream
                // (task 1.5e's content gate), the EDITOR when an accepted
                // off-air ask has not been drawn (task 6.16a), the list when
                // a settled thumbnail waits for its one-per-pass decode
                // (task 6.16b). The producer owns the three cadence markers
                // and the audience's budget and answers the loop's question —
                // task 6.18a2 moves the arithmetic, not its meaning: a horizon
                // in the future is a SLEEP and never a spin (the first fixture
                // measured a spinning stand-down at ~20,000 iterations a
                // second, which is precisely the busy loop 1.5e exists to
                // prevent), and a slide waiting on its codec leaves the loop
                // ASLEEP until the pump wakes it (the re-ask lesson).
                const PreviewProducer::Wake owed = fPreview.previewWake(now, fFrames);
                if (owed.owed) {
                    if (now >= owed.nextAllowed) {
                        previewDue = true;
                    } else {
                        wakeAt = std::min(wakeAt, owed.nextAllowed);
                    }
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
            // ahead of the draw — both from the same snapshot the frame
            // draws and gets credited for (task 6.4).
            applyPendingSceneChange(snapshot);
            frame(snapshot);
            continue;
        }
        if (previewDue) {
            // A publish without an audience present still samples whatever
            // the audience currently shows (task 6.4). Task 6.6a (review
            // R6) gives this branch the same graphics-queue guard the
            // presenting frame and housekeeping take: the producer's
            // commit may not interleave with this branch's acquire, draw
            // and readback. (Held video needs no doneVk here: the
            // readback is CPU-synchronous, so the sampling has completed
            // by the time the branch is done — and the frame's lease
            // keeps the slot off the producer's table throughout.)
            std::lock_guard queueLock(graphicsQueueMutex());
            // The audience's held frame rides the publish (task 6.18b): the
            // transport answers for its own slot, and the acquire stays on
            // this thread under the queue lock, like frame()'s own.
            SkImage* held = fTransport.heldVideoImage();
            // No present this round, so the honest tag is the revision of
            // the last presented frame — the content a republish redraws
            // (task 6.4: presentNow==false means nothing newer was applied).
            // Task 6.11a makes that literal: the preview redraws the LAST
            // PRESENTED FRAME, captured blends and all. This branch used to
            // re-draw the live composition at a fresh timestamp, which
            // advanced the crossfade with the audience asleep — the preview
            // could run a fade to its end and report it done while the panel
            // still showed a half-blended picture.
            // The off-air edit rides the same decision (task 6.16a): an idle
            // show is exactly when the operator edits, so the target cannot
            // wait for the audience to produce a frame of its own.
            fPreview.drawTargets(previewHandoff(fPresentedFrame, held));
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
