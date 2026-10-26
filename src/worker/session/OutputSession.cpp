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
#include "preview/PreviewStream.h"
#include "render/SceneRenderer.h"

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

// Test-only fault injection (task 6.4, review R4): the fixtures must
// force two windows rather than race for a millisecond — a command
// landing while the render thread is mid scene-apply (transport opens
// really do take hundreds of ms, so this models reality), and present
// failures that must not be acknowledged. Failure arming belongs to a
// real scene change so a worker's startup frames are never the ones
// eaten: OPENSHOW_FAULT_SCENE_PRESENT_FAILS re-arms on every applied
// scene switch and the next that-many presents fail. Absent the
// environment every knob is off and production pays nothing but two
// never-taken branches per scene apply / present.
struct Faults {
    int applyDelayMs = 0;         // OPENSHOW_FAULT_SCENE_APPLY_MS
    int scenePresentFails = 0;    // OPENSHOW_FAULT_SCENE_PRESENT_FAILS
    int armedPresents = 0;        // runtime: scene-change failures left to burn
    int holdMs = 0;               // OPENSHOW_FAULT_HOLD_MS: sleep after a failure
    Faults() {
        const auto read = [](const char* name) {
            const char* value = std::getenv(name);
            return value != nullptr ? std::atoi(value) : 0;
        };
        applyDelayMs = read("OPENSHOW_FAULT_SCENE_APPLY_MS");
        scenePresentFails = read("OPENSHOW_FAULT_SCENE_PRESENT_FAILS");
        holdMs = read("OPENSHOW_FAULT_HOLD_MS");
    }
};

// Render-thread only (mutated by frame()/applyPendingSceneChange alone).
Faults& faults() {
    static Faults instance;
    return instance;
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
        // Task 6.10 (review R12): the store's contents are the pin set.
        // Before the preload below puts this scene's still in the cache —
        // and before any churn of other scenes can evict anything — the
        // whole held set is re-pinned, so a prepared scene's picture is
        // guaranteed at present time no matter what the budget does to
        // the scenes the store has let go.
        syncScenePins();
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
            mediaFailed(MediaFault::kImage, background.assetPath, "background image: " + error);
        } else {
            mediaFaultCleared(MediaFault::kImage, background.assetPath);
        }
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
        mediaFailed(MediaFault::kVideo, background.assetPath, "background preload: " + error);
    }
    // A successful probe deliberately clears nothing: the audience still
    // has the previous background until this one's transport opens, and the
    // report of what is on air belongs to that transport (mediaPlaying),
    // not to a probe of something not yet shown.
}

bool OutputSession::presentScene(uint64_t prepared, uint64_t* revisionOut,
                                 SceneTransitionKind kind) {
    {
        std::lock_guard<std::mutex> lock(fMutex);
        if (prepared == 0 || fScenes.find(prepared) == fScenes.end()) {
            return false;
        }
        // Queued state (task 6.4): pending names the accepted command
        // until a presented frame carries its scene. Re-presenting exactly
        // what is confirmed on air starts no frame, so it queues nothing.
        const bool alreadyConfirmed = fStatus.scene.active == prepared &&
                                      fDesiredScene == prepared &&
                                      !fStatus.scene.transitioning;
        fStatus.scene.pending = alreadyConfirmed ? 0 : prepared;
        if (prepared != fDesiredScene) {
            fDesiredScene = prepared;
            fDesiredTransition = kind;  // travels with the change to the render thread
            ++fRevision;
            syncScenePins();  // task 6.10: the outgoing scene stays pinned while
                              // the fade draws it; the commit releases it
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

void OutputSession::releasePreviewSlot(uint32_t slot, uint64_t seq,
                                       const std::string& stream) {
    std::shared_ptr<PreviewStream> target;
    {
        std::lock_guard<std::mutex> lock(fMutex);
        // Stream identity (task 6.5, review R5): a release frees a slot
        // of the stream it NAMES, and only the live one. A delayed
        // release from a retired stream — or one naming nothing — finds
        // no target here, and PreviewStream::release still demands the
        // exact published sequence.
        if (fPreviewStream && fPreviewStream->name() == stream) {
            target = fPreviewStream;
        }
    }
    if (target) {
        target->release(slot, seq);
    }
}

bool OutputSession::takePreviewReady(PreviewReady* out) {
    std::lock_guard<std::mutex> lock(fMutex);
    // The current stream's name (empty while none is live): notifications
    // queued by a stream that has since been replaced are dropped here
    // rather than sent — after a replacement the consumer maps the new
    // segment, and an old stream's slot/seq would describe nothing it
    // owns (task 6.5).
    const std::string live = fPreviewStream ? fPreviewStream->name() : std::string();
    for (auto& pending : fPendingReady) {
        if (!pending) {
            continue;
        }
        if (pending->stream != live) {
            pending.reset();
            continue;
        }
        if (out) {
            *out = *pending;
        }
        pending.reset();
        return true;
    }
    return false;
}

bool OutputSession::takeMediaError(MediaError* out) {
    std::lock_guard<std::mutex> lock(fMutex);
    if (!fPendingMediaError) {
        return false;
    }
    if (out) {
        *out = *fPendingMediaError;
    }
    fPendingMediaError.reset();
    return true;
}

void OutputSession::mediaPlaying(const openshow::scene::Background& background) {
    std::lock_guard<std::mutex> lock(fMutex);
    // Task 6.7c (review R18): the transport is UP, which is not yet the
    // statement "the audience is watching frames". playing waits for the
    // first frame the transport actually hands over (the frame refresh
    // promotes it); position -1 and the lane say what is true meanwhile.
    fStatus.media.playing = false;
    fStatus.media.path = background.assetPath;
    fStatus.media.error.clear();  // a successful open replaces any old failure
    fMediaFault.reset();
    fStatus.media.position = -1.0;  // until the transport presents its head
    fStatus.media.audio = AvPlayer::laneName(mediaLaneLocked());
}

AvPlayer::AudioLane OutputSession::mediaLaneLocked() const {
    // The session's knowledge outranks the transport's ignorance (task
    // 6.7c, review R18): an asset whose audio never reached AvPlayer — a
    // stream no decoder can handle — leaves the transport honestly
    // reporting "none", while the audience is missing sound the file has.
    if (!fExpectedAudioLoss.empty()) {
        return AvPlayer::AudioLane::kDegraded;
    }
    return fAudio.audioLaneState();
}

void OutputSession::mediaStopped() {
    std::lock_guard<std::mutex> lock(fMutex);
    fStatus.media.playing = false;
    fStatus.media.position = -1.0;
    fStatus.media.audio = "none";  // no transport, so no lane to report
    // The on-air asset's known audio loss went out with the transport it
    // describes (task 6.7c): the next identity recomputes it.
    fExpectedAudioLoss.clear();
    // The last error stays visible (the operator still needs to read it);
    // path stays so the message and the asset that caused it stay paired.
}

void OutputSession::mediaFailed(MediaFault fault, const std::string& path,
                                const std::string& message) {
    std::lock_guard<std::mutex> lock(fMutex);
    mediaFailedLocked(fault, path, message);
}

void OutputSession::mediaFailedLocked(MediaFault fault, const std::string& path,
                                      const std::string& message) {
    fStatus.media.path = path;
    // The same failure repeated (a re-prepare of a still-broken asset, or
    // the frame refresh re-reading one death every frame) updates the text
    // but notifies only when the message actually changes — the socket must
    // not spam, and this is now a per-frame path.
    const bool isNew = fStatus.media.error != message;
    fStatus.media.error = message;
    fMediaFault = std::make_pair(fault, path);
    if (isNew) {
        fPendingMediaError = MediaError{path, message};
    }
}

void OutputSession::mediaFaultCleared(MediaFault fault, const std::string& path) {
    std::lock_guard<std::mutex> lock(fMutex);
    mediaFaultClearedLocked(fault, path);
}

void OutputSession::mediaFaultClearedLocked(MediaFault fault, const std::string& path) {
    // Recovery is typed (task 6.7c): it clears only the fault it is the
    // recovery OF. Anything else on file — another class, another asset —
    // stays exactly where it is, because nothing about this event speaks
    // for it.
    if (!fMediaFault || fMediaFault->first != fault || fMediaFault->second != path) {
        return;
    }
    fMediaFault.reset();
    fStatus.media.error.clear();
}

void OutputSession::refreshMediaStateLocked() {
    if (fBackground == nullptr) {
        return;
    }
    const PlaybackPipeline::Stats transport = fBackground->stats();
    const std::string& path = fAppliedBackground.assetPath;
    fStatus.media.position = fBackground->position();
    const AvPlayer::AudioLane lane = mediaLaneLocked();
    fStatus.media.audio = AvPlayer::laneName(lane);

    if (!transport.fatalError.empty()) {
        // A decode that died after a successful open (review R18): no
        // further frame can ever arrive, so the picture is frozen and
        // "playing" would be a lie. The transport's own reason is the
        // media's story, and it holds the shared error text — the lane
        // above still tells the audio truth.
        fStatus.media.playing = false;
        mediaFailedLocked(MediaFault::kVideo, path,
                          "background decode failed: " + transport.fatalError);
        return;
    }
    if (transport.eof) {
        fStatus.media.playing = false;
        if (fBackground->looping()) {
            // A looping transport must never reach an end: the rewind
            // itself failed (or the media stopped yielding frames), which
            // is a fault, not a finished clip.
            mediaFailedLocked(MediaFault::kVideo, path,
                              "background ended while looping (the media cannot be read again)");
            return;
        }
        // The clip simply finished: accurate status, no fault — and this
        // asset's earlier video fault no longer describes anything.
        mediaFaultClearedLocked(MediaFault::kVideo, path);
    } else if (fAudio.paused()) {
        // Held, not playing (task 6.7d): the audience sees a still frame at
        // the position the hold latched, and the lane beside it says
        // "paused". Nothing is broken, so no fault is implied.
        fStatus.media.playing = false;
    } else if (transport.submitted > 0) {
        // Frames are flowing: the point at which status may claim playback.
        fStatus.media.playing = true;
    }

    // The audible lane (review R18): a fault lane is a fault the operator
    // must see; a healthy lane is the recovery that clears it. A fault
    // already on file keeps its text — the transport's start path can name
    // a reason more specific than a lane ever states, and rewriting it each
    // frame would notify the same degradation over and over.
    const bool laneFault = lane == AvPlayer::AudioLane::kDegraded ||
                           lane == AvPlayer::AudioLane::kNoOutput;
    const bool audioFaultOnFile = fMediaFault && fMediaFault->first == MediaFault::kAudio &&
                                  fMediaFault->second == path;
    if (!laneFault) {
        mediaFaultClearedLocked(MediaFault::kAudio, path);
    } else if (!audioFaultOnFile) {
        mediaFailedLocked(
            MediaFault::kAudio, path,
            !fExpectedAudioLoss.empty()
                ? fExpectedAudioLoss
                : (lane == AvPlayer::AudioLane::kNoOutput
                       ? "background audio has no output (no audio server connection)"
                       : "background audio is not audible (no source could be opened, or "
                         "the audio server refused the stream)"));
    }
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
    // semaphore: nothing ever sampled it. The release carries the
    // replaced submission's sequence as its proof of ownership (6.6a).
    if (dropped && fVideoPresenter && dropped->token != submission.token) {
        fVideoPresenter->onDropped(dropped->token, dropped->sequence);
    }
    wakeRenderThread();
}

// --- background transport control (task 6.7d, review R19) ---

void OutputSession::requestMediaSeek(double seconds) {
    fPendingSeekSeconds.store(seconds > 0.0 ? seconds : 0.0, std::memory_order_relaxed);
    fPendingTransportOp.store(kOpSeek, std::memory_order_release);
    wakeRenderThread();
}

void OutputSession::requestMediaPause() {
    fPendingTransportOp.store(kOpPause, std::memory_order_release);
    wakeRenderThread();
}

void OutputSession::requestMediaPlay() {
    fPendingTransportOp.store(kOpPlay, std::memory_order_release);
    wakeRenderThread();
}

std::vector<OutputSession::ScheduledOp> OutputSession::parseScheduledOps(
    const std::string& spec) {
    // "seek@900:0.6,seek@1100:0.2,pause@2400,play@3200" — milliseconds after
    // the transport opened. Anything malformed is reported and skipped: a
    // fixture that quietly mis-parsed would assert on an op that never ran.
    std::vector<ScheduledOp> ops;
    size_t start = 0;
    while (start < spec.size()) {
        const size_t comma = spec.find(',', start);
        const std::string entry =
            spec.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        start = comma == std::string::npos ? spec.size() : comma + 1;
        if (entry.empty()) {
            continue;
        }
        const size_t at = entry.find('@');
        const size_t colon = entry.find(':', at == std::string::npos ? 0 : at);
        ScheduledOp op;
        const std::string kind = at == std::string::npos ? entry : entry.substr(0, at);
        if (kind == "seek") {
            op.kind = kOpSeek;
        } else if (kind == "pause") {
            op.kind = kOpPause;
        } else if (kind == "play") {
            op.kind = kOpPlay;
        } else {
            std::fprintf(stderr, "openshow-worker: test transport op '%s' is unknown\n",
                         entry.c_str());
            continue;
        }
        if (at == std::string::npos) {
            std::fprintf(stderr, "openshow-worker: test transport op '%s' has no time\n",
                         entry.c_str());
            continue;
        }
        try {
            op.atMs = std::stoi(entry.substr(at + 1, colon == std::string::npos
                                                          ? std::string::npos
                                                          : colon - at - 1));
            if (op.kind == kOpSeek) {
                if (colon == std::string::npos) {
                    std::fprintf(stderr,
                                 "openshow-worker: test seek '%s' has no target\n", entry.c_str());
                    continue;
                }
                op.seconds = std::stod(entry.substr(colon + 1));
            }
        } catch (const std::exception&) {
            std::fprintf(stderr, "openshow-worker: test transport op '%s' does not parse\n",
                         entry.c_str());
            continue;
        }
        ops.push_back(op);
    }
    return ops;
}

void OutputSession::applyTransportOps() {
    // The transport's thread: AvPlayer's fan-out walks the attached-pipeline
    // list, which only the thread swapping transports may touch (6.6b's
    // rule). A seek also blocks until the audio answers — normally a few
    // milliseconds, bounded at 500 — because the clock must not be re-anchored
    // on a jump the media did not make; 6.9b moves that wait off this loop.
    const int op = fPendingTransportOp.exchange(kOpNone, std::memory_order_acq_rel);
    bool applied = false;
    if (op != kOpNone) {
        applyTransportOp(op, fPendingSeekSeconds.load(std::memory_order_relaxed));
        applied = true;
    } else {
        // The test schedule rides the same seam, so the scenarios drive the
        // transport exactly as 6.15a's RPC will. Ops fire in schedule order;
        // everything whose time has passed goes out on this pass (the last
        // seek wins, exactly as a burst of operator clicks would), and the
        // cursor only advances past what was applied.
        const double elapsedMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                      fScheduledStart)
                .count();
        while (fScheduledCursor < fScheduledOps.size() &&
               elapsedMs >= fScheduledOps[fScheduledCursor].atMs) {
            applyTransportOp(fScheduledOps[fScheduledCursor].kind,
                             fScheduledOps[fScheduledCursor].seconds);
            ++fScheduledCursor;
            applied = true;
        }
    }
    if (!applied) {
        return;
    }
    // An op the operator issued is an op status must report on this pass. A
    // held transport presents nothing after the hold — and the per-frame
    // refresh is what normally carries "playing" — so without this the fact
    // would sit stale until the half-second heartbeat noticed it (task 6.7d,
    // review R19: a pause that still reads "playing" is the swallowed op
    // wearing a different hat).
    std::lock_guard<std::mutex> lock(fMutex);
    refreshMediaStateLocked();
}

void OutputSession::applyTransportOp(int op, double seconds) {
    if (fBackground == nullptr) {
        std::fprintf(stderr, "openshow-worker: transport op ignored, no background is open\n");
        return;
    }
    if (op == kOpPause) {
        fAudio.pause();
        std::fprintf(stderr, "openshow-worker: transport held at %.3f s\n",
                     fBackground->position());
        return;
    }
    if (op == kOpPlay) {
        fAudio.play();
        std::fprintf(stderr, "openshow-worker: transport resumed\n");
        return;
    }
    std::string error;
    if (fAudio.seekTo(seconds, &error)) {
        std::fprintf(stderr, "openshow-worker: transport seek landed at %.3f s\n", seconds);
        // A seek that landed is the recovery for a previous refusal on this
        // asset (typed exactly as such): the parts agree again.
        mediaFaultCleared(MediaFault::kSync, fAppliedBackground.assetPath);
        return;
    }
    // The audio refused the jump. The picture moved (the video seek applies
    // on its own thread), so the audience now hears one position and sees
    // another — that is what the operator has to be told, and the reported
    // position stays whatever the transport actually presents.
    mediaFailed(MediaFault::kSync, fAppliedBackground.assetPath,
                "background seek to " + std::to_string(seconds) +
                    " s was refused: " + (error.empty() ? "no reason given" : error));
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

void OutputSession::applyPendingLayerChanges(const FrameSnapshot& snapshot) {
    // The snapshot is captured and drawn by this same thread: read it
    // directly, no second opinion from the guarded desired state (that
    // read is what let a late command ride into an early frame). Staging
    // fAppliedMask here is bookkeeping for the frame's commit; the status
    // publication itself happens only after a successful present.
    const uint32_t desired = snapshot.mask;
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
}

void OutputSession::applyPendingDisplayChange(const FrameSnapshot& snapshot) {
    const int desired = snapshot.monitor;
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
    // monitor/monitorName join the other applied fields in the frame's
    // commit block: a moved window is on air when its frame presents.
}

void OutputSession::applyPendingSceneChange(const FrameSnapshot& snapshot) {
    const uint64_t desired = snapshot.scene;
    if (desired == fAppliedScene) {
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
    // Task 3.1: the applied scene is what the production layers draw. The
    // store may evict its entry later, so a copy shared with the
    // composition keeps the on-air scene alive as long as it is drawn —
    // here that copy is the snapshot's pin, so what is applied is exactly
    // what the run loop captured, whatever commands arrived since.
    fAppliedScene = desired;
    // Task 5.5: the render thread keeps its own handle on the on-air scene
    // (the composition gets a copy) — the draw path and the background
    // transport both read the same thing from here.
    fRenderScene = snapshot.content;
    fScene->setActiveScene(snapshot.content, snapshot.kind);
    if (fBackgroundMedia) {
        // The transport follows the scene in the same breath — and this
        // whole function runs ahead of frame()'s graphics queue lock
        // precisely because the swap's join() waits on threads that may
        // be blocked acquiring it (deadlock otherwise).
        applyBackgroundTransport(snapshot.content ? snapshot.content->background
                                                 : openshow::scene::Background{});
    }
    // active/revision are NOT published here (task 6.4, review R4): the
    // apply only stages the scene. The commit after a successful present
    // is what tells callers the scene is on air.
}

void OutputSession::syncScenePins() {
    // fMutex held. The cache lives in the renderer and its pin operations
    // are map arithmetic under the cache's own lock — no disk, no waiting —
    // so this is safe from both the RPC thread and the frame loop.
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
    // full every time: the bounded store, the queued present, the applied
    // scene, and the render thread's on-air copy (a fade still draws the
    // scene it came from until the commit says the transition settled).
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
    };
    for (const auto& entry : fScenes) {
        collect(entry.second);
    }
    const auto pinned = [&](uint64_t revision) {
        const auto found = fScenes.find(revision);
        if (found != fScenes.end()) {
            collect(found->second);
        } else if (fRenderScene) {
            collect(*fRenderScene);  // store-evicted but still on air
        }
    };
    pinned(fDesiredScene);
    pinned(fAppliedScene);
    if (fRenderScene) {
        collect(*fRenderScene);
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
        // Colour fill or still image: no video transport, so the media
        // status goes idle (a stale video error is not the story of the
        // scene now on air). A still is different: the picture it could not
        // decode IS the story, and the colour fill is a degradation rather
        // than a success (task 6.7c, review R18). So the asset the audience
        // is looking at is always NAMED, and the error is set only when
        // that asset is what cannot be shown.
        //
        // The cache is consulted BEFORE fMutex, so even the pure lookup of
        // 6.10 keeps this probe out of the lock that status and layer
        // commands wait on. Task 6.10 made it a lookup with no disk access:
        // the pin taken at prepare guarantees the preload's answer is
        // still resident, so a miss here genuinely means "no picture for
        // this asset", which is exactly what the report below says.
        const bool brokenStill =
            wanted.hasImage && !wanted.isVideo &&
            (fScene == nullptr || !fScene->imageCache()->has(wanted.assetPath));
        std::lock_guard<std::mutex> lock(fMutex);
        fStatus.media.playing = false;
        fStatus.media.position = -1.0;
        fStatus.media.audio = "none";
        // The asset the audience is looking at, whether or not it works:
        // media.path always names what media.error describes, and a healthy
        // still is an asset on air like any other. (The identity short-cut
        // above means a re-present of the SAME still cannot rewrite this,
        // which is why the rule has to hold in both directions.)
        fStatus.media.path = wanted.hasImage ? wanted.assetPath : std::string();
        if (brokenStill) {
            // R18's rule read from the cache's side: the picture the
            // audience should be seeing is not here, and the colour fill
            // they get instead is a degradation, so it is NAMED. The
            // prepare's own preload already reports a file it could not
            // decode (with the better message — the codec's reason); this
            // half covers the picture that was there and is not any more,
            // which with the 6.10 pins should be a cache bug rather than
            // a show event, and must never pass as success. The apply
            // path runs on an identity change, not per frame, so this
            // cannot turn into notification noise.
            const bool alreadyReported = fMediaFault && fMediaFault->first == MediaFault::kImage &&
                                         fMediaFault->second == wanted.assetPath;
            if (!alreadyReported) {
                mediaFailedLocked(MediaFault::kImage, wanted.assetPath,
                                  "background image: '" + wanted.assetPath +
                                      "' is not in the decoded cache; the slide is drawing "
                                      "its colour fill");
            }
        } else {
            fStatus.media.error.clear();
            fMediaFault.reset();
        }
        return;
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
        mediaFailed(MediaFault::kVideo, wanted.assetPath, "background video: " + error);
        return;  // the colour fill stands; the scene stays legible
    }

    // Embedded audio, when it can be decoded, becomes the audible mix and
    // the clock video paces against (the M5 checkpoint rule); a silent
    // file plays silent on the player's monotonic clock. Either way the
    // player is up: transport commands have one owner.
    openshow::media::MediaInfo info;
    AvPlayer::Config config;
    config.loop = true;
    bool wantsAudio = false;
    // What the audience is missing, when the clip's audio cannot be heard
    // (task 6.7c, review R18). AvPlayer cannot know this: a stream no
    // decoder can handle never reaches it as a source at all, so its own
    // lane would honestly report "none" — silence by nature — while the
    // file plainly carries sound. The session knows, and says so.
    std::string audioLoss;
    if (openshow::media::probeMedia(wanted.assetPath, &info, &error) ==
        openshow::media::ProbeStatus::kOk) {
        if (!info.audioStreams.empty()) {
            if (info.audioStreams.front().decoderAvailable) {
                config.videoAudioPath = wanted.assetPath;
                wantsAudio = true;
            } else {
                audioLoss = "background video has an audio stream no decoder can handle; "
                            "playing muted";
            }
        }
    } else {
        // The container opened a moment ago, so a second probe failing is
        // a curiosity rather than a story; it stays in the log. (Nothing
        // was expected yet, so no audio is claimed lost.)
        std::fprintf(stderr, "openshow-worker: background audio probe %s: %s\n",
                     wanted.assetPath.c_str(), error.c_str());
    }
    if (!fAudio.start(config, &error)) {
        std::fprintf(stderr, "openshow-worker: background audio: %s (video continues, "
                             "paced by the monotonic clock)\n",
                     error.c_str());
        if (wantsAudio) {  // only a real loss when audio was expected
            audioLoss = "background video audio failed to start: " + error + "; playing muted";
        }
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
        mediaFailed(MediaFault::kVideo, wanted.assetPath, "background decode: " + error);
        // No quiesce boundary is needed here (6.6b): a false start() means
        // no pacing or decode thread was ever spawned, so nothing can read
        // the presenter or the clock after this point. The audio player
        // started for this transport goes down with it (idempotent).
        setVideoClock(nullptr);
        fVideoPresenter = nullptr;
        fAudio.detachPipeline(fBackground.get());
        fAudio.stop();
        fBackground->close();
        fBackground.reset();
        return;
    }
    // The video transport is up. The audio's bad news (if any) is recorded
    // on top of the success, in that order: mediaPlaying clears the previous
    // asset's reports, and this asset's known audio loss then rides them.
    // The loss also drives the lane (a stream no decoder can handle never
    // reaches AvPlayer as a source, so its own lane would truthfully say
    // "none"), and it belongs to this identity: a new background recomputes
    // it from scratch.
    mediaPlaying(wanted);
    if (!audioLoss.empty()) {
        {
            std::lock_guard<std::mutex> lock(fMutex);
            fExpectedAudioLoss = audioLoss;
        }
        mediaFailed(MediaFault::kAudio, wanted.assetPath, audioLoss);
    }
    // The M5 clock rule, visible at transport start (checkpoint rule:
    // "use the audio clock when audio is present and a monotonic clock
    // otherwise"): one line naming which clock now paces the video.
    std::fprintf(stderr, "openshow-worker: background transport %s (%s clock)\n",
                 wanted.assetPath.c_str(),
                 fAudio.audioLive() ? "audio" : "monotonic");
    // Test-only transport script (task 6.7d, review R19), armed per transport
    // and applied by the render loop through the same seam 6.15a's RPC will
    // use. Production leaves the variable unset and this stays empty.
    fScheduledCursor = 0;
    fScheduledStart = std::chrono::steady_clock::now();
    const char* const opSpec = std::getenv("OPENSHOW_TEST_TRANSPORT_OPS");
    fScheduledOps = opSpec != nullptr ? parseScheduledOps(opSpec) : std::vector<ScheduledOp>{};
    if (!fScheduledOps.empty()) {
        std::fprintf(stderr, "openshow-worker: %zu test transport ops armed for %s\n",
                     fScheduledOps.size(), wanted.assetPath.c_str());
    }
    fNeedsPresent = true;  // the head frame arrives soon; redraw when it does
}

void OutputSession::teardownBackgroundTransport() {
    mediaStopped();
    // The transport this script was armed for is going away (task 6.7d): a
    // pending intent and any later scheduled ops must not fire against a
    // successor that was never asked for them.
    fScheduledOps.clear();
    fScheduledCursor = 0;
    fPendingTransportOp.store(kOpNone, std::memory_order_release);
    if (!fBackground) {
        fAudio.stop();  // idempotent: a never-started player tears down clean
        fAppliedBackground = openshow::scene::Background{};
        return;
    }
    // Quiesce the producer FIRST (task 6.6b, review R6). submitVideo()
    // reads the presenter pointer outside fMutex, so the only moment it is
    // safe to change anything the sink touches — the presenter, the clock,
    // the pending seam — is behind the stop/join generation boundary:
    // requestStop wakes every wait the pacing thread can park in (clock
    // sleep, ring poll, uploader commit) and join closes the window for
    // good. After it the render thread owns the seam exclusively until the
    // next start(), which creates the new pacing thread after its presenter
    // is already published (thread creation orders the two).
    fBackground->requestStop();
    fBackground->join();
    // Presenter and clock down next, then the seam forgets the slots:
    // their GPU lifetime belongs to the pipeline being closed. Outstanding
    // leases die with the pipeline — the joined producer parked on one was
    // woken by requestStop, and close() releases every slot (task 6.6a).
    fVideoPresenter = nullptr;
    setVideoClock(nullptr);
    fAudio.detachPipeline(fBackground.get());
    {
        std::lock_guard<std::mutex> lock(fMutex);
        fPendingVideo.reset();
        fDrawnVideo.reset();
    }
    fBackground->close();
    fBackground.reset();
    // The audio player outlived no clock reader: its clock function was
    // only ever handed to the (now joined) pipeline, so stopping it here
    // cannot race a pacing thread either.
    fAudio.stop();
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

void OutputSession::publishPreviewFrame(double elapsed, SkImage* video,
                                        uint64_t snapshotRevision) {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps = 0;
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
    }
    // Task 6.4 (review R4): the tag names the revision of the content this
    // publish draws — the snapshot the presenting frame drew, or the last
    // presented frame on a republish. Reading the latest desired fRevision
    // here tagged frames with scenes nobody had drawn yet.
    const uint32_t revision = static_cast<uint32_t>(snapshotRevision);
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
    ready.stream = stream->name();  // ownership identity (task 6.5)
    std::lock_guard<std::mutex> lock(fMutex);
    fPendingReady[slot] = ready;
    ++fStatus.preview.published;
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
    applyPreviewState();
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
        // The held frame's lease (task 6.6a, review R6): fDrawnVideo's
        // image is sampled by every redraw until this decision replaces
        // it, so the presenter keeps its slot out of the producer until
        // onReplaced names it. The replaced frame's last sampling already
        // handed back its doneVk (each frame's onPresented runs at the
        // end of its own iteration, before the next decision), which is
        // what makes releasing here safe for the producer.
        std::optional<VideoSubmission> toDraw;
        std::optional<VideoSubmission> replaced;
        {
            std::lock_guard<std::mutex> lock(fMutex);
            if (fPendingVideo && fVideoPresenter &&
                videoSubmissionDue(*fPendingVideo,
                                   videoClockSecondsLocked(std::chrono::steady_clock::now()))) {
                replaced = fDrawnVideo;
                toDraw = fPendingVideo;
                fDrawnVideo = toDraw;
                fPendingVideo.reset();
            }
        }
        if (replaced && fVideoPresenter && replaced->token != toDraw->token) {
            fVideoPresenter->onReplaced(replaced->token, replaced->sequence);
        }
        if (!toDraw && fDrawnVideo) {
            toDraw = fDrawnVideo;
        }
        if (toDraw && fVideoPresenter) {
            videoImage = fVideoPresenter->acquireImage(toDraw->token, toDraw->sequence);
            if (!videoImage) {
                // The token no longer yields an image for its generation:
                // stop sampling it and give back exactly the lease this
                // submission owns (nothing if another generation already
                // holds the slot).
                fVideoPresenter->onReplaced(toDraw->token, toDraw->sequence);
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
        // always are). NOTE: SceneRenderer::draw leaves its output-rule
        // transform (the fit of the authored 1920x1080 canvas) on this
        // canvas — the dev video rect is in swapchain pixels, so it must
        // be drawn on the identity.
        const bool videoIsBackground =
            videoImage != nullptr && fRenderScene && fRenderScene->background.isVideo;
        fScene->draw(surface->getCanvas(), fSwapchain->width(), fSwapchain->height(), elapsed,
                     statusLine, videoIsBackground ? videoImage : nullptr);
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
            if (videoDone != VK_NULL_HANDLE && fVideoPresenter) {
                fVideoPresenter->onPresented(videoToken, videoDone);
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
    }

    // Preview (task 1.4) runs after the audience frame is presented: the
    // extra pass never delays the swapchain work it follows, and its
    // throttling, content gate and drop policy live in publishPreviewFrame.
    // The tag is the snapshot's revision — the scene these pixels carry —
    // not whatever a later command has meanwhile made desired (task 6.4).
    if (fFrames > 0) {
        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - fStartTime)
                .count();
        publishPreviewFrame(elapsed, videoImage, snapshot.revision);
    }

    // The present's sampling obligation is handed back last:
    // publishPreviewFrame's readback above is CPU-synchronous, so by now
    // every sampling of the slot this frame queued — output and preview —
    // is complete, and doneVk's signal puts a GPU fence behind it. This
    // does NOT free the slot: the frame stays leased while fDrawnVideo
    // holds it (task 6.6a), and the producer waits for the lease to end
    // AND for every registered doneVk before writing the slot again.
    if (videoDone != VK_NULL_HANDLE && fVideoPresenter) {
        fVideoPresenter->onPresented(videoToken, videoDone);
    }

    if (presentedFrame) {
        fPresentedRevision = snapshot.revision;
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
        // returned.
        refreshMediaStateLocked();
        // The commit (task 6.4, review R4): applied state is published
        // only from a frame that reached the compositor, and it describes
        // exactly what that frame drew — the staged applied fields, all
        // written by this frame's apply* calls, plus the snapshot's
        // revision. Queued state (pending) is recomputed against the newest
        // accepted command, so it settles only when the audience really
        // has it; transitioning says the committed switch has not visually
        // settled yet.
        if (presentedFrame) {
            fStatus.layerMask = fAppliedMask;
            fStatus.monitor = fAppliedMonitor;
            fStatus.monitorName =
                fAppliedMonitor >= 0 && fAppliedMonitor < static_cast<int>(fDisplays.size())
                    ? fDisplays[fAppliedMonitor].name
                    : "";
            fStatus.scene.active = fAppliedScene;
            fStatus.scene.transitioning = fScene && fScene->transitionRunning();
            fStatus.scene.pending = fDesiredScene == fAppliedScene ? 0 : fDesiredScene;
            fStatus.revision = snapshot.revision;
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
    {
        std::lock_guard<std::mutex> lock(fMutex);
        refreshMediaStateLocked();
    }

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
        // Transport control first (task 6.7d): a seek or a hold the audience
        // asked for changes what this pass must decide, and the apply belongs
        // on this thread (see applyTransportOps for the discipline).
        applyTransportOps();

        bool presentNow = false;
        bool previewDue = false;
        FrameSnapshot snapshot;
        auto wakeAt = hardDeadline;
        {
            std::lock_guard<std::mutex> lock(fMutex);
            presentNow = fRevision != fPresentedRevision ||
                         fDesiredMonitor != fAppliedMonitor ||
                         fDesiredPreview.generation != fAppliedPreviewGeneration ||
                         fNeedsPresent || fWindow->framebufferResized();
            // The revision this frame represents, captured in the same
            // critical section as the decision and therefore ahead of
            // every apply the frame performs (see frame()'s header note).
            if (!presentNow && fPendingVideo && fVideoPresenter) {
                // The seam's own gate (task 6.7d, review R19): a frame is
                // due against its DEADLINE in the clock's domain, never
                // against its media pts — those two disagree by a pause,
                // a seek offset or a loop wrap, and comparing the wrong one
                // either presents early or holds the picture for seconds.
                const double clock = videoClockSecondsLocked(now);
                if (videoSubmissionDue(*fPendingVideo, clock)) {
                    presentNow = true;  // frame() re-checks and consumes it
                } else {
                    wakeAt = std::min(
                        wakeAt,
                        now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                  std::chrono::duration<double>(
                                      fPendingVideo->deadlineSeconds - clock)));
                }
            }
            // The capture (task 6.4): taken in the same critical section as
            // the decision — unconditionally, because housekeeping can add
            // a present reason after the lock — so this snapshot is
            // everything owed as of now and nothing that arrives later.
            // frame() draws exactly it and its successful present credits
            // exactly its revision.
            snapshot.revision = fRevision;
            snapshot.mask = fDesiredMask;
            snapshot.scene = fDesiredScene;
            snapshot.kind = fDesiredTransition;
            snapshot.monitor = fDesiredMonitor;
            if (snapshot.scene != fCapturedSceneId) {
                // Entries are immutable per id and fDesiredScene is
                // eviction-exempt, so the lookup under this same lock is
                // the one pin the snapshot needs (shared across all
                // captures of one scene id).
                const auto entry = fScenes.find(snapshot.scene);
                fCapturedScene = entry != fScenes.end()
                                     ? std::make_shared<const openshow::scene::Scene>(
                                           entry->second)
                                     : nullptr;
                fCapturedSceneId = snapshot.scene;
            }
            snapshot.content = fCapturedScene;
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
            const double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - fStartTime)
                    .count();
            SkImage* held = nullptr;
            if (fVideoPresenter && fDrawnVideo) {
                held = fVideoPresenter->acquireImage(fDrawnVideo->token,
                                                     fDrawnVideo->sequence);
            }
            // No present this round, so the honest tag is the revision of
            // the last presented frame — the content a republish redraws
            // (task 6.4: presentNow==false means nothing newer was applied).
            publishPreviewFrame(elapsed, held, fPresentedRevision);
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
