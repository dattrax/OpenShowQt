/*
 * Task 3.1–3.2 fixtures: production scene presentation — wrapping,
 * alignment, fallback and fit, image/background composition and cut +
 * crossfade transitions (PLAN.md acceptance).
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
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkData.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRect.h"
#include "include/core/SkSurface.h"

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
    if (!worker::renderSceneSnapshot(subject, path, kWidth, kHeight, worker::SnapshotOptions{},
                                     error)) {
        return false;
    }
    return loadPng(path, frame, error);
}

// Renders a crossfade stage (or a layer-disabled frame) through the same
// entry point and returns the decoded pixels for measurement.
bool renderFixture(const scene::Scene& subject, const std::string& name,
                   const worker::SnapshotOptions& options, Frame* frame, std::string* error) {
    const std::string path = "scene-render-" + name + ".png";
    if (!worker::renderSceneSnapshot(subject, path, kWidth, kHeight, options, error)) {
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
    // Per-item shrinkToFit (task 4.3): the item shrank below its authored
    // 260 px and the resulting paragraph fits the box height.
    check(layout.items.size() == 1 && layout.items[0].fontSize < 260.0f,
          "oversized stanza shrinks to fit its own box");
    check(layout.items.empty() ||
              layout.items[0].height <= layout.items[0].boxHeight + 1.0f,
          "fitted paragraph fits its box height");
    check(!layout.items.empty() && layout.items[0].lineCount == 12,
          "each authored line survived the fit as one layout line");

    // A stanza that already fits keeps its authored size (no gratuitous shrinking).
    const auto calm = engine.layoutScene(singleItemScene({"FINE LINE ONE", "FINE LINE TWO"}, 72.0));
    check(!calm.items.empty() && calm.items[0].fontSize == 72.0f,
          "fits-with-room stanza is not shrunk");

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

void testPerBoxPlacement() {
    // Task 4.3: every item is centred inside its own authored box instead
    // of stacking into one shared block — upper box in the top third, lower
    // box in the bottom third of the authored 1920x1080 canvas.
    scene::Scene subject = singleItemScene({"UPPER BOX"}, 100.0);
    subject.items[0].x = 160.0;
    subject.items[0].y = 100.0;
    subject.items[0].width = 1600.0;
    subject.items[0].height = 300.0;
    scene::TextItem second;
    second.lines = {"lower box words"};
    second.fontSize = 60.0;
    second.color = 0xFFE0A0u;
    second.x = 160.0;
    second.y = 680.0;
    second.width = 1600.0;
    second.height = 300.0;
    subject.items.push_back(std::move(second));

    Frame frame;
    std::string error;
    if (!renderFixture(subject, "per-box", &frame, &error)) {
        check(false, "two-box fixture rendered: " + error);
        return;
    }
    check(inkBands(frame) >= 2, "two boxes draw as two separated bands");

    // Engine view: the boxes travel with their items, and two items that
    // each fit their own box keep their authored sizes (no shared shrink).
    worker::SceneTextEngine engine;
    if (!engine.create(&error)) {
        check(false, "engine created for box assertions: " + error);
        return;
    }
    const auto layout = engine.layoutScene(subject);
    check(layout.items.size() == 2, "layout visits both items");
    if (layout.items.size() == 2) {
        check(layout.items[0].boxY == 100.0f && layout.items[1].boxY == 680.0f,
              "authored boxes travel with their items");
        check(layout.items[0].fontSize == 100.0f && layout.items[1].fontSize == 60.0f,
              "items that fit their boxes keep their own sizes");
    }

    // Vertical centring per box: rendered alone, each item's ink centroid
    // lands at its own box centre (250 and 830 authored px, at kScale).
    scene::Scene upperOnly = subject;
    upperOnly.items.pop_back();
    scene::Scene lowerOnly = subject;
    lowerOnly.items.erase(lowerOnly.items.begin());
    Frame upper;
    Frame lower;
    if (!renderFixture(upperOnly, "box-upper", &upper, &error) ||
        !renderFixture(lowerOnly, "box-lower", &lower, &error)) {
        check(false, "single-box fixture rendered: " + error);
        return;
    }
    const double upperCenter = measureInk(upper).centerY;
    const double lowerCenter = measureInk(lower).centerY;
    check(std::abs(upperCenter - 250.0 * kScale) <= 12.0,
          "upper item centres in its own box (" + std::to_string(upperCenter) + ")");
    check(std::abs(lowerCenter - 830.0 * kScale) <= 12.0,
          "lower item centres in its own box (" + std::to_string(lowerCenter) + ")");
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
    if (!check(worker::renderSceneSnapshot(subject, directPath, kWidth, kHeight,
                                           worker::SnapshotOptions{}, &error),
               "direct render: " + error) ||
        !check(worker::renderSceneSnapshot(restored, wirePath, kWidth, kHeight,
                                           worker::SnapshotOptions{}, &error),
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

/* --- task 3.2: image backgrounds, crossfade stages, layer toggles ---------- */

// Writes a red/green/blue banded PNG. verticalThirds gives left-to-right
// stripes, otherwise top-to-bottom bands. The images are deliberately not
// canvas-shaped so the cover crop direction carries meaning.
bool writeBandPng(const std::string& path, int width, int height, bool verticalThirds,
                  std::string* error) {
    auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));
    if (!surface) {
        if (error) {
            *error = "fixture image surface failed";
        }
        return false;
    }
    const SkColor colors[3] = {SkColorSetRGB(235, 40, 40), SkColorSetRGB(40, 200, 40),
                               SkColorSetRGB(40, 40, 235)};
    for (int i = 0; i < 3; ++i) {
        SkPaint paint;
        paint.setColor(colors[i]);
        const SkRect band =
            verticalThirds
                ? SkRect::MakeXYWH(static_cast<float>(width) * i / 3.0f, 0.0f,
                                   static_cast<float>(width) / 3.0f, static_cast<float>(height))
                : SkRect::MakeXYWH(0.0f, static_cast<float>(height) * i / 3.0f,
                                   static_cast<float>(width), static_cast<float>(height) / 3.0f);
        surface->getCanvas()->drawRect(band, paint);
    }
    return worker::writePngRaster(surface.get(), path, error);
}

scene::Scene bandImageScene(const std::string& path, std::vector<std::string> lines) {
    scene::Scene subject;
    subject.showId = "show1";
    subject.slideId = "bg1";
    subject.background.color = 0x102030u;  // the fill the image covers
    subject.background.hasImage = true;
    subject.background.assetId = "asset1";
    subject.background.assetPath = path;
    if (!lines.empty()) {
        scene::TextItem item;
        item.lines = std::move(lines);
        item.fontSize = 200.0;
        item.color = 0xFFFFFFu;
        subject.items.push_back(std::move(item));
    }
    return subject;
}

// Colour equality within tolerance — the band assertions use generously
// separated primaries, so PNG colour-management drift (a handful of units)
// never crosses one.
bool closeTo(SkColor color, SkColor expected, int tolerance = 24) {
    return !differs(color, expected, tolerance);
}

void testImageBackgroundCover() {
    // Cover fit (the lyric default): the image fills the authored canvas
    // edge to edge — no letterbox bars — and the overflow is cropped
    // equally about the centre. Text composites over it.
    std::string error;
    if (!check(writeBandPng("scene-render-bg-wide.png", 1920, 540, true, &error),
               "wide band image written: " + error) ||
        !check(writeBandPng("scene-render-bg-tall.png", 540, 1920, false, &error),
               "tall band image written: " + error)) {
        return;
    }
    const SkColor red = SkColorSetRGB(235, 40, 40);
    const SkColor green = SkColorSetRGB(40, 200, 40);
    const SkColor blue = SkColorSetRGB(40, 40, 235);

    Frame wide;
    const scene::Scene wideScene = bandImageScene("scene-render-bg-wide.png", {"COVER"});
    if (!check(renderFixture(wideScene, "bg-wide", &wide, &error),
               "wide image background rendered: " + error)) {
        return;
    }
    // The canvas shows image x 480..1440 (scale 2, centred): the far left
    // still lands in the red third, mid-left through right land green,
    // green and blue; the top and bottom rows are image colour too, which
    // is what separates cover from a letterboxed contain.
    check(closeTo(wide.at(50, 270), red), "cover: canvas left shows the red third");
    check(closeTo(wide.at(200, 270), green), "cover: crop brings green left of centre");
    check(closeTo(wide.at(480, 270), green), "cover: canvas centre shows the green third");
    check(closeTo(wide.at(860, 270), blue), "cover: canvas right shows the blue third");
    check(closeTo(wide.at(200, 6), green), "cover: no letterbox bar on top");
    check(closeTo(wide.at(200, 534), green), "cover: no letterbox bar on bottom");
    check(measureInk(wide).count > 1000, "words draw over the image background");

    Frame tall;
    const scene::Scene tallScene = bandImageScene("scene-render-bg-tall.png", {});
    if (!check(renderFixture(tallScene, "bg-tall", &tall, &error),
               "tall image background rendered: " + error)) {
        return;
    }
    // The portrait crop shows only the middle band, so the canvas is
    // uniformly green: the top and bottom thirds are cropped away.
    check(closeTo(tall.at(4, 4), green), "cover: portrait top-left is the middle band");
    check(closeTo(tall.at(956, 4), green), "cover: portrait top-right is the middle band");
    check(closeTo(tall.at(4, 536), green), "cover: portrait bottom-left is the middle band");
    check(closeTo(tall.at(956, 536), green), "cover: portrait bottom edge is the middle band");
    check(measureInk(tall).count == 0, "cover: portrait crop is uniformly the middle band");
}

void testMissingBackgroundFile() {
    // The compiler's promise: file existence is the renderer's problem.
    // An unreadable assetPath degrades to the plain colour fill, words
    // still on top, no crash.
    std::string error;
    const scene::Scene subject =
        bandImageScene("scene-render-no-such-file.png", {"STILL HERE"});
    Frame frame;
    if (!check(renderFixture(subject, "bg-missing", &frame, &error),
               "background with a missing file rendered: " + error)) {
        return;
    }
    check(closeTo(frame.at(2, 2), SkColorSetRGB(0x10, 0x20, 0x30)),
          "missing image falls back to the scene colour");
    check(measureInk(frame).count > 1000, "missing image still shows the words");
}

void testCrossfadeStages() {
    // The snapshot renders arbitrary stages of the same dissolve the live
    // layers composite: the background crossfades by drawing the incoming
    // scene over the outgoing one at alpha mediaBlend (the over-composite
    // of two opaque pictures is an exact lerp); the words dip through the
    // text track. Media and text advance independently.
    std::string error;
    scene::Scene from;
    from.showId = "show1";
    from.slideId = "fade-a";
    from.background.color = 0xE03030u;
    scene::TextItem words;
    words.lines = {"ONE"};
    words.fontSize = 300.0;
    words.color = 0xFFFFFFu;
    from.items.push_back(std::move(words));
    scene::Scene to;
    to.showId = "show1";
    to.slideId = "fade-b";
    to.background.color = 0x3030E0u;  // plain blue: fully faded over means no ink

    Frame start, mid, end, textOnly;
    worker::SnapshotOptions options;
    options.fadeFrom = &from;
    options.mediaBlend = 0.0f;
    options.textBlend = 0.0f;
    const bool rendered =
        check(renderFixture(to, "fade-0", options, &start, &error),
              "crossfade stage 0 rendered: " + error);
    options.mediaBlend = 0.5f;
    options.textBlend = 0.5f;
    const bool renderedMid = rendered && check(renderFixture(to, "fade-50", options, &mid, &error),
                                               "crossfade stage 0.5 rendered: " + error);
    options.mediaBlend = 1.0f;
    options.textBlend = 1.0f;
    const bool renderedEnd = renderedMid &&
                             check(renderFixture(to, "fade-100", options, &end, &error),
                                   "crossfade stage 1 rendered: " + error);
    if (!renderedEnd) {
        return;
    }
    options.mediaBlend = 1.0f;
    options.textBlend = 0.3f;  // media arrived while the words are still dissolving
    if (!check(renderFixture(to, "fade-tracks", options, &textOnly, &error),
               "crossfade split-track frame rendered: " + error)) {
        return;
    }

    const SkColor cornerStart = start.at(2, 2);
    const SkColor cornerEnd = end.at(2, 2);
    check(closeTo(cornerStart, SkColorSetRGB(0xE0, 0x30, 0x30)), "stage 0 is the old background");
    check(closeTo(cornerEnd, SkColorSetRGB(0x30, 0x30, 0xE0)), "stage 1 is the new background");
    const Ink inkStart = measureInk(start);
    const Ink inkMid = measureInk(mid);
    const Ink inkEnd = measureInk(end);
    check(inkStart.count > 5000, "stage 0 shows the old words at full strength");
    check(inkEnd.count == 0, "stage 1 has no trace of the old words");
    // The background is the average of the endpoints (over-composite of
    // opaques at alpha 128 is the midpoint to rounding).
    const SkColor expect = SkColorSetARGB(
        0xFF, (SkColorGetR(cornerStart) + SkColorGetR(cornerEnd)) / 2,
        (SkColorGetG(cornerStart) + SkColorGetG(cornerEnd)) / 2,
        (SkColorGetB(cornerStart) + SkColorGetB(cornerEnd)) / 2);
    check(closeTo(mid.at(2, 2), expect, 16),
          "stage 0.5 background is the midpoint (got " +
              std::to_string(SkColorGetR(mid.at(2, 2))) + "," +
              std::to_string(SkColorGetG(mid.at(2, 2))) + "," +
              std::to_string(SkColorGetB(mid.at(2, 2))) + " want " +
              std::to_string(SkColorGetR(expect)) + "," + std::to_string(SkColorGetG(expect)) +
              "," + std::to_string(SkColorGetB(expect)) + ")");
    check(inkMid.count > inkStart.count / 4 && inkMid.count < inkStart.count,
          "stage 0.5 words are dissolving (ink " + std::to_string(inkMid.count) + " of " +
              std::to_string(inkStart.count) + ")");
    // Independent tracks: media 1.0 means the background already arrived,
    // text 0.3 means the words are still mostly the old ones.
    check(closeTo(textOnly.at(2, 2), cornerEnd, 8), "split frame: media track completed");
    check(measureInk(textOnly).count > inkStart.count / 4,
          "split frame: text track still dissolving");
}

void testSceneLayerToggles() {
    // The UI's layer switches against a presented scene: a disabled layer
    // dispatches nothing. Background off leaves the words on the black
    // clear; words off leaves the composed background alone.
    std::string error;
    if (!check(writeBandPng("scene-render-bg-tall.png", 540, 1920, false, &error),
               "toggle band image written: " + error)) {
        return;
    }
    const scene::Scene subject = bandImageScene("scene-render-bg-tall.png", {"LAYERS"});

    worker::SnapshotOptions noBackground;
    noBackground.background = false;
    Frame frame;
    if (!check(renderFixture(subject, "layers-nobg", noBackground, &frame, &error),
               "background-off frame rendered: " + error)) {
        return;
    }
    check(closeTo(frame.at(2, 2), SK_ColorBLACK, 8),
          "background off: the frame is the plain black clear");
    check(measureInk(frame).count > 1000, "background off: words still dispatch");

    worker::SnapshotOptions noWords;
    noWords.words = false;
    if (!check(renderFixture(subject, "layers-nowords", noWords, &frame, &error),
               "words-off frame rendered: " + error)) {
        return;
    }
    check(closeTo(frame.at(2, 2), SkColorSetRGB(40, 200, 40)),
          "words off: the image background is drawn");
    check(measureInk(frame).count == 0, "words off: no ink anywhere");
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
    testPerBoxPlacement();
    testWireSceneRendersIdentically();
    testDemoSlidesRender();
    testImageBackgroundCover();
    testMissingBackgroundFile();
    testCrossfadeStages();
    testSceneLayerToggles();

    if (failures == 0) {
        std::printf("scene_render_tests: all fixtures passed\n");
        return 0;
    }
    std::fprintf(stderr, "scene_render_tests: %d failure(s)\n", failures);
    return 1;
}
