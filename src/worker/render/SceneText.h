/*
 * Production text layout for compiled scenes (task 3.1).
 *
 * Everything a Scene says about type — colours, families, sizes, the lines
 * of each item — becomes pixels here: SkParagraph paragraphs wrapped at the
 * authored canvas width, centred (the lyric default), with per-run font
 * fallback through the fontconfig-backed FontCollection, and a whole-block
 * shrink so an over-tall stanza still fits (FreeShow's shrinkToFit,
 * docs/reference.md §7). The engine is the one font setup the worker has:
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

#include <memory>
#include <string>
#include <vector>

#include "include/core/SkFontMgr.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkTypeface.h"
#include "modules/skparagraph/include/FontCollection.h"
#include "modules/skparagraph/include/Paragraph.h"
#include "modules/skunicode/include/SkUnicode.h"
#include "openshow/scene/scene.h"

class SkCanvas;
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
    // the caller's canvas transform must already implement).
    void drawLayout(SkCanvas* canvas, const SceneLayout& layout) const;

private:
    sk_sp<SkFontMgr> fFontMgr;
    sk_sp<SkTypeface> fRegular;
    sk_sp<SkTypeface> fBold;
    sk_sp<skia::textlayout::FontCollection> fFontCollection;
    sk_sp<SkUnicode> fUnicode;
};

// Renders the whole scene (background colour fill + text) on the CPU and
// writes a PNG. No display, Vulkan or windowing needed; backs the 3.1
// fixtures and --snapshot --scene-file for offline review. Image
// backgrounds compose in 3.2; until then hasImage is skipped, matching the
// compiler's stance that file existence is the renderer's problem.
bool renderSceneSnapshot(const openshow::scene::Scene& scene, const std::string& path,
                         int width, int height, std::string* error);

// Encodes a raster surface as PNG at path. Shared by the snapshot helpers.
bool writePngRaster(SkSurface* surface, const std::string& path, std::string* error);

}  // namespace openshow::worker
