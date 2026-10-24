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
// Null means nothing has been presented: the demo picture renders.
using ActiveScene = std::shared_ptr<const openshow::scene::Scene>;

class TestScene {
public:
    static constexpr float kCanvasWidth = kSceneCanvasWidth;
    static constexpr float kCanvasHeight = kSceneCanvasHeight;

    TestScene() = default;
    ~TestScene();

    TestScene(const TestScene&) = delete;
    TestScene& operator=(const TestScene&) = delete;

    bool create(std::string* error);

    // Draws the scene into width x height device pixels, scaling the canvas
    // with the output rule and centring it. statusLine is the small line of
    // diagnostics; timeSeconds drives the clock so a live loop visibly moves.
    void draw(SkCanvas* canvas, int widthPx, int heightPx, double timeSeconds,
              const std::string& statusLine);
    // Deterministic variant: fixed wall clock so two frames with the same
    // layer configuration are pixel identical (used by --check-layers).
    void draw(SkCanvas* canvas, int widthPx, int heightPx, double timeSeconds,
              std::time_t wallClock, const std::string& statusLine);

    // Renders the compiled scene instead of the demo content (task 3.1):
    // the background fills the scene colour, the words layer lays the
    // scene's text out through SceneTextEngine, and the demo photo stands
    // down. Render-thread only, like draw(); the layers pick the change up
    // on their next draw.
    void setActiveScene(ActiveScene scene) { fActiveScene = std::move(scene); }
    const ActiveScene& activeScene() const { return fActiveScene; }

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

    // Shared text services; the layer objects own the drawn content. The
    // engine outlives the layers (declared before fLayers).
    std::unique_ptr<SceneTextEngine> fText;
    LayerStack fLayers;
    ActiveScene fActiveScene;
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

}  // namespace openshow::worker
