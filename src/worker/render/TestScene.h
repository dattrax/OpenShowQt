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
 */

#pragma once

#include <ctime>
#include <string>
#include <utility>
#include <vector>

#include "include/core/SkCanvas.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkImage.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkTypeface.h"
#include "modules/skunicode/include/SkUnicode.h"
#include "modules/skparagraph/include/FontCollection.h"

#include "render/Layer.h"

namespace openshow::worker {

class TestScene {
public:
    static constexpr float kCanvasWidth = 1920.0f;
    static constexpr float kCanvasHeight = 1080.0f;

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

    // Shared text services; the layer objects own the drawn content.
    sk_sp<SkFontMgr> fFontMgr;
    sk_sp<SkTypeface> fRegular;
    sk_sp<skia::textlayout::FontCollection> fFontCollection;
    sk_sp<SkUnicode> fUnicode;
    LayerStack fLayers;
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
