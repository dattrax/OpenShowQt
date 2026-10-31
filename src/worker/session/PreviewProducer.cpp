/*
 * PreviewProducer implementation (task 6.18a2). The bodies moved out of
 * OutputSession behaviour-for-behaviour: the stream lifecycle and its
 * generation rule (1.4), the selection and batch generation gates and the
 * publish gate that re-reads them (6.16a/b, R16), the shared draw-and-readback
 * fill and the audience's deferral arithmetic (6.11a, R13), the three-slot
 * ownership proof on releases and notifications (6.5, R5), and the thumbnail
 * queue with its one-decode-per-pump bound and lookahead (6.16b, R10/R16).
 * What the session still hands in on every pass — the frame, the video, the
 * frame counter, the clock, the on-air scene — is exactly what the producer
 * used to reach into the session for; see the header for the contract.
 */

#include "session/PreviewProducer.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <thread>
#include <utility>

#include <unistd.h>

#include "include/core/SkCanvas.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"

#include "preview/PreviewStream.h"
#include "render/SceneRenderer.h"
#include "session/Faults.h"

namespace openshow::worker {

namespace {

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

PreviewProducer::~PreviewProducer() {
    // The thumbnail lane goes first (task 6.21d): its decode is bounded by
    // the file, the session must not die with a reader still in flight,
    // and nothing publishes after this point — the picture would have
    // nowhere to go. The worker only ever touched its own struct, so
    // joining here races nothing.
    if (fThumbnailDecode) {
        fThumbnailDecode->worker.join();
        fThumbnailDecode.reset();
    }
    // The stream unlink belongs here too, not only in shutdown(): a caller
    // that drops a producer without closing it must not leak the shm segment.
    // (A live consumer's slot ownership still defers the actual unlink to
    // this object's last reference — the same rule the session honoured.)
    std::lock_guard<std::mutex> lock(fMutex);
    fStream.reset();
}

void PreviewProducer::attachRenderer(GrDirectContext* context, SceneRenderer* audience) {
    std::lock_guard<std::mutex> lock(fMutex);
    fContext = context;
    fAudience = audience;
}

void PreviewProducer::detachRenderer() {
    std::lock_guard<std::mutex> lock(fMutex);
    fContext = nullptr;
    fAudience = nullptr;
}

// --- stream lifecycle (command thread) ---

bool PreviewProducer::start(uint32_t width, uint32_t height, uint32_t fps,
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
    ++fStreamRequestCounter;
    DesiredStream desired;
    desired.enabled = true;
    desired.width = width;
    desired.height = height;
    desired.fps = fps;
    desired.name = "/openshow-preview-" + std::to_string(static_cast<long>(::getpid())) + "-" +
                   std::to_string(1000u + fStreamRequestCounter);
    desired.generation = fDesired.generation + 1;
    fDesired = std::move(desired);

    // Fresh counters for the new stream; running flips true when the
    // render thread has actually built the segment and surface.
    fStatus = PreviewStatus{};
    fStatus.name = fDesired.name;
    fStatus.width = width;
    fStatus.height = height;
    fStatus.fps = fps;
    for (auto& pending : fPendingReady) {
        pending.reset();  // slots belong to the stream being replaced
    }
    // A new stream is a new generation domain for the off-air target
    // (task 6.16a): the accepted request resets, and the selection itself
    // stands down until the frontend asks again — pixels of a scene nobody
    // re-asked for have no stream to arrive on.
    fSelection = SelectionDesired{};
    fAppliedSelectionGeneration = fSelection.generation;
    fStatus.selection.active = false;

    if (info) {
        info->name = fDesired.name;
        info->width = width;
        info->height = height;
        info->slotCount = protocol::kPreviewSlotCount;
        info->bytesPerSlot =
            static_cast<uint32_t>(protocol::previewSlotStrideBytes(width, height));
    }
    return true;
}

bool PreviewProducer::stop() {
    std::lock_guard<std::mutex> lock(fMutex);
    if (!fDesired.enabled) {
        return false;
    }
    fDesired.enabled = false;
    ++fDesired.generation;
    for (auto& pending : fPendingReady) {
        pending.reset();
    }
    // No stream, no off-air target (task 6.16a): the content copy goes
    // back with it, so a stopped preview does not sit on a scene the store
    // would otherwise be free to evict.
    fSelection = SelectionDesired{};
    fAppliedSelectionGeneration = fSelection.generation;
    fStatus.selection = PreviewStatus::Selection{};
    // And no list either (task 6.16b). The batch is stood down completely:
    // its scenes go, the companion renderer goes with its decoded pictures
    // (12 MiB of them at most, and nothing left to draw them into), and the
    // accepted generation resets so a client that asks again after a
    // restart is not refused by its own stale counter. None of this touched
    // the store or the pins — thumbnails were never in either — so the
    // caller's pin re-derivation has only the selection's still to learn.
    fThumbnailQueue.clear();
    fThumbnailRequest = 0;
    // The destination goes with the batch (task 6.22g, review A7): a decode
    // in flight for the renderer that is about to die is refused by the next
    // pump rather than installed into whatever comes after it.
    setThumbnailDestinationLocked(nullptr, 0, 0);
    fStatus.thumbnails = PreviewStatus::Thumbnails{};
    return true;
}

// --- the editor's off-air target ---

bool PreviewProducer::select(uint64_t revision, std::uint32_t request, const ScenePin& pin,
                             std::string* error) {
    std::lock_guard<std::mutex> lock(fMutex);
    if (!fDesired.enabled) {
        // The honest refusal, in the order the operator can act on it: no
        // stream means there is nowhere for these pixels to go. (The gate
        // is the DESIRED stream, not status().running: preview.start is
        // acknowledged before the render thread has built the segment, and
        // a selection that arrives in that window is a normal race, not a
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
    const std::uint32_t accepted = fSelection.request;
    if (static_cast<std::int32_t>(request - accepted) <= 0) {
        if (error != nullptr) {
            *error = "preview.select request " + std::to_string(request) +
                     " does not follow the accepted " + std::to_string(accepted) +
                     " (the frontend's counter must advance)";
        }
        return false;
    }
    ActiveScene content;
    if (revision != 0 && !pin(revision, &content)) {
        // The store is bounded, so this is the ordinary way to be wrong:
        // the frontend named a revision that no longer exists (or never
        // did). It must re-prepare rather than wait for imagery that is
        // never coming.
        if (error != nullptr) {
            *error = "preview.select scene " + std::to_string(revision) +
                     " is not in the prepared store";
        }
        return false;
    }
    // The pin comes from the store rather than a second lookup on the
    // render thread: entries are immutable per revision, and the store may
    // evict this one before the pass that draws it (R4's rule, applied to a
    // target that has no present to pin it).
    fSelection.content = std::move(content);
    fSelection.revision = revision;
    fSelection.request = request;
    ++fSelection.generation;
    // `active` is the accept, not the draw: it says this stream has an
    // off-air target registered, which is what the frontend's status mirror
    // needs in order to know its ask was understood. Whether the frame made
    // it out is what published/dropped/stale are for.
    fStatus.selection.active = revision != 0;
    fStatus.selection.revision = revision;
    fStatus.selection.request = request;
    return true;
}

// --- thumbnails (task 6.16b, review R16) ---

bool PreviewProducer::requestThumbnails(const std::vector<ThumbnailEntry>& batch,
                                        uint32_t width, uint32_t height,
                                        std::uint32_t request, ThumbnailAck* ack,
                                        std::string* error) {
    std::unique_lock<std::mutex> lock(fMutex);
    if (!fDesired.enabled) {
        // preview.select's first refusal, for the same reason: there is
        // nowhere for these pixels to go. The gate is the DESIRED stream,
        // so a band that arrives while preview.start is still being built
        // is a normal race and not a refusal.
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
    const std::size_t bytesPerSlot =
        protocol::previewSlotStrideBytes(fDesired.width, fDesired.height);
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
    // The queue is what bounds the memory a list can cost, so the cap on
    // the batch is the bound itself. Refusing beats truncating: a client
    // that asked for sixty slides is buggy, and drawing the first thirty of
    // them without saying so would have it believe it had pictures for
    // sixty.
    if (batch.size() > protocol::kMaxThumbnailBatch) {
        if (error != nullptr) {
            *error = "preview.thumbnails takes at most " +
                     std::to_string(protocol::kMaxThumbnailBatch) + " scenes, asked for " +
                     std::to_string(batch.size()) + " (request the visible band)";
        }
        return false;
    }
    // THE GENERATION GATE, the batch's own. preview.select's rule applies
    // to a list exactly as it does to one edit: a replayed ask — a retry
    // after a dropped reply, a scroll handler fired twice — must not
    // resurrect a batch the operator has already replaced. An empty batch
    // is a stand-down and a stand-down is a change like any other, so it
    // needs a new number too.
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

    // The list draws with a compositor of its own (see createCompanion),
    // and a batch of a DIFFERENT extent gets a fresh one: its decoded
    // backgrounds are sized for the old ask, and a cache keyed on the path
    // alone would otherwise serve a 160-pixel picture to a 512-pixel
    // request. Building it happens OUTSIDE the lock — assembling layers and
    // borrowing a text engine is font work, and this lock is what the
    // render thread takes to snapshot a frame — and published under it.
    const bool extentChanged = fThumbnailWidth != width || fThumbnailHeight != height;
    std::shared_ptr<SceneRenderer> built;
    if (!batch.empty() && (fThumbnailScene == nullptr || extentChanged)) {
        if (fAudience == nullptr) {
            if (error != nullptr) {
                *error = "preview.thumbnails: the session has no compositor to draw with";
            }
            return false;
        }
        SceneRenderer* const audience = fAudience;
        lock.unlock();
        std::string buildError;
        built = SceneRenderer::createCompanion(*audience, kThumbnailImageBudget, &buildError);
        lock.lock();
        if (!built && error != nullptr) {
            *error = "preview.thumbnails could not build the list's renderer: " + buildError;
        }
        if (!built) {
            return false;
        }
    }

    // REPLACE, which is the whole coalescing and cancellation rule:
    // whatever was queued for an older list stops being owed, and its
    // scenes and decoded pixels go with it. The newest band is the only
    // debt.
    fThumbnailQueue.clear();
    fThumbnailQueue.reserve(batch.size());
    for (const ThumbnailEntry& entry : batch) {
        ThumbnailJob job;
        job.key = entry.key;
        // Per-slide id, and the only name a scene the store has never held
        // can have. Non-zero for the usual reason, and 32 bits is plenty
        // because an id only has to be unique among the slides of one
        // batch.
        if (++fThumbnailIdCounter == 0) {
            fThumbnailIdCounter = 1;
        }
        job.id = fThumbnailIdCounter;
        // A transient copy: no store entry, no revision, no pin. This scene
        // cannot be presented by anything, which is the point — a slide
        // list is a picture of a slide, not show content.
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
    // The destination is published as a PAIR (task 6.22g, review A7): the
    // companion and the extent a decode was scaled for travel together, and
    // an extent change retires the cache whose pixels were sized for the old
    // ask even when this batch is empty and no renderer is needed yet —
    // otherwise the next band at the new extent finds `built` null, keeps the
    // old companion, and its cache answers a 512-pixel request with a
    // 160-pixel picture without a decode ever being asked.
    setThumbnailDestinationLocked(built ? std::move(built)
                                        : (extentChanged ? nullptr : fThumbnailScene),
                                  width, height);
    fThumbnailRequest = request;
    // The accept, not the draw: this stream now owes a band.
    fStatus.thumbnails.active = !fThumbnailQueue.empty();
    fStatus.thumbnails.request = request;
    fStatus.thumbnails.width = width;
    fStatus.thumbnails.height = height;
    syncThumbnailCountersLocked();
    if (ack != nullptr) {
        ack->request = request;
        ack->width = width;
        ack->height = height;
    }
    return true;
}

void PreviewProducer::syncThumbnailCountersLocked() {
    std::size_t pending = 0;
    for (const ThumbnailJob& job : fThumbnailQueue) {
        if (!job.decoded) {
            ++pending;
        }
    }
    fStatus.thumbnails.queued = fThumbnailQueue.size();
    fStatus.thumbnails.decodePending = pending;
    // The list's own picture memory, reported rather than assumed: this is
    // where a frontend (or a test) can see that scrolling a document spends
    // a bounded number of bytes in a cache that is not the show's. The lock
    // order is the producer's then the cache's, never the reverse — the
    // cache knows nothing about the producer, so no path can invert it.
    if (fThumbnailScene != nullptr) {
        const SceneImageCache::Stats images = fThumbnailScene->imageCache()->stats();
        fStatus.thumbnails.decodedBytes = images.decodedBytes;
        fStatus.thumbnails.entries = images.entries;
        fStatus.thumbnails.evictions = images.evictions;
        fStatus.thumbnails.budget = images.budget;
    } else {
        // No destination, no decoded pictures (task 6.22g, review A7): the
        // bytes belonged to a companion that is gone — an extent change or a
        // stand-down retires them — and reporting them beside a list that
        // holds none would be the same lie as installing their pixels.
        fStatus.thumbnails.decodedBytes = 0;
        fStatus.thumbnails.entries = 0;
        fStatus.thumbnails.evictions = 0;
        fStatus.thumbnails.budget = 0;
    }
}

void PreviewProducer::setThumbnailDestinationLocked(std::shared_ptr<SceneRenderer> renderer,
                                                    std::uint32_t width,
                                                    std::uint32_t height) {
    // The pair (companion, extent) is what a lane decode's pixels were made
    // for, so any change to it is a new generation and every decode in flight
    // for the old pair becomes refuseable. Note what is NOT a change: the
    // same renderer handed back at the same extent, which is a band replacing
    // another over pictures both bands want — reuse, and the reason the
    // identity is the destination rather than the batch's request number.
    if (renderer != fThumbnailScene || width != fThumbnailWidth || height != fThumbnailHeight) {
        ++fThumbnailCacheGeneration;
    }
    fThumbnailScene = std::move(renderer);
    fThumbnailWidth = width;
    fThumbnailHeight = height;
    syncThumbnailCountersLocked();
}

PreviewProducer::ThumbnailJob* PreviewProducer::thumbnailHeadLocked() {
    if (!fStatus.running || fThumbnailQueue.empty() || fThumbnailScene == nullptr) {
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

namespace {

// One thumbnail file read (task 6.21d): the scaled decode, plus the
// scripted cost when this path is the fixture's subject. Runs on the
// decode lane, holding no producer lock — the picture it returns is
// installed by the command thread's next pump, never here.
sk_sp<SkImage> decodeThumbnailFile(const std::string& path, uint32_t width,
                                   uint32_t height, std::string* error) {
    sk_sp<SkImage> image = SceneImageCache::decodeAtMost(path, width, height, error);
    if (faults().delaysThumbnailDecode(path)) {
        std::fprintf(stderr, "openshow-worker: scripted slow thumbnail decode: %d ms for %s\n",
                     faults().thumbnailDecodeDelayMs, path.c_str());
        for (int elapsed = 0; elapsed < faults().thumbnailDecodeDelayMs; elapsed += 20) {
            std::this_thread::sleep_for(std::chrono::milliseconds(
                std::min(20, faults().thumbnailDecodeDelayMs - elapsed)));
        }
    }
    return image;
}

}  // namespace

bool PreviewProducer::pumpThumbnailDecode() {
    // 6.21d (review F5): this call never reads a file. It COLLECTS a
    // finished lane decode (install + settle, the tail the inline decode
    // always had) or HANDS the lane the next path, and returns either way
    // — the command thread that answers the operator's present must not be
    // the thread a cold 4K backdrop keeps busy, not even for one file.
    // One lane, one file in flight: the bound that used to be "one decode
    // per wake" is now "one decode per lane", which is the same memory
    // story (the lookahead still measures finished jobs) with the wait
    // removed from the pump.
    if (fThumbnailDecode) {
        if (!fThumbnailDecode->done.load(std::memory_order_acquire)) {
            return false;  // the file is still being read; nothing else opens
        }
        const std::string path = std::move(fThumbnailDecode->path);
        sk_sp<SkImage> image = std::move(fThumbnailDecode->image);
        // The destination this picture was read FOR (task 6.22g, review A7).
        // Taken at the hand-off, before the worker started: the generation of
        // the companion whose cache was to hold it, and the extent its pixels
        // were scaled into.
        const std::uint64_t generation = fThumbnailDecode->cacheGeneration;
        const std::uint32_t decodeWidth = fThumbnailDecode->width;
        const std::uint32_t decodeHeight = fThumbnailDecode->height;
        fThumbnailDecode->worker.join();  // done means it returned; this reaps
        fThumbnailDecode.reset();
        bool compatible = false;
        std::uint32_t wantWidth = 0;
        std::uint32_t wantHeight = 0;
        std::uint64_t wantGeneration = 0;
        {
            std::lock_guard<std::mutex> lock(fMutex);
            wantWidth = fThumbnailWidth;
            wantHeight = fThumbnailHeight;
            wantGeneration = fThumbnailCacheGeneration;
            // THE COMPATIBILITY GATE. Path identity alone is not enough for
            // derived pixels: the batch, the companion and the extent can all
            // have moved while the codec ran, and a 160x90 picture is not a
            // 512x288 one. So the completion is accepted only into the
            // destination it names — the same companion generation AND the
            // same extent, the extent checked as well so a future site that
            // changed one without the other still cannot inherit. A batch
            // replaced at the SAME extent over the same renderer stays
            // compatible: reuse across bands is the feature, and this gate is
            // what keeps it honest rather than wholesale.
            compatible = fThumbnailScene != nullptr &&
                         fThumbnailCacheGeneration == generation &&
                         fThumbnailWidth == decodeWidth && fThumbnailHeight == decodeHeight;
            if (compatible) {
                // Installs (or remembers the failure) into the LIST's cache and
                // nothing else: the audience's pinned budget is not a place a
                // thumbnail's pixels ever go.
                fThumbnailScene->imageCache()->install(path, image);
                // Settle by path, not by position: the batch may have been
                // replaced while the codec ran, and a new band that wants the
                // same picture finds it already installed (the next pump
                // settles it for free), while one that no longer names it has
                // lost nothing. A job only becomes drawn-ready when its LAST
                // path settled — a slide with a background and a foreground
                // picture shows both or is not owed a frame yet (task 6.17b).
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
            }
            syncThumbnailCountersLocked();
        }
        if (compatible) {
            return true;
        }
        // Discarded, and nothing settles for it: a picture for a destination
        // that no longer exists describes no slide the operator is waiting
        // for, and settling a job on it would have the draw show pixels of the
        // wrong size or a failure the codec never reported. The path stays
        // owed, so the pass below hands it to the lane again against the
        // CURRENT destination — the re-decode is the cost of the change, and
        // it is paid once, not every frame.
        std::fprintf(stderr,
                     "openshow-worker: thumbnail decode discarded: %s was read for %ux%u of "
                     "generation %llu, the list now wants %ux%u of %llu\n",
                     path.c_str(), decodeWidth, decodeHeight,
                     static_cast<unsigned long long>(generation), wantWidth, wantHeight,
                     static_cast<unsigned long long>(wantGeneration));
        // FALLS THROUGH: this call can start the decode the list does want.
    }

    // No lane running: settle what needs no codec (free, so a whole band of
    // lyric slides is marked in one call) and find the first path that does.
    std::string path;
    uint32_t width = 0;
    uint32_t height = 0;
    std::uint64_t generation = 0;
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
                // The destination this decode is for, read with the extent it
                // was scaled to (task 6.22g, review A7): the collect below
                // asks for both back.
                generation = fThumbnailCacheGeneration;
                break;  // exactly one path handed to the lane
            }
            if (!path.empty()) {
                break;  // the lane takes it outside the lock
            }
            if (allSettled(job)) {
                job.decoded = true;
            }
        }
        syncThumbnailCountersLocked();
    }
    if (path.empty()) {
        // A head that just became ready is owed a frame; the caller turns
        // "settled" into that wake.
        return settled;
    }

    // Outside the lock, and off the command thread. Scaled: a backdrop
    // for a 160x90 thumbnail must not cost a 33 MiB allocation on the way,
    // which is the memory growth this task's acceptance names.
    fThumbnailDecode = std::make_unique<ThumbnailDecodeLane>();
    fThumbnailDecode->path = path;
    // The destination travels with the request (6.22g): whatever the batch
    // looks like when the file comes back, the picture belongs to THIS pair.
    fThumbnailDecode->cacheGeneration = generation;
    fThumbnailDecode->width = width;
    fThumbnailDecode->height = height;
    ThumbnailDecodeLane* lane = fThumbnailDecode.get();
    lane->worker = std::thread([lane, path, width, height] {
        lane->image = decodeThumbnailFile(path, width, height, &lane->error);
        lane->done.store(true, std::memory_order_release);
    });
    return settled;
}

// --- slot ownership and notifications (task 6.5, review R5) ---

void PreviewProducer::releaseSlot(uint32_t slot, uint64_t seq, const std::string& stream) {
    std::shared_ptr<PreviewStream> target;
    {
        std::lock_guard<std::mutex> lock(fMutex);
        // Stream identity (task 6.5, review R5): a release frees a slot of
        // the stream it NAMES, and only the live one. A delayed release
        // from a retired stream — or one naming nothing — finds no target
        // here, and PreviewStream::release still demands the exact
        // published sequence.
        if (fStream && fStream->name() == stream) {
            target = fStream;
        }
    }
    if (target) {
        target->release(slot, seq);
    }
}

bool PreviewProducer::takeReady(PreviewReady* out) {
    std::lock_guard<std::mutex> lock(fMutex);
    // The current stream's name (empty while none is live): notifications
    // queued by a stream that has since been replaced are dropped here
    // rather than sent — after a replacement the consumer maps the new
    // segment, and an old stream's slot/seq would describe nothing it owns
    // (task 6.5).
    const std::string live = fStream ? fStream->name() : std::string();
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

// --- reports ---

PreviewProducer::PreviewStatus PreviewProducer::status() const {
    std::lock_guard<std::mutex> lock(fMutex);
    return fStatus;
}

bool PreviewProducer::running() const {
    std::lock_guard<std::mutex> lock(fMutex);
    return fStatus.running;
}

bool PreviewProducer::streamChangePending() const {
    std::lock_guard<std::mutex> lock(fMutex);
    return fDesired.generation != fAppliedGeneration;
}

ActiveScene PreviewProducer::selectedScene() const {
    std::lock_guard<std::mutex> lock(fMutex);
    return fSelection.content;
}

// --- render thread ---

void PreviewProducer::applyStreamState() {
    DesiredStream desired;
    std::shared_ptr<PreviewStream> retire;
    {
        std::lock_guard<std::mutex> lock(fMutex);
        if (fDesired.generation == fAppliedGeneration) {
            return;
        }
        desired = fDesired;
        retire = std::move(fStream);
        fAppliedGeneration = desired.generation;
    }
    // Outside the lock: an RPC thread answering a release may still hold a
    // reference, and the unlink then happens on its last drop.
    retire.reset();
    fPreviewSurface.reset();

    if (!desired.enabled) {
        std::lock_guard<std::mutex> lock(fMutex);
        fStatus = PreviewStatus{};
        // The list's surface is a GPU allocation for a target that no
        // longer exists; it comes back with the next batch.
        fThumbnailSurface.reset();
        return;
    }

    auto stream = std::make_shared<PreviewStream>();
    std::string error;
    const bool ok = stream->create(desired.name, desired.width, desired.height, &error);
    std::lock_guard<std::mutex> lock(fMutex);
    fStatus.running = ok;
    fStatus.name = desired.name;
    fStatus.width = desired.width;
    fStatus.height = desired.height;
    fStatus.fps = desired.fps;
    fStatus.error = ok ? std::string() : error;
    if (ok) {
        fStream = std::move(stream);
        fLastPreview = std::chrono::steady_clock::time_point{};  // tick immediately
        // Same for the other targets: a new stream is due its first frame
        // at once, and their cadence markers must not inherit the old
        // stream's.
        fLastSelection = std::chrono::steady_clock::time_point{};
        fLastThumbnail = std::chrono::steady_clock::time_point{};
        fPublishedAudienceFrames = 0;  // a fresh stream owes its first frame
        // A standing batch judged against the NEW stream (task 6.16b): the
        // segment is reserved for its frame size, so a smaller stream
        // cannot hold the thumbnails that were accepted for a bigger one.
        // Standing the batch down is the safe answer and an honest one —
        // the counters go to zero, `active` goes false, and a client that
        // still wants its list asks again at an extent the new stream can
        // carry. Dropping silently into a slot overflow would be neither.
        if (fStatus.thumbnails.active &&
            !protocol::thumbnailExtentFits(fThumbnailWidth, fThumbnailHeight,
                                           fStream->bytesPerSlot())) {
            fThumbnailQueue.clear();
            fThumbnailRequest = 0;
            setThumbnailDestinationLocked(nullptr, 0, 0);  // 6.22g: with its
                                                           // in-flight decode
            fThumbnailSurface.reset();
            fStatus.thumbnails = PreviewStatus::Thumbnails{};
        }
    }
}

PreviewProducer::PreviewFill PreviewProducer::fillPreviewSlot(
    const SceneFrame& frame, const SkImage* sceneVideo, const SkImage* overlayVideo,
    PreviewStream& stream, int slot, uint32_t width, uint32_t height, SceneRenderer* renderer,
    sk_sp<SkSurface>* surface) {
    // One target's draw into a claimed slot, shared by every target of the
    // stream (task 6.16a, and the list's thumbnails in 6.16b): they differ
    // in WHICH frame they draw, WHOSE request the result answers, and with
    // which compositor and surface they draw it, and nowhere else. Two
    // copies of this would mean two cost policies, and the audience's
    // budget is exactly the kind of number that drifts when it is computed
    // twice.
    //
    // WHICH compositor and WHICH surface are therefore parameters, and used
    // as such: the slide list draws with its own renderer into its own
    // surface precisely so that a 160x90 row neither reads the audience's
    // decoded-image cache nor resizes the surface the audience's mirror is
    // drawn into. The first version of this function took both and used
    // neither — the row came back the slide's fill colour rather than its
    // backdrop, because the show's cache had never heard of the picture the
    // list had decoded, and every row alternation reallocated the mirror's
    // surface. A shared helper that ignores what makes its callers different
    // is worse than two helpers.
    sk_sp<SkSurface>& target = *surface;
    if (target == nullptr || target->width() != static_cast<int>(width) ||
        target->height() != static_cast<int>(height)) {
        const SkImageInfo info =
            SkImageInfo::Make(static_cast<int>(width), static_cast<int>(height),
                              kRGBA_8888_SkColorType, kPremul_SkAlphaType);
        target = SkSurfaces::RenderTarget(fContext, skgpu::Budgeted::kNo, info);
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
        // Only the audience target ever passes one — an edit of a scene
        // with no video on its background has no business showing somebody
        // else's.
        target->getCanvas()->drawImageRect(
            overlayVideo, SkRect::MakeIWH(static_cast<int>(width), static_cast<int>(height)),
            SkSamplingOptions(SkFilterMode::kLinear));
    }

    const SkImageInfo dst = SkImageInfo::Make(static_cast<int>(width),
                                              static_cast<int>(height),
                                              kRGBA_8888_SkColorType, kPremul_SkAlphaType);
    // Packed rows (the frame's own width, not the stream's stride) for
    // every target: for a frame of the stream's size the two are the same
    // number, and for a thumbnail it is what lets the consumer copy a frame
    // of a size it was not told about at stream.start with one memcpy and
    // no padding to skip. The header's rowBytes says which it is, on every
    // publish.
    const bool readbackOk =
        target->readPixels(dst, stream.slotPixels(slot), width * 4, 0, 0);
    // Whatever the frame did to the panel's time is settled here, success
    // or not: the milliseconds were spent either way.
    const double costSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (costSeconds > kPreviewBudgetSeconds) {
        // Give the audience the overrun back. Standing down for
        // cost × (period / budget − 1) keeps the AVERAGE preview cost at or
        // under the share it was lent, whatever one frame cost: a preview
        // that needs 40 ms gives up its frames (here, roughly 3 in 4)
        // instead of taking 40 ms out of every audience frame.
        fDeferredUntil = std::chrono::steady_clock::now() +
                         std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                             std::chrono::duration<double>(
                                 costSeconds * (kAudienceFrameSeconds / kPreviewBudgetSeconds -
                                                1.0)));
    } else {
        fDeferredUntil = std::chrono::steady_clock::time_point{};
    }
    {
        std::lock_guard<std::mutex> lock(fMutex);
        fStatus.worstMs = std::max(fStatus.worstMs, costSeconds * 1000.0);
    }
    return readbackOk ? PreviewFill::kFilled : PreviewFill::kReadbackFailed;
}

void PreviewProducer::publishMirrorFrame(const DrawInput& input) {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps = 0;
    std::shared_ptr<PreviewStream> stream;
    {
        std::lock_guard<std::mutex> lock(fMutex);
        if (!fStatus.running) {
            return;
        }
        width = fStatus.width;
        height = fStatus.height;
        fps = fStatus.fps;
        stream = fStream;
    }
    // The tag is the FRAME's revision (task 6.11a), which is what makes it
    // honest both ways: after a present it is the revision this frame put
    // on the panel (task 6.4's rule), and on a republish it is the revision
    // of the presented frame being redrawn. Asking the store for its latest
    // DESIRED revision here would tag frames with scenes nobody had drawn
    // yet.
    const SceneFrame& frame = *input.audienceFrame;
    const uint32_t revision = static_cast<uint32_t>(frame.revision);
    if (!stream) {
        return;
    }

    // Content gate (task 1.5e): republish only when the presented frame
    // changed since the last publish — a slide that sits still must not
    // keep ticking preview frames.
    if (input.frames == fPublishedAudienceFrames) {
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
    if (now < fDeferredUntil) {
        std::lock_guard<std::mutex> lock(fMutex);
        ++fStatus.deferred;
        return;
    }
    fLastPreview = now;

    const int slot = stream->claim();
    if (slot < 0) {
        // Every slot is still owned by the consumer: drop the preview
        // frame, never block the output for it (PLAN.md "Preview").
        std::lock_guard<std::mutex> lock(fMutex);
        ++fStatus.dropped;
        return;
    }

    // The same scene through the same draw path as the audience output,
    // only smaller — and the SAME FRAME: what this preview publishes is the
    // composition the audience was shown (task 6.11a, review R13), never
    // one the preview captured for itself, so no preview frame can carry a
    // scene, a blend or a clock the panel has not been shown.
    // Video belongs to what the audience sees: a background video (task
    // 5.5) goes into the scene's background layer through draw() itself,
    // anything else (the 1.5e bench) stays the dev overlay over the whole
    // preview — the readPixels inside fillPreviewSlot is CPU-synchronous,
    // which keeps this sampling of a released slot ordered ahead of any
    // later reuse.
    const ActiveScene* onAir = input.audienceScene;
    const bool asSceneBackground =
        input.video != nullptr && onAir != nullptr && *onAir && (*onAir)->background.isVideo;
    const PreviewFill fill =
        fillPreviewSlot(frame, asSceneBackground ? input.video : nullptr,
                        asSceneBackground ? nullptr : input.video, *stream, slot, width, height,
                        fAudience, &fPreviewSurface);
    if (fill == PreviewFill::kNoSurface) {
        stream->abort(slot);
        std::lock_guard<std::mutex> lock(fMutex);
        ++fStatus.dropped;
        fStatus.running = false;
        fStatus.error = "could not create the preview surface";
        return;
    }
    if (fill == PreviewFill::kReadbackFailed) {
        stream->abort(slot);
        std::lock_guard<std::mutex> lock(fMutex);
        ++fStatus.dropped;
        fStatus.error = "preview readback failed";
        return;
    }

    const uint64_t publishedNs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch())
            .count());
    const uint64_t seq =
        stream->publish(slot, revision, protocol::kPreviewTargetAudience, 0, publishedNs);
    fPublishedAudienceFrames = input.frames;  // the gate counts this publish

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
    ++fStatus.published;
}

bool PreviewProducer::publishSelectionFrame(const DrawInput& input) {
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
        if (!fStatus.running || fSelection.revision == 0 ||
            fSelection.generation == fAppliedSelectionGeneration) {
            // Nothing asked, or this ask has already been answered: one
            // request is one frame, not a second stream to keep ticking.
            return false;
        }
        content = fSelection.content;
        revision = fSelection.revision;
        request = fSelection.request;
        generation = fSelection.generation;
        width = fStatus.width;
        height = fStatus.height;
        fps = fStatus.fps;
        stream = fStream;
    }
    if (!stream || !content) {
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    // The stream's cadence, per target: the mirror and the edit share the
    // transport and so share its rate, but neither waits on the other's
    // last frame (see previewWake, which wakes for whichever is due
    // first).
    if (now - fLastSelection < std::chrono::duration<double>(1.0 / fps)) {
        return false;
    }
    if (now < fDeferredUntil) {
        std::lock_guard<std::mutex> lock(fMutex);
        ++fStatus.selection.deferred;
        return false;
    }
    fLastSelection = now;

    const int slot = stream->claim();
    if (slot < 0) {
        // The consumer owns every slot: drop, and stay owed — the next
        // pass tries again for the same request. Dropping is bounded (the
        // loop's cadence), the ask never is.
        std::lock_guard<std::mutex> lock(fMutex);
        ++fStatus.selection.dropped;
        return false;
    }

    // THE FRAME, and the whole point of the task: a SceneFrame built by
    // hand from the pinned scene, at full strength with no fade, carrying
    // the revision of the scene rather than of any present. Nothing here
    // calls captureFrame (the only call that advances a crossfade),
    // commitFrame, setActiveScene or the transport's apply — so the
    // audience keeps its scene, its blend, its clock and its media exactly
    // as they were, however many edits are drawn while they watch.
    SceneFrame frame;
    frame.scene = content;
    frame.fadeFrom = nullptr;
    frame.mediaBlend = 1.0f;
    frame.textBlend = 1.0f;
    frame.timeSeconds = std::chrono::duration<double>(now - input.startTime).count();
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
    const ActiveScene* onAir = input.audienceScene;
    const bool sameVideoAsset =
        input.video != nullptr && onAir != nullptr && *onAir &&
        (*onAir)->background.isVideo && content->background.isVideo &&
        content->background.assetPath == (*onAir)->background.assetPath;

    const PreviewFill fill =
        fillPreviewSlot(frame, sameVideoAsset ? input.video : nullptr, nullptr, *stream, slot,
                        width, height, fAudience, &fPreviewSurface);
    if (fill == PreviewFill::kNoSurface) {
        stream->abort(slot);
        std::lock_guard<std::mutex> lock(fMutex);
        ++fStatus.selection.dropped;
        fStatus.running = false;
        fStatus.error = "could not create the preview surface";
        return false;
    }
    if (fill == PreviewFill::kReadbackFailed) {
        stream->abort(slot);
        std::lock_guard<std::mutex> lock(fMutex);
        ++fStatus.selection.dropped;
        fStatus.error = "preview readback failed";
        return false;
    }

    const uint64_t publishedNs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch())
            .count());
    bool stale = false;
    {
        // THE PUBLISH GATE. The draw above took time, and a frontend that
        // kept editing may have asked again since; publishing THAT would
        // put an old scene's pixels on the wire tagged with a generation
        // the operator has already moved past. The generation is re-read
        // here, under the same lock the accept takes, and a superseded
        // frame is destroyed rather than published — the slot closes on its
        // previous value, so the consumer never sees half a frame either.
        std::lock_guard<std::mutex> lock(fMutex);
        if (fSelection.generation != generation) {
            ++fStatus.selection.stale;
            stale = true;
        }
    }
    if (stale) {
        stream->abort(slot);
        // Stay owed: the newer ask is what the render thread draws next,
        // and this one is nobody's picture any more.
        return false;
    }

    const uint64_t seq = stream->publish(slot, static_cast<uint32_t>(revision),
                                         protocol::kPreviewTargetSelection, request,
                                         publishedNs);
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
        ++fStatus.selection.published;
    }
    return true;
}

bool PreviewProducer::publishThumbnailFrame(const DrawInput& input) {
    // One slide of the pending band, drawn at the band's own extent. The
    // gates are preview.select's in the same order, and for the same
    // reasons: cadence, the audience's budget, a slot that is free, and
    // only then the draw.
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
        fps = fStatus.fps;
        stream = fStream;
        // A reference of our own for the duration of the draw: the command
        // thread can retire this compositor (a batch of another extent
        // discards its decode cache), and it must not be able to do it
        // under a pointer the render thread is painting with.
        renderer = fThumbnailScene;
    }
    if (!stream || !renderer || !content) {
        return false;
    }
    // THE SLOT BOUND, checked again here rather than only at the accept:
    // preview.start can replace the stream with a smaller segment while a
    // batch stands, and writing w*h*4 of pixels into a slot that was
    // reserved for less is not a bug this feature is allowed to have. The
    // batch goes down and says so in its own counters; the client, which
    // mirrors `active`, asks again at a size the new stream can carry.
    if (!protocol::thumbnailExtentFits(width, height, stream->bytesPerSlot())) {
        std::lock_guard<std::mutex> lock(fMutex);
        fThumbnailQueue.clear();
        fThumbnailRequest = 0;
        // The destination goes too (task 6.22g): the pixels the standing
        // companion holds were sized for a batch this stream can no longer
        // carry, and a decode in flight for it must be refused rather than
        // installed hereafter. `renderer` above keeps the object alive until
        // this pass is done with it.
        setThumbnailDestinationLocked(nullptr, 0, 0);
        fStatus.thumbnails = PreviewStatus::Thumbnails{};
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    // The stream's cadence, again per target: three kinds of picture share
    // one rate and none of them waits on another's last frame.
    if (now - fLastThumbnail < std::chrono::duration<double>(1.0 / std::max(1u, fps))) {
        return false;
    }
    if (now < fDeferredUntil) {
        std::lock_guard<std::mutex> lock(fMutex);
        ++fStatus.thumbnails.deferred;
        return false;
    }
    fLastThumbnail = now;

    const int slot = stream->claim();
    if (slot < 0) {
        // The consumer owns every slot: drop, and stay owed. A band of
        // thirty against three slots is a bounded queue, not a backlog —
        // the slides that do not fit are retried, the ones the operator has
        // scrolled past are gone with the next batch.
        std::lock_guard<std::mutex> lock(fMutex);
        ++fStatus.thumbnails.dropped;
        return false;
    }

    // A frame built the way the off-air edit builds its (task 6.16a), from
    // a scene that is not the audience's and never will be: no fade to
    // blend, both tracks at full strength, the id of the slide as its tag.
    // Nothing here calls captureFrame, commitFrame, setActiveScene or the
    // transport's apply, and the renderer doing the drawing is not the one
    // that composed the audience's frame — so the show's scene, blend,
    // clock, pins and decoded-image budget are untouched by thirty
    // thumbnails painting.
    SceneFrame frame;
    frame.scene = content;
    frame.fadeFrom = nullptr;
    frame.mediaBlend = 1.0f;
    frame.textBlend = 1.0f;
    frame.timeSeconds = std::chrono::duration<double>(now - input.startTime).count();
    frame.wallClock = std::time(nullptr);
    frame.revision = id;

    // Its background comes from the LIST's own cache, where the command
    // thread put a scaled decode. A miss draws the colour fill: the draw
    // path never opens a file (task 6.10's rule, and the reason the pump
    // exists), and a picture that arrives half a second later arrives with
    // the next band. A video background follows the selection's rule —
    // sample the audience's frame only when the two scenes name the same
    // asset, because a scene with no transport has no business wearing
    // someone else's.
    const ActiveScene* onAir = input.audienceScene;
    const bool sameVideoAsset =
        input.video != nullptr && onAir != nullptr && *onAir &&
        (*onAir)->background.isVideo && content->background.isVideo &&
        content->background.assetPath == (*onAir)->background.assetPath;

    const PreviewFill fill =
        fillPreviewSlot(frame, sameVideoAsset ? input.video : nullptr, nullptr, *stream, slot,
                        width, height, renderer.get(), &fThumbnailSurface);
    if (fill == PreviewFill::kNoSurface) {
        stream->abort(slot);
        std::lock_guard<std::mutex> lock(fMutex);
        ++fStatus.thumbnails.dropped;
        fStatus.running = false;
        fStatus.error = "could not create the thumbnail surface";
        return false;
    }
    if (fill == PreviewFill::kReadbackFailed) {
        stream->abort(slot);
        std::lock_guard<std::mutex> lock(fMutex);
        ++fStatus.thumbnails.dropped;
        fStatus.error = "thumbnail readback failed";
        return false;
    }

    const uint64_t publishedNs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch())
            .count());
    bool stale = false;
    {
        // THE PUBLISH GATE, the batch's version of it. The draw took time
        // and the operator may have scrolled in the middle of it; pixels of
        // a slide nobody is looking at must not reach the wire tagged with
        // a generation the client has already replaced. The slot closes on
        // its previous value, so nothing half-written is ever readable.
        std::lock_guard<std::mutex> lock(fMutex);
        if (fThumbnailRequest != request) {
            ++fStatus.thumbnails.stale;
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
        ++fStatus.thumbnails.published;
    }
    return true;
}

void PreviewProducer::drawTargets(const DrawInput& input) {
    // ONE preview frame per pass (task 6.16a, and three targets share that
    // one by 6.16b). The audience mirror, the editor's off-air render and
    // the list's thumbnails share one stream, one cadence and one audience
    // budget, so they take turns rather than all drawing in the pass the
    // audience is waiting on: whichever has gone longest without a frame
    // goes first, and the others stay owed for the next one. During a burst
    // of edit asks that is what keeps the panel's picture arriving; while a
    // show plays it is what keeps an edit from waiting for the audience to
    // go quiet; and a band of thirty thumbnails fills a list without ever
    // taking two frames in a row from either of them. A target that stood
    // down for its own cadence or for the budget does not consume the pass
    // — the next one draws.
    const bool mirrorOwed =
        input.frames > 0 && input.frames != fPublishedAudienceFrames;
    bool selectionOwed = false;
    bool thumbnailOwed = false;
    {
        std::lock_guard<std::mutex> lock(fMutex);
        selectionOwed = fStatus.running && fSelection.revision != 0 &&
                        fSelection.generation != fAppliedSelectionGeneration;
        thumbnailOwed = thumbnailHeadLocked() != nullptr;
    }
    // The cadence markers are render-thread-only (see the members' note),
    // so the turn order reads them directly: oldest-outstanding first.
    if (thumbnailOwed && (!selectionOwed || fLastThumbnail < fLastSelection) &&
        (!mirrorOwed || fLastThumbnail < fLastPreview)) {
        if (publishThumbnailFrame(input)) {
            return;
        }
    }
    if (selectionOwed && (!mirrorOwed || fLastSelection <= fLastPreview)) {
        if (publishSelectionFrame(input)) {
            return;
        }
    }
    if (mirrorOwed) {
        publishMirrorFrame(input);
    }
}

PreviewProducer::Wake PreviewProducer::previewWake(std::chrono::steady_clock::time_point now,
                                                   unsigned long long audienceFrames) const {
    // The sleep decision the run loop used to compute inline (task 1.5e):
    // either preview target can owe a frame — the MIRROR when the
    // audience's last presented frame has not reached the stream (the
    // content gate), the EDITOR when an accepted off-air ask has not been
    // drawn (task 6.16a), the LIST when a settled head is waiting (task
    // 6.16b). Each has its own cadence marker and they share the
    // audience's budget, so the loop sleeps to whichever comes round first.
    // The re-ask lesson applies to all three: a publish that stood down
    // left its own marker where it was, so a horizon in the future is a
    // SLEEP and never a spin — the first fixture measured a spinning
    // preview loop at ~20,000 iterations a second, which is precisely the
    // busy loop 1.5e exists to prevent.
    std::lock_guard<std::mutex> lock(fMutex);
    Wake wake;
    if (!fStatus.running) {
        return wake;
    }
    const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(1.0 / std::max(1u, fStatus.fps)));
    const auto horizonOf = [&](const std::chrono::steady_clock::time_point& last) {
        return std::max(last + period, fDeferredUntil);
    };
    const bool mirrorOwed = audienceFrames != fPublishedAudienceFrames;
    const bool selectionOwed = fSelection.revision != 0 &&
                               fSelection.generation != fAppliedSelectionGeneration;
    // The list's head counts as owed only once its background has been
    // settled on the command thread — which is exactly what the publish
    // checks, because it asks this same function. Ask the same question in
    // both places and a slide waiting on its codec leaves the loop ASLEEP
    // until the pump wakes it, instead of spinning on a frame that cannot
    // be produced.
    const ThumbnailJob* head =
        (fStatus.running && !fThumbnailQueue.empty() && fThumbnailScene != nullptr)
            ? &fThumbnailQueue.front()
            : nullptr;
    const bool thumbnailOwed = head != nullptr && head->decoded;
    if (!(mirrorOwed || selectionOwed || thumbnailOwed)) {
        return wake;
    }
    // The sooner horizon of whichever targets are owed: three cadence
    // markers, the minimum of the ones that matter.
    auto nextAllowed = std::chrono::steady_clock::time_point::max();
    const auto consider = [&](const std::chrono::steady_clock::time_point& last) {
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
    wake.owed = true;
    wake.nextAllowed = nextAllowed;
    return wake;
}

void PreviewProducer::shutdown() {
    // Render thread (or the thread destroying the session, before the GPU
    // context this session's surfaces live on goes away).
    fPreviewSurface.reset();
    fThumbnailSurface.reset();
    std::lock_guard<std::mutex> lock(fMutex);
    fStream.reset();  // unlinks the preview segment (unless a release call still holds it)
    // The off-air target's content pin goes with the stream it was drawn
    // for (task 6.16a): a closed session must not sit on a scene the store
    // would otherwise be free to evict, and no later pass exists to draw
    // it.
    fSelection = SelectionDesired{};
    fAppliedSelectionGeneration = fSelection.generation;
    fStatus.selection = PreviewStatus::Selection{};
}

}  // namespace openshow::worker
