/*
 * TestScene: the task 1.1 picture, composed through the LayerStack so the
 * layer architecture (PLAN.md "Output layer model") is exercised from the
 * first frame: one RenderLayer per composition tab (background, images,
 * words, aux placeholder, clock), drawn bottom to top straight into the
 * target canvas with no per-layer render-to-texture. Everything is scaled
 * from the authored 1920x1080 canvas like the real output
 * (docs/reference.md section 7: zoom = width / 1920, letterboxed). It also
 * proves the pipeline pieces: SkParagraph text through harfbuzz with a
 * fontconfig font manager, and an image encoded to PNG and decoded back
 * with SkCodec.
 *
 * Task 3.1 graduates the font/text machinery into SceneTextEngine and makes
 * the background and words layers scene-aware: while a presentation is on
 * air (setActiveScene, render thread) they draw the compiled scene's
 * colours and laid-out text instead of the demo content, and the demo photo
 * stands down. With no active scene the 1.x picture renders exactly as
 * before, which is what --check-layers and the snapshots pin.
 *
 * Task 6.1a (M6 review R1) separates the two lives this class has been
 * leading. A Profile decides at create() time: kDemo keeps the 1.x picture
 * exactly as the development drivers and snapshots always had it;
 * kProduction is what a --socket session composes for the audience — a
 * black start (no demo gradient, photo or title before the first
 * presentation), the clock and aux layers off, and no diagnostic text, so
 * startup and clear-all are uniformly black including the footer region,
 * and output and preview carry the identical scene. The scene layers and
 * transitions themselves are shared: presenting a scene composes the same
 * pixels in either profile (pinned by --check-production). 6.1b completes
 * the graduation by splitting the sources; the profile is what makes the
 * production behaviour true now.
 */

#pragma once

#include <ctime>
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
// Null means nothing has been presented: the demo picture renders (and in
// the production profile, the black clear stays untouched).
using ActiveScene = std::shared_ptr<const openshow::scene::Scene>;

// How a presentation takes effect. FreeShow defaults every transition to
// fade (docs/reference.md §7: text 500ms, media 800ms); a cut is "none".
enum class SceneTransitionKind { kFade, kCut };

// The crossfade in flight, read by the scene-aware layers on each draw.
// In steady state fFrom is null and both blends are 1. Media (background
// and image) and text fade on independent clocks matching FreeShow's two
// transition defaults. Owned by TestScene; advanced once per frame.
struct SceneTransition {
    ActiveScene fFrom;
    float fMediaBlend = 1.0f;  // 0 = all fFrom, 1 = all on-air
    float fTextBlend = 1.0f;

    bool fading() const { return fFrom != nullptr; }
};

class TestScene {
public:
    static constexpr float kCanvasWidth = kSceneCanvasWidth;
    static constexpr float kCanvasHeight = kSceneCanvasHeight;

    TestScene() = default;
    ~TestScene();

    TestScene(const TestScene&) = delete;
    TestScene& operator=(const TestScene&) = delete;

    bool create(std::string* error);
    // Task 6.1a: which life this compositor leads. kDemo (the default, and
    // what every development driver and snapshot always used) builds the
    // full 1.x picture. kProduction is the audience compositor of a
    // --socket session: no demo assets are even built, the clock and aux
    // layers start disabled, and with no active scene the layers draw
    // nothing — the black clear is the startup and clear-all frame.
    enum class Profile { kDemo, kProduction };
    bool create(std::string* error, Profile profile);
    bool production() const { return fProduction; }

    // Draws the scene into width x height device pixels, scaling the canvas
    // with the output rule and centring it. statusLine is the small line of
    // diagnostics; timeSeconds drives the clock so a live loop visibly moves.
    // backgroundVideo (task 5.5) is the frame the session's scheduler
    // decided to draw; the background layer consumes it when the on-air
    // scene's background names a video asset, and nothing else looks at it.
    void draw(SkCanvas* canvas, int widthPx, int heightPx, double timeSeconds,
              const std::string& statusLine, const SkImage* backgroundVideo = nullptr);
    // Deterministic variant: fixed wall clock so two frames with the same
    // layer configuration are pixel identical (used by --check-layers).
    void draw(SkCanvas* canvas, int widthPx, int heightPx, double timeSeconds,
              std::time_t wallClock, const std::string& statusLine,
              const SkImage* backgroundVideo = nullptr);

    // Renders the compiled scene instead of the demo content (task 3.1):
    // the background fills the scene colour (or its cover-fitted image),
    // the words layer lays the scene's text out through SceneTextEngine,
    // and the demo photo stands down. Render-thread only, like draw(); the
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

    // Layer toggles, mirroring what the RPC will drive later.
    void setLayerEnabled(LayerId id, bool enabled) { fLayers.setEnabled(id, enabled); }
    bool layerEnabled(LayerId id) const { return fLayers.isEnabled(id); }
    void applyLayerOverrides(const std::vector<std::pair<LayerId, bool>>& overrides) {
        for (const auto& [id, enabled] : overrides) {
            setLayerEnabled(id, enabled);
        }
    }

private:
    bool createPhoto(sk_sp<SkImage>* photo, std::string* error);
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
    bool fProduction = false;  // from create()'s Profile (6.1a)
};

// Renders the test scene on the CPU and writes a PNG. No display, Vulkan or
// windowing needed; used by the test suite and for offline review.
bool renderTestSnapshot(const std::string& path, int width, int height,
                        const std::vector<std::pair<LayerId, bool>>& layerOverrides,
                        std::string* error);

// Composites frames on the CPU with different layer toggles and checks the
// model: everything off is pure black, each layer's pixels appear only in
// its region, and toggles round-trip to identical frames. Reports the first
// failed expectation in error. Backs --check-layers.
bool verifyLayerComposition(std::string* error);

// Drives the scene transition state machine (task 3.2) on the CPU: pops
// in the first presentation, times both crossfade tracks (media 800ms,
// text 500ms) from drawn frames, completes them cleanly, and proves a cut
// swaps in one frame — including cancelling a fade in flight. Pixel
// assertions through the same layers the audience window uses.
bool verifySceneTransition(std::string* error);

// Task 6.1a (review R1): the production profile's composition rules on
// the CPU. The production compositor starts with clock and aux off and
// draws a uniformly black frame with nothing presented — every pixel
// black, the footer/clock region included; a presented scene composes
// pixel-identical production and demo (clock-off) frames, so the audience
// scene is the proven layer path, not a demo-flavoured one; and the demo
// profile still paints the full 1.x picture when asked for explicitly.
// Backs --check-production.
bool verifyProductionCompositor(std::string* error);

}  // namespace openshow::worker
