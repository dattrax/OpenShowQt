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

#include <ctime>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "include/core/SkCanvas.h"
#include "include/core/SkImage.h"
#include "include/core/SkRefCnt.h"
#include "openshow/scene/scene.h"

#include "render/Layer.h"
#include "render/SceneText.h"

namespace openshow::worker {

// The presentation currently on air, shared with the layers that read it.
// Null means nothing has been presented: production draws nothing (the
// black clear stands), a decorated development renderer shows its demo.
using ActiveScene = std::shared_ptr<const openshow::scene::Scene>;

// How a presentation takes effect. FreeShow defaults every transition to
// fade (docs/reference.md §7: text 500ms, media 800ms); a cut is "none".
enum class SceneTransitionKind { kFade, kCut };

// The crossfade in flight, read by the scene-aware layers on each draw.
// In steady state fFrom is null and both blends are 1. Media (background
// and image) and text fade on independent clocks matching FreeShow's two
// transition defaults. Owned by SceneRenderer; advanced once per frame.
struct SceneTransition {
    ActiveScene fFrom;
    float fMediaBlend = 1.0f;  // 0 = all fFrom, 1 = all on-air
    float fTextBlend = 1.0f;

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

    // Production is the default: background/images/words tabs on, aux off
    // (not part of the agreed use case) and clock off (the audience clock
    // is called up explicitly by the operator, R1). Passing a demo builder
    // opts into the development picture and re-enables the clock as the
    // live-frame indicator, exactly as the pre-6.1 development scene did.
    bool create(std::string* error, const DemoBuilder& demo = nullptr);

    // True when no decoration was built: no demo content exists and the
    // session composes the audience picture. The session uses this to
    // suppress the diagnostics footer — output and preview then carry the
    // identical scene (6.1a).
    bool production() const { return !fDemo.gradient && !fDemo.photo && !fDemo.titlePaint; }

    // Draws the composition into width x height device pixels, scaling the
    // canvas with the output rule and centring it. statusLine is the small
    // diagnostics line the clock layer prefixes (production passes the
    // empty string); timeSeconds drives the clock so a live loop visibly
    // moves. backgroundVideo (task 5.5) is the frame the session's
    // scheduler decided to draw; the background layer consumes it when the
    // on-air scene's background names a video asset, and nothing else
    // looks at it.
    void draw(SkCanvas* canvas, int widthPx, int heightPx, double timeSeconds,
              const std::string& statusLine, const SkImage* backgroundVideo = nullptr);
    // Deterministic variant: fixed wall clock so two frames with the same
    // layer configuration are pixel identical (used by --check-layers).
    void draw(SkCanvas* canvas, int widthPx, int heightPx, double timeSeconds,
              std::time_t wallClock, const std::string& statusLine,
              const SkImage* backgroundVideo = nullptr);

    // Presents the compiled scene (task 3.1): the background fills the
    // scene colour (or its cover-fitted image / video frame), the words
    // layer lays the scene's text out through SceneTextEngine, and any
    // demo decoration stands down. Render-thread only, like draw(); the
    // layers pick the change up on their next draw. A fade (the default)
    // crossfades the outgoing scene into this one over the media and text
    // durations; a cut swaps in one frame.
    void setActiveScene(ActiveScene scene,
                        SceneTransitionKind kind = SceneTransitionKind::kFade);
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
    // Advances the fade for a frame at timeSeconds (see SceneTransition).
    void advanceTransition(double timeSeconds);

    // Shared text services; the layer objects own the drawn content. The
    // engine outlives the layers (declared before fLayers).
    std::unique_ptr<SceneTextEngine> fText;
    SceneImageCache fImages;     // persists background images across scenes
    SceneTransition fTransition;
    bool fTransitionRunning = false;
    double fTransitionStart = -1.0;  // first draw timeSeconds of the fade
    LayerStack fLayers;
    ActiveScene fActiveScene;
    DemoDecoration fDemo;        // development decoration; empty in production
};

}  // namespace openshow::worker
