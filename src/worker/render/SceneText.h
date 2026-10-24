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
 * TestScene borrows its typefaces, and the production layers draw through
 * layoutScene()/drawLayout(), so what the fixture tests assert is what the
 * audience window renders.
 *
 * Coordinates are authored 1920x1080 units: callers apply the output rule
 * (scale = width / 1920, centred) before drawing, exactly like every other
 * layer. Sizes in a TextItem are authored units too — 0 means the engine's
 * default lyric size.
 */

#pragma once

#include <map>
#include <memory>
#include <string>
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

// The size a TextItem with fontSize 0 (the model's "renderer default")
// gets — a lyric body size on the authored canvas.
inline constexpr float kSceneDefaultFontSize = 96.0f;

// Shrink-to-fit never goes below this authored size.
inline constexpr float kSceneMinFontSize = 12.0f;

class SceneTextEngine {
public:
    SceneTextEngine();
    ~SceneTextEngine();

    SceneTextEngine(const SceneTextEngine&) = delete;
    SceneTextEngine& operator=(const SceneTextEngine&) = delete;

    // Assembles the fontconfig font manager, the default and bold
    // typefaces and the paragraph FontCollection. Fills error on failure.
    bool create(std::string* error);

    // The faces every worker text draws with when a scene names no family.
    const sk_sp<SkTypeface>& regular() const { return fRegular; }
    const sk_sp<SkTypeface>& bold() const { return fBold; }

    // Paragraph services for callers building their own paragraphs (the
    // demo title) with the engine's font resolution.
    const sk_sp<skia::textlayout::FontCollection>& fontCollection() const {
        return fFontCollection;
    }
    const sk_sp<SkUnicode>& unicode() const { return fUnicode; }

    // One item laid out: wrapped at maxWidth, centred, explicit line height.
    struct LaidOutItem {
        std::unique_ptr<skia::textlayout::Paragraph> paragraph;
        float fontSize = 0.0f;   // authored px actually used (after fit scale)
        float height = 0.0f;     // measured paragraph height
        int lineCount = 0;       // wrapped line count
        bool hasText = false;    // false: no lines at all, draws nothing
    };

    // scale multiplies the item's size before layout (the fit pass).
    LaidOutItem layout(const openshow::scene::TextItem& item, float scale,
                       float maxWidth) const;

    // A whole scene's text: every item at scale, then re-laid out under a
    // smaller scale until the block (heights plus the inter-item gap) fits
    // maxHeight — the shrinkToFit rule. Items keep their relative sizes.
    struct SceneLayout {
        std::vector<LaidOutItem> items;
        float blockHeight = 0.0f;  // heights + gaps, no margins
        float gap = 0.0f;          // authored px between items
        float scale = 1.0f;        // the fit scale that was applied
    };
    SceneLayout layoutScene(const openshow::scene::Scene& scene) const;

    // Draws a laid-out scene's text centred in the authored canvas (which
    // the caller's canvas transform must already implement). The optional
    // paint's alpha dissolves the whole block for the fade transition.
    void drawLayout(SkCanvas* canvas, const SceneLayout& layout,
                    const SkPaint* paint = nullptr) const;

private:
    sk_sp<SkFontMgr> fFontMgr;
    sk_sp<SkTypeface> fRegular;
    sk_sp<SkTypeface> fBold;
    sk_sp<skia::textlayout::FontCollection> fFontCollection;
    sk_sp<SkUnicode> fUnicode;
};

// Decodes and holds scene background images. Every path seen is kept:
// backgrounds persist across lyric changes (the M3 continuity rule wants
// an unchanged background not to re-decode) and a fade needs the outgoing
// and incoming pictures at once.
class SceneImageCache {
public:
    // Decodes the image at path (SkCodec), or returns the cached one.
    // Null on failure; callers fall back to the colour fill — file
    // existence is the renderer's problem, the compiler only names the path.
    sk_sp<SkImage> imageFor(const std::string& path);

private:
    std::map<std::string, sk_sp<SkImage>> fImages;
};

// Draws a scene's background: the colour fill, and the cover-fitted image
// on top when hasImage. Cover scales to fill the authored canvas (the
// lyric default), centring and cropping the overflow; the paint's alpha
// applies to the whole background for a fade. Image load failure leaves
// the colour fill standing alone. The caller's canvas already carries the
// output-rule transform.
void drawSceneBackground(SkCanvas* canvas, const openshow::scene::Scene& scene,
                         SceneImageCache* images, const SkPaint* paint = nullptr);

// Options for renderSceneSnapshot: which layers to draw (mirroring the
// LayerStack enable mask) and an optional crossfade stage to render.
struct SnapshotOptions {
    bool background = true;
    bool words = true;
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
