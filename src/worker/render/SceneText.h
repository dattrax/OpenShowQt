/*
 * Production scene presentation primitives (tasks 3.1–3.2).
 *
 * Everything a Scene says about type — colours, families, sizes, the lines
 * of each item — becomes pixels here: SkParagraph paragraphs wrapped at the
 * authored canvas width, centred (the lyric default), with per-run font
 * fallback through the fontconfig-backed FontCollection, and a whole-block
 * shrink so an over-tall stanza still fits (FreeShow's shrinkToFit,
 * docs/reference.md §7). The background composites the same way the layer
 * model composites: colour fill, then the background image cover-fitted
 * (3.2), with a per-layer alpha dissolve for the fade transition — no
 * render-to-texture, transparency goes straight into the paint, per
 * render/Layer.h. The engine is the one font setup the worker has:
 * SceneRenderer owns one, the development decoration (render/TestScene.h)
 * builds its demo title through it, and the layers draw through
 * layoutScene()/drawLayout(), so what the fixture tests assert is what the
 * audience window renders.
 *
 * Coordinates are authored 1920x1080 units: callers apply the output rule
 * (scale = width / 1920, centred) before drawing, exactly like every other
 * layer. Sizes in a SceneItem are authored units too — 0 means the engine's
 * default lyric size.
 */

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "include/core/SkFontMgr.h"
#include "include/core/SkImage.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkTypeface.h"
#include "modules/skparagraph/include/FontCollection.h"
#include "modules/skparagraph/include/Paragraph.h"
#include "modules/skunicode/include/SkUnicode.h"
#include "openshow/scene/scene.h"

class SkCanvas;
class SkPaint;
class SkSurface;

namespace openshow::worker {

// The authored canvas every scene is designed against
// (docs/reference.md §7: zoom = output width / 1920).
inline constexpr float kSceneCanvasWidth = 1920.0f;
inline constexpr float kSceneCanvasHeight = 1080.0f;

// Authored-canvas layout constants. Text lives inside the margins; wrapping
// happens at kSceneCanvasWidth - 2 * kSceneMarginX and the fitted block
// must fit between the vertical margins.
inline constexpr float kSceneMarginX = 160.0f;
inline constexpr float kSceneMarginY = 100.0f;

// The size a SceneItem with fontSize 0 (the model's "renderer default")
// gets — a lyric body size on the authored canvas.
inline constexpr float kSceneDefaultFontSize = 96.0f;

// Shrink-to-fit never goes below this authored size.
inline constexpr float kSceneMinFontSize = 12.0f;

class SceneImageCache;  // below; drawLayout looks images up through it

class SceneTextEngine {
public:
    SceneTextEngine();
    ~SceneTextEngine();

    SceneTextEngine(const SceneTextEngine&) = delete;
    SceneTextEngine& operator=(const SceneTextEngine&) = delete;

    // Assembles the fontconfig font manager and the default and bold
    // typefaces. No paragraph FontCollection is built here: that mutable
    // half belongs to the threads that shape (see fontCollection(), task
    // 6.22a). Fills error on failure.
    bool create(std::string* error);

    // The faces every worker text draws with when a scene names no family.
    const sk_sp<SkTypeface>& regular() const { return fRegular; }
    const sk_sp<SkTypeface>& bold() const { return fBold; }

    // Paragraph services for callers building their own paragraphs (the
    // demo title) with the engine's font resolution.
    //
    // The FontCollection is the mutable half of shaping (task 6.22a, review
    // A1): skparagraph's FontCollection resolves family names through
    // fontconfig into a lock-free face-cache hash table (pinned source:
    // modules/skparagraph/src/FontCollection.cpp — a lookup and an insert
    // without a mutex). Two threads shaping through ONE collection — the
    // frame loop's cache miss and the prepare executor's prewarm, or a
    // thumbnail companion — race inside that table. So the collection is
    // OWNED PER THREAD: this call returns the calling thread's own instance,
    // created on first use over the engine's shared SkFontMgr, and no thread
    // ever touches another's. What the engine shares is what is thread-safe
    // by construction: the font manager (the fontconfig port guards its own
    // typeface cache and defers to fontconfig's thread-safe matching above
    // 2.13.93), ref-counted typefaces, and Unicode. The cost is one cold
    // family resolution per family per thread (the worker has a handful);
    // the alternative — one collection under a lock — would make rendering
    // wait for preparation, which is the very coupling 6.9b removed.
    //
    // A layout built through one thread's collection still travels to
    // another (that is the prewarm hand-off); what does NOT travel freely is
    // a PAINT of a shared handle — see drawLayout.
    sk_sp<skia::textlayout::FontCollection> fontCollection() const;
    const sk_sp<SkUnicode>& unicode() const { return fUnicode; }

    // One item laid out: wrapped at its authored box width, centred,
    // explicit line height. The box fields are the item's authored box
    // (task 4.3) — the region drawLayout centres the paragraph in.
    //
    // Task 6.17b makes the laid-out stack heterogeneous: an image entry
    // has no paragraph and no text; its box is where the picture is
    // stretched and assetPath names what to look up in the image cache.
    // drawLayout draws the entries in this order, so the stack's draw
    // order survives layout untouched.
    struct LaidOutItem {
        std::unique_ptr<skia::textlayout::Paragraph> paragraph;
        float fontSize = 0.0f;   // authored px actually used (after fit scale)
        float height = 0.0f;     // measured paragraph height
        int lineCount = 0;       // wrapped line count
        bool hasText = false;    // false: no lines at all, draws nothing
        float boxX = 0.0f;
        float boxY = 0.0f;
        float boxWidth = 0.0f;
        float boxHeight = 0.0f;
        bool isImage = false;    // an image entry: box + assetPath, nothing else
        std::string assetPath;
    };

    // scale multiplies the item's size before layout (the fit pass).
    LaidOutItem layout(const openshow::scene::SceneItem& item, float scale,
                       float maxWidth) const;

    // A whole scene's text: each item laid out inside its own box and
    // shrunk (FreeShow's per-item shrinkToFit) until it fits that box's
    // height, floor at kSceneMinFontSize. Every scene item carries its
    // authored box since 4.3, so items no longer stack into one block.
    struct SceneLayout {
        std::vector<LaidOutItem> items;
    };
    SceneLayout layoutScene(const openshow::scene::Scene& scene) const;

    // A renderer's layout cache (task 6.9b). The expensive half of a scene
    // change is shaping its text, and until now that cost was paid on the
    // frame loop at the switch itself: a layout cache miss inside draw()
    // runs the whole shrink-to-fit pass before the audience sees a pixel.
    // This cache lets the prepare executor BUILD the layout on its own
    // thread — through the same const layoutScene(), no engine mutation —
    // so the audience's first miss finds the work already done. It is built
    // per SceneRenderer (see newLayoutCache): the ENGINE is what a companion
    // borrows, and four slots cannot be shared between the audience and a
    // churning slide list (task 6.16b, review R16).
    //
    // Shape: four slots, least-recently-used, keyed by the shared_ptr the
    // store shares (one immutable instance per prepared revision, task
    // 6.18a), with the raw Scene pointer kept as a second key so a fixture
    // that draws a scene nobody prepared still hits. Every method is safe
    // from any thread; a build runs OUTSIDE the lock, exactly like the
    // image cache builds a blur outside its own. Handles are handed from the
    // building thread to the painting thread (find/put are the synchronising
    // point); a handle is painted by one thread at a time, as drawLayout's
    // comment spells out.
    class LayoutCache {
    public:
        // The returned layout is RETAINED (task 6.21a, review F1): the handle
        // keeps the layout's bytes alive for as long as any consumer holds it,
        // even after the cache replaces the slot or evicts it for a fifth
        // scene. The draw path takes one handle per layout it draws and holds
        // it across the draw; the cache's side of the rule is that replacing
        // or evicting a slot releases only the cache's OWN reference. Before
        // this, find()/put() handed out a pointer into a mutable slot that a
        // prepare on another thread could rewrite mid-draw.
        std::shared_ptr<const SceneLayout> find(const openshow::scene::Scene* scene) const;
        SceneLayout build(const openshow::scene::Scene& scene) const;
        // Stores the layout and hands back a retained handle to the stored
        // bytes. Which slot it lands in (replace-in-place or the LRU victim)
        // is the cache's business; the caller's contract is the handle. The
        // four-slot contract still means the scenes one frame asks for (its
        // own and its fade's) cannot push each other out of the CACHE — and
        // even when a prepare's churn does, a frame mid-draw holds what it
        // is drawing.
        std::shared_ptr<const SceneLayout> put(
            const std::shared_ptr<const openshow::scene::Scene>& shared,
            const openshow::scene::Scene* scene, SceneLayout layout) const;

    private:
        friend class SceneTextEngine;
        LayoutCache(const SceneTextEngine* engine) : fEngine(engine) {}
        struct Slot {
            std::shared_ptr<const openshow::scene::Scene> shared;
            const openshow::scene::Scene* scene = nullptr;
            std::shared_ptr<const SceneLayout> layout;
            std::uint64_t used = 0;
        };

        const SceneTextEngine* fEngine;
        mutable std::mutex fMutex;
        mutable Slot fSlots[4];
        mutable std::uint64_t fClock = 0;
    };

    // A renderer owns exactly one LayoutCache (built through this factory)
    // and draws its text through it. It deliberately lives on the RENDERER,
    // not the engine, even though a companion borrows the engine (task 6.16b,
    // review R16): a slide list churning a document must not evict the
    // audience's layouts nor charge the audience its shaping. The cache BUILDS
    // through this engine — fonts shared, the MUTABLE shaping state owned
    // per-thread (6.22a, review A1, see fontCollection()) — but its four
    // slots, its LRU clock and its miss counter belong to one renderer. The
    // factory exists because the constructor is private (a cache
    // only ever points at a live engine); a static member reaches it.
    static std::unique_ptr<LayoutCache> newLayoutCache(const SceneTextEngine& engine) {
        return std::unique_ptr<LayoutCache>(new LayoutCache(&engine));
    }

    // Draws a laid-out scene's text (which the caller's canvas transform
    // must map from authored units): each paragraph centred in its item's
    // box, and each image entry of the stack drawn stretched into its box
    // from `images` (task 6.17b) — lookup only, an uncached path draws
    // nothing rather than decoding. The optional paint's alpha dissolves
    // the whole block for the fade transition — carried by a
    // ParagraphPainter so the alpha rides each glyph draw and the
    // composition pass keeps no intermediate render target (task 6.11b,
    // review R13); a fading image item takes the same alpha on its own
    // paint. A null `images` draws only the text items.
    //
    // `textItems=false` is the mirror half of the same switch (task 6.20d):
    // the words toggle drawn off, the stack's IMAGE entries still draw, in
    // the same authored order, with the same fade alpha. The two switches
    // exist so the operator's `words` and `images` toggles filter one
    // authored stack by kind — a stack omits, it never reorders.
    //
    // A layout is not frozen until it has been drawn: skparagraph completes
    // each line's text blobs on that line's FIRST paint (pinned source:
    // modules/skparagraph/src/TextLine.cpp — buildTextBlob appends to
    // fTextBlobCache), so drawing mutates the Paragraphs a handle points at.
    // A handle may therefore CROSS threads — the prewarm hand-off (6.9b)
    // shapes on the prepare executor and paints here, on the frame loop —
    // but exactly one thread may paint a given handle at a time. Production
    // keeps that by ownership: a renderer's frame loop is its renderer's only
    // painter, and a thumbnail companion is a renderer of its own (6.16b,
    // review R16). Painting one handle from two threads is not a slow path,
    // it is heap corruption; testConcurrentShapingOwnership pins the two
    // shapes that are supported instead — contexts painting their own bytes
    // concurrently, and a shaped-elsewhere handle painting this thread's
    // pixels.
    void drawLayout(SkCanvas* canvas, const SceneLayout& layout,
                    const SkPaint* paint = nullptr,
                    SceneImageCache* images = nullptr, bool textItems = true) const;

private:
    sk_sp<SkFontMgr> fFontMgr;
    sk_sp<SkTypeface> fRegular;
    sk_sp<SkTypeface> fBold;
    // Per-thread FontCollections (6.22a, review A1 — see fontCollection()).
    // The map lock is held only for the probe and the sk_sp copy; the
    // collection itself is used OUTSIDE it, by its owning thread alone.
    // Entries live for the engine's lifetime: the worker's shaping threads
    // are few and long-lived, and a dead thread's table dies with the engine.
    mutable std::mutex fCollectionsMutex;
    mutable std::map<std::thread::id, sk_sp<skia::textlayout::FontCollection>> fCollections;
    sk_sp<SkUnicode> fUnicode;
};

// Decodes and holds scene background images, within a byte budget.
//
// Task 6.10 (review R12) gave this cache its shape. Before it, every path
// ever seen was retained forever — a 3840×2160 RGBA image is ~31.6 MiB of
// CPU pixels that neither the 16-scene store nor the 64 MiB Ganesh cache
// bounded, and a cached null meant a repaired file never came back on the
// worker's own terms. Four rules now hold:
//
//   BUDGET. Entries are accounted by decoded bytes and the cache never
//   exceeds kDecodedImageBudget: a sequence of a hundred distinct 4K
//   backgrounds plateaus instead of growing.
//
//   PINS. What the session can still put on air — every prepared scene's
//   still, held by reference count — is never evicted. The budget bounds
//   the transient, not the operator's content: a scene that has been
//   prepared is guaranteed to present its picture, not a colour fill,
//   however hard the churn around it. (A pinned image whose file went away
//   later is also guaranteed: the cached success, not a re-read, is what
//   presents — see the repair rule.)
//
//   LIMITS. The encoded file and the declared dimensions are checked
//   BEFORE pixels are allocated: a 2 GB file is refused by its stat and a
//   20000×20000 header by its numbers, not by an allocation that fails.
//
//   LOADS OUTSIDE DRAW. imageFor() is a pure lookup — the render thread
//   asks, and either gets the picture or the colour fill, never a disk
//   read. Decoding belongs to loadFor()/refreshFor(), which the session
//   calls from prepare on the RPC thread. A failure is a zero-byte
//   placeholder that the draw path never retries at frame rate, while a
//   prepare always retries: the file the operator put back comes back, and
//   eviction (R18's retry made explicit) simply forgets the placeholder so
//   the next prepare tries again.
class SceneImageCache {
public:
    // Ceiling on decoded pixel bytes held (pins included: pins are trusted
    // to be content, the budget is what bounds everything else). Six
    // distinct 4K backgrounds at ~33 MiB each; a worship service cycles
    // more backgrounds than that between songs and the memory must not.
    static constexpr std::size_t kDecodedImageBudget = 192u * 1024u * 1024u;
    // Largest encoded file considered an image at all (a TIFF can be huge;
    // a stage backdrop should not be), and the decoded extent limits —
    // beyond these the file is not an asset but an incident.
    static constexpr std::uint64_t kMaxEncodedFileBytes = 64u * 1024u * 1024u;
    static constexpr std::int32_t kMaxImageDimension = 8192;
    static constexpr std::uint64_t kMaxDecodedPixels = 40u * 1000u * 1000u;

    // A second budget for a second target (task 6.16b, review R16). The
    // slide list's thumbnails decode their backgrounds at thumbnail size,
    // and they must not share the audience's 192 MiB ceiling for two
    // reasons: the audience's budget is a promise about SHOW content, and a
    // scrolled list would spend it on pictures nobody is about to present.
    // A companion cache therefore carries its own, small, budget. The
    // limits above are NOT relaxed: a file that is not an asset at full
    // size is not an asset at 160x90 either.
    explicit SceneImageCache(std::size_t budget = kDecodedImageBudget)
        : fBudget(budget) {}

    struct Stats {
        std::size_t decodedBytes = 0;   // pixel bytes of the cached images
                                        // (blur backdrops included)
        std::size_t entries = 0;        // cached paths, failures included
        std::size_t pins = 0;           // live pin references
        std::size_t evictions = 0;      // images forgotten to stay in budget
        std::size_t preloadHits = 0;    // lifetime refreshFor calls that found
                                        // a decoded success already cached —
                                        // the proof a re-prepare skipped the
                                        // disk (task 6.9a's live check)
        std::size_t budget = 0;         // what evictLocked is holding the line at
        std::size_t backdrops = 0;      // blur backdrops held (6.17c2)
        std::size_t backdropComputations = 0;  // lifetime builds: ONE per
                                               // image is the pinned rule
        // The DRAW-BOUNDARY misses (task 6.9b, review R10): decodes and
        // blur builds the caches could not answer and the DRAW paid. A
        // prepare covers a scene's pixels, backdrop and text; anything
        // still missing at the frame boundary is the prewarm's miss, and
        // the fixtures count exactly that number instead of timing luck.
        std::size_t drawDecodes = 0;    // draw-path decodes (loadFor)
        std::size_t drawBackdrops = 0;  // draw-path backdrop builds
    };

    // Decode a file at MOST maxW x maxH pixels (task 6.16b). Skia is asked
    // for the closest supported scaled size, so a 4K backdrop for a 160x90
    // thumbnail costs the codec a fraction of the full decode rather than
    // all of it — which is what makes a 20-slide list affordable to picture.
    // Because PNG has no scaled mode at all, the box is finished off with a
    // resample: what comes back always fits it, so a cache that stores the
    // result cannot be handed a 5.8 MB picture for a 57 KB row.
    // The pre-allocation gates are the full-size ones, unchanged. Static
    // because the result is installed explicitly rather than loaded into a
    // cache by a lookup: decoding belongs to a loader thread, and this is how
    // one hands pixels to a cache it owns.
    static sk_sp<SkImage> decodeAtMost(const std::string& path, std::uint32_t maxW,
                                       std::uint32_t maxH, std::string* error);

    // Put an already-decoded picture into the cache and account for it
    // exactly as a load would (a null records a remembered failure, so a
    // broken file is not re-tried at the next pass). This is the loader's
    // way in for pixels a lookup could never have produced — a scaled
    // thumbnail decode — and it keeps the ONE rule that matters: the draw
    // path only ever looks up, never decodes.
    sk_sp<SkImage> install(const std::string& path, sk_sp<SkImage> image);

    // Lookup only — callable from the draw path. Returns the cached image,
    // or null when the path is uncached or its cached load failed. It never
    // touches the disk, which is what keeps a cold file from being paid
    // for at 60 Hz.
    sk_sp<SkImage> imageFor(const std::string& path);

    // The blur fit's backdrop (task 6.17c2, review R17): the picture
    // cover-fitted to the authored canvas and blurred at the reference's
    // strength, its alpha pre-baked at the reference's 0.3. Computed ONCE
    // per image — the first blurred draw pays it, every later frame takes
    // a map probe — because a per-frame blur is exactly what the
    // composition budget refuses (R13's rule). The build runs off-lock on
    // the drawing thread: one blurred frame at a slide change, never a
    // blurred frame at 60 Hz. The bytes are accounted into the cache's
    // total, so the budget keeps its whole promise, and a backdrop dies
    // with its image (eviction or invalidation). Null answers for an
    // uncached or failed path — the same degradation as the image itself.
    sk_sp<SkImage> backdropFor(const std::string& path);

    // True when a lookup would find a picture (no LRU touch, no counting):
    // the session's broken-still report asks this on the apply path.
    bool has(const std::string& path);

    // The loading entry points. Both decode off-lock (a slow file must
    // never block the draw path's lookups) and account the result.
    // loadFor() is the prefetch: a cached answer of any kind wins, so a
    // per-prepare pass over the store never re-decodes. refreshFor() is
    // the operator-facing one (task 6.7c, review R18): a cached failure is
    // re-decoded — the missing file that has been put back must come back
    // on air and clear its error — while a cached success is kept.
    // `drawBoundary` (task 6.9b) says WHO is paying: a decode that starts
    // here on the draw path is a prepare that did not cover its scene,
    // and it is counted as such (Stats::drawDecodes).
    sk_sp<SkImage> loadFor(const std::string& path);
    sk_sp<SkImage> refreshFor(const std::string& path, std::string* error,
                              bool drawBoundary = false);

    // Prepare-time blur build (task 6.9b): the blurred companion of a
    // resident image, computed NOW on the caller's thread and stored.
    // Returns false when the image itself is absent (nothing to blur —
    // refreshFor is the call that decodes first); true when the backdrop
    // is in the cache, whether this call built it or found it there. The
    // draw path keeps calling backdropFor(), which then only ever pays
    // the map probe — the frame boundary stops being where a blurred
    // background discovers its 4K blur (p95 100 ms, R10).
    bool prewarmBackdrop(const std::string& path);

    // Reference-counted protection against eviction. pin() attaches to the
    // path (a not-yet-cached path pins too: the count precedes the load
    // when the session prepares before it prefetches); a path is evictable
    // only when its count reaches zero. Pinned bytes always load, even
    // past the budget — that excess is the content, and it is visible in
    // stats() rather than hidden by refusing the operator's scene.
    void pin(const std::string& path);
    void unpin(const std::string& path);

    // Re-derive the pins from the set, atomically: after the call the
    // pinned paths are EXACTLY the distinct paths named, each with one
    // reference; every other path's pins drop to zero. This is what keeps
    // a session's pin ledger honest without shadow bookkeeping — the
    // caller states WHOEVER can still be put on air and forgets nothing,
    // and no sequence of repeated syncs can drift the count.
    void syncPinSet(const std::vector<std::string>& paths);

    // Forget a path entirely (its bytes AND its failure placeholder): the
    // explicit invalidation an asset repair or a dropped reference uses.
    // Eviction is invalidation chosen by the budget; this is invalidation
    // chosen by the caller.
    void invalidate(const std::string& path);

    Stats stats() const;

private:
    struct Entry {
        sk_sp<SkImage> image;      // null = a remembered failure (0 bytes)
        sk_sp<SkImage> backdrop;   // blur fit's cached backdrop (6.17c2)
        unsigned pins = 0;
        std::uint64_t seen = 0;    // LRU clock: lowest evicts first
        std::size_t bytes = 0;     // pixel bytes, 0 for a failure
        std::size_t backdropBytes = 0;  // the backdrop's bytes, in fBytes too
    };

    // Budget enforcement, on-lock: forget unpinned entries, least-recently
    // seen first, until the bytes fit or nothing can go. Never evicts the
    // entry just inserted at `protect`.
    void evictLocked(std::string_view protect);
    // Drop every pin (syncPinSet's release half). Deliberately does not
    // evict: the budget resettles only once syncPinSet has re-pinned the
    // survivors, so a sync can never free an image the caller still holds.
    void releaseAllPinsLocked();

    mutable std::mutex fMutex;
    std::map<std::string, Entry> fEntries;
    std::size_t fBytes = 0;
    // What evictLocked holds the line at: kDecodedImageBudget for the show's
    // renderer, something much smaller for a companion's (task 6.16b). One
    // number per cache, reported in stats() so a test can assert which cache
    // it is looking at rather than assuming the class-wide constant.
    std::size_t fBudget = kDecodedImageBudget;
    std::uint64_t fClock = 0;
    std::size_t fPins = 0;
    std::size_t fEvictions = 0;
    std::size_t fPreloadHits = 0;  // refreshFor early returns (6.9a evidence)
    std::size_t fBackdropComputations = 0;  // 6.17c2's once-per-image pin
    // The draw-boundary evidence (task 6.9b): who paid the cache's two
    // expensive builds. fDrawDecodes counts refreshFor calls that came
    // from the draw path (loadFor) and had to reach the file; the live
    // draws' imageFor is a pure lookup and counts nothing. fDrawBackdrops
    // counts backdropFor builds — a prewarmed blur answers from the cache
    // and never lands here.
    std::size_t fDrawDecodes = 0;
    std::size_t fDrawBackdrops = 0;
};

// Draws a scene's background: the colour fill — flat, or a vertical
// gradient to background.gradient when hasGradient (task 6.17c3, review
// R17) — and the fitted media on top when hasImage (the fit is the scene's,
// task 6.17c1/c2, review R17). Cover
// scales to fill the authored canvas (the lyric default), centring and
// cropping the overflow; contain scales it inside, centred, the fill showing
// in the bars; blur draws the CONTAIN-fitted picture over the cached
// blurred-cover backdrop at the reference's 0.3 (the backdrop comes from
// SceneImageCache::backdropFor — built once per image, never here). The
// paint's alpha applies to the whole background for a fade — the background
// is ONE group and the alpha is that group's, so a fade is the lerp the
// blend says it is (task 6.11b, review R13). Image load failure leaves the
// colour fill standing alone. The caller's canvas already carries the
// output-rule transform.
//
// Task 5.5: a video background draws the frame the session's scheduler
// offered via `video` (cover-fit like the image); no frame yet — or a
// video that never opened — leaves the colour fill, the same degradation
// as a missing image, and the session reports the failure (media status).
void drawSceneBackground(SkCanvas* canvas, const openshow::scene::Scene& scene,
                         SceneImageCache* images, const SkPaint* paint = nullptr,
                         const SkImage* video = nullptr);

// Options for renderSceneSnapshot: which layers to draw (mirroring the
// LayerStack enable mask) and an optional crossfade stage to render.
struct SnapshotOptions {
    bool background = true;
    bool words = true;
    // The authored stack's picture kind (task 6.20d): the same gate the
    // `images` layer toggle carries in production. words and images filter
    // one stack independently — either one standing keeps the stack drawn.
    bool images = true;
    // Crossfade: when fadeFrom is set, the background composites fadeFrom
    // under scene at mediaBlend and the text dissolves fadeFrom over
    // scene at textBlend (0 = entirely fadeFrom, 1 = entirely scene) —
    // the same two tracks (media 800 ms, text 500 ms by default) the live
    // transition runs.
    const openshow::scene::Scene* fadeFrom = nullptr;
    float mediaBlend = 1.0f;
    float textBlend = 1.0f;
};

// Renders the whole scene (background colour/image + text) on the CPU and
// writes a PNG. No display, Vulkan or windowing needed; backs the 3.1 and
// 3.2 fixtures and --snapshot --scene-file for offline review. Background
// and words are dispatched as independent layers honouring SnapshotOptions.
bool renderSceneSnapshot(const openshow::scene::Scene& scene, const std::string& path,
                         int width, int height, const SnapshotOptions& options,
                         std::string* error);

// Encodes a raster surface as PNG at path. Shared by the snapshot helpers.
bool writePngRaster(SkSurface* surface, const std::string& path, std::string* error);

}  // namespace openshow::worker
