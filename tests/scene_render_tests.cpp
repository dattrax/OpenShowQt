/*
 * Task 3.1 fixtures: production scene text — wrapping, alignment, fallback
 * and fit (PLAN.md acceptance).
 *
 * Each fixture renders a known Scene through the same SceneTextEngine and
 * CPU snapshot path the audience layers use (no display, no Vulkan), then
 * measures pixels: ink bounding boxes, row bands and centroids. Assertions
 * are geometric and tolerant (antialiasing, font availability) but each one
 * pins a named behaviour. The fixtures land as PNGs beside the test binary
 * for offline review.
 *
 * Backgrounds are read from a rendered corner pixel, not the nominal colour:
 * the PNG round trip can shift channels slightly through colour management,
 * and the fixtures must measure ink, not re-derive the codec.
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "include/codec/SkCodec.h"
#include "include/core/SkBitmap.h"
#include "include/core/SkColor.h"
#include "include/core/SkData.h"
#include "include/core/SkImageInfo.h"

#include "openshow/scene/demo.h"
#include "openshow/scene/scene.h"

#include "render/SceneText.h"

namespace scene = openshow::scene;
namespace worker = openshow::worker;

namespace {

int failures = 0;

constexpr int kWidth = 960;   // authored 1920x1080 at the 0.5 output scale
constexpr int kHeight = 540;
constexpr double kScale = 0.5;

// Device-pixel bounds of the text content area (authored margins x 0.5,
// plus a few pixels of slack for antialiasing).
constexpr int kContentLeft = static_cast<int>(worker::kSceneMarginX * kScale) - 4;
constexpr int kContentRight =
    static_cast<int>((worker::kSceneCanvasWidth - worker::kSceneMarginX) * kScale) + 4;
constexpr int kContentTop = static_cast<int>(worker::kSceneMarginY * kScale) - 4;
constexpr int kContentBottom =
    static_cast<int>((worker::kSceneCanvasHeight - worker::kSceneMarginY) * kScale) + 4;

bool check(bool condition, const std::string& what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++failures;
    }
    return condition;
}

struct Frame {
    int width = 0;
    int height = 0;
    std::vector<SkColor> pixels;

    SkColor at(int x, int y) const { return pixels[static_cast<size_t>(y) * width + x]; }
};

bool loadPng(const std::string& path, Frame* frame, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        *error = "cannot read " + path;
        return false;
    }
    const std::string bytes((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
    sk_sp<SkData> data = SkData::MakeWithCopy(bytes.data(), bytes.size());
    auto codec = SkCodec::MakeFromData(std::move(data));
    if (!codec) {
        *error = "cannot decode " + path;
        return false;
    }
    const SkImageInfo info =
        codec->getInfo().makeColorType(kN32_SkColorType).makeAlphaType(kPremul_SkAlphaType);
    SkBitmap bitmap;
    if (!bitmap.tryAllocPixels(info)) {
        *error = "cannot allocate " + path;
        return false;
    }
    if (codec->getPixels(info, bitmap.getPixels(), bitmap.rowBytes()) != SkCodec::kSuccess) {
        *error = "cannot decode pixels of " + path;
        return false;
    }
    frame->width = info.width();
    frame->height = info.height();
    frame->pixels.resize(static_cast<size_t>(frame->width) * frame->height);
    for (int y = 0; y < frame->height; ++y) {
        for (int x = 0; x < frame->width; ++x) {
            frame->pixels[static_cast<size_t>(y) * frame->width + x] = bitmap.getColor(x, y);
        }
    }
    return true;
}

bool renderFixture(const scene::Scene& subject, const std::string& name, Frame* frame,
                   std::string* error) {
    const std::string path = "scene-render-" + name + ".png";
    if (!worker::renderSceneSnapshot(subject, path, kWidth, kHeight, error)) {
        return false;
    }
    return loadPng(path, frame, error);
}

bool differs(SkColor a, SkColor b, int tolerance = 16) {
    const int dr = std::abs(static_cast<int>(SkColorGetR(a)) - static_cast<int>(SkColorGetR(b)));
    const int dg = std::abs(static_cast<int>(SkColorGetG(a)) - static_cast<int>(SkColorGetG(b)));
    const int db = std::abs(static_cast<int>(SkColorGetB(a)) - static_cast<int>(SkColorGetB(b)));
    return dr > tolerance || dg > tolerance || db > tolerance;
}

struct Ink {
    int left = 0, top = 0, right = 0, bottom = 0;  // inclusive
    int count = 0;
    double centerX = 0.0;
    double centerY = 0.0;
};

// Pixels differing from the frame's own corner colour (the scene's
// background fill).
Ink measureInk(const Frame& frame) {
    const SkColor background = frame.at(2, 2);
    Ink ink;
    ink.left = frame.width;
    ink.top = frame.height;
    ink.right = -1;
    ink.bottom = -1;
    double sumX = 0.0, sumY = 0.0;
    for (int y = 0; y < frame.height; ++y) {
        for (int x = 0; x < frame.width; ++x) {
            if (differs(frame.at(x, y), background)) {
                ++ink.count;
                sumX += x;
                sumY += y;
                ink.left = std::min(ink.left, x);
                ink.top = std::min(ink.top, y);
                ink.right = std::max(ink.right, x);
                ink.bottom = std::max(ink.bottom, y);
            }
        }
    }
    if (ink.count > 0) {
        ink.centerX = sumX / ink.count;
        ink.centerY = sumY / ink.count;
    }
    return ink;
}

// Groups of consecutive inked rows: one band per drawn line for text
// without touching ascenders/descenders.
int inkBands(const Frame& frame) {
    const SkColor background = frame.at(2, 2);
    int bands = 0;
    bool inside = false;
    for (int y = 0; y < frame.height; ++y) {
        bool rowHasInk = false;
        for (int x = 0; x < frame.width; ++x) {
            if (differs(frame.at(x, y), background)) {
                rowHasInk = true;
                break;
            }
        }
        if (rowHasInk && !inside) {
            ++bands;
            inside = true;
        } else if (!rowHasInk) {
            inside = false;
        }
    }
    return bands;
}

bool frameUniform(const Frame& frame) {
    const SkColor background = frame.at(2, 2);
    for (int y = 0; y < frame.height; y += 3) {
        for (int x = 0; x < frame.width; x += 3) {
            if (differs(frame.at(x, y), background)) {
                return false;
            }
        }
    }
    return true;
}

scene::Scene singleItemScene(std::vector<std::string> lines, double fontSize,
                             const std::string& family = "") {
    scene::Scene subject;
    subject.showId = "show1";
    subject.slideId = "slide1";
    subject.background.color = 0x102030u;
    scene::TextItem item;
    item.lines = std::move(lines);
    item.fontSize = fontSize;
    item.fontFamily = family;
    item.color = 0xFFFFFFu;
    subject.items.push_back(std::move(item));
    return subject;
}

/* --- fixtures ------------------------------------------------------------ */

void testEmptyScenesDrawNothing() {
    Frame frame;
    std::string error;
    if (!renderFixture(scene::clearedScene(), "empty", &frame, &error)) {
        check(false, "cleared scene rendered: " + error);
        return;
    }
    check(measureInk(frame).count == 0, "cleared scene draws no ink at all");

    // The demo interlude (no lines) is the same promise through the compiler.
    const auto documents = scene::demoDocuments();
    scene::Scene interlude;
    if (!check(scene::compileDemoSlide(documents, 3, &interlude),
               "demo interlude compiles")) {
        return;
    }
    if (!renderFixture(interlude, "interlude", &frame, &error)) {
        check(false, "interlude rendered: " + error);
        return;
    }
    check(frameUniform(frame), "interlude is one flat background colour");
}

void testTextIsCentred() {
    Frame frame;
    std::string error;
    if (!renderFixture(singleItemScene({"Grace"}, 120.0), "centre", &frame, &error)) {
        check(false, "centre fixture rendered: " + error);
        return;
    }
    const Ink ink = measureInk(frame);
    check(ink.count > 500, "single item draws real glyphs (ink " + std::to_string(ink.count) + ")");
    check(std::abs(ink.centerX - kWidth / 2.0) <= 4.0,
          "ink centroid is centred horizontally (" + std::to_string(ink.centerX) + ")");
    check(std::abs(ink.centerY - kHeight / 2.0) <= 12.0,
          "ink centroid is centred vertically (" + std::to_string(ink.centerY) + ")");

    // Different content, same centring.
    if (!renderFixture(singleItemScene({"A comparatively much longer lyric line"}, 64.0),
                       "centre-long", &frame, &error)) {
        check(false, "long centre fixture rendered: " + error);
        return;
    }
    const Ink longInk = measureInk(frame);
    check(std::abs(longInk.centerX - kWidth / 2.0) <= 4.0, "longer line stays centred");
    check(longInk.count > ink.count, "longer line is more ink than Grace");
}

void testWrapping() {
    // One long authored line: must wrap inside the content area across
    // several lines, and wrapping must use the available width.
    std::string line;
    for (int i = 0; i < 60; ++i) {
        line += "hope";
        line += ' ';
    }
    Frame frame;
    std::string error;
    if (!renderFixture(singleItemScene({line}, 48.0), "wrap", &frame, &error)) {
        check(false, "wrap fixture rendered: " + error);
        return;
    }
    const Ink ink = measureInk(frame);
    const int wrapLeft = static_cast<int>(worker::kSceneMarginX * kScale);
    const int wrapRight =
        static_cast<int>((worker::kSceneCanvasWidth - worker::kSceneMarginX) * kScale);
    check(ink.count > 2000, "wrapped text draws plenty of ink");
    check(ink.left >= wrapLeft - 2, "wrapping respects the left margin");
    check(ink.right <= wrapRight + 2, "wrapping respects the right margin");
    const int bandCount = inkBands(frame);
    check(bandCount >= 3, "one long line became >= 3 drawn lines (" +
                             std::to_string(bandCount) + ")");
    // Wrapped text fills most of the wrap width (words, not a short ragged edge).
    check(ink.right - ink.left >= static_cast<int>(0.8 * (wrapRight - wrapLeft)),
          "wrapped block fills the authored width");
}

void testFitShrinksOversizedStanzas() {
    // Twelve authored lines at a size that cannot possibly fit 880 authored
    // px: the whole block must shrink to fit, lines intact, nothing clipped.
    std::vector<std::string> lines;
    for (int i = 1; i <= 12; ++i) {
        lines.push_back("VERSE LINE " + std::to_string(i));
    }
    const scene::Scene subject = singleItemScene(lines, 260.0);

    // Direct engine assertions (the same layout the words layer caches).
    worker::SceneTextEngine engine;
    std::string error;
    if (!engine.create(&error)) {
        check(false, "engine created for fit assertions: " + error);
        return;
    }
    const auto layout = engine.layoutScene(subject);
    check(layout.scale < 1.0f, "oversized stanza applies a shrink (scale " +
                                   std::to_string(layout.scale) + ")");
    check(layout.blockHeight <= worker::kSceneCanvasHeight - 2 * worker::kSceneMarginY + 1.0f,
          "fitted block height fits the vertical margins (" +
              std::to_string(layout.blockHeight) + ")");
    check(!layout.items.empty() && layout.items[0].lineCount == 12,
          "each authored line survived the fit as one layout line");

    // A stanza that already fits keeps scale 1 (no gratuitous shrinking).
    const auto calm = engine.layoutScene(singleItemScene({"FINE LINE ONE", "FINE LINE TWO"}, 72.0));
    check(calm.scale == 1.0f, "fits-with-room stanza is not shrunk");

    // And the snapshot shows the whole shrunk block inside the content area.
    Frame frame;
    if (!renderFixture(subject, "fit", &frame, &error)) {
        check(false, "fit fixture rendered: " + error);
        return;
    }
    const Ink ink = measureInk(frame);
    check(ink.count > 3000, "shrunk stanza still fills the slide");
    check(ink.left >= kContentLeft && ink.right <= kContentRight,
          "shrunk stanza stays inside the horizontal margins");
    check(ink.top >= kContentTop && ink.bottom <= kContentBottom,
          "shrunk stanza stays inside the vertical margins (top " + std::to_string(ink.top) +
              " bottom " + std::to_string(ink.bottom) + ")");
}

void testFallbackAndShaping() {
    Frame frame;
    std::string error;

    // (a) An unknown family must fall through to a real face, not blanks.
    const std::string text = "Fallback still renders clearly";
    if (!renderFixture(singleItemScene({text}, 72.0, "OpenShow No Such Family 47"),
                       "fallback-family", &frame, &error)) {
        check(false, "unknown-family fixture rendered: " + error);
        return;
    }
    const Ink fallback = measureInk(frame);
    if (!renderFixture(singleItemScene({text}, 72.0), "fallback-default", &frame, &error)) {
        check(false, "default-family fixture rendered: " + error);
        return;
    }
    const Ink normal = measureInk(frame);
    check(fallback.count > 2000, "unknown family still draws ink (" +
                                     std::to_string(fallback.count) + ")");
    // The fallback face should be in the same ballpark as the default face.
    check(fallback.count > normal.count / 2 && fallback.count < normal.count * 2,
          "unknown-family fallback resolves to a comparable face");

    // (b) Shaping sample: Greek, accents, an em dash through harfbuzz.
    if (!renderFixture(
            singleItemScene({"\xce\xa9 \xce\xb2 \xcf\x80 \xc2\xbd \xe2\x80\x94 caf\xc3\xa9 na\xc3\xafve"},
                            96.0),
            "fallback-shaping", &frame, &error)) {
        check(false, "shaping fixture rendered: " + error);
        return;
    }
    check(measureInk(frame).count > 800, "Greek/accented shaping draws real glyphs");

    // (c) Script fallback: glyphs the default Latin face cannot provide.
    //     Whatever fontconfig resolves must draw ink, not blanks.
    if (!renderFixture(singleItemScene({"\xe6\xb0\xb8\xe6\x81\x92\xe8\x89\xaf\xe5\x96\x84"},
                                       96.0),
                       "fallback-script", &frame, &error)) {
        check(false, "script fallback fixture rendered: " + error);
        return;
    }
    check(measureInk(frame).count > 400, "non-Latin script falls back to drawn glyphs");
}

void testMultiItemStacking() {
    scene::Scene subject = singleItemScene({"FIRST BLOCK"}, 100.0);
    scene::TextItem second;
    second.lines = {"second block of words"};
    second.fontSize = 60.0;
    second.color = 0xFFE0A0u;
    subject.items.push_back(std::move(second));

    Frame frame;
    std::string error;
    if (!renderFixture(subject, "stack", &frame, &error)) {
        check(false, "stack fixture rendered: " + error);
        return;
    }
    const Ink ink = measureInk(frame);
    check(inkBands(frame) >= 2, "two items draw as separate stacked bands");
    check(std::abs(ink.centerY - kHeight / 2.0) <= 12.0,
          "stacked block stays vertically centred (" + std::to_string(ink.centerY) + ")");

    // Engine view: heights accumulate with a gap, sizes keep their ratio.
    worker::SceneTextEngine engine;
    if (!engine.create(&error)) {
        check(false, "engine created for stack assertions: " + error);
        return;
    }
    const auto layout = engine.layoutScene(subject);
    check(layout.items.size() == 2, "layout visits both items");
    check(layout.gap > 0.0f, "items are separated by a gap");
    if (layout.items.size() == 2) {
        check(layout.items[0].fontSize > layout.items[1].fontSize,
              "relative item sizes survive stacking");
    }
}

void testWireSceneRendersIdentically() {
    // The scene.prepare payload path: serialise, re-validate, render — the
    // worker draws byte-equal pictures from wire scenes.
    const scene::Scene subject =
        singleItemScene({"Come, all people", "", "Sing the song of grace"}, 72.0);
    const std::string text = boost::json::serialize(scene::toJson(subject));
    boost::system::error_code ec;
    const boost::json::value parsed = boost::json::parse(text, ec);
    if (!check(!ec, "fixture serialises to wire JSON")) {
        return;
    }
    scene::Scene restored;
    if (!check(scene::fromJson(parsed, &restored), "wire JSON re-validates")) {
        return;
    }
    const std::string directPath = "scene-render-wire-direct.png";
    const std::string wirePath = "scene-render-wire-restored.png";
    std::string error;
    if (!check(worker::renderSceneSnapshot(subject, directPath, kWidth, kHeight, &error),
               "direct render: " + error) ||
        !check(worker::renderSceneSnapshot(restored, wirePath, kWidth, kHeight, &error),
               "wire render: " + error)) {
        return;
    }
    std::ifstream a(directPath, std::ios::binary);
    std::ifstream b(wirePath, std::ios::binary);
    const std::string aBytes((std::istreambuf_iterator<char>(a)),
                             std::istreambuf_iterator<char>());
    const std::string bBytes((std::istreambuf_iterator<char>(b)),
                             std::istreambuf_iterator<char>());
    check(!aBytes.empty() && aBytes == bBytes,
          "wire scene renders byte-identically to the in-memory scene");
}

void testDemoSlidesRender() {
    // All four compiled demo slides render through the production path:
    // the title card is rich, the stanza fills, the cue is small, and the
    // interlude is flat (checked in the empty-scene fixture).
    const auto documents = scene::demoDocuments();
    const std::vector<int> minimumInk = {5000, 3000, 500, 0};
    for (std::size_t i = 0; i < minimumInk.size(); ++i) {
        scene::Scene subject;
        std::string error;
        if (!check(scene::compileDemoSlide(documents, i, &subject, &error),
                   "demo slide " + std::to_string(i) + " compiles: " + error)) {
            continue;
        }
        Frame frame;
        if (!renderFixture(subject, "demo-" + std::to_string(i), &frame, &error)) {
            check(false, "demo slide " + std::to_string(i) + " rendered: " + error);
            continue;
        }
        const Ink ink = measureInk(frame);
        check(ink.count >= minimumInk[i], "demo slide " + std::to_string(i) +
                                              " ink " + std::to_string(ink.count) +
                                              " >= " + std::to_string(minimumInk[i]));
        if (i < 3) {
            // Alignment is the bounding-box centre: per-line mass centroid
            // shifts with glyph density across a multi-line block.
            const double boxCenterX = (ink.left + ink.right) / 2.0;
            check(std::abs(boxCenterX - kWidth / 2.0) <= 8.0,
                  "demo slide " + std::to_string(i) + " is horizontally centred (box cx " +
                      std::to_string(boxCenterX) + ")");
        }
    }
}

}  // namespace

int main() {
    // Font availability is the one environment dependency: skip (77) rather
    // than fail a machine with no usable system fonts.
    worker::SceneTextEngine probe;
    std::string error;
    if (!probe.create(&error)) {
        std::fprintf(stderr, "scene_render_tests: no usable fonts (%s); skipping\n",
                     error.c_str());
        return 77;
    }

    testEmptyScenesDrawNothing();
    testTextIsCentred();
    testWrapping();
    testFitShrinksOversizedStanzas();
    testFallbackAndShaping();
    testMultiItemStacking();
    testWireSceneRendersIdentically();
    testDemoSlidesRender();

    if (failures == 0) {
        std::printf("scene_render_tests: all fixtures passed\n");
        return 0;
    }
    std::fprintf(stderr, "scene_render_tests: %d failure(s)\n", failures);
    return 1;
}
