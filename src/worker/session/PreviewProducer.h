/*
 * PreviewProducer: the one owner of the operator's picture (task 6.18a2,
 * review R5/R11/R13), extracted from OutputSession as the second half of
 * the 6.18a graduation and placed next to SceneStore for the same reason:
 * what used to be preview state smeared across the session — the desired
 * stream and its generation, the live shm stream, the three targets'
 * cadence and budget markers, the editor's selection, the slide list's
 * thumbnail queue, the pending ready notifications and the status mirror —
 * is now one object with one set of rules.
 *
 * The rules this class owns, in its own words:
 *
 *  - Three targets, one stream. The audience mirror, the editor's off-air
 *    selection (task 6.16a) and the slide list's thumbnails (task 6.16b)
 *    share one shm segment, one cadence and the audience's timing budget.
 *    A draw pass publishes at most ONE frame: whichever owed target has
 *    gone longest without a picture draws, the others stay owed.
 *  - The generation gates. A selection or a thumbnail batch is accepted
 *    only on a request strictly after the accepted one (modulo 2^32), and
 *    each publish re-reads its generation under the lock: a replayed ask
 *    cannot resurrect imagery of a scene the operator left behind, and a
 *    frame superseded mid-draw is destroyed rather than published (R16).
 *  - The audience's budget (R13). Every target draws through one fill, so
 *    one cost policy governs all three: a preview frame that cost more
 *    than the audience can lend defers the next attempt until the average
 *    is inside the share again. The mirrors never advance the audience's
 *    show: a frame comes IN from the session (the one the audience drew,
 *    or the last it was shown) and this object never composes, captures or
 *    credits a frame of its own — no second owner of a frame's revision
 *    (R13 again), no second owner of a slot's ownership (R5).
 *  - Releases name their stream (R5). A release for a retired or unknown
 *    stream, an unknown slot, or a sequence that is not exactly the slot's
 *    published one changes nothing; ready notifications queued by a
 *    replaced stream are dropped, never sent.
 *  - Thumbnails are bounded three ways: the batch cap is the memory bound,
 *    the decode lookahead bounds how fast it can grow, and the companion
 *    renderer's own cache budget bounds the working set. The queue IS the
 *    coalescing rule: a newer band replaces the old wholesale. The draw
 *    never opens a file — the command thread's pump decodes, one file per
 *    call (task 6.10's rule applied to the list).
 *  - Status is a mirror of accepted intent plus honest counters
 *    (published/dropped/deferred/stale per target). `active` means the ask
 *    was understood; published means a frame reached the stream.
 *
 * Thread rules. Every method takes this object's own mutex, so a command
 * thread and the render thread may call concurrently, and the session may
 * call while holding its own — the lock order is session -> producer ->
 * (SceneStore via the select pin, PreviewStream), never the reverse, and
 * the producer calls back through no caller code except the two documented
 * seams: the `pin` argument of select() and the caller's own wake after an
 * accepted command (this object never touches GLFW). Two command methods
 * release the producer lock mid-call for their declared heavy work —
 * createCompanion's font setup in requestThumbnails, one file decode in
 * pumpThumbnailDecode — exactly as the session did before the extraction,
 * which is what keeps that work out of the render thread's fast sections
 * (R10). The GPU surfaces are render-thread-only members, touched by
 * applyStreamState()/drawTargets() and by shutdown() on the thread that
 * destroys the context they live on.
 *
 * What stays with the session: the frame itself (SceneRenderer capture and
 * present), the clock the frame times are taken against, the wake, and the
 * image-cache pin union that the accepted selection contributes to. In
 * other words: frames in, slots out.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "include/core/SkRefCnt.h"
#include "include/core/SkSurface.h"

#include "openshow/protocol/preview_wire.h"
#include "openshow/scene/scene.h"

#include "render/Layer.h"  // ActiveScene, SceneFrame

class GrDirectContext;

namespace openshow::worker {

class PreviewStream;
class SceneRenderer;

class PreviewProducer {
public:
    PreviewProducer() = default;
    ~PreviewProducer();

    PreviewProducer(const PreviewProducer&) = delete;
    PreviewProducer& operator=(const PreviewProducer&) = delete;

    // The GPU surface the targets draw into is built on this context, and
    // the audience mirror and the off-air selection draw with this
    // compositor. Attached by the session's open() and detached by its
    // close(); the pointers are only ever used from the render thread (and
    // the command thread's companion build), between attach and detach.
    // A producer with no renderer attached refuses the asks that need one.
    void attachRenderer(GrDirectContext* context, SceneRenderer* audience);
    void detachRenderer();

    // --- stream lifecycle (command thread) ---

    // What preview.start reports: where the frames will live and how the
    // segment is laid out (openshow/protocol/preview_wire.h). The segment
    // itself is created by the render thread on its next frame, so wait
    // for the first preview.ready notification (or status().running)
    // before mapping.
    struct PreviewInfo {
        std::string name;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t slotCount = 0;
        uint32_t bytesPerSlot = 0;
    };

    // Enables the preview pipeline (task 1.4): the same scene, drawn to a
    // GPU offscreen surface at the requested size, is read back into
    // rotating shm slots at the requested cadence. Calling start again
    // replaces the stream under a new name. Geometry limits keep a preview
    // a preview, not a second output.
    bool start(uint32_t width, uint32_t height, uint32_t fps, PreviewInfo* info,
               std::string* error);

    // Stands the stream down. Returns true when anything changed — the
    // caller wakes the render thread and re-derives its image pins on a
    // change (the selection's still goes with the stream).
    bool stop();

    // --- the editor's off-air target (task 6.16a, review R16) ---

    // `revision` is a scene in the prepared store; the render thread draws
    // THAT scene into the preview stream — no present, no capture, so the
    // audience's scene, playback and crossfades are untouched by the act
    // of looking at an edit. Revision 0 stands the target down and the
    // stream goes back to mirroring the audience alone.
    //
    // `request` is the frontend's own generation for the ask, and it must
    // come strictly after the last accepted one (modulo 2^32, so a
    // frontend that has been open for four billion edits still advances).
    // That rule is what makes stale imagery impossible rather than
    // unlikely: a replay of an old ask cannot resurrect a picture of a
    // scene the operator left behind, and every frame the stream carries
    // names the generation it answers.
    //
    // `pin` resolves a store revision into its immutable content (the
    // session binds it to SceneStore::pin). The pin is taken AT ACCEPT —
    // a content copy, exactly as a frame snapshot pins its present —
    // because the store may evict the revision while the render thread
    // works, and an edit vanishing halfway to the panel is the same race
    // R4 found for presents. `pin` must not re-enter this object.
    using ScenePin = std::function<bool(uint64_t revision, ActiveScene* out)>;
    bool select(uint64_t revision, std::uint32_t request, const ScenePin& pin,
                std::string* error);

    // --- thumbnails (task 6.16b, review R16) ---

    // One slide of a thumbnail batch. `key` is the FRONTEND's name for the
    // slide (a slide id, in practice): the producer never interprets it,
    // it only refuses an empty or absurd one and hands it back in the ack
    // beside the id that names the entry on the wire. `scene` is a
    // compiled scene — the same payload scene.prepare takes, and it goes
    // nowhere near the store: no revision, no pin, nothing presentable.
    struct ThumbnailEntry {
        std::string key;
        openshow::scene::Scene scene;
    };

    // What the worker made of a batch. `accepted` pairs each entry's key
    // with the id its frames will carry (the slot header's `revision`),
    // and `refused` names the entries it will not draw and why — a scene
    // that does not compile costs that slide its picture, not the whole
    // band.
    struct ThumbnailAck {
        std::uint32_t request = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::vector<std::pair<std::string, std::uint32_t>> accepted;
        std::vector<std::pair<std::string, std::string>> refused;
    };

    // Replaces the pending thumbnail batch with this one, at `width` x
    // `height`, and answers with the per-slide ids. Replacement is the
    // coalescing rule and the cancellation rule at once: whatever was
    // queued for an older list is gone, so a band the operator has
    // scrolled past costs nothing further, and the newest ask is the only
    // debt.
    //
    // Refused, in the order the operator can act on it: no preview stream
    // running; an extent outside the agreed box or too big for a slot
    // (thumbnailExtentFits); a batch over kMaxThumbnailBatch; a request
    // that does not come strictly after the accepted one, named on both
    // sides (the rule select follows — a replayed ask must not resurrect
    // pictures of a list nobody is looking at); a key empty or over
    // kMaxThumbnailKeyLength. An empty batch with a new request stands the
    // target down. Releases its own lock for the companion renderer's
    // font setup and re-takes it to commit.
    bool requestThumbnails(const std::vector<ThumbnailEntry>& batch, uint32_t width,
                           uint32_t height, std::uint32_t request, ThumbnailAck* ack,
                           std::string* error);

    // Advances the thumbnail decode lane, on the CALLER's thread (the RPC
    // loop, which wakes periodically). The rule that keeps the render thread
    // clean (task 6.10) is that a draw never opens a file, so the pictures a
    // thumbnail needs have to be decoded somewhere else — and since 6.21d
    // (review F5) even the command thread does not read the file inline:
    // one call at most HANDS one path to the lane (a single worker thread,
    // the bound this feature keeps), collects a finished decode and settles
    // it, and never waits. Settled-without-codec work (nothing to decode, or
    // already cached) still completes on the spot. Returns true when a call
    // settled at least one job (so the caller wakes the render thread); a
    // lane mid-file answers false and the caller keeps serving commands —
    // the whole point, since one slow cold file used to hold status,
    // release, present and shutdown for its every millisecond.
    bool pumpThumbnailDecode();

    // The consumer finished copying slot@seq of the named stream; the slot
    // becomes claimable again. Proof of ownership (task 6.5, review R5):
    // the release must name the stream that is currently live — releases
    // for a retired or unknown stream, an unknown slot or a sequence that
    // is not exactly the slot's published one are ignored.
    void releaseSlot(uint32_t slot, uint64_t seq, const std::string& stream);

    // Pops one queued preview.ready notification (RPC thread). At most one
    // is queued per slot: if the RPC side lags, the newest frame for that
    // slot is all that matters. Ready notifications left queued for a
    // retired stream are dropped, never sent: the socket must not describe
    // slots of a segment the consumer no longer has (task 6.5).
    struct PreviewReady {
        uint32_t slot = 0;
        uint64_t seq = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t rowBytes = 0;
        uint32_t revision = 0;
        // Which target and which request these pixels answer (task 6.16a):
        // the consumer needs both before it copies anything, because "the
        // newest frame of slot 2" no longer says whose picture it is.
        uint32_t target = 0;
        uint32_t request = 0;
        uint64_t publishedNs = 0;
        std::string stream;  // segment name this frame belongs to (task 6.5)
    };
    bool takeReady(PreviewReady* out);

    // --- reports (any thread) ---

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
        // Task 6.11a (review R13): the audience's timing budget. A preview
        // frame that cost more than the audience can spare defers the next
        // one instead of repeating on every frame; `deferred` counts the
        // publish attempts that stood down, and `worstMs` is the most
        // expensive preview frame since the stream started — what the
        // preview is charging the output, on the wire.
        uint64_t deferred = 0;
        double worstMs = 0.0;
        std::string error;
        // The editor's off-air target (task 6.16a, review R16): a prepared
        // scene the audience is NOT shown, rendered into the same stream so
        // the edit canvas can show the worker's own picture. `active` says a
        // selection is registered (whatever the render thread has managed to
        // draw since); revision/request name the request it answers, and the
        // counters are the same three questions as the mirror's — what
        // reached the socket, what the consumer's slots blocked, and what a
        // newer request superseded before it could be published.
        struct Selection {
            bool active = false;
            uint64_t revision = 0;
            std::uint32_t request = 0;
            uint64_t published = 0;
            uint64_t dropped = 0;
            uint64_t deferred = 0;
            uint64_t stale = 0;
        } selection;
        // The slide list's target (task 6.16b, review R16): a BATCH of
        // scenes the store has never held, drawn small on demand. `active`
        // says a batch stands (its request and extent are the accepted
        // ones); `queued` counts the slides not yet drawn, and
        // `decodePending` the ones waiting on a background decode that runs
        // on the thumbnail lane — the pair is what distinguishes "the
        // worker is working through the list" from "the worker is idle with
        // a list it will never draw". published/dropped/deferred/stale are
        // the same three questions as the other two targets, plus the
        // superseded-batch count.
        struct Thumbnails {
            bool active = false;
            std::uint32_t request = 0;
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            uint64_t queued = 0;
            uint64_t decodePending = 0;
            uint64_t published = 0;
            uint64_t dropped = 0;
            uint64_t deferred = 0;
            uint64_t stale = 0;
            // The LIST's decoded-picture cache, on the wire next to the
            // show's own (status.scene.images): the evidence that a scrolled
            // list plateaus instead of growing, and that its bytes are not
            // the audience's. `budget` is the ceiling this cache holds the
            // line at — a different number from the renderer's default is
            // what proves the two caches are really two.
            uint64_t decodedBytes = 0;
            uint64_t entries = 0;
            uint64_t evictions = 0;
            uint64_t budget = 0;
        } thumbnails;
    };

    PreviewStatus status() const;

    // True when the shm segment is live (the accepted-but-not-yet-built
    // stream is `start`ed, not `running`; gates that must answer during
    // that window check the desired stream, as the refusals' comments say).
    bool running() const;

    // True when a start/stop is waiting for the render thread to create or
    // retire the segment (the session's frame loop counts this as a reason
    // to present).
    bool streamChangePending() const;

    // The accepted selection's pinned content (null when none). The
    // session's image-pin union takes it: the selection's still is drawn
    // by the render thread like any other background, so it is held for as
    // long as the selection is registered — even after the store evicts
    // the revision the pin was taken from.
    ActiveScene selectedScene() const;

    // --- render thread ---

    // Creates or retires the stream the accepted commands asked for
    // (task 1.4's generation rule). Called at the top of every frame.
    void applyStreamState();

    // Everything one handoff gives the producer (task 6.18a2's "frames
    // in"): the frame the audience drew (a presenting frame) or was last
    // shown (a republish), the video slot the audience's transport is
    // showing, the session's presented-frame counter for the mirror's
    // content gate, the clock its built frames time-stamp against, and the
    // on-air scene the producer compares a target's background against
    // before it samples anyone's video. Render-thread state, read without
    // a second opinion, exactly as when the session held this code.
    struct DrawInput {
        const SceneFrame* audienceFrame = nullptr;
        SkImage* video = nullptr;
        unsigned long long frames = 0;
        std::chrono::steady_clock::time_point startTime{};
        const ActiveScene* audienceScene = nullptr;
    };

    // Decides which preview target draws and publishes at most one frame
    // (task 6.16a/b): the three share the stream, its cadence and the
    // audience's budget, so they take turns rather than all drawing in the
    // pass the audience is waiting on.
    void drawTargets(const DrawInput& input);

    // Whether a preview target owes a frame and when the next attempt may
    // run — the run loop's sleep decision (task 1.5e). The question is
    // asked against the same three markers the publish checks, so a target
    // waiting on cadence, the budget or its codec leaves the loop ASLEEP
    // until the moment it can produce, never spinning on a frame it cannot.
    struct Wake {
        bool owed = false;
        std::chrono::steady_clock::time_point nextAllowed{};
    };
    Wake previewWake(std::chrono::steady_clock::time_point now,
                     unsigned long long audienceFrames) const;

    // The session's close() calls this before the GPU context goes away:
    // the surfaces are allocations on it, and the stream unlink belongs to
    // this object's shutdown, not to a later destructor on someone else's
    // thread.
    void shutdown();

private:
    // One accepted slide of the pending batch. Used only through
    // fThumbnailQueue: the scene is a transient the store has never held,
    // `decoded` says its background has been settled by the command thread
    // — decoded, failed, or not needed — and `id` is the name its frames
    // carry.
    struct ThumbnailJob {
        std::string key;
        std::uint32_t id = 0;
        ActiveScene content;
        // Everything this thumbnail must decode before it can draw (task
        // 6.16b's still background first, then the stack's foreground
        // stills in draw order — task 6.17b), deduplicated and non-empty.
        std::vector<std::string> imagePaths;
        // Per-path settle flag (decoded, failed, or found in the cache):
        // the job owes its picture once every path has been settled.
        std::vector<bool> pathSettled;
        bool decoded = false;
    };

    // The batch's head, under the same lock both the pump and the draw
    // use: non-null only when a slide stands and its background has been
    // settled (decoded, failed, or not needed). The run loop's horizon asks
    // this and the publish asks it, so the loop cannot spin on a slide
    // whose codec nobody has run yet.
    ThumbnailJob* thumbnailHeadLocked();
    // The batch's own two counters, re-derived from the queue wherever the
    // queue or its decode flags change: `queued` is what the worker still
    // owes a picture of, `decodePending` the part of that waiting on a
    // codec. Both are reported rather than inferred by a frontend because
    // the pair is the difference between "working through the list" and
    // "idle with a list it will never draw".
    void syncThumbnailCountersLocked();

    // What one draw-and-readback did to the stream (task 6.16a). Every
    // target shares this body, so the audience's cost policy — the reason
    // a preview exists without owning the frame it is drawn out of — is
    // one implementation rather than three that drift. `renderer` and
    // `surface` travel together (task 6.16b): a thumbnail is drawn by the
    // companion compositor into a surface of the batch's own size, and a
    // single shared surface holder would reallocate the audience's surface
    // on every pass that alternated a 640x360 mirror with a 160x90
    // thumbnail.
    enum class PreviewFill { kFilled, kNoSurface, kReadbackFailed };
    PreviewFill fillPreviewSlot(const SceneFrame& frame, const SkImage* sceneVideo,
                                const SkImage* overlayVideo, PreviewStream& stream, int slot,
                                uint32_t width, uint32_t height, SceneRenderer* renderer,
                                sk_sp<SkSurface>* surface);

    // The audience mirror of the frame the session drew.
    void publishMirrorFrame(const DrawInput& input);
    // The editor's target (task 6.16a): draws the accepted off-air
    // selection and publishes it with the generation it answers. Returns
    // true only when a frame reached the stream — a pass that owed nothing,
    // or stood down for the audience, draws the mirror instead, and the ask
    // stays owed until it lands.
    bool publishSelectionFrame(const DrawInput& input);
    // The list's target (task 6.16b): draws the head of the batch whose
    // background has been decoded, at the batch's own extent, and publishes
    // it with the batch generation and the slide's id. Returns true only
    // when a frame reached the stream; the slide is popped when it does, so
    // one accepted slide is one picture and a draw that stood down or was
    // superseded leaves it queued for the next pass.
    bool publishThumbnailFrame(const DrawInput& input);

    // Desired stream state (command thread sets; the render thread applies
    // on generation change and creates/retires the segment).
    struct DesiredStream {
        bool enabled = false;
        uint32_t width = 640;
        uint32_t height = 360;
        uint32_t fps = 15;
        std::string name;  // shm object name when enabled
        uint64_t generation = 0;
    };

    // The editor's off-air target (task 6.16a, review R16). The accepted
    // scene is PINNED AT ACCEPT — a content copy of the store entry,
    // exactly as a frame snapshot pins its present.
    struct SelectionDesired {
        ActiveScene content;  // null: no selection, the stream mirrors alone
        uint64_t revision = 0;
        std::uint32_t request = 0;      // the accepted generation
        std::uint32_t generation = 0;   // bumped per accepted change (the wake)
    };

    mutable std::mutex fMutex;

    DesiredStream fDesired;              // fMutex
    uint32_t fStreamRequestCounter = 0;  // fMutex, names the segments
    uint64_t fAppliedGeneration = 0;     // fMutex (the render thread writes)
    std::shared_ptr<PreviewStream> fStream;  // fMutex; shared because a
    // release answering RPC may hold it exactly as the render thread
    // retires it — destruction defers to the last reference.
    std::optional<PreviewReady> fPendingReady[protocol::kPreviewSlotCount];  // fMutex

    // Cadence markers (task 1.5e's per-target rate) and the audience
    // budget: no preview attempt before fDeferredUntil, set when one cost
    // more than the audience can lend (task 6.11a, review R13). Written
    // only by applyStreamState()/publish* and read by drawTargets()/
    // previewWake() — all on the thread running the frame loop, so they are
    // render-thread-only members, exactly as they were in the session.
    std::chrono::steady_clock::time_point fLastPreview;     // render thread
    std::chrono::steady_clock::time_point fLastSelection;   // render thread
    std::chrono::steady_clock::time_point fLastThumbnail;   // render thread
    std::chrono::steady_clock::time_point fDeferredUntil;   // render thread

    // Content gate (task 1.5e): the preview republishes only when the
    // presented frame changed since the last publish. This is the
    // audience-frame counter at the last mirror publish; the session's own
    // counter rides DrawInput. Written by publishMirrorFrame/
    // applyStreamState and read by drawTargets()/previewWake() — all on
    // the frame loop, like the cadence markers.
    unsigned long long fPublishedAudienceFrames = 0;  // render thread

    SelectionDesired fSelection;              // fMutex
    // The generation the last SUCCESSFUL selection publish satisfied: a
    // dropped or budget-deferred attempt leaves the ask owed, so one
    // accepted request yields exactly one frame rather than a second
    // stream.
    std::uint32_t fAppliedSelectionGeneration = 0;  // fMutex

    // The list's batch (task 6.16b, review R16). One pending set, replaced
    // wholesale per accepted request: the queue IS the coalescing, so a
    // slide the operator has scrolled past stops being owed the moment a
    // newer band arrives, and its memory goes with it. Bounded by
    // kMaxThumbnailBatch, and every entry is a compiled scene the store
    // has never held — no revision, no pin, nothing presentable — so a
    // scrolled list cannot grow the show's picture memory or its 16-entry
    // store.
    std::vector<ThumbnailJob> fThumbnailQueue;  // fMutex
    // The accepted batch generation, following select's rule (strictly
    // after, modulo 2^32). Distinct from the selection's own counter: two
    // targets ask independently and neither can advance the other's.
    std::uint32_t fThumbnailRequest = 0;  // fMutex
    // Per-slide ids, handed out per accepted entry and named in each
    // frame's `revision`. Non-zero: 0 means "no id", the same convention
    // every other identity here uses.
    std::uint32_t fThumbnailIdCounter = 0;  // fMutex
    std::uint32_t fThumbnailWidth = 0;      // fMutex, the accepted extent
    std::uint32_t fThumbnailHeight = 0;     // fMutex

    // The decode lane (task 6.21d, review F5). ONE worker thread, at most
    // one file in flight: the pump hands it a path, never waits, and the
    // NEXT pump call publishes the finished picture (install + settle by
    // path, under fMutex — the same tail the inline decode had, and just as
    // tolerant of a batch replaced mid-file). The struct is command-thread
    // state — only pumpThumbnailDecode() and the destructor touch the
    // unique_ptr — and the worker writes ONLY its own fields before raising
    // `done` (release), so the publishing read is ordered. No producer lock
    // is held across the file: that is the whole finding. Shutdown joins
    // the lane (a decode is bounded by the file), publishing nothing.
    struct ThumbnailDecodeLane {
        std::string path;             // the file this lane reads (command thread)
        sk_sp<SkImage> image;         // the worker's answer; null on failure
        std::string error;            // and what the codec said, if anything
        std::atomic<bool> done{false};
        std::thread worker;
    };
    std::unique_ptr<ThumbnailDecodeLane> fThumbnailDecode;  // command thread

    // The companion compositor (task 6.16b): built on the command thread
    // at the first batch (font setup belongs nowhere near a frame) and
    // published under fMutex, which is also what makes the render thread's
    // read of it safe. Shared text engine, own layout cache, own decoded
    // images at a small budget. Null until the first ask.
    //
    // Shared rather than unique because the render thread draws with it:
    // the command thread may drop it (an extent change discards its decode
    // cache) while a thumbnail publish is in flight, and a raw pointer
    // across that moment is a use-after-free with a codec in the middle.
    // The publish takes its own reference under the lock and the last one
    // out destroys.
    std::shared_ptr<SceneRenderer> fThumbnailScene;  // fMutex

    // The GPU surface handles (attachRenderer/detachRenderer). The context
    // is only ever used on the render thread; the audience compositor also
    // serves the command thread's createCompanion.
    GrDirectContext* fContext = nullptr;   // fMutex
    SceneRenderer* fAudience = nullptr;    // fMutex

    // The mirror's surface is the stream's size; the list's surface is the
    // batch's own, or the two thrash one allocation per pass. Render-thread
    // GPU resources, detached by shutdown(); declared behind the lock they
    // are never taken under.
    sk_sp<SkSurface> fPreviewSurface;    // render thread only
    sk_sp<SkSurface> fThumbnailSurface;  // render thread only

    PreviewStatus fStatus;  // fMutex
};

}  // namespace openshow::worker
