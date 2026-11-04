/*
 * SceneRenderer: the production compositor (task 6.1b — graduated out of
 * TestScene, review R1). One RenderLayer per composition tab (background,
 * images, words, aux, clock — PLAN.md "Output layer model"), drawn bottom
 * to top straight into the target canvas with no per-layer
 * render-to-texture, scaled from the authored 1920x1080 canvas by the
 * output rule (docs/reference.md §7: zoom = width / 1920, letterboxed).
 *
 * This is what a --socket session composes for the audience: a black
 * start (the clear itself, with nothing presented), the clock and aux
 * layers off, and scene layers/transitions that reveal a presentation —
 * colour fill, cover-fitted image or live video background (task 5.5)
 * under laid-out text, faded or cut (task 3.2).
 *
 * The development picture lives outside this class. create() takes an
 * optional DemoDecoration — the 1.x backdrop gradient, the demo photo
 * standing in for slide images, and the demo title standing in for scene
 * words, all painted only while nothing is on air — built by the
 * development harness (render/TestScene.h) through a callback that
 * receives the renderer's text engine. The decorated renderer also starts
 * with the clock layer enabled: the development live-frame indicator.
 * The decoration rides the same five tabs, so layer toggles keep exactly
 * their meaning in either composition; production passes no builder and
 * no demo pixel or diagnostics can exist on screen.
 */

#pragma once

#include <atomic>
#include <ctime>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "include/core/SkCanvas.h"
#include "include/core/SkImage.h"
#include "include/core/SkRefCnt.h"
#include "openshow/domain/documents.h"  // TransitionOptions drives the blend (task TR.2)
#include "openshow/scene/scene.h"

#include "render/Layer.h"
#include "render/SceneText.h"

namespace openshow::worker {

// How a cue moves is not named in this header any more (task TR.2). The
// command's whole answer — `domain::TransitionOptions`, the kind plus both
// durations — travels from `scene.present` through the store and lands here
// undivided, because a record narrowed on the way is a promise dropped on the
// way: the renderer used to keep its own fade/cut vocabulary and its own two
// millisecond constants, which is why the worker's hello could not claim
// `scene.transition-options.v1`. Drawing the record is what earns that name.

// The live crossfade: the state machine the audience's frame clock drives.
// In steady state fFrom is null. It holds NO blend: the blends belong to a
// frame (SceneFrame), and this one only says who is fading, when the fade
// began, and how long each track takes, so asking about the composition cannot
// move it along (task 6.11a, review R13). Owned by SceneRenderer.
struct SceneTransition {
    ActiveScene fFrom;
    double fStartSeconds = -1.0;  // the frame clock the fade began at
    // Negative until the fade's first audience frame stamps it: a fade
    // begun between two frames is measured from the first one drawn, which
    // is what makes its durations the audience's durations rather than a race
    // against the command thread.
    //
    // The two tracks' lengths, in the seconds this struct's own clock
    // measures in (task TR.2). Converted ONCE, when the cue lands, rather
    // than per frame: captureFrame runs at the panel's rate, and a divide in
    // it would be paid sixty times a second for a number that cannot change
    // while the fade runs — the record belongs to the cue, so nothing that
    // happens after it is stamped can retime it. A zero means that track
    // lands on the fade's first frame, which is an answer the operator gave,
    // not a missing value; fadeBlend tests it before the elapsed-time rule.
    double fTextSeconds = openshow::domain::kDefaultTransitionTextMs / 1000.0;
    double fMediaSeconds = openshow::domain::kDefaultTransitionMediaMs / 1000.0;

    bool fading() const { return fFrom != nullptr; }
};

// The development decoration (task 6.1b): demo content for the same tabs,
// drawn only while no scene is on air. titlePaint receives the canvas
// already transformed by the output rule (authored coordinates), so the
// development harness owns the SkParagraph machinery and the renderer
// never names it. Production create() passes no builder, leaving every
// member empty.
struct DemoDecoration {
    std::function<void(SkCanvas*)> titlePaint;  // words tab, demo title
    sk_sp<SkImage> photo;                       // images tab, demo photo
    bool gradient = false;                      // background tab, 1.x backdrop
};


class SceneRenderer {
public:
    static constexpr float kCanvasWidth = kSceneCanvasWidth;
    static constexpr float kCanvasHeight = kSceneCanvasHeight;

    // Builds the decoration from the renderer's own text services
    // (fontconfig faces, FontCollection, Unicode). Returns false with
    // error set when the demo content cannot be produced.
    using DemoBuilder =
        std::function<bool(const SceneTextEngine&, DemoDecoration*, std::string*)>;

    SceneRenderer() = default;
    ~SceneRenderer();

    SceneRenderer(const SceneRenderer&) = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;

    // A renderer for a SECOND target (task 6.16b): the same compositor with
    // a decoded-image cache of a size of its own. See createCompanion().
    explicit SceneRenderer(std::size_t imageBudget) : fImages(imageBudget) {}

    // Production is the default: background/images/words tabs on, aux off
    // (not part of the agreed use case) and clock off (the audience clock
    // is called up explicitly by the operator, R1). Passing a demo builder
    // opts into the development picture and re-enables the clock as the
    // live-frame indicator, exactly as the pre-6.1 development scene did.
    bool create(std::string* error, const DemoBuilder& demo = nullptr);

    // The compositor for a second target (task 6.16b, review R16) — the
    // slide list's thumbnails — built as a companion of a live renderer.
    //
    // It is a SEPARATE renderer rather than the same one drawing a different
    // scene, for one specific reason: the words layer caches laid-out text
    // in a handful of slots keyed on the scene. Alternating the audience's
    // scene with a band of thirty other scenes evicts the audience's layout
    // every pass and re-shapes it on the next, which is the show paying for
    // a preview — exactly the cost this task must not add. Own slots, own
    // budget, own decoded-image cache, and the audience's layout survives
    // however much the list churns.
    //
    // What it SHARES is the text engine: that is fontconfig faces, typefaces
    // and ICU, stateless with respect to layouts, and duplicating it would
    // be megabytes of font data for a target that draws the same glyphs as
    // the show. Layouts are built on the render thread, which is the only
    // thread either renderer draws on, so sharing it needs no lock.
    //
    // `primary` must outlive the companion; the session's first renderer
    // does. Returns nullptr with `error` set when the layers cannot be
    // built (in practice: no usable typeface, which the primary already
    // proved there is).
    static std::unique_ptr<SceneRenderer> createCompanion(const SceneRenderer& primary,
                                                          std::size_t imageBudget,
                                                          std::string* error);

    // True when no decoration was built: no demo content exists and the
    // session composes the audience picture. The session uses this to
    // suppress the diagnostics footer — output and preview then carry the
    // identical scene (6.1a).
    bool production() const { return !fDemo.gradient && !fDemo.photo && !fDemo.titlePaint; }

    // --- one captured frame, drawn to every target (task 6.11a, R13) -----
    //
    // The audience panel and the operator's preview show the same moment,
    // so both must draw the same values: same scene, same fade blends, same
    // clock, same revision tag. Three calls carry that contract:
    //
    //   captureFrame(clock)  the immutable picture the audience is owed now
    //   draw(frame, ...)     paint one captured frame into one target
    //   commitFrame(frame)   credit the frame that reached the panel
    //
    // captureFrame is the only thing that moves a fade and the frame loop
    // calls it exactly once per audience frame; draw() and the layers read
    // their whole composition from the frame they were handed. So a second
    // target, a hundred preview ticks or a consumer that stopped reading
    // its slots can none of them complete a fade, skip a blend or restage a
    // scene the audience has not been shown — the failure the review found
    // when the preview called the same draw() the output did, with its own
    // later timestamp.

    // The picture the audience is owed at this instant, as an immutable
    // snapshot tagged with the command revision these pixels carry. This is
    // where the crossfade advances and where it begins: a fade started
    // between frames takes its first captureFrame as its origin, so its
    // 800 ms are the audience's 800 ms and not a race against the command
    // thread. A wallClock of 0 means "whatever the audience's clock says";
    // selftests pass a fixed one so two frames with the same layers come out
    // pixel identical.
    SceneFrame captureFrame(double timeSeconds, std::uint64_t revision,
                            std::time_t wallClock = 0);

    // Credits a captured frame with reaching the audience. This is where a
    // completed fade is retired — nowhere else — so the event-driven loop
    // keeps presenting until the settled picture is really on the panel: a
    // frame that never made it (an acquire timeout, a failed submit) leaves
    // the fade running, and the audience is never left holding a blend
    // nobody intends to finish.
    void commitFrame(const SceneFrame& frame);

    // Draws one captured frame into width x height device pixels, scaling
    // the canvas with the output rule and centring it. backgroundVideo
    // (task 5.5) is the frame the session's scheduler decided to draw; the
    // background layer consumes it when the on-air scene's background names
    // a video asset, and nothing else looks at it. statusLine is the small
    // diagnostics line the clock layer prefixes — target chrome, like the
    // size, so it belongs to the draw call rather than the frame (the
    // audience's footer and the preview's empty one are the same scene).
    void draw(const SceneFrame& frame, SkCanvas* canvas, int widthPx, int heightPx,
              const std::string& statusLine = {}, const SkImage* backgroundVideo = nullptr);

    // Single-target convenience for the development harness and the CLI:
    // capture, draw and commit in one call, which is right only when this
    // call owns the frame. The session never uses it — its preview draws
    // the frame the audience was shown, not one of its own.
    void drawLive(SkCanvas* canvas, int widthPx, int heightPx, double timeSeconds,
                  std::time_t wallClock = 0, const std::string& statusLine = {},
                  const SkImage* backgroundVideo = nullptr);

    // Presents the compiled scene (task 3.1): the background fills the
    // scene colour (or its cover-fitted image / video frame), the words
    // layer lays the scene's text out through SceneTextEngine, and any
    // demo decoration stands down. Render-thread only, like draw(); the
    // layers pick the change up on their next draw.
    //
    // The record is the cue's whole answer, and the renderer draws all of it
    // (task TR.2): a Cut swaps in one frame, and a Fade crossfades the
    // outgoing scene into this one on the record's OWN two clocks — the words
    // and foreground items on `textDurationMs`, the background composition on
    // `mediaDurationMs`, either of which may be 0 to land on the fade's first
    // captured frame. There is NO default argument, deliberately: every call
    // states how the audience is about to get this slide, because the entry
    // points that have no cue to ask (the development harness, the snapshot
    // CLI) are exactly where a record invented silently would be invisible,
    // and they say `TransitionOptions::defaults()` in the open.
    void setActiveScene(ActiveScene scene, const openshow::domain::TransitionOptions& options);
    const ActiveScene& activeScene() const { return fActiveScene; }
    // The background image cache (task 5.5): the session prefetches a
    // prepared scene's background through it from its RPC thread, so the
    // first frame after a present never pays the codec on the render
    // thread. The cache is internally synchronised for exactly this.
    SceneImageCache* imageCache() { return &fImages; }
    // True while a crossfade still owes frames: the event-driven loop must
    // keep presenting until the blends complete, or the audience is stuck
    // on the fade's first frame (which still looks like the old scene).
    bool transitionRunning() const { return fTransitionRunning; }

    // How many text layouts the words layer has SHAPED since this renderer
    // was created (task 6.16a). Drawing a scene is cheap once its text is
    // laid out, and laying text out is not; with the audience and the
    // editor's off-air target drawing alternating frames, a cache keyed on
    // the scene pointer can turn one edit into a re-shape every pass, which
    // is cost the audience pays for a preview's benefit. The counter is the
    // evidence a fixture asserts on: two steady scenes alternating across
    // many passes must cost a handful of builds, not one per pass.
    std::uint64_t textLayoutBuilds() const {
        return fTextLayoutBuilds.load(std::memory_order_relaxed);
    }

    // Prewarm a prepared scene's text layout into THIS renderer's own cache
    // (task 6.9b, review R10). The shaping runs on the CALLER's thread — the
    // prepare executor's, never the frame loop's — and lands in the same
    // four slots the production draw reads, so the frame boundary that
    // switches to a prepared scene finds its paragraphs already built. It is
    // charged to no miss counter (this is not a draw); it only moves when the
    // work is paid. A companion never prewarms — the audience's layouts stay
    // the audience's (R16 isolation). Safe from any thread: the cache builds
    // outside its lock, and the shaping itself runs in the calling thread's
    // own FontCollection (6.22a, review A1).
    void prewarmLayouts(std::shared_ptr<const openshow::scene::Scene> scene) const {
        if (fLayouts && scene) {
            const openshow::scene::Scene* key = scene.get();
            fLayouts->put(std::move(scene), key, fLayouts->build(*scene));
        }
    }

    // Layer toggles, driven by the RPC layer controls (and the dev CLI).
    void setLayerEnabled(LayerId id, bool enabled) { fLayers.setEnabled(id, enabled); }
    bool layerEnabled(LayerId id) const { return fLayers.isEnabled(id); }
    void applyLayerOverrides(const std::vector<std::pair<LayerId, bool>>& overrides) {
        for (const auto& [id, enabled] : overrides) {
            setLayerEnabled(id, enabled);
        }
    }

    // The renderer's shared text services (fonts, FontCollection, Unicode).
    // Exposed for the DemoBuilder; the production session never asks.
    const SceneTextEngine& textEngine() const { return *fText; }

private:
    // The layer stack, built by create() (its own engine) and by
    // createCompanion() (a borrowed one). Kept out of both so the two cannot
    // drift apart on layer order or on the aux/clock defaults.
    bool buildLayers(std::string* error, const DemoBuilder& demo);

    // Shared text services; the layer objects own the drawn content. The
    // engine outlives the layers (declared before fLayers): shared because a
    // companion renderer borrows the primary's (see createCompanion), and
    // the primary outlives it by construction.
    std::shared_ptr<SceneTextEngine> fText;
    SceneImageCache fImages;     // persists background images across scenes
    // The words layer's layout-build counter (declared ahead of fLayers so
    // it outlives the layer that writes it).
    std::atomic<std::uint64_t> fTextLayoutBuilds{0};
    // This renderer's OWN text-layout cache (task 6.9b): one per renderer,
    // built through the shared engine but never shared with it — a companion
    // gets its own instance, so the R16 isolation holds (a preview's churn
    // cannot evict the audience's layouts). Declared before fLayers so it
    // outlives the layer that reads it; the engine outlives this.
    std::unique_ptr<SceneTextEngine::LayoutCache> fLayouts;
    SceneTransition fTransition;
    bool fTransitionRunning = false;
    LayerStack fLayers;
    ActiveScene fActiveScene;
    DemoDecoration fDemo;        // development decoration; empty in production
};

}  // namespace openshow::worker
