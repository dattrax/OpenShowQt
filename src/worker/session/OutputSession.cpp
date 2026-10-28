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
    // Task 6.11a (review R13): the cost of an expensive preview target,
    // paid inside the measured region of publishPreviewFrame so the
    // audience-budget policy is tested against real milliseconds.
    int previewDelayMs = 0;       // OPENSHOW_FAULT_PREVIEW_MS
    Faults() {
        const auto read = [](const char* name) {
            const char* value = std::getenv(name);
            return value != nullptr ? std::atoi(value) : 0;
        };
        applyDelayMs = read("OPENSHOW_FAULT_SCENE_APPLY_MS");
        scenePresentFails = read("OPENSHOW_FAULT_SCENE_PRESENT_FAILS");
        holdMs = read("OPENSHOW_FAULT_HOLD_MS");
        previewDelayMs = read("OPENSHOW_FAULT_PREVIEW_MS");
    }
};

// Render-thread only (mutated by frame()/applyPendingSceneChange alone).
Faults& faults() {
    static Faults instance;
    return instance;
}

// The audience's frame period at 60 Hz, and the share of it the preview is
// lent (task 6.11a, review R13). The preview is composed on the render
// thread, so every millisecond it takes is a millisecond the audience's
// next frame waits: a preview frame that costs more than its share defers
// its own next attempt until the average is inside the share again, rather
// than taking its cost out of every audience frame.
constexpr double kAudienceFrameSeconds = 1.0 / 60.0;
constexpr double kPreviewShareOfAudienceFrame = 0.25;
constexpr double kPreviewBudgetSeconds =
    kAudienceFrameSeconds * kPreviewShareOfAudienceFrame;

// The slide list's thumbnails (task 6.16b, review R16): what the list's own
// renderer may hold in decoded pixels, and how far ahead of the draw head
// its background decoder is allowed to run.
//
// Both numbers are arithmetic rather than taste. A cached entry is a decode
// that has been brought inside the batch's own box, and the protocol caps that
// box at 512x512 — so the largest picture this cache can ever hold is 1 MiB of
// premul pixels, whatever the file behind it was (a 40-megapixel JPEG costs
// that much transient memory while it decodes and nothing after). Thirty-one
// of them, the whole permitted batch at the largest legal size, is 31 MiB; the
// lookahead of four is what stops the working set from being that: the decoder
// runs four backgrounds ahead of the frame the render thread has actually
// taken, so four in flight is 4 MiB against a 12 MiB ceiling that a scrolling
// document cannot reach. The budget is the backstop for a band that is drawn
// and re-drawn; the lookahead is the bound on how fast memory can grow at all.
constexpr std::size_t kThumbnailImageBudget = 12u * 1024u * 1024u;
constexpr std::size_t kThumbnailDecodeLookahead = 4;

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
    return true;
}

void OutputSession::close() {
    // The background transport decodes into frame slots on this device
    // and context, so it goes before every GPU resource it references
    // (task 5.5). fAudio.stop() is idempotent: a never-started player
    // tears down as cleanly as a running one.
    teardownBackgroundTransport();
    // The standalone track is deliberately NOT part of the background
    // transport's lifetime (task 6.15b) — which is exactly why close()
    // must end it by name. Live first, teardown's rule: a command that
    // races this refuses, never lands on a stopping player; the sticky
    // error keeps its path.
    fStandaloneLive.store(false, std::memory_order_release);
    fMediaSourceMask.store(fMediaSourceMask.load(std::memory_order_relaxed) & ~2,
                           std::memory_order_release);
    fStandalonePlayer.stop();  // idempotent
    {
        std::lock_guard<std::mutex> lock(fStandaloneSwapMutex);
        fStandaloneSwap.reset();  // nobody left to apply it
    }
    {
        std::lock_guard<std::mutex> lock(fMutex);
        fStatus.media.standalone.live = false;
        fStatus.media.standalone.playing = false;
        fStatus.media.standalone.position = -1.0;
        fStatus.media.standalone.duration = -1.0;
        fStatus.media.standalone.audio = "none";
        fStatus.media.standalone.loops = 0;
        rebuildMediaSourcesLocked();  // drops the track's row with its mask bit
    }
    if (fSwapchain) {
        fSwapchain->destroy();
        fSwapchain.reset();
    }
    fPreviewSurface.reset();
    {
        std::lock_guard<std::mutex> lock(fMutex);
        fPreviewStream.reset();  // unlinks the preview segment (unless a release call still holds it)
        // The off-air target's content pin goes with the stream it was drawn
        // for (task 6.16a): a closed session must not sit on a scene the
        // store would otherwise be free to evict, and no later pass exists
        // to draw it.
        fDesiredSelection = SelectionDesired{};
        fAppliedSelectionGeneration = fDesiredSelection.generation;
        fStatus.preview.selection = PreviewStatus::Selection{};
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

bool OutputSession::prepareScene(const openshow::scene::Scene& scene,
                                 uint64_t* revisionOut) {
    {
        std::lock_guard<std::mutex> lock(fMutex);
        // The store mints the id and bounds itself (task 6.18a): nothing here
        // touches the output, so no revision moves and no frame is provoked.
        const uint64_t revision = fStore.prepare(scene);
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
    if (fScene == nullptr) {
        return;
    }
    // Foreground pictures (task 6.17b): the same discipline as the
    // background's still — decoded NOW, on this RPC thread, through the
    // REFRESHING entry point so a file that was missing and has been put
    // back comes back at the next prepare — and every failure NAMED on
    // stderr while the old scene is still on air. The MEDIA LANE
    // deliberately stays background-only: status.media names what the
    // audience's transport is showing, and a stack picture's decode error
    // would evict that truth with a file nobody is playing. A missing
    // foreground picture is honest on screen — its box shows what is
    // under it, which is visibly not the slide the operator composed —
    // and per-item fault reporting is the R19 media lane's to grow.
    for (const openshow::scene::SceneItem& item : scene.items) {
        if (!item.isImage || item.assetPath.empty()) {
            continue;
        }
        std::string error;
        if (fScene->imageCache()->refreshFor(item.assetPath, &error) == nullptr) {
            std::fprintf(stderr, "openshow-worker: foreground image %s: %s\n",
                         item.assetPath.c_str(), error.c_str());
        }
    }
    const openshow::scene::Background& background = scene.background;
    if (!background.hasImage) {
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
    // A new stream is a new generation domain for the off-air target
    // (task 6.16a): the accepted request resets, and the selection itself
    // stands down until the frontend asks again — pixels of a scene nobody
    // re-asked for have no stream to arrive on.
    fDesiredSelection = SelectionDesired{};
    fAppliedSelectionGeneration = fDesiredSelection.generation;
    fStatus.preview.selection.active = false;

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
        // No stream, no off-air target (task 6.16a): the content copy goes
        // back with it, so a stopped preview does not sit on a scene the
        // store would otherwise be free to evict.
        fDesiredSelection = SelectionDesired{};
        fAppliedSelectionGeneration = fDesiredSelection.generation;
        fStatus.preview.selection = PreviewStatus::Selection{};
        // And no list either (task 6.16b). The batch is stood down completely:
        // its scenes go, the companion renderer goes with its decoded
        // pictures (12 MiB of them at most, and nothing left to draw them
        // into), and the accepted generation resets so a client that asks
        // again after a restart is not refused by its own stale counter.
        // None of this touched the store or the pins — thumbnails were never
        // in either — so syncScenePins() has nothing to learn here.
        fThumbnailQueue.clear();
        fThumbnailRequest = 0;
        fThumbnailWidth = 0;
        fThumbnailHeight = 0;
        fThumbnailScene.reset();
        fStatus.preview.thumbnails = PreviewStatus::Thumbnails{};
        syncScenePins();  // the selection's still is no longer held
    }
    wakeRenderThread();
}

bool OutputSession::selectPreviewScene(uint64_t revision, std::uint32_t request,
                                       std::string* error) {
    std::unique_lock<std::mutex> lock(fMutex);
    if (!fDesiredPreview.enabled) {
        // The honest refusal, in the order the operator can act on it: no
        // stream means there is nowhere for these pixels to go. (The gate is
        // the DESIRED stream, not status().preview.running: preview.start is
        // acknowledged before the render thread has built the segment, and a
        // selection that arrives in that window is a normal race, not a
        // refusal.)
        if (error != nullptr) {
            *error = "no preview stream is running: preview.start first";
        }
        return false;
    }
    // THE GENERATION GATE. `request` is the frontend's own counter, so the
    // rule is one-directional: it must come strictly after the accepted one
    // in modular order. A frontend that replays an ask it already made — a
    // retry after a dropped reply, a frame its socket buffered, a signal
    // fired twice — is refused here rather than resurrecting imagery of a
    // scene the operator has moved on from. 0 means "stand down"; it is a
    // change like any other and needs a new generation too.
    const std::uint32_t accepted = fDesiredSelection.request;
    if (static_cast<std::int32_t>(request - accepted) <= 0) {
        if (error != nullptr) {
            *error = "preview.select request " + std::to_string(request) +
                     " does not follow the accepted " + std::to_string(accepted) +
                     " (the frontend's counter must advance)";
        }
        return false;
    }
    ActiveScene content;
    if (revision != 0 && !fStore.pin(revision, &content)) {
        // The store is bounded, so this is the ordinary way to be wrong: the
        // frontend named a revision that no longer exists (or never did). It
        // must re-prepare rather than wait for imagery that is never coming.
        if (error != nullptr) {
            *error = "preview.select scene " + std::to_string(revision) +
                     " is not in the prepared store";
        }
        return false;
    }
    // The pin comes from the store rather than a second lookup on the render
    // thread: entries are immutable per revision, and the store may evict this
    // one before the pass that draws it (R4's rule, applied to a target that
    // has no present to pin it).
    fDesiredSelection.content = std::move(content);
    fDesiredSelection.revision = revision;
    fDesiredSelection.request = request;
    ++fDesiredSelection.generation;
    // `active` is the accept, not the draw: it says this stream has an
    // off-air target registered, which is what the frontend's status mirror
    // needs in order to know its ask was understood. Whether the frame made
    // it out is what published/dropped/stale are for.
    fStatus.preview.selection.active = revision != 0;
    fStatus.preview.selection.revision = revision;
    fStatus.preview.selection.request = request;
    // The accepted scene's still joins the pin set NOW, not on the render
    // pass that draws it, so the frame loop never opens a file (task 6.10's
    // rule for a target that has no present to pin it through).
    syncScenePins();
    lock.unlock();
    // The ask is a reason to wake: an idle show with nothing else owed
    // sleeps in glfwWaitEventsTimeout, and an edit that waits for the next
    // housekeeping tick is an edit the operator watched not happen.
    wakeRenderThread();
    return true;
}

bool OutputSession::requestThumbnails(const std::vector<ThumbnailEntry>& batch,
                                      uint32_t width, uint32_t height,
                                      std::uint32_t request, ThumbnailAck* ack,
                                      std::string* error) {
    std::unique_lock<std::mutex> lock(fMutex);
    if (!fDesiredPreview.enabled) {
        // preview.select's first refusal, for the same reason: there is
        // nowhere for these pixels to go. The gate is the DESIRED stream, so
        // a band that arrives while preview.start is still being built is a
        // normal race and not a refusal.
        if (error != nullptr) {
            *error = "no preview stream is running: preview.start first";
        }
        return false;
    }
    // The extent is checked against the slot its pixels would live in, not
    // just against a box: the segment was reserved for the stream's own
    // frame size, so 512x512 is a legal thumbnail at 1024x576 and an
    // impossible one at 320x180. The same helper the consumer uses to bound
    // the copy declares the rule, which is how both halves stay honest about
    // a frame whose size is not the stream's.
    const std::size_t bytesPerSlot = protocol::previewSlotStrideBytes(
        fDesiredPreview.width, fDesiredPreview.height);
    if (!protocol::thumbnailExtentFits(width, height, bytesPerSlot)) {
        if (error != nullptr) {
            *error = "preview.thumbnails extent " + std::to_string(width) + "x" +
                     std::to_string(height) + " is outside " +
                     std::to_string(protocol::kMinThumbnailDimension) + ".." +
                     std::to_string(protocol::kMaxThumbnailDimension) +
                     " pixels, or does not fit a " + std::to_string(bytesPerSlot) +
                     "-byte preview slot";
        }
        return false;
    }
    // The queue is what bounds the memory a list can cost, so the cap on the
    // batch is the bound itself. Refusing beats truncating: a client that
    // asked for sixty slides is buggy, and drawing the first thirty of them
    // without saying so would have it believe it had pictures for sixty.
    if (batch.size() > protocol::kMaxThumbnailBatch) {
        if (error != nullptr) {
            *error = "preview.thumbnails takes at most " +
                     std::to_string(protocol::kMaxThumbnailBatch) + " scenes, asked for " +
                     std::to_string(batch.size()) + " (request the visible band)";
        }
        return false;
    }
    // THE GENERATION GATE, the batch's own. preview.select's rule applies to
    // a list exactly as it does to one edit: a replayed ask — a retry after a
    // dropped reply, a scroll handler fired twice — must not resurrect a
    // batch the operator has already replaced. An empty batch is a stand-down
    // and a stand-down is a change like any other, so it needs a new number
    // too.
    const std::uint32_t accepted = fThumbnailRequest;
    if (static_cast<std::int32_t>(request - accepted) <= 0) {
        if (error != nullptr) {
            *error = "preview.thumbnails request " + std::to_string(request) +
                     " does not follow the accepted " + std::to_string(accepted) +
                     " (the frontend's counter must advance)";
        }
        return false;
    }
    for (const ThumbnailEntry& entry : batch) {
        if (entry.key.empty()) {
            if (error != nullptr) {
                *error = "preview.thumbnails entry has an empty key";
            }
            return false;
        }
        if (entry.key.size() > protocol::kMaxThumbnailKeyLength) {
            if (error != nullptr) {
                *error = "preview.thumbnails key '" + entry.key.substr(0, 32) + "' is " +
                         std::to_string(entry.key.size()) + " characters, over the " +
                         std::to_string(protocol::kMaxThumbnailKeyLength) + " limit";
            }
            return false;
        }
    }

    // The list draws with a compositor of its own (see createCompanion), and
    // a batch of a DIFFERENT extent gets a fresh one: its decoded backgrounds
    // are sized for the old ask, and a cache keyed on the path alone would
    // otherwise serve a 160-pixel picture to a 512-pixel request. Building it
    // happens OUTSIDE the lock — assembling layers and borrowing a text
    // engine is font work, and fMutex is what the render thread takes to
    // snapshot a frame — and published under it.
    const bool extentChanged = fThumbnailWidth != width || fThumbnailHeight != height;
    std::shared_ptr<SceneRenderer> built;
    if (!batch.empty() && (fThumbnailScene == nullptr || extentChanged)) {
        if (fScene == nullptr) {
            if (error != nullptr) {
                *error = "preview.thumbnails: the session has no compositor to draw with";
            }
            return false;
        }
        lock.unlock();
        std::string buildError;
        built = SceneRenderer::createCompanion(*fScene, kThumbnailImageBudget, &buildError);
        if (!built && error != nullptr) {
            *error = "preview.thumbnails could not build the list's renderer: " + buildError;
        }
        lock.lock();
        if (!built) {
            return false;
        }
    }

    // REPLACE, which is the whole coalescing and cancellation rule: whatever
    // was queued for an older list stops being owed, and its scenes and
    // decoded pixels go with it. The newest band is the only debt.
    fThumbnailQueue.clear();
    fThumbnailQueue.reserve(batch.size());
    for (const ThumbnailEntry& entry : batch) {
        ThumbnailJob job;
        job.key = entry.key;
        // Per-slide id, and the only name a scene the store has never held
        // can have. Non-zero for the usual reason, and 32 bits is plenty
        // because an id only has to be unique among the slides of one batch.
        if (++fThumbnailIdCounter == 0) {
            fThumbnailIdCounter = 1;
        }
        job.id = fThumbnailIdCounter;
        // A transient copy: no store entry, no revision, no pin. This scene
        // cannot be presented by anything, which is the point — a slide list
        // is a picture of a slide, not show content.
        job.content = std::make_shared<const openshow::scene::Scene>(entry.scene);
        // What the command thread has to decode before this one can be
        // drawn: a still background (6.16b) and the stack's foreground
        // stills (6.17b), in draw order and deduplicated. A video belongs
        // to a transport this scene will never have, so its thumbnail gets
        // the colour fill the audience itself would draw with no frame to
        // show — the same honesty rule as an off-air edit (6.16a), not a
        // borrowed frame. The model never lets a foreground item name a
        // video; a hand-built wire that says otherwise resolves to a failed
        // decode, settled like any other.
        {
            std::vector<std::string> unique;
            const auto want = [&](const std::string& path) {
                if (!path.empty() &&
                    std::find(unique.begin(), unique.end(), path) == unique.end()) {
                    unique.push_back(path);
                }
            };
            if (entry.scene.background.hasImage && !entry.scene.background.isVideo) {
                want(entry.scene.background.assetPath);
            }
            for (const openshow::scene::SceneItem& item : entry.scene.items) {
                if (item.isImage) {
                    want(item.assetPath);
                }
            }
            job.imagePaths = std::move(unique);
            job.pathSettled.assign(job.imagePaths.size(), false);
        }
        if (ack != nullptr) {
            ack->accepted.emplace_back(job.key, job.id);
        }
        fThumbnailQueue.push_back(std::move(job));
    }
    if (built) {
        fThumbnailScene = std::move(built);
    }
    fThumbnailRequest = request;
    fThumbnailWidth = width;
    fThumbnailHeight = height;
    // The accept, not the draw: this stream now owes a band.
    fStatus.preview.thumbnails.active = !fThumbnailQueue.empty();
    fStatus.preview.thumbnails.request = request;
    fStatus.preview.thumbnails.width = width;
    fStatus.preview.thumbnails.height = height;
    syncThumbnailCountersLocked();
    if (ack != nullptr) {
        ack->request = request;
        ack->width = width;
        ack->height = height;
    }
    lock.unlock();
    // A band the operator is looking at is a reason to wake: an idle show
    // sleeps, and a list that fills in only at the next housekeeping tick
    // looks broken even though every frame of it is correct.
    wakeRenderThread();
    return true;
}

void OutputSession::syncThumbnailCountersLocked() {
    std::size_t pending = 0;
    for (const ThumbnailJob& job : fThumbnailQueue) {
        if (!job.decoded) {
            ++pending;
        }
    }
    fStatus.preview.thumbnails.queued = fThumbnailQueue.size();
    fStatus.preview.thumbnails.decodePending = pending;
    // The list's own picture memory, reported rather than assumed: this is
    // where a frontend (or a test) can see that scrolling a document spends
    // a bounded number of bytes in a cache that is not the show's. The lock
    // order is the session's then the cache's, never the reverse — the cache
    // knows nothing about the session, so no path can invert it.
    if (fThumbnailScene != nullptr) {
        const SceneImageCache::Stats images = fThumbnailScene->imageCache()->stats();
        fStatus.preview.thumbnails.decodedBytes = images.decodedBytes;
        fStatus.preview.thumbnails.entries = images.entries;
        fStatus.preview.thumbnails.evictions = images.evictions;
        fStatus.preview.thumbnails.budget = images.budget;
    }
}

OutputSession::ThumbnailJob* OutputSession::thumbnailHeadLocked() {
    if (!fStatus.preview.running || fThumbnailQueue.empty() || fThumbnailScene == nullptr) {
        return nullptr;
    }
    // Only the head, and only once its background has been settled. That
    // second half is what keeps the run loop honest: the horizon asks this
    // same question, so a slide whose codec has not run yet is simply NOT
    // owed — the loop sleeps until the command thread settles it and wakes
    // this thread, rather than spinning on a frame it can never produce.
    ThumbnailJob& head = fThumbnailQueue.front();
    return head.decoded ? &head : nullptr;
}

bool OutputSession::pumpThumbnailDecode() {
    // Settled with no codec involved (nothing to decode, or already in the
    // cache) is free, so a whole band of lyric slides is marked in one call.
    // A real file is not free, and exactly ONE is decoded per call: this
    // thread answers the operator's commands, and thirty 4K backdrops decoded
    // back to back is thirty chances to delay a present the audience is
    // waiting on. The loop wakes periodically anyway.
    std::string path;
    uint32_t width = 0;
    uint32_t height = 0;
    bool settled = false;
    {
        std::lock_guard<std::mutex> lock(fMutex);
        if (fThumbnailQueue.empty() || fThumbnailScene == nullptr) {
            return false;
        }
        SceneImageCache* cache = fThumbnailScene->imageCache();
        // A job is complete when every path it names has been settled; the
        // lookahead counts finished JOBS, as the memory bound was set in
        // 6.16b — a slide with two pictures spends two decodes inside one
        // lookahead slot, still a bounded, reported number.
        const auto allSettled = [](const ThumbnailJob& job) {
            for (bool ready : job.pathSettled) {
                if (!ready) {
                    return false;
                }
            }
            return true;
        };
        std::size_t decodedAhead = 0;
        for (ThumbnailJob& job : fThumbnailQueue) {
            if (job.decoded) {
                ++decodedAhead;
                continue;
            }
            for (std::size_t i = 0; i < job.imagePaths.size(); ++i) {
                if (job.pathSettled[i]) {
                    continue;
                }
                if (cache->has(job.imagePaths[i])) {
                    // Answered for free: a picture an earlier band already
                    // installed, or one the run ahead of this very pass
                    // decoded for a job behind it.
                    job.pathSettled[i] = true;
                    settled = true;
                    continue;
                }
                // The first path that needs a file. Run ahead of the draw
                // head by the lookahead and no further: the decoded
                // pictures between here and the frame that consumes them
                // are the memory this feature costs, and the bound is what
                // lets a list scroll without the budget having to win a
                // race against eviction.
                if (decodedAhead >= kThumbnailDecodeLookahead) {
                    break;
                }
                path = job.imagePaths[i];
                width = fThumbnailWidth;
                height = fThumbnailHeight;
                break;  // exactly one decode per pump call
            }
            if (!path.empty()) {
                break;  // the decode happens outside the lock
            }
            if (allSettled(job)) {
                job.decoded = true;
            }
        }
        syncThumbnailCountersLocked();
    }
    if (path.empty()) {
        if (settled) {
            wakeRenderThread();  // a head that just became ready is owed a frame
        }
        return settled;
    }

    // Outside the lock, and outside the render thread. Scaled: a backdrop for
    // a 160x90 thumbnail must not cost a 33 MiB allocation on the way, which
    // is the memory growth this task's acceptance names.
    std::string decodeError;
    const sk_sp<SkImage> image = SceneImageCache::decodeAtMost(path, width, height, &decodeError);
    {
        std::lock_guard<std::mutex> lock(fMutex);
        if (fThumbnailScene != nullptr) {
            // Installs (or remembers the failure) into the LIST's cache and
            // nothing else: the audience's pinned budget is not a place a
            // thumbnail's pixels ever go.
            fThumbnailScene->imageCache()->install(path, image);
        }
        // Settle by path, not by position: the batch may have been replaced
        // while the codec ran, and a new band that wants the same picture
        // finds it already installed (the next pump settles it for free),
        // while one that no longer names it has lost nothing. A job only
        // becomes drawn-ready when its LAST path settled — a slide with a
        // background and a foreground picture shows both or is not owed a
        // frame yet (task 6.17b).
        for (ThumbnailJob& job : fThumbnailQueue) {
            if (job.decoded) {
                continue;
            }
            bool complete = true;
            for (std::size_t i = 0; i < job.imagePaths.size(); ++i) {
                if (!job.pathSettled[i]) {
                    if (job.imagePaths[i] == path) {
                        job.pathSettled[i] = true;
                    } else {
                        complete = false;
                    }
                }
            }
            job.decoded = complete;
        }
        syncThumbnailCountersLocked();
    }
    wakeRenderThread();
    return true;
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
    // The transport's mode and mix answer from the first status (task
    // 6.15a): the frame refresh keeps them current, but the half-second
    // before the first frame should already read the truth — the mask is
    // stored before this call so the list starts complete.
    fStatus.media.live = true;
    fStatus.media.loop = fAudio.loopEnabled();
    rebuildMediaSourcesLocked();
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
    // The mode and the mix went out with the transport that held them
    // (task 6.15a): a successor recomputes loop, duration and sources at
    // its own bring-up, and until then there is nothing to acknowledge.
    // live is the point: the path above stays for the sticky error, but
    // a dead transport must not read as commandable.
    fStatus.media.live = false;
    fStatus.media.loop = false;
    fStatus.media.duration = -1.0;
    // Sources are cross-transport now (task 6.15b): the background's row
    // went with its mask bit, but a standalone row survives — the track
    // outlives the transport it happened to be heard beside.
    rebuildMediaSourcesLocked();
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
    // The standalone track refreshes on the same heartbeat and the same
    // frame path (task 6.15b) — and BEFORE the background's early return,
    // because the track is exactly the case where the background is gone:
    // an audio-only session has no frames, and the half-second housekeep
    // is what keeps its position and lane honest.
    refreshStandaloneMediaLocked();
    if (fBackground == nullptr) {
        // The mix echo spans lifetimes (task 6.15b). Below, the sources
        // list is rebuilt as part of the background's own refresh; with no
        // background there are no frames to carry it, and a gain the
        // operator set on the track would be applied to the mixer and
        // never RE-PUBLISHED — the row would sit at whatever the swap
        // wrote. Cheap enough to rebuild every pass regardless: it reads
        // atomics from mixers that already exist.
        rebuildMediaSourcesLocked();
        return;
    }
    const PlaybackPipeline::Stats transport = fBackground->stats();
    const std::string& path = fAppliedBackground.assetPath;
    fStatus.media.position = fBackground->position();
    const AvPlayer::AudioLane lane = mediaLaneLocked();
    fStatus.media.audio = AvPlayer::laneName(lane);
    // Transport mode and mix (task 6.15a, review R15): the loop intent
    // EOF will honour, the clip's probed length, and the registered
    // sources' mixer echo. All read on the transport's own thread; a
    // source this transport never registered is absent rather than
    // falsely listed.
    fStatus.media.loop = fAudio.loopEnabled();
    const double clipSeconds = fBackground->clipSeconds();
    fStatus.media.duration = clipSeconds > 0.0 ? clipSeconds : -1.0;
    rebuildMediaSourcesLocked();

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

void OutputSession::requestMediaLoop(bool enabled) {
    fPendingLoop.store(enabled ? 1 : 0, std::memory_order_release);
    wakeRenderThread();
}

bool OutputSession::mediaSourceLive(AudioSource source) const {
    const int mask = fMediaSourceMask.load(std::memory_order_acquire);
    return (mask & (source == AudioSource::kVideo ? 1 : 2)) != 0;
}

bool OutputSession::requestMediaGain(AudioSource source, double gain, std::string* error) {
    if (source == AudioSource::kVideo ? !mediaTransportLive() : !standaloneLive()) {
        if (error != nullptr) {
            *error = source == AudioSource::kVideo ? "no background media is playing"
                                                   : "no standalone track is playing";
        }
        return false;
    }
    // See requestMediaMute: for the track, live IS registration; the
    // video leg still needs its own gate for the live-but-silent clip.
    if (source == AudioSource::kVideo && !mediaSourceLive(source)) {
        if (error != nullptr) {
            *error = "this background has no video audio source";
        }
        return false;
    }
    fPendingGain.store(gain, std::memory_order_relaxed);
    fPendingGainSource.store(source == AudioSource::kVideo ? AvPlayer::kVideoInput
                                                           : AvPlayer::kSecondInput,
                             std::memory_order_release);
    wakeRenderThread();
    return true;
}

bool OutputSession::requestMediaMute(AudioSource source, bool muted, std::string* error) {
    if (source == AudioSource::kVideo ? !mediaTransportLive() : !standaloneLive()) {
        if (error != nullptr) {
            *error = source == AudioSource::kVideo ? "no background media is playing"
                                                   : "no standalone track is playing";
        }
        return false;
    }
    // The standalone gate above already IS the registration: bit1 of the
    // mask is stored by the swap that started the player and cleared by
    // the one that stopped it, so a live track has its mixer slot — there
    // is no second kind of "absent" to refuse. The video leg keeps its
    // own gate because a playing silent clip is live without one.
    if (source == AudioSource::kVideo && !mediaSourceLive(source)) {
        if (error != nullptr) {
            *error = "this background has no video audio source";
        }
        return false;
    }
    fPendingMute.store(muted ? 1 : 0, std::memory_order_relaxed);
    fPendingMuteSource.store(source == AudioSource::kVideo ? AvPlayer::kVideoInput
                                                           : AvPlayer::kSecondInput,
                             std::memory_order_release);
    wakeRenderThread();
    return true;
}

// --- standalone production track (task 6.15b, review R15) ---

bool OutputSession::requestStandaloneAudio(const std::string& path, std::string* error) {
    // A removal always accepts, even with a broken track on file: taking
    // the track down is the operator's last resort, and the sticky error
    // stays paired with the path it describes (5.5d's rule, read inside
    // this block).
    if (path.empty()) {
        {
            std::lock_guard<std::mutex> lock(fStandaloneSwapMutex);
            fStandaloneSwap = path;
        }
        wakeRenderThread();
        return true;
    }
    // The same probe scene.prepare runs, on the caller's thread and under
    // no lock: a file that is not decodable audio at that path is a
    // REFUSED request that changes nothing — the old track, if any, keeps
    // playing, and the operator hears the reason now rather than after an
    // accepted bring-up. (A file that vanishes between here and the swap
    // is the bring-up's bad news, named through the block's own error.)
    openshow::media::MediaInfo info;
    std::string probeError;
    switch (openshow::media::probeMedia(path, &info, &probeError)) {
        case openshow::media::ProbeStatus::kOk:
            break;
        case openshow::media::ProbeStatus::kOpenFailed:
            if (error != nullptr) {
                *error = probeError.empty() ? "standalone track: '" + path + "' cannot be opened"
                                            : probeError;
            }
            return false;
        case openshow::media::ProbeStatus::kNoUsableStreams:
            if (error != nullptr) {
                *error = "standalone track: '" + path + "' carries no usable stream";
            }
            return false;
    }
    if (info.audioStreams.empty()) {
        if (error != nullptr) {
            *error = "standalone track: '" + path + "' carries no audio stream";
        }
        return false;
    }
    if (!info.audioStreams.front().decoderAvailable) {
        if (error != nullptr) {
            *error = "standalone track: '" + path + "' has audio no decoder can handle (" +
                     info.audioStreams.front().codec + ")";
        }
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(fStandaloneSwapMutex);
        fStandaloneSwap = path;
    }
    wakeRenderThread();
    return true;
}

void OutputSession::standaloneFailedLocked(const std::string& message) {
    // The track's own sticky error, deduplicated on the message exactly
    // like the background's: the lane reconcile runs every half second,
    // and the socket must not spam.
    const bool isNew = fStatus.media.standalone.error != message;
    fStatus.media.standalone.error = message;
    if (isNew) {
        fPendingMediaError = MediaError{fStatus.media.standalone.path, message};
    }
}

void OutputSession::rebuildMediaSourcesLocked() {
    fStatus.media.sources.clear();
    const int sourceMask = fMediaSourceMask.load(std::memory_order_relaxed);
    if ((sourceMask & 1) != 0) {
        fStatus.media.sources.push_back({"video", fAudio.volume(AvPlayer::kVideoInput),
                                         fAudio.muted(AvPlayer::kVideoInput)});
    }
    if ((sourceMask & 2) != 0) {
        // The echo comes from the TRACK's player: identity kSecondInput,
        // which AvPlayer's own table resolves — the standalone-only
        // transport hands that identity whichever mixer index it got (6.7b).
        fStatus.media.sources.push_back({"standalone",
                                         fStandalonePlayer.volume(AvPlayer::kSecondInput),
                                         fStandalonePlayer.muted(AvPlayer::kSecondInput)});
    }
}

void OutputSession::swapStandalonePendingLocked() {
    std::optional<std::string> swap;
    {
        std::lock_guard<std::mutex> lock(fStandaloneSwapMutex);
        swap.swap(fStandaloneSwap);
    }
    if (!swap) {
        return;
    }
    // A queued intent for THIS identity addresses the mixer of the track
    // that is going away: dropped, never delayed onto a successor that
    // was never asked (teardown's rule, per identity).
    if (fPendingGainSource.load(std::memory_order_relaxed) == AvPlayer::kSecondInput) {
        fPendingGainSource.store(-1, std::memory_order_release);
    }
    if (fPendingMuteSource.load(std::memory_order_relaxed) == AvPlayer::kSecondInput) {
        fPendingMuteSource.store(-1, std::memory_order_release);
    }
    fStandalonePlayer.stop();  // idempotent: a never-started player tears down clean
    fStandaloneLive.store(false, std::memory_order_release);
    fMediaSourceMask.store(fMediaSourceMask.load(std::memory_order_relaxed) & ~2,
                           std::memory_order_release);
    rebuildMediaSourcesLocked();
    fStandalonePath = *swap;
    if (swap->empty()) {
        // A removal: the block goes idle — except the error, which stays
        // sticky beside the path it describes (the operator still needs
        // to read what broke).
        fStatus.media.standalone.live = false;
        fStatus.media.standalone.playing = false;
        fStatus.media.standalone.position = -1.0;
        fStatus.media.standalone.duration = -1.0;
        fStatus.media.standalone.audio = "none";
        fStatus.media.standalone.loops = 0;
        std::fprintf(stderr, "openshow-worker: standalone track removed\n");
        return;
    }
    AvPlayer::Config config;
    config.secondAudioPath = *swap;
    // The production bed loops, the background transport's own default
    // (FreeShow's rule): the removal is the stop, and a walk-in track
    // that falls silent at its EOF mid-service is its own surprise.
    config.loop = true;
    std::string startError;
    if (!fStandalonePlayer.start(config, &startError)) {
        fStatus.media.standalone.live = false;
        fStatus.media.standalone.path = *swap;
        standaloneFailedLocked("standalone track: " +
                               (startError.empty() ? "the audio transport refused it"
                                                   : startError));
        std::fprintf(stderr, "openshow-worker: standalone track %s: %s\n", swap->c_str(),
                     startError.c_str());
        return;
    }
    fStandaloneLive.store(true, std::memory_order_release);
    fMediaSourceMask.store(fMediaSourceMask.load(std::memory_order_relaxed) | 2,
                           std::memory_order_release);
    // A successful bring-up replaces any earlier failure (mediaPlaying's
    // rule), and the block's path names what the block's error describes.
    fStatus.media.standalone.error.clear();
    fStatus.media.standalone.live = true;
    fStatus.media.standalone.path = *swap;
    fStatus.media.standalone.playing = false;  // the refresh promotes it, lane first
    fStatus.media.standalone.position = -1.0;
    // The lane seeded from the player that just came up: `live` and the
    // lane are one statement about the same transport, so the swap that
    // sets the first owes the second an honest value — leaving the last
    // block's "none" in place would let status say "live" beside "none"
    // until the next refresh (a real window: the selection's own wait
    // returns on live, and reads the lane right there).
    fStatus.media.standalone.audio = AvPlayer::laneName(fStandalonePlayer.audioLaneState());
    // A fresh transport has not wrapped; the count is THIS file's story.
    fStatus.media.standalone.loops = 0;
    double duration = -1.0;
    openshow::media::MediaInfo info;
    std::string probeError;
    if (openshow::media::probeMedia(*swap, &info, &probeError) ==
            openshow::media::ProbeStatus::kOk &&
        info.durationSeconds > 0.0) {
        duration = info.durationSeconds;
    }
    fStatus.media.standalone.duration = duration;
    rebuildMediaSourcesLocked();
    std::fprintf(stderr, "openshow-worker: standalone track %s (%s clock)\n",
                 swap->c_str(), fStandalonePlayer.audioLive() ? "audio" : "monotonic");
}

void OutputSession::refreshStandaloneMediaLocked() {
    MediaStatus::Standalone& standalone = fStatus.media.standalone;
    if (!fStandaloneLive.load(std::memory_order_relaxed)) {
        return;  // while no player is up, the swap owns the block's fields
    }
    const AvPlayer::AudioLane lane = fStandalonePlayer.audioLaneState();
    standalone.audio = AvPlayer::laneName(lane);
    standalone.position = fStandalonePlayer.clockSeconds();
    // The track's own feeder answers the one question the lane cannot: with
    // the loop on, a starved feed is SILENCE the sink paces as usual. The
    // rewind counter is the difference between "still walking" and "ran
    // out", and the fixture watches it rather than trusting the lane.
    standalone.loops = fStandalonePlayer.feederStats(true).loops;
    // "Playing" means the transport is advancing: held freezes it, and a
    // transport that opened NO source has nothing to advance (its position
    // is a clock, not music). A no-output machine still runs — the same
    // monotonic-clock transport the video uses when the server is absent —
    // so it plays while its lane names why nothing is heard.
    standalone.playing = !fStandalonePlayer.paused() && lane != AvPlayer::AudioLane::kDegraded;
    // The lane fault is the track's own story (R18's rule, per block): a
    // fault lane is NAMED, a healthy one is the recovery that clears it.
    // An error set while the player was down (a bring-up that failed) is
    // never reached here — live is false, and only a successful swap
    // clears it, so this reconcile cannot erase a story it does not
    // describe.
    if (lane == AvPlayer::AudioLane::kNoOutput) {
        standaloneFailedLocked("standalone audio has no output (no audio server connection)");
    } else if (lane == AvPlayer::AudioLane::kDegraded) {
        standaloneFailedLocked("standalone audio is not audible (no source could be opened, or "
                               "the audio server refused the stream)");
    } else {
        standalone.error.clear();
    }
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
    // The mode and mix slots (task 6.15a, review R15). Applied here, on
    // the transport's own thread: setLoop walks the attached-pipeline list
    // (6.6b's rule), and the mixer identity table belongs to the transport
    // this loop owns. Each slot is independent of the momentary op above —
    // muting a source must not swallow a pending seek, and a loop toggle
    // is meaningless as an instant, it is the value EOF will honour.
    // The standalone selection first (task 6.15b): a pass that both swaps
    // the track and carries a gain for it must land the gain on the NEW
    // player's mixer, not the one being stopped. The status fields it
    // touches are the same ones the refresh below samples.
    {
        std::lock_guard<std::mutex> lock(fMutex);
        swapStandalonePendingLocked();
    }
    const int loop = fPendingLoop.exchange(-1, std::memory_order_acq_rel);
    if (loop >= 0 && fBackground != nullptr) {
        fAudio.setLoop(loop == 1);
        std::fprintf(stderr, "openshow-worker: transport loop %s\n",
                     loop == 1 ? "on" : "off");
        applied = true;
    }
    // The gain and mute slots carry a SOURCE IDENTITY, which in production
    // selects the player one-to-one: the background transport never
    // registers a second mixer input, and the track's player has no video
    // identity — kSecondInput is the track, kVideoInput is the background,
    // and each player's own table resolves the index. The per-player guard
    // keeps an intent that raced a teardown from reaching a successor
    // (the exchange drops it: a dropped command, never a late one).
    const int gainSource = fPendingGainSource.exchange(-1, std::memory_order_acq_rel);
    if (gainSource >= 0) {
        const bool forTrack = gainSource == AvPlayer::kSecondInput;
        AvPlayer& player = forTrack ? fStandalonePlayer : fAudio;
        if (forTrack ? fStandaloneLive.load(std::memory_order_relaxed) : fBackground != nullptr) {
            player.setVolume(gainSource,
                             static_cast<float>(fPendingGain.load(std::memory_order_relaxed)));
            applied = true;
        }
    }
    const int muteSource = fPendingMuteSource.exchange(-1, std::memory_order_acq_rel);
    if (muteSource >= 0) {
        const bool forTrack = muteSource == AvPlayer::kSecondInput;
        AvPlayer& player = forTrack ? fStandalonePlayer : fAudio;
        if (forTrack ? fStandaloneLive.load(std::memory_order_relaxed) : fBackground != nullptr) {
            player.setMute(muteSource, fPendingMute.load(std::memory_order_relaxed) == 1);
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
    Status snapshot = fStatus;
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
    // The editor's off-air target (task 6.16a): its still is drawn by the
    // render thread like any other background, so it is held for as long as
    // the selection is registered — even after the store evicts the revision
    // the pin was taken from. That is the whole point of pinning at accept:
    // looking at an edit must never leave a background image unpinned while
    // something is still drawing it.
    if (fDesiredSelection.content) {
        collect(*fDesiredSelection.content);
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
    // The identity table mirrors what THIS transport handed AvPlayer:
    // embedded audio registered its mixer slot when the probe found the
    // stream (the slot exists even if the feeder's own open later failed
    // — the gain belongs to the slot). Bit1 belongs to the standalone
    // track's lifetime (task 6.15b) and is preserved: the background is
    // not this track's owner and never rebuilds its registration. Stored
    // BEFORE mediaPlaying so the first status it publishes already lists
    // the sources (task 6.15a).
    fMediaSourceMask.store((fMediaSourceMask.load(std::memory_order_relaxed) & ~1) |
                              (config.videoAudioPath.empty() ? 0 : 1),
                          std::memory_order_release);
    fMediaTransportLive.store(true, std::memory_order_release);
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
    // Same rule for the 6.15a slots — and live goes first: an RPC that
    // checked the flag a breath ago has its armed intent cleared here,
    // so a dropped command is a dropped command, never a late one fired
    // against a successor transport that was never asked. The standalone
    // track's bits and slots are NOT touched (task 6.15b): the track
    // outlives this transport, and its pending gain belongs to its own
    // player.
    fMediaTransportLive.store(false, std::memory_order_release);
    fMediaSourceMask.store(fMediaSourceMask.load(std::memory_order_relaxed) & ~1,
                           std::memory_order_release);
    fPendingLoop.store(-1, std::memory_order_release);
    if (fPendingGainSource.load(std::memory_order_relaxed) == AvPlayer::kVideoInput) {
        fPendingGainSource.store(-1, std::memory_order_release);
    }
    if (fPendingMuteSource.load(std::memory_order_relaxed) == AvPlayer::kVideoInput) {
        fPendingMuteSource.store(-1, std::memory_order_release);
    }
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
        // The list's surface is a GPU allocation for a target that no longer
        // exists; it comes back with the next batch.
        fThumbnailSurface.reset();
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
        // Same for the other targets: a new stream is due its first frame at
        // once, and their cadence markers must not inherit the old stream's.
        fLastSelection = std::chrono::steady_clock::time_point{};
        fLastThumbnail = std::chrono::steady_clock::time_point{};
        fPreviewPublishedFrames = 0;  // a fresh stream owes its first frame
        // A standing batch judged against the NEW stream (task 6.16b): the
        // segment is reserved for its frame size, so a smaller stream cannot
        // hold the thumbnails that were accepted for a bigger one. Standing
        // the batch down is the safe answer and an honest one — the counters
        // go to zero, `active` goes false, and a client that still wants its
        // list asks again at an extent the new stream can carry. Dropping
        // silently into a slot overflow would be neither.
        if (fStatus.preview.thumbnails.active &&
            !protocol::thumbnailExtentFits(fThumbnailWidth, fThumbnailHeight,
                                           fPreviewStream->bytesPerSlot())) {
            fThumbnailQueue.clear();
            fThumbnailRequest = 0;
            fThumbnailWidth = 0;
            fThumbnailHeight = 0;
            fThumbnailScene.reset();
            fThumbnailSurface.reset();
            fStatus.preview.thumbnails = PreviewStatus::Thumbnails{};
        }
    }
}

void OutputSession::drawPreviewTarget(const SceneFrame& audienceFrame, SkImage* video) {
    // ONE preview frame per pass (task 6.16a, and three targets share that
    // one by 6.16b). The audience mirror, the editor's off-air render and the
    // list's thumbnails share one stream, one cadence and one audience
    // budget, so they take turns rather than all drawing in the pass the
    // audience is waiting on: whichever has gone longest without a frame goes
    // first, and the others stay owed for the next one. During a burst of
    // edit asks that is what keeps the panel's picture arriving; while a show
    // plays it is what keeps an edit from waiting for the audience to go
    // quiet; and a band of thirty thumbnails fills a list without ever
    // taking two frames in a row from either of them. A target that stood
    // down for its own cadence or for the budget does not consume the pass —
    // the next one draws.
    const bool mirrorOwed = fFrames > 0 && fFrames != fPreviewPublishedFrames;
    bool selectionOwed = false;
    bool thumbnailOwed = false;
    {
        std::lock_guard<std::mutex> lock(fMutex);
        selectionOwed = fStatus.preview.running && fDesiredSelection.revision != 0 &&
                        fDesiredSelection.generation != fAppliedSelectionGeneration;
        thumbnailOwed = thumbnailHeadLocked() != nullptr;
    }
    if (thumbnailOwed && (!selectionOwed || fLastThumbnail < fLastSelection) &&
        (!mirrorOwed || fLastThumbnail < fLastPreview)) {
        if (publishThumbnailFrame(video)) {
            return;
        }
    }
    if (selectionOwed && (!mirrorOwed || fLastSelection <= fLastPreview)) {
        if (publishSelectionFrame(video)) {
            return;
        }
    }
    if (mirrorOwed) {
        publishPreviewFrame(audienceFrame, video);
    }
}

OutputSession::PreviewFill OutputSession::fillPreviewSlot(const SceneFrame& frame,
                                                          const SkImage* sceneVideo,
                                                          const SkImage* overlayVideo,
                                                          PreviewStream& stream, int slot,
                                                          uint32_t width, uint32_t height,
                                                          SceneRenderer* renderer,
                                                          sk_sp<SkSurface>* surface) {
    // One target's draw into a claimed slot, shared by every target of the
    // stream (task 6.16a, and the list's thumbnails in 6.16b): they differ in
    // WHICH frame they draw, WHOSE request the result answers, and with which
    // compositor and surface they draw it, and nowhere else. Two copies of
    // this would mean two cost policies, and the audience's budget is exactly
    // the kind of number that drifts when it is computed twice.
    //
    // WHICH compositor and WHICH surface are therefore parameters, and used as
    // such: the slide list draws with its own renderer into its own surface
    // precisely so that a 160x90 row neither reads the audience's decoded-image
    // cache nor resizes the surface the audience's mirror is drawn into. The
    // first version of this function took both and used neither — the row came
    // back the slide's fill colour rather than its backdrop, because the
    // show's cache had never heard of the picture the list had decoded, and
    // every row alternation reallocated the mirror's surface. A shared helper
    // that ignores what makes its callers different is worse than two helpers.
    sk_sp<SkSurface>& target = *surface;
    if (target == nullptr || target->width() != static_cast<int>(width) ||
        target->height() != static_cast<int>(height)) {
        const SkImageInfo info =
            SkImageInfo::Make(static_cast<int>(width), static_cast<int>(height),
                              kRGBA_8888_SkColorType, kPremul_SkAlphaType);
        target = SkSurfaces::RenderTarget(fContext.get(), skgpu::Budgeted::kNo, info);
        if (!target) {
            return PreviewFill::kNoSurface;
        }
    }

    // The measured region is everything the render thread spends on the
    // preview: composition plus the CPU readback that hands the slot over.
    // The fault knob sits inside it, so the deferral policy is tested
    // against a real cost rather than a microsecond — and inside it for
    // BOTH targets, because an off-air edit is the same work on the same
    // thread and pays the audience the same way.
    const auto started = std::chrono::steady_clock::now();
    if (faults().previewDelayMs > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(faults().previewDelayMs));
    }
    // The diagnostics status line stays empty so it does not crowd the
    // preview; video goes where the caller decided it belongs.
    renderer->draw(frame, target->getCanvas(), static_cast<int>(width),
                   static_cast<int>(height), std::string(), sceneVideo);
    if (overlayVideo != nullptr) {
        // The development placement only (task 1.5e): the bench's video,
        // when it is not the scene's own background, over the whole preview.
        // Only the audience target ever passes one — an edit of a scene with
        // no video on its background has no business showing somebody
        // else's.
        target->getCanvas()->drawImageRect(
            overlayVideo, SkRect::MakeIWH(static_cast<int>(width), static_cast<int>(height)),
            SkSamplingOptions(SkFilterMode::kLinear));
    }

    const SkImageInfo dst = SkImageInfo::Make(static_cast<int>(width),
                                              static_cast<int>(height),
                                              kRGBA_8888_SkColorType, kPremul_SkAlphaType);
    // Packed rows (the frame's own width, not the stream's stride) for every
    // target: for a frame of the stream's size the two are the same number,
    // and for a thumbnail it is what lets the consumer copy a frame of a size
    // it was not told about at stream.start with one memcpy and no padding to
    // skip. The header's rowBytes says which it is, on every publish.
    const bool readbackOk =
        target->readPixels(dst, stream.slotPixels(slot), width * 4, 0, 0);
    // Whatever the frame did to the panel's time is settled here, success or
    // not: the milliseconds were spent either way.
    const double costSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (costSeconds > kPreviewBudgetSeconds) {
        // Give the audience the overrun back. Standing down for
        // cost × (period / budget − 1) keeps the AVERAGE preview cost at or
        // under the share it was lent, whatever one frame cost: a preview
        // that needs 40 ms gives up its frames (here, roughly 3 in 4)
        // instead of taking 40 ms out of every audience frame.
        fPreviewDeferredUntil =
            std::chrono::steady_clock::now() +
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(costSeconds *
                                              (kAudienceFrameSeconds / kPreviewBudgetSeconds -
                                               1.0)));
    } else {
        fPreviewDeferredUntil = std::chrono::steady_clock::time_point{};
    }
    {
        std::lock_guard<std::mutex> lock(fMutex);
        fStatus.preview.worstMs = std::max(fStatus.preview.worstMs, costSeconds * 1000.0);
    }
    return readbackOk ? PreviewFill::kFilled : PreviewFill::kReadbackFailed;
}

void OutputSession::publishPreviewFrame(const SceneFrame& frame, SkImage* video) {
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
    // The tag is the FRAME's revision (task 6.11a), which is what makes it
    // honest both ways: after a present it is the revision this frame put on
    // the panel (task 6.4's rule), and on a republish it is the revision of
    // the presented frame being redrawn. Asking the store for its latest
    // DESIRED revision here would tag frames with scenes nobody had drawn yet.
    const uint32_t revision = static_cast<uint32_t>(frame.revision);
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
    // Audience timing budget (task 6.11a, review R13): the preview is drawn
    // on the render thread, so its cost comes straight out of the frame the
    // audience is waiting for. A frame that cost more than the audience can
    // lend stands this attempt down instead of taking it; the content stays
    // owed (the gates above are untouched), so the picture arrives as soon
    // as the audience can pay for it again.
    if (now < fPreviewDeferredUntil) {
        std::lock_guard<std::mutex> lock(fMutex);
        ++fStatus.preview.deferred;
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

    // The same scene through the same draw path as the audience output, only
    // smaller — and the SAME FRAME: what this preview publishes is the
    // composition the audience was shown (task 6.11a, review R13), never one
    // the preview captured for itself, so no preview frame can carry a
    // scene, a blend or a clock the panel has not been shown.
    // Video belongs to what the audience sees: a background video (task 5.5)
    // goes into the scene's background layer through draw() itself, anything
    // else (the 1.5e bench) stays the dev overlay over the whole preview —
    // the readPixels inside fillPreviewSlot is CPU-synchronous, which keeps
    // this sampling of a released slot ordered ahead of any later reuse.
    const bool asSceneBackground =
        video != nullptr && fRenderScene && fRenderScene->background.isVideo;
    const PreviewFill fill =
        fillPreviewSlot(frame, asSceneBackground ? video : nullptr,
                        asSceneBackground ? nullptr : video, *stream, slot, width, height,
                        fScene.get(), &fPreviewSurface);
    if (fill == PreviewFill::kNoSurface) {
        stream->abort(slot);
        std::lock_guard<std::mutex> lock(fMutex);
        ++fStatus.preview.dropped;
        fStatus.preview.running = false;
        fStatus.preview.error = "could not create the preview surface";
        return;
    }
    if (fill == PreviewFill::kReadbackFailed) {
        stream->abort(slot);
        std::lock_guard<std::mutex> lock(fMutex);
        ++fStatus.preview.dropped;
        fStatus.preview.error = "preview readback failed";
        return;
    }

    const uint64_t publishedNs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch())
            .count());
    const uint64_t seq = stream->publish(slot, revision, protocol::kPreviewTargetAudience, 0,
                                         publishedNs);
    fPreviewPublishedFrames = fFrames;  // the gate counts this publish

    PreviewReady ready;
    ready.slot = static_cast<uint32_t>(slot);
    ready.seq = seq;
    ready.width = width;
    ready.height = height;
    ready.rowBytes = stream->rowBytes();
    ready.revision = revision;
    ready.target = protocol::kPreviewTargetAudience;
    ready.request = 0;
    ready.publishedNs = publishedNs;
    ready.stream = stream->name();  // ownership identity (task 6.5)
    std::lock_guard<std::mutex> lock(fMutex);
    fPendingReady[slot] = ready;
    ++fStatus.preview.published;
}

bool OutputSession::publishSelectionFrame(SkImage* video) {
    // The gates, in the order that costs least to answer.
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps = 0;
    std::shared_ptr<PreviewStream> stream;
    ActiveScene content;
    uint64_t revision = 0;
    std::uint32_t request = 0;
    std::uint32_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(fMutex);
        if (!fStatus.preview.running || fDesiredSelection.revision == 0 ||
            fDesiredSelection.generation == fAppliedSelectionGeneration) {
            // Nothing asked, or this ask has already been answered: one
            // request is one frame, not a second stream to keep ticking.
            return false;
        }
        content = fDesiredSelection.content;
        revision = fDesiredSelection.revision;
        request = fDesiredSelection.request;
        generation = fDesiredSelection.generation;
        width = fStatus.preview.width;
        height = fStatus.preview.height;
        fps = fStatus.preview.fps;
        stream = fPreviewStream;
    }
    if (!stream || !content) {
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    // The stream's cadence, per target: the mirror and the edit share the
    // transport and so share its rate, but neither waits on the other's
    // last frame (see run()'s horizon, which wakes for whichever is due
    // first).
    if (now - fLastSelection < std::chrono::duration<double>(1.0 / fps)) {
        return false;
    }
    if (now < fPreviewDeferredUntil) {
        std::lock_guard<std::mutex> lock(fMutex);
        ++fStatus.preview.selection.deferred;
        return false;
    }
    fLastSelection = now;

    const int slot = stream->claim();
    if (slot < 0) {
        // The consumer owns every slot: drop, and stay owed — the next pass
        // tries again for the same request. Dropping is bounded (the loop's
        // cadence), the ask never is.
        std::lock_guard<std::mutex> lock(fMutex);
        ++fStatus.preview.selection.dropped;
        return false;
    }

    // THE FRAME, and the whole point of the task: a SceneFrame built by
    // hand from the pinned scene, at full strength with no fade, carrying
    // the revision of the scene rather than of any present. Nothing here
    // calls captureFrame (the only call that advances a crossfade),
    // commitFrame, setActiveScene or the transport's apply — so the audience
    // keeps its scene, its blend, its clock and its media exactly as they
    // were, however many edits are drawn while they watch.
    SceneFrame frame;
    frame.scene = content;
    frame.fadeFrom = nullptr;
    frame.mediaBlend = 1.0f;
    frame.textBlend = 1.0f;
    frame.timeSeconds =
        std::chrono::duration<double>(now - fStartTime).count();
    frame.wallClock = std::time(nullptr);
    frame.revision = revision;

    // Media, honestly: an off-air scene draws its background STILL out of
    // the pinned image cache (prepared scenes are preloaded and pinned, so
    // the cost is not paid here). A VIDEO background is a transport that
    // belongs to the present — an unpublished scene has none — so the only
    // frame this render may sample is the one the audience's own transport
    // is showing, and only when the two scenes name the same asset. Any
    // other video stands as the colour fill it draws with no frame to show:
    // pretending otherwise would put a moving picture behind an edit that
    // the audience has no reason to be seeing.
    const bool sameVideoAsset =
        video != nullptr && fRenderScene && fRenderScene->background.isVideo &&
        content->background.isVideo &&
        content->background.assetPath == fRenderScene->background.assetPath;

    const PreviewFill fill = fillPreviewSlot(frame, sameVideoAsset ? video : nullptr, nullptr,
                                             *stream, slot, width, height, fScene.get(),
                                             &fPreviewSurface);
    if (fill == PreviewFill::kNoSurface) {
        stream->abort(slot);
        std::lock_guard<std::mutex> lock(fMutex);
        ++fStatus.preview.selection.dropped;
        fStatus.preview.running = false;
        fStatus.preview.error = "could not create the preview surface";
        return false;
    }
    if (fill == PreviewFill::kReadbackFailed) {
        stream->abort(slot);
        std::lock_guard<std::mutex> lock(fMutex);
        ++fStatus.preview.selection.dropped;
        fStatus.preview.error = "preview readback failed";
        return false;
    }

    const uint64_t publishedNs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch())
            .count());
    bool stale = false;
    {
        // THE PUBLISH GATE. The draw above took time, and a frontend that
        // kept editing may have asked again since; publishing THAT would put
        // an old scene's pixels on the wire tagged with a generation the
        // operator has already moved past. The generation is re-read here,
        // under the same lock the accept takes, and a superseded frame is
        // destroyed rather than published — the slot closes on its previous
        // value, so the consumer never sees half a frame either.
        std::lock_guard<std::mutex> lock(fMutex);
        if (fDesiredSelection.generation != generation) {
            ++fStatus.preview.selection.stale;
            stale = true;
        }
    }
    if (stale) {
        stream->abort(slot);
        // Stay owed: the newer ask is what the render thread draws next, and
        // this one is nobody's picture any more.
        return false;
    }

    const uint64_t seq =
        stream->publish(slot, static_cast<uint32_t>(revision), protocol::kPreviewTargetSelection,
                        request, publishedNs);
    PreviewReady ready;
    ready.slot = static_cast<uint32_t>(slot);
    ready.seq = seq;
    ready.width = width;
    ready.height = height;
    ready.rowBytes = stream->rowBytes();
    ready.revision = static_cast<uint32_t>(revision);
    ready.target = protocol::kPreviewTargetSelection;
    ready.request = request;
    ready.publishedNs = publishedNs;
    ready.stream = stream->name();
    {
        std::lock_guard<std::mutex> lock(fMutex);
        // Satisfied only now: this request has produced exactly one frame.
        // The mirror's content gate is deliberately NOT touched — these
        // pixels are not the audience's, and the panel's own picture still
        // has to reach the stream on its own terms.
        fAppliedSelectionGeneration = generation;
        fPendingReady[slot] = ready;
        ++fStatus.preview.selection.published;
    }
    return true;
}

bool OutputSession::publishThumbnailFrame(SkImage* video) {
    // One slide of the pending band, drawn at the band's own extent. The
    // gates are preview.select's in the same order, and for the same reasons:
    // cadence, the audience's budget, a slot that is free, and only then the
    // draw.
    uint32_t fps = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::uint32_t id = 0;
    std::uint32_t request = 0;
    std::shared_ptr<PreviewStream> stream;
    std::shared_ptr<SceneRenderer> renderer;
    ActiveScene content;
    {
        std::lock_guard<std::mutex> lock(fMutex);
        ThumbnailJob* head = thumbnailHeadLocked();
        if (head == nullptr) {
            return false;
        }
        content = head->content;
        id = head->id;
        request = fThumbnailRequest;
        width = fThumbnailWidth;
        height = fThumbnailHeight;
        fps = fStatus.preview.fps;
        stream = fPreviewStream;
        // A reference of our own for the duration of the draw: the command
        // thread can retire this compositor (a batch of another extent
        // discards its decode cache), and it must not be able to do it under
        // a pointer the render thread is painting with.
        renderer = fThumbnailScene;
    }
    if (!stream || !renderer || !content) {
        return false;
    }
    // THE SLOT BOUND, checked again here rather than only at the accept:
    // preview.start can replace the stream with a smaller segment while a
    // batch stands, and writing w*h*4 of pixels into a slot that was reserved
    // for less is not a bug this feature is allowed to have. The batch goes
    // down and says so in its own counters; the client, which mirrors
    // `active`, asks again at a size the new stream can carry.
    if (!protocol::thumbnailExtentFits(width, height, stream->bytesPerSlot())) {
        std::lock_guard<std::mutex> lock(fMutex);
        fThumbnailQueue.clear();
        fThumbnailRequest = 0;
        fThumbnailWidth = 0;
        fThumbnailHeight = 0;
        fStatus.preview.thumbnails = PreviewStatus::Thumbnails{};
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    // The stream's cadence, again per target: three kinds of picture share
    // one rate and none of them waits on another's last frame.
    if (now - fLastThumbnail < std::chrono::duration<double>(1.0 / std::max(1u, fps))) {
        return false;
    }
    if (now < fPreviewDeferredUntil) {
        std::lock_guard<std::mutex> lock(fMutex);
        ++fStatus.preview.thumbnails.deferred;
        return false;
    }
    fLastThumbnail = now;

    const int slot = stream->claim();
    if (slot < 0) {
        // The consumer owns every slot: drop, and stay owed. A band of
        // thirty against three slots is a bounded queue, not a backlog — the
        // slides that do not fit are retried, the ones the operator has
        // scrolled past are gone with the next batch.
        std::lock_guard<std::mutex> lock(fMutex);
        ++fStatus.preview.thumbnails.dropped;
        return false;
    }

    // A frame built the way the off-air edit builds its (task 6.16a), from a
    // scene that is not the audience's and never will be: no fade to blend,
    // both tracks at full strength, the id of the slide as its tag. Nothing
    // here calls captureFrame, commitFrame, setActiveScene or the transport's
    // apply, and the renderer doing the drawing is not the one that composed
    // the audience's frame — so the show's scene, blend, clock, pins and
    // decoded-image budget are untouched by thirty thumbnails painting.
    SceneFrame frame;
    frame.scene = content;
    frame.fadeFrom = nullptr;
    frame.mediaBlend = 1.0f;
    frame.textBlend = 1.0f;
    frame.timeSeconds = std::chrono::duration<double>(now - fStartTime).count();
    frame.wallClock = std::time(nullptr);
    frame.revision = id;

    // Its background comes from the LIST's own cache, where the command
    // thread put a scaled decode. A miss draws the colour fill: the draw path
    // never opens a file (task 6.10's rule, and the reason the pump exists),
    // and a picture that arrives half a second later arrives with the next
    // band. A video background follows the selection's rule — sample the
    // audience's frame only when the two scenes name the same asset, because
    // a scene with no transport has no business wearing someone else's.
    const bool sameVideoAsset =
        video != nullptr && fRenderScene && fRenderScene->background.isVideo &&
        content->background.isVideo &&
        content->background.assetPath == fRenderScene->background.assetPath;

    const PreviewFill fill = fillPreviewSlot(frame, sameVideoAsset ? video : nullptr, nullptr,
                                             *stream, slot, width, height, renderer.get(),
                                             &fThumbnailSurface);
    if (fill == PreviewFill::kNoSurface) {
        stream->abort(slot);
        std::lock_guard<std::mutex> lock(fMutex);
        ++fStatus.preview.thumbnails.dropped;
        fStatus.preview.running = false;
        fStatus.preview.error = "could not create the thumbnail surface";
        return false;
    }
    if (fill == PreviewFill::kReadbackFailed) {
        stream->abort(slot);
        std::lock_guard<std::mutex> lock(fMutex);
        ++fStatus.preview.thumbnails.dropped;
        fStatus.preview.error = "thumbnail readback failed";
        return false;
    }

    const uint64_t publishedNs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch())
            .count());
    bool stale = false;
    {
        // THE PUBLISH GATE, the batch's version of it. The draw took time and
        // the operator may have scrolled in the middle of it; pixels of a
        // slide nobody is looking at must not reach the wire tagged with a
        // generation the client has already replaced. The slot closes on its
        // previous value, so nothing half-written is ever readable.
        std::lock_guard<std::mutex> lock(fMutex);
        if (fThumbnailRequest != request) {
            ++fStatus.preview.thumbnails.stale;
            stale = true;
        }
    }
    if (stale) {
        stream->abort(slot);
        // Nothing to re-queue: the superseded batch is gone and the queue
        // holds the new one.
        return false;
    }

    const PreviewStream::FrameExtent extent{width, height, width * 4};
    const uint64_t seq =
        stream->publish(slot, id, protocol::kPreviewTargetThumbnail, request, publishedNs,
                        &extent);
    PreviewReady ready;
    ready.slot = static_cast<uint32_t>(slot);
    ready.seq = seq;
    ready.width = width;
    ready.height = height;
    ready.rowBytes = width * 4;  // packed, and named in the notification
    ready.revision = id;         // the per-slide id, on this target
    ready.target = protocol::kPreviewTargetThumbnail;
    ready.request = request;
    ready.publishedNs = publishedNs;
    ready.stream = stream->name();
    {
        std::lock_guard<std::mutex> lock(fMutex);
        // The slide is spent: one accepted slide, one picture. Erased by id
        // rather than by position because a batch may have been replaced
        // between the gate above and here, and an id the queue no longer
        // names has already gone with it.
        for (auto job = fThumbnailQueue.begin(); job != fThumbnailQueue.end(); ++job) {
            if (job->id == id) {
                fThumbnailQueue.erase(job);
                break;
            }
        }
        syncThumbnailCountersLocked();
        fPendingReady[slot] = ready;
        ++fStatus.preview.thumbnails.published;
    }
    return true;
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
        // The picture in fComposedFrame is on the panel now: credit it (the
        // only place a completed crossfade is retired) and remember it as
        // the frame every preview redraws until the audience is owed a
        // newer one (task 6.11a).
        fScene->commitFrame(fComposedFrame);
        fPresentedFrame = fComposedFrame;
    }

    // Preview (task 1.4) runs after the audience frame is presented: the
    // extra pass never delays the swapchain work it follows, and its
    // throttling, content gate and drop policy live in publishPreviewFrame.
    // It draws fComposedFrame — the snapshot this frame put on the panel,
    // revision included (task 6.11a, review R13). Before that it asked the
    // renderer for a composition of its own at a later timestamp, which
    // advanced the live crossfade a second time: the preview ran the fade to
    // a blend the audience had not been shown, on a clock the audience's
    // 800 ms does not own.
    drawPreviewTarget(fComposedFrame, videoImage);

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
            // What the output owes (task 6.18a): the store answers both of its
            // questions — an accepted command newer than the newest presented
            // frame, and a destination the window is not on yet — and hands
            // back the snapshot that frame must draw in the same critical
            // section, so the decision and the capture are one instant.
            SceneStore::Decision decision;
            fStore.decide(&decision);
            presentNow = decision.commandsDue || decision.destinationDue ||
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
            // The capture (task 6.4, taken by the store in 6.18a): it came from
            // the same critical section as the decision above — unconditionally,
            // because housekeeping can add a present reason after the lock — so
            // this snapshot is everything owed as of now and nothing that
            // arrives later, its scene content pinned from the store's entry.
            // frame() draws exactly it and its successful present credits
            // exactly its revision.
            snapshot = std::move(decision.snapshot);
            if (!presentNow && fStatus.preview.running) {
                // Either preview target can owe a frame: the MIRROR when the
                // audience's last presented frame has not reached the stream
                // (task 1.5e's content gate), the EDITOR when an accepted
                // off-air ask has not been drawn (task 6.16a). Each has its
                // own cadence marker and they share the audience's budget, so
                // the loop sleeps to whichever comes round first. The re-ask
                // lesson applies to both: a publish that stood down left its
                // own marker where it was, so a horizon in the future is a
                // SLEEP and never a spin — the first fixture measured that
                // loop at ~20,000 iterations a second, which is precisely the
                // busy loop 1.5e exists to prevent.
                const auto period =
                    std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        std::chrono::duration<double>(
                            1.0 / std::max(1u, fStatus.preview.fps)));
                const auto horizonOf = [&](const std::chrono::steady_clock::time_point& last) {
                    return std::max(last + period, fPreviewDeferredUntil);
                };
                const bool mirrorOwed = fFrames != fPreviewPublishedFrames;
                const bool selectionOwed =
                    fDesiredSelection.revision != 0 &&
                    fDesiredSelection.generation != fAppliedSelectionGeneration;
                // The list's head counts as owed only once its background has
                // been settled on the command thread — which is exactly what
                // the publish checks, because it asks this same function. Ask
                // the same question in both places and a slide waiting on its
                // codec leaves the loop ASLEEP until the pump wakes it,
                // instead of spinning on a frame that cannot be produced.
                const bool thumbnailOwed = thumbnailHeadLocked() != nullptr;
                if (mirrorOwed || selectionOwed || thumbnailOwed) {
                    // The sooner horizon of whichever targets are owed: three
                    // cadence markers, the minimum of the ones that matter.
                    auto nextAllowed = std::chrono::steady_clock::time_point::max();
                    const auto consider = [&](
                        const std::chrono::steady_clock::time_point& last) {
                        nextAllowed = std::min(nextAllowed, horizonOf(last));
                    };
                    if (mirrorOwed) {
                        consider(fLastPreview);
                    }
                    if (selectionOwed) {
                        consider(fLastSelection);
                    }
                    if (thumbnailOwed) {
                        consider(fLastThumbnail);
                    }
                    if (now >= nextAllowed) {
                        previewDue = true;
                    } else {
                        wakeAt = std::min(wakeAt, nextAllowed);
                    }
                }
            }        }

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
            SkImage* held = nullptr;
            if (fVideoPresenter && fDrawnVideo) {
                held = fVideoPresenter->acquireImage(fDrawnVideo->token,
                                                     fDrawnVideo->sequence);
            }
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
            drawPreviewTarget(fPresentedFrame, held);
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
