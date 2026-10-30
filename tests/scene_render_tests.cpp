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
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "include/codec/SkCodec.h"
#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkData.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRect.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkSurface.h"
#include "include/effects/SkImageFilters.h"

#include "openshow/domain/documents.h"
#include "openshow/domain/storage.h"
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

// Pixel-weighted centre Y of each consecutive band of inked rows — one
// entry per drawn line. The distance between two entries is the rendered
// line pitch (task 4.4: it scales with the item's line spacing).
std::vector<double> bandCenterYs(const Frame& frame) {
    const SkColor background = frame.at(2, 2);
    std::vector<double> centers;
    double sum = 0.0;
    long long count = 0;
    bool inside = false;
    for (int y = 0; y < frame.height; ++y) {
        long long rowInk = 0;
        for (int x = 0; x < frame.width; ++x) {
            if (differs(frame.at(x, y), background)) {
                ++rowInk;
            }
        }
        if (rowInk > 0) {
            inside = true;
            sum += static_cast<double>(y) * static_cast<double>(rowInk);
            count += rowInk;
        } else if (inside) {
            centers.push_back(sum / static_cast<double>(count));
            sum = 0.0;
            count = 0;
            inside = false;
        }
    }
    if (count > 0) {
        centers.push_back(sum / static_cast<double>(count));
    }
    return centers;
}

scene::Scene singleItemScene(std::vector<std::string> lines, double fontSize,
                             const std::string& family = "") {
    scene::Scene subject;
    subject.showId = "show1";
    subject.slideId = "slide1";
    subject.background.color = 0x102030u;
    scene::SceneItem item;
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
    scene::SceneItem second;
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

// Task 6.17a (review R17), the pixel end of the chain. Every fixture above
// hands the renderer a hand-built Scene; this one starts where an operator
// starts — a document — because the thing that had to be proved is the seam
// between them. Two claims, measured:
//
//   - a SECOND authored box reaches the pixels as its own words inside its own
//     rectangle, in the model's draw order, rather than being dropped or
//     folded into the first box's layout;
//   - a service written before boxes existed draws the SAME frame it drew
//     before this change — checked against a literal transcription of what the
//     single-box compiler emitted, byte for byte, not argued from re-reading
//     the code that changed.
void testAuthoredBoxStackRenders() {
    namespace domain = openshow::domain;
    std::string error;

    // --- two authored boxes -> two pieces of ink, each where its box is ---
    domain::Documents docs;
    const domain::Id show = docs.createShow("Stacked", &error);
    const domain::Id slide = docs.appendSlide(show, &error);
    docs.setSlideLines(show, slide, {"bottom of the stack"}, &error);
    domain::SlideItem title;
    title.lines = {"A TITLE STRIP"};
    title.x = 160.0;
    title.y = 60.0;
    title.width = 1600.0;
    title.height = 180.0;
    check(!docs.addItem(show, slide, title, &error).empty(),
          "the document accepts a second box");

    scene::Scene compiled;
    if (!check(scene::compileSlide(docs, show, slide, &compiled, &error) &&
                   compiled.items.size() == 2,
               "a two-box slide compiles to two wire items: " + error)) {
        return;
    }
    Frame stacked;
    if (!renderFixture(compiled, "authored-stack", &stacked, &error)) {
        check(false, "the two-box slide renders: " + error);
        return;
    }
    check(inkBands(stacked) >= 2, "both authored boxes draw as separated ink bands");

    // Each box alone lands at the centre of its OWN box (150 and 540 authored
    // y). Without this, "two bands" would still pass if the compiler drew both
    // items in the first box's rectangle and one happened to sit above the
    // other's baseline.
    scene::Scene onlyTitle = compiled;
    onlyTitle.items.erase(onlyTitle.items.begin());
    scene::Scene onlyBottom = compiled;
    onlyBottom.items.erase(onlyBottom.items.begin() + 1);
    Frame titleFrame;
    Frame bottomFrame;
    if (!renderFixture(onlyTitle, "authored-title-only", &titleFrame, &error) ||
        !renderFixture(onlyBottom, "authored-bottom-only", &bottomFrame, &error)) {
        check(false, "the single-box renders: " + error);
        return;
    }
    const double titleCentre = measureInk(titleFrame).centerY;
    const double bottomCentre = measureInk(bottomFrame).centerY;
    check(std::abs(titleCentre - 150.0 * kScale) <= 12.0,
          "the second box's words centre in the second box (" + std::to_string(titleCentre) + ")");
    check(std::abs(bottomCentre - 540.0 * kScale) <= 12.0,
          "the first box's words stay in the first box (" + std::to_string(bottomCentre) + ")");

    // And through the wire, unchanged: the picture the worker paints from a
    // scene.prepare payload of a stacked slide is the picture of the scene.
    const std::string wire = boost::json::serialize(scene::toJson(compiled));
    boost::system::error_code parseError;
    scene::Scene fromWire;
    if (check(scene::fromJson(boost::json::parse(wire, parseError), &fromWire, &error) &&
                  !parseError && fromWire == compiled,
              "a stacked scene round-trips through the wire unchanged")) {
        Frame stackedAgain;
        if (renderFixture(fromWire, "authored-stack-wire", &stackedAgain, &error)) {
            check(stackedAgain.pixels == stacked.pixels,
                  "the stacked scene renders identically before and after the wire");
        } else {
            check(false, "the wire-stacked scene renders: " + error);
        }
    }

    // --- a format-1 service still draws yesterday's frame ---
    const auto root = std::filesystem::temp_directory_path() /
                      ("openshow-render-legacy-" + std::to_string(static_cast<long>(::getpid())));
    std::filesystem::create_directories(root);
    {
        std::ofstream out(root / "service.json");
        // Deliberately the whole old shape: format 1, no items, the slide's own
        // lines and geometry. This is a file from before the change, not a file
        // this build wrote and renamed.
        out << R"svc({"format":1,"nextOrdinal":9,"shows":[{"id":"show1","name":"Old","notes":"","defaultStyleId":"","slides":[{"id":"slide1","group":"","lines":["Ancient grace","","still centred"],"notes":"","styleId":"","backgroundAssetId":"","x":160,"y":100,"width":1600,"height":880}]}],"styles":[],"assets":[],"playlists":[]})svc";
    }
    domain::Documents legacy;
    scene::Scene legacyCompiled;
    if (check(domain::loadService(&legacy, root, &error) &&
                  scene::compileSlide(legacy, "show1", "slide1", &legacyCompiled, &error) &&
                  legacyCompiled.items.size() == 1,
              "a format-1 service loads and compiles to one item: " + error)) {
        // What the single-box compiler emitted for the same slide, transcribed
        // literally: one item carrying the slide's lines at SceneItem's own
        // defaults, because those defaults ARE the legacy lyric box (pinned in
        // scene_tests). If migration or compilation ever shifts a default by a
        // pixel, this equality is what breaks.
        scene::Scene oldShape;
        oldShape.showId = "show1";
        oldShape.slideId = "slide1";
        scene::SceneItem only;
        only.lines = {"Ancient grace", "", "still centred"};
        oldShape.items.push_back(only);
        check(legacyCompiled == oldShape,
              "the migrated slide compiles to the exact scene the single-box compiler emitted");
        Frame now;
        Frame then;
        if (renderFixture(legacyCompiled, "legacy-now", &now, &error) &&
            renderFixture(oldShape, "legacy-old", &then, &error)) {
            check(now.pixels == then.pixels,
                  "and the frame is byte-for-byte the frame that file always drew");
            check(inkBands(now) >= 1, "which is still ink");
        } else {
            check(false, "the legacy frames render: " + error);
        }
    }
    std::filesystem::remove_all(root);
}

void testTypographyAlignsAndSpaces() {
    // Task 4.4: the style's align places the lines inside the authored
    // box, lineSpacing opens the line box, and an item that carries
    // neither still draws centred at the renderer's own line box.
    scene::Scene centred = singleItemScene({"Edge ink"}, 100.0);  // struct defaults
    scene::Scene leftAligned = centred;
    leftAligned.items[0].align = "left";
    scene::Scene rightAligned = centred;
    rightAligned.items[0].align = "right";

    Frame centreFrame;
    Frame leftFrame;
    Frame rightFrame;
    std::string error;
    if (!renderFixture(centred, "align-centre", &centreFrame, &error) ||
        !renderFixture(leftAligned, "align-left", &leftFrame, &error) ||
        !renderFixture(rightAligned, "align-right", &rightFrame, &error)) {
        check(false, "alignment fixtures rendered: " + error);
        return;
    }
    const Ink centreInk = measureInk(centreFrame);
    const Ink leftInk = measureInk(leftFrame);
    const Ink rightInk = measureInk(rightFrame);

    // The default box spans authored x 160..1760 (device 80..880).
    const double boxLeft = scene::kDefaultBoxX * kScale;
    const double boxRight = (scene::kDefaultBoxX + scene::kDefaultBoxWidth) * kScale;
    check(leftInk.left >= boxLeft - 3.0 && leftInk.left <= boxLeft + 14.0,
          "left-aligned ink hugs the box's left edge (" + std::to_string(leftInk.left) + ")");
    check(rightInk.right <= boxRight + 3.0 && rightInk.right >= boxRight - 16.0,
          "right-aligned ink hugs the box's right edge (" + std::to_string(rightInk.right) + ")");
    check(leftInk.centerX < centreInk.centerX - 100.0 &&
              rightInk.centerX > centreInk.centerX + 100.0,
          "the default (no align set) sits between the aligned edges");

    // Line spacing: the pitch between two drawn lines scales with the
    // multiplier. 100px at the default 1.25 is a 62.5px device pitch;
    // doubling the multiplier doubles the measured pitch.
    scene::Scene tight = singleItemScene({"line one", "line two"}, 100.0);
    scene::Scene loose = tight;
    loose.items[0].lineSpacing = 2.5;
    Frame tightFrame;
    Frame looseFrame;
    if (!renderFixture(tight, "spacing-tight", &tightFrame, &error) ||
        !renderFixture(loose, "spacing-loose", &looseFrame, &error)) {
        check(false, "spacing fixtures rendered: " + error);
        return;
    }
    const std::vector<double> tightCentres = bandCenterYs(tightFrame);
    const std::vector<double> looseCentres = bandCenterYs(looseFrame);
    check(tightCentres.size() == 2 && looseCentres.size() == 2,
          "two lines draw as two bands at both spacings");
    if (tightCentres.size() == 2 && looseCentres.size() == 2) {
        const double tightPitch = tightCentres[1] - tightCentres[0];
        const double loosePitch = looseCentres[1] - looseCentres[0];
        const double expectedTight =
            100.0 * openshow::domain::kDefaultLineSpacing * kScale;
        check(std::abs(tightPitch - expectedTight) <= 4.0,
              "the default line box measures 1.25 x fontSize (" +
                  std::to_string(tightPitch) + ")");
        check(std::abs(loosePitch - tightPitch * 2.0) <= tightPitch * 0.15,
              "lineSpacing 2.5 doubles the pitch (" + std::to_string(loosePitch) + " vs " +
                  std::to_string(tightPitch) + ")");
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
        scene::SceneItem item;
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

/* --- task 6.17c1 (review R17): the contain fit letterboxes, and both
 *     draw paths honour the scene's choice -------------------------------- */

void testBackgroundFitContain() {
    // Contain fit: the whole picture inside the authored canvas, centred on
    // both axes, with the scene's colour fill showing in the bars. The wide
    // fixture bars the top and bottom (scale 1), the tall one the sides;
    // "exactly the fill" and "exactly the picture" are both measured, and
    // the cover fixtures above already pin the other half of the switch.
    std::string error;
    if (!check(writeBandPng("scene-render-fit-wide.png", 1920, 540, true, &error),
               "wide fit image written: " + error) ||
        !check(writeBandPng("scene-render-fit-tall.png", 540, 1920, false, &error),
               "tall fit image written: " + error)) {
        return;
    }
    const SkColor red = SkColorSetRGB(235, 40, 40);
    const SkColor green = SkColorSetRGB(40, 200, 40);
    const SkColor blue = SkColorSetRGB(40, 40, 235);
    const SkColor fill = SkColorSetRGB(0x10, 0x20, 0x30);

    scene::Scene wide = bandImageScene("scene-render-fit-wide.png", {});
    wide.background.fit = openshow::domain::BackgroundFit::Contain;
    Frame wideFrame;
    if (!check(renderFixture(wide, "fit-wide-contain", &wideFrame, &error),
               "wide contained background rendered: " + error)) {
        return;
    }
    // 1920x540 inside 1920x1080: full width, and bars of 270 canvas px — 135
    // device px — above and below. The bar rows are exactly the fill...
    check(closeTo(wideFrame.at(480, 2), fill), "contain: the very top row is the fill");
    check(closeTo(wideFrame.at(480, 130), fill), "contain: the bar runs to just inside its edge");
    check(closeTo(wideFrame.at(480, 536), fill), "contain: the very bottom row is the fill");
    // ...and the picture row is exactly the picture, at the same x mapping
    // cover used (scale 1 = full width): red | green | blue thirds.
    check(closeTo(wideFrame.at(480, 142), green), "contain: the picture starts below the bar");
    check(closeTo(wideFrame.at(50, 270), red), "contain: left third unchanged by the letterbox");
    check(closeTo(wideFrame.at(480, 270), green), "contain: centre is the picture, not a bar");
    check(closeTo(wideFrame.at(860, 270), blue), "contain: right third reaches its edge");

    scene::Scene tall = bandImageScene("scene-render-fit-tall.png", {});
    tall.background.fit = openshow::domain::BackgroundFit::Contain;
    Frame tallFrame;
    if (!check(renderFixture(tall, "fit-tall-contain", &tallFrame, &error),
               "tall contained background rendered: " + error)) {
        return;
    }
    // 540x1920 inside 1920x1080: full height, side bars of 808 canvas px —
    // 404 device px — each side, and the three horizontal bands whole.
    check(closeTo(tallFrame.at(2, 270), fill), "contain: the far left is a fill bar");
    check(closeTo(tallFrame.at(398, 270), fill), "contain: the left bar runs to its edge");
    check(closeTo(tallFrame.at(410, 270), green), "contain: the picture's middle band at x410");
    check(closeTo(tallFrame.at(956, 270), fill), "contain: the far right is a fill bar");
    check(closeTo(tallFrame.at(480, 4), red), "contain: the top band is whole, not cropped");
    check(closeTo(tallFrame.at(480, 536), blue), "contain: the bottom band is whole too");

    // Explicit cover is the unchanged rule, spelled out: the same wide
    // picture with fit set to COVER by name leaves no bar anywhere.
    scene::Scene wideCover = wide;
    wideCover.background.fit = openshow::domain::BackgroundFit::Cover;
    Frame coverFrame;
    if (!check(renderFixture(wideCover, "fit-wide-cover", &coverFrame, &error),
               "explicit-cover background rendered: " + error)) {
        return;
    }
    check(!closeTo(coverFrame.at(480, 2), fill) && !closeTo(coverFrame.at(480, 536), fill),
          "cover by name draws like cover by default: the crop fills top and bottom");
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
    scene::SceneItem words;
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

/* --- task 6.11b (review R13): the background fade composites a GROUP --- */

// A flat PNG: one colour, at full opacity or at a uniform alpha below 255.
// The translucent picture is the case that tells a group apart from two
// separately attenuated draws; the size is deliberately not canvas-shaped so
// the cover fit (and the mapping the group has to reproduce) is exercised.
bool writeFlatPng(const std::string& path, SkColor color, std::string* error,
                  uint8_t alpha = 0xFF) {
    auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(120, 80));
    if (!surface) {
        if (error) {
            *error = "flat image surface failed";
        }
        return false;
    }
    SkPaint paint;
    paint.setColor(SkColorSetARGB(alpha, SkColorGetR(color), SkColorGetG(color),
                                  SkColorGetB(color)));
    surface->getCanvas()->drawRect(SkRect::MakeWH(120, 80), paint);
    return worker::writePngRaster(surface.get(), path, error);
}

// A scene whose background is one flat picture over a colour fill chosen to
// be LOUD against it: any fill leaking through a mid-fade frame — the exact
// way the shipped two-draw fade leaked it — moves the measurement hard.
scene::Scene flatImageScene(const std::string& path, uint32_t fillColor) {
    scene::Scene subject;
    subject.showId = "show1";
    subject.slideId = "blend1";
    subject.background.color = fillColor;
    subject.background.hasImage = true;
    subject.background.assetId = "asset1";
    subject.background.assetPath = path;
    return subject;
}

// Foreground image items through the production draw (task 6.17b, review
// R17). The claim is about pixels in three parts: a picture is stretched into
// the authored box it was given (nothing else decides its size), it draws in
// the stack's ARRAY order so the operator's stacking survives, and a picture
// that is not in the cache draws nothing — the composition shows what is
// under it instead of stalling the frame or inventing a placeholder.
void testForegroundImageItems() {
    std::string error;
    const std::string plate = "scene-render-fg-plate.png";    // flat magenta
    const std::string band = "scene-render-fg-band.png";      // red | green | blue
    const std::string dark = "scene-render-fg-dark.png";      // flat black
    const SkColor magenta = SkColorSetRGB(220, 40, 200);
    if (!check(writeFlatPng(plate, magenta, &error), "foreground plate written: " + error) ||
        !check(writeFlatPng(dark, SkColorSetRGB(0, 0, 0), &error),
               "foreground dark plate written: " + error) ||
        !check(writeBandPng(band, 120, 80, true, &error), "foreground band written: " + error)) {
        return;
    }
    const auto imageItem = [](const std::string& path, double x, double y, double width,
                              double height) {
        scene::SceneItem item;
        item.isImage = true;
        item.assetId = "asset-plate";
        item.assetPath = path;
        item.x = x;
        item.y = y;
        item.width = width;
        item.height = height;
        return item;
    };
    const auto textItem = [](const char* word, double x, double y, double width, double height) {
        scene::SceneItem item;
        item.lines = {word};
        item.color = 0xFFFFFFu;
        item.fontSize = 200.0;
        item.x = x;
        item.y = y;
        item.width = width;
        item.height = height;
        return item;
    };
    const auto sceneWith = [](std::vector<scene::SceneItem> items, uint32_t fill) {
        scene::Scene subject;
        subject.showId = "show1";
        subject.slideId = "fg1";
        subject.background.color = fill;
        subject.items = std::move(items);
        return subject;
    };
    // Pixels inside a rectangle that are near-white: the words, and nothing
    // else in these fixtures is.
    const auto whiteIn = [](const Frame& frame, int left, int top, int right, int bottom) {
        int count = 0;
        for (int y = top; y <= bottom && y < frame.height; ++y) {
            for (int x = left; x <= right && x < frame.width; ++x) {
                const SkColor pixel = frame.at(x, y);
                if (SkColorGetR(pixel) > 200 && SkColorGetG(pixel) > 200 &&
                    SkColorGetB(pixel) > 200) {
                    ++count;
                }
            }
        }
        return count;
    };

    // --- 1. the authored box is where the picture goes ---
    const uint32_t fill = 0x3030E0u;  // blue behind everything
    const scene::Scene placed =
        sceneWith({imageItem(plate, 400.0, 200.0, 480.0, 320.0)}, fill);
    Frame frame;
    if (!check(renderFixture(placed, "fg-placed", &frame, &error),
               "a foreground picture rendered: " + error)) {
        return;
    }
    // Authored (400,200)-(880,520) at the 0.5 output scale is device
    // (200,100)-(440,260).
    check(closeTo(frame.at(320, 180), magenta), "the picture fills the box it was authored in");
    check(closeTo(frame.at(100, 50), SkColorSetRGB(0x30, 0x30, 0xE0)),
          "and the scene's own fill is still the background");
    check(closeTo(frame.at(460, 180), SkColorSetRGB(0x30, 0x30, 0xE0)),
          "nothing spills past the box's right edge");
    check(closeTo(frame.at(200, 280), SkColorSetRGB(0x30, 0x30, 0xE0)),
          "nor below its bottom edge");
    const Ink bounds = measureInk(frame);
    check(bounds.left >= 199 && bounds.right <= 441 && bounds.top >= 99 &&
              bounds.bottom <= 261,
          "the ink of the frame is the box: a foreground picture is positioned and sized "
          "by its authored rectangle and nothing else");

    // --- 2. stretched, not centred at native size and not letterboxed ---
    // The band image is 120x80. Drawn into the WHOLE canvas it must show all
    // three thirds across the full 960 device pixels, uniform top to bottom:
    // a native-size draw would leave 60 device pixels in the middle, and a
    // contain-fit would bar the top and bottom.
    const scene::Scene stretched =
        sceneWith({imageItem(band, 0.0, 0.0, 1920.0, 1080.0)}, fill);
    Frame full;
    if (!check(renderFixture(stretched, "fg-stretch", &full, &error),
               "a stretched foreground picture rendered: " + error)) {
        return;
    }
    check(closeTo(full.at(60, 270), SkColorSetRGB(235, 40, 40)),
          "stretch: the picture's left third reaches the canvas's left edge");
    check(closeTo(full.at(480, 270), SkColorSetRGB(40, 200, 40)),
          "stretch: its middle third lands at the canvas's centre");
    check(closeTo(full.at(900, 270), SkColorSetRGB(40, 40, 235)),
          "and its right third reaches the canvas's right edge");
    check(closeTo(full.at(60, 4), SkColorSetRGB(235, 40, 40)) &&
              closeTo(full.at(60, 535), SkColorSetRGB(235, 40, 40)),
          "and the stretch is vertical too: no letterbox band above or below");

    // --- 3. the stack's array order IS the draw order ---
    const std::vector<double> box{400.0, 200.0, 1120.0, 680.0};  // authored, covers the words
    scene::Scene imageOnTop =
        sceneWith({textItem("COVER", box[0], box[1], box[2], box[3]),
                   imageItem(plate, box[0], box[1], box[2], box[3])},
                  fill);
    Frame over;
    if (!check(renderFixture(imageOnTop, "fg-over-text", &over, &error),
               "a picture stacked over words rendered: " + error)) {
        return;
    }
    // Device box: x 200..760, y 100..440.
    check(closeTo(over.at(480, 270), magenta),
          "the later item of the stack is the one the audience sees");
    check(whiteIn(over, 202, 102, 758, 438) == 0,
          "no glyph of the earlier item leaks through the picture that covers it");

    scene::Scene textOnTop =
        sceneWith({imageItem(plate, box[0], box[1], box[2], box[3]),
                   textItem("COVER", box[0], box[1], box[2], box[3])},
                  fill);
    Frame under;
    if (!check(renderFixture(textOnTop, "fg-under-text", &under, &error),
               "words stacked over a picture rendered: " + error)) {
        return;
    }
    check(whiteIn(under, 202, 102, 758, 438) > 500,
          "the same words in the same box, moved AFTER the picture, draw on top of it");
    check(!closeTo(under.at(480, 270), SkColorSetRGB(0x30, 0x30, 0xE0)),
          "and the picture is still under them: the fill shows nowhere inside the box");

    // --- 4. a picture that cannot be decoded draws NOTHING ---
    // The lookup-only rule (6.10) applied to the foreground: the frame the
    // operator gets is the composition without that item, which is visibly
    // not the slide they composed, rather than a stall or a placeholder.
    scene::Scene missing =
        sceneWith({imageItem("scene-render-fg-no-such-file.png", box[0], box[1], box[2], box[3]),
                   textItem("COVER", box[0], box[1], box[2], box[3])},
                  fill);
    Frame gone;
    if (!check(renderFixture(missing, "fg-missing", &gone, &error),
               "a foreground picture with a missing file rendered: " + error)) {
        return;
    }
    check(whiteIn(gone, 202, 102, 758, 438) > 500,
          "the words over the missing picture still draw");
    // The composition WITHOUT that item: the same slide with its words over
    // the fill and no picture between them. Equality against that frame is
    // the claim — not equality with the slide whose picture drew.
    const scene::Scene textOnly =
        sceneWith({textItem("COVER", box[0], box[1], box[2], box[3])}, fill);
    Frame alone;
    if (!check(renderFixture(textOnly, "fg-text-only", &alone, &error),
               "the same slide with no picture at all rendered: " + error)) {
        return;
    }
    int worst = 0;
    for (int y = 0; y < gone.height; y += 3) {
        for (int x = 0; x < gone.width; x += 3) {
            const SkColor a = gone.at(x, y);
            const SkColor b = alone.at(x, y);
            worst = std::max({worst,
                              std::abs(static_cast<int>(SkColorGetR(a)) -
                                       static_cast<int>(SkColorGetR(b))),
                              std::abs(static_cast<int>(SkColorGetG(a)) -
                                       static_cast<int>(SkColorGetG(b))),
                              std::abs(static_cast<int>(SkColorGetB(a)) -
                                       static_cast<int>(SkColorGetB(b)))});
        }
    }
    std::printf("measured: missing foreground file vs the same slide with no item — worst %d/255\n",
                worst);
    check(worst <= 2,
          "and the frame is the composition WITHOUT the item, to the pixel: a missing "
          "foreground picture is an honest absence, not a black rectangle");
}

// A fading stack dissolves its pictures with its words: the alpha the fade
// carries rides the image paint, with no render-to-texture (R13's rule for
// the whole composition). Measured against a white fill, a black picture at
// half strength is the mid grey the blend says, and the two ends are the
// steady frames.
void testForegroundImageFade() {
    std::string error;
    // Written here as well as in the item fixture: each test owns the files it
    // reads, so running one without the other still measures something.
    if (!check(writeFlatPng("scene-render-fg-dark.png", SkColorSetRGB(0, 0, 0), &error),
               "the fade fixture's black plate is written: " + error)) {
        return;
    }
    scene::Scene plain;
    plain.showId = "show1";
    plain.slideId = "fade-plain";
    plain.background.color = 0xFFFFFFu;
    scene::Scene withPicture;
    withPicture.showId = "show1";
    withPicture.slideId = "fade-picture";
    withPicture.background.color = 0xFFFFFFu;
    withPicture.items.push_back(
        [&] {
            scene::SceneItem item;
            item.isImage = true;
            item.assetId = "asset-dark";
            item.assetPath = "scene-render-fg-dark.png";
            item.x = 400.0;
            item.y = 200.0;
            item.width = 480.0;
            item.height = 320.0;
            return item;
        }());

    Frame mid;
    worker::SnapshotOptions options;
    options.fadeFrom = &plain;
    options.mediaBlend = 1.0f;
    options.textBlend = 0.5f;
    if (!check(renderFixture(withPicture, "fg-fade-mid", options, &mid, &error),
               "a foreground picture mid-fade rendered: " + error)) {
        return;
    }
    options.textBlend = 1.0f;
    Frame on;
    check(renderFixture(withPicture, "fg-fade-on", options, &on, &error),
          "and the same scene at the end of the fade");
    const int grey = SkColorGetG(mid.at(320, 180));
    std::printf("measured: foreground picture fade — half strength g%d, steady g%d, fill g%d\n",
                grey, static_cast<int>(SkColorGetG(on.at(320, 180))),
                static_cast<int>(SkColorGetG(mid.at(100, 50))));
    check(grey > 100 && grey < 160,
          "at half strength the picture is the grey its own alpha says, measured g" +
              std::to_string(grey));
    check(SkColorGetG(on.at(320, 180)) < 40,
          "and at full strength it is the steady picture: the mid frame is a blend, "
          "not a different drawing");
    check(SkColorGetG(mid.at(100, 50)) > 200,
          "the fill outside the box is unaffected by the fading item");
}

/* --- task 6.20d (review R17): the two toggles filter the ONE stack ------ */

// The resolution of the images-layer difference, in pixels: the operator's
// `words` and `images` toggles each control their own kind inside the
// authored item stack — words off leaves the authored pictures standing,
// images off leaves the words standing — and a stack OMITS, it never
// reorders, so hiding the picture reveals the words at exactly the pixels
// they held before, not at pixels a re-stacked layout would have chosen.
void testWordsImagesFilterTheStack() {
    std::string error;
    const std::string plate = "scene-render-kind-toggle.png";  // flat magenta
    const SkColor magenta = SkColorSetRGB(220, 40, 200);
    const SkColor fill = SkColorSetRGB(0x30, 0x30, 0xE0);
    if (!check(writeFlatPng(plate, magenta, &error), "kind-toggle plate written: " + error)) {
        return;
    }
    scene::SceneItem words;
    words.lines = {"STAND"};
    words.color = 0xFFFFFFu;
    words.fontSize = 300.0;
    words.x = 200.0;
    words.y = 250.0;
    words.width = 1520.0;
    words.height = 560.0;
    scene::SceneItem picture;
    picture.isImage = true;
    picture.assetId = "asset-kind";
    picture.assetPath = plate;
    picture.x = 100.0;
    picture.y = 100.0;
    picture.width = 600.0;
    picture.height = 900.0;
    scene::Scene subject;
    subject.showId = "show1";
    subject.slideId = "kind1";
    subject.background.color = 0x3030E0u;
    // Authored order: words first, picture over them. The overlap is the
    // whole question: with both up the picture hides part of the word, and
    // each toggle alone must change ONLY its own kind's pixels.
    subject.items = {words, picture};

    // Device geometry at the 0.5 output rule: the plate covers device
    // (50,50)-(350,500); the glyph block of STAND sits mid-box around
    // device x ~240..715, so its left flank hides behind the plate.
    const auto whiteIn = [](const Frame& frame, int left, int top, int right, int bottom) {
        int count = 0;
        for (int y = top; y <= bottom && y < frame.height; ++y) {
            for (int x = left; x <= right && x < frame.width; ++x) {
                const SkColor pixel = frame.at(x, y);
                if (SkColorGetR(pixel) > 200 && SkColorGetG(pixel) > 200 &&
                    SkColorGetB(pixel) > 200) {
                    ++count;
                }
            }
        }
        return count;
    };

    Frame both;
    if (!check(renderFixture(subject, "kind-both", &both, &error),
               "the both-kinds frame rendered: " + error)) {
        return;
    }
    check(closeTo(both.at(120, 260), magenta),
          "both up: the authored picture draws over the words");
    const int wordsRight = whiteIn(both, 420, 120, 940, 420);
    check(wordsRight > 200, "both up: the words draw where no picture covers them");
    check(whiteIn(both, 60, 120, 345, 420) == 0,
          "both up: no glyph leaks through the picture the operator stacked there");

    worker::SnapshotOptions imagesOff;
    imagesOff.images = false;
    Frame picsGone;
    if (!check(renderFixture(subject, "kind-images-off", imagesOff, &picsGone, &error),
               "the images-off frame rendered: " + error)) {
        return;
    }
    check(!closeTo(picsGone.at(120, 260), magenta),
          "images off: the authored picture is gone from the audience");
    check(closeTo(picsGone.at(120, 260), fill),
          "and where it stood, the scene's own fill shows — omission, not replacement");
    check(whiteIn(picsGone, 60, 120, 345, 420) > 200,
          "images off reveals the words the picture was covering, at the pixels they always "
          "held — the stack filtered by kind does not move what remains");
    check(std::abs(whiteIn(picsGone, 420, 120, 940, 420) - wordsRight) <= 4,
          "and the uncovered half of the word is the same pixels as the both-up frame, glyph "
          "for glyph");

    worker::SnapshotOptions wordsOff;
    wordsOff.words = false;
    Frame textGone;
    if (!check(renderFixture(subject, "kind-words-off", wordsOff, &textGone, &error),
               "the words-off frame rendered: " + error)) {
        return;
    }
    check(closeTo(textGone.at(120, 260), magenta),
          "words off: the authored picture keeps standing — the toggle that hides words "
          "stopped hiding pictures with them");
    check(whiteIn(textGone, 60, 120, 940, 420) == 0, "words off: and not one glyph remains");

    worker::SnapshotOptions stackOff;
    stackOff.words = false;
    stackOff.images = false;
    Frame nothing;
    if (!check(renderFixture(subject, "kind-both-off", stackOff, &nothing, &error),
               "the both-off frame rendered: " + error)) {
        return;
    }
    check(measureInk(nothing).count == 0,
          "either bit alone keeps the stack dispatched; both down is the only way off");
}

// The rule the acceptance names, stated without reference to how anything is
// drawn: compositing a background at blend `t` means the group — that scene's
// fill with its picture OVER it — at that alpha. Every steady frame below is
// already the group at full strength (the fill is opaque, so a steady frame
// is `picture over fill` and nothing else), so the frame a fade owes at
// blend t is the per-pixel lerp of the two steady frames. Nothing in this
// expectation shares code with the path under test.
SkColor blendExpected(SkColor from, SkColor to, uint8_t alpha) {
    const auto channel = [alpha](int a, int b) {
        return static_cast<uint8_t>((a * (255 - alpha) + b * alpha + 127) / 255);
    };
    return SkColorSetARGB(0xFF, channel(SkColorGetR(from), SkColorGetR(to)),
                          channel(SkColorGetG(from), SkColorGetG(to)),
                          channel(SkColorGetB(from), SkColorGetB(to)));
}

// One measured fade stage: the two scenes' steady background frames (no
// words, so the background is all there is) and the stage itself, all three
// through the production snapshot and the same PNG round trip.
struct BlendCase {
    Frame from;
    Frame to;
    Frame mid;
    uint8_t alpha = 0;
};

bool renderBlendCase(const scene::Scene& from, const scene::Scene& to, float mediaBlend,
                     const std::string& name, BlendCase* out, std::string* error) {
    out->alpha = static_cast<uint8_t>(mediaBlend * 255.0f + 0.5f);
    worker::SnapshotOptions steady;
    steady.words = false;
    if (!check(renderFixture(from, "blend-" + name + "-from", steady, &out->from, error),
               "steady outgoing rendered: " + *error) ||
        !check(renderFixture(to, "blend-" + name + "-to", steady, &out->to, error),
               "steady incoming rendered: " + *error)) {
        return false;
    }
    worker::SnapshotOptions stage;
    stage.words = false;
    stage.fadeFrom = &from;
    stage.mediaBlend = mediaBlend;
    return check(renderFixture(to, "blend-" + name + "-mid", stage, &out->mid, error),
                 "fade stage rendered: " + *error);
}

// Worst channel deviation from the expectation, over every pixel: the count
// over tolerance is what fails a fixture, and the worst is what the ledger
// records (R13's numbers were measured this way, so a regression that puts
// the two-draw leak back is 22 to 46 units out and cannot hide inside a
// colour-management allowance).
struct BlendError {
    int worst = 0;
    int overTolerance = 0;
    int worstX = 0, worstY = 0;
    SkColor want = SK_ColorBLACK, got = SK_ColorBLACK;
};

BlendError measureBlend(const BlendCase& c, int tolerance) {
    BlendError e;
    for (int y = 0; y < c.mid.height; ++y) {
        for (int x = 0; x < c.mid.width; ++x) {
            const SkColor want = blendExpected(c.from.at(x, y), c.to.at(x, y), c.alpha);
            const SkColor got = c.mid.at(x, y);
            const int dev = std::max(
                std::abs(static_cast<int>(SkColorGetR(got)) - static_cast<int>(SkColorGetR(want))),
                std::max(std::abs(static_cast<int>(SkColorGetG(got)) -
                                  static_cast<int>(SkColorGetG(want))),
                         std::abs(static_cast<int>(SkColorGetB(got)) -
                                  static_cast<int>(SkColorGetB(want)))));
            if (dev > e.worst) {
                e.worst = dev;
                e.worstX = x;
                e.worstY = y;
                e.want = want;
                e.got = got;
            }
            if (dev > tolerance) {
                ++e.overTolerance;
            }
        }
    }
    return e;
}

std::string describe(const BlendError& e) {
    return std::to_string(e.overTolerance) + " px over tolerance, worst " +
           std::to_string(e.worst) + "/255 at (" + std::to_string(e.worstX) + "," +
           std::to_string(e.worstY) + "), got " + std::to_string(SkColorGetR(e.got)) + "," +
           std::to_string(SkColorGetG(e.got)) + "," + std::to_string(SkColorGetB(e.got)) +
           " want " + std::to_string(SkColorGetR(e.want)) + "," +
           std::to_string(SkColorGetG(e.want)) + "," + std::to_string(SkColorGetB(e.want));
}

// Every blend deviation this task's fixtures measure goes on the record,
// passing or not: the numbers in PLAN.md task 6.11b come from here, so a
// regression that quietly re-introduces the two-draw leak is visible in the
// log as well as in the failure.
void report(const std::string& what, const BlendError& e) {
    std::printf("measured: %-42s worst %3d/255 (%d px over tolerance)\n", what.c_str(), e.worst,
                e.overTolerance);
}

void testImageCrossfadeBlend() {
    // R13's complaint in one line: the shipped fade attenuated the incoming
    // scene's colour FILL and its picture separately, so the fill was counted
    // again under an opaque picture and the audience's own picture was
    // attenuated twice. Measured on that code, an opaque still over a loud
    // fill sat 46/255 away from the lerp — and a scene fading to ITSELF, the
    // lyric change over one still that happens at every service, landed 46
    // units off the frame the audience was already looking at.
    std::string error;
    if (!check(writeFlatPng("scene-render-flat-red.png", SkColorSetRGB(224, 48, 48), &error),
               "red flat still written: " + error) ||
        !check(writeFlatPng("scene-render-flat-blue.png", SkColorSetRGB(48, 48, 224), &error),
               "blue flat still written: " + error)) {
        return;
    }
    const uint32_t loudFill = 0x28C828u;  // green: 200 in the channel the blues share
    const scene::Scene redStill = flatImageScene("scene-render-flat-red.png", loudFill);
    const scene::Scene blueStill = flatImageScene("scene-render-flat-blue.png", loudFill);

    // 1. Opaque picture to opaque picture, both blends the shipped code was
    //    furthest out at. Tolerance 3: these are flat colours through one
    //    lossless PNG round trip, so the only room is 8-bit rounding.
    BlendCase half;
    if (renderBlendCase(redStill, blueStill, 0.5f, "opaque", &half, &error)) {
        const BlendError e = measureBlend(half, 3);
        report("opaque stills, blend 0.5", e);
        check(e.overTolerance == 0,
              "opaque image to image at 0.5 is the lerp of the two steady frames (" +
                  describe(e) + ")");
        // The contamination has a signature: the fill carries 200 of green
        // and the two stills carry 48, so a leaking fill lifts the frame's
        // green channel by tens. Say it out loud, because "the midpoint is
        // wrong" is a claim a future colour-management allowance could eat.
        const int greenLeak = static_cast<int>(SkColorGetG(half.mid.at(480, 270))) -
                              static_cast<int>(SkColorGetG(half.from.at(480, 270)) +
                                               SkColorGetG(half.to.at(480, 270))) /
                                  2;
        check(std::abs(greenLeak) <= 3,
              "the incoming scene's colour fill contributes nothing under an opaque picture (" +
                  std::to_string(greenLeak) + ")");
    }

    BlendCase quarter;
    if (renderBlendCase(redStill, blueStill, 0.25f, "quarter", &quarter, &error)) {
        const BlendError e = measureBlend(quarter, 3);
        report("opaque stills, blend 0.25", e);
        check(e.overTolerance == 0, "opaque image to image at 0.25 is the lerp (" + describe(e) + ")");
    }

    // 2. The same still on both sides — a lyric change over one background.
    //    The mid-fade frame must be the steady frame: R13's "even the same
    //    image can dim during a lyric change", and the case the solid-colour
    //    fixtures could not see because both ends of the lerp were the same
    //    colour only when the fill matched the picture.
    BlendCase itself;
    if (renderBlendCase(blueStill, blueStill, 0.5f, "itself", &itself, &error)) {
        const BlendError e = measureBlend(itself, 3);
        report("one still fading to itself, blend 0.5", e);
        check(e.overTolerance == 0,
              "a scene fading to itself never leaves the steady frame (" + describe(e) + ")");
        check(closeTo(itself.mid.at(480, 270), SkColorSetRGB(48, 48, 224), 8),
              "and that frame is the picture, not a dimmed or fill-mixed version of it");
    }

    // 3. Geometry: the group must map the picture exactly where the steady
    //    path does, or the composite is right in colour and wrong on screen.
    //    The outgoing side is banded, the incoming side is the portrait crop
    //    (uniformly the middle band), so the lerp keeps the banded side's
    //    transition columns — a group with a wrong mapping moves them.
    if (!check(writeBandPng("scene-render-bg-wide.png", 1920, 540, true, &error),
               "wide band image written: " + error) ||
        !check(writeBandPng("scene-render-bg-tall.png", 540, 1920, false, &error),
               "tall band image written: " + error)) {
        return;
    }
    BlendCase bands;
    if (renderBlendCase(bandImageScene("scene-render-bg-wide.png", {}),
                        bandImageScene("scene-render-bg-tall.png", {}), 0.5f, "bands", &bands,
                        &error)) {
        const BlendError e = measureBlend(bands, 4);
        report("cover-cropped bands, blend 0.5", e);
        check(e.overTolerance == 0,
              "the group maps a cover-cropped picture where the steady path puts it (" +
                  describe(e) + ")");
        // Non-vacuity: the expectation is only interesting if the banded
        // side really does change across the canvas — with one flat colour
        // the lerp is trivially right and the mapping is never tested.
        int transitions = 0;
        for (int x = 1; x < bands.from.width; ++x) {
            const SkColor a = bands.from.at(x, 270);
            const SkColor b = bands.from.at(x - 1, 270);
            const int d =
                std::max(std::abs(static_cast<int>(SkColorGetR(a)) - static_cast<int>(SkColorGetR(b))),
                         std::max(std::abs(static_cast<int>(SkColorGetG(a)) -
                                           static_cast<int>(SkColorGetG(b))),
                                  std::abs(static_cast<int>(SkColorGetB(a)) -
                                           static_cast<int>(SkColorGetB(b)))));
            if (d > 60) {
                ++transitions;
            }
        }
        check(transitions >= 2,
              "the banded side still has its two transitions at the mid stage (a flat frame "
              "would make the lerp check vacuous)");
    }
}

// The fade group path (R13's single-draw composite) must carry the scene's
// fit too. The claim, stated so it cannot be vacuous: at any blend the frame
// owes the per-pixel lerp of the two STEADY frames — the contained one
// included. If the group quietly reverted to cover mid-fade, the bars that
// are fill in the steady contain frame would be PICTURE during the fade, and
// every bar pixel would fail the lerp by tens of units.
void testBackgroundFitContainFade() {
    std::string error;
    if (!check(writeFlatPng("scene-render-fit-fade.png", SkColorSetRGB(224, 48, 48), &error),
               "flat still for the contained fade written: " + error)) {
        return;
    }
    const scene::Scene redFill = [] {
        scene::Scene subject;
        subject.showId = "show1";
        subject.slideId = "fit-fade-from";
        subject.background.color = 0xE03030u;
        return subject;
    }();
    scene::Scene containStill = flatImageScene("scene-render-fit-fade.png", 0x28C828u);
    containStill.slideId = "fit-fade-to";
    containStill.background.fit = openshow::domain::BackgroundFit::Contain;
    BlendCase half;
    if (!renderBlendCase(redFill, containStill, 0.5f, "contain", &half, &error)) {
        return;
    }
    const BlendError e = measureBlend(half, 6);
    report("fill to contained still, blend 0.5", e);
    check(e.overTolerance == 0,
          "a contained picture fades as the group it steady-draws — bars stay fill "
          "through the whole blend (" +
              describe(e) + ")");
    // And the geometry survives the shader: a 120x80 contain on the 1920x1080
    // canvas scales to 1620x1080, so the device bar columns sit left of x 75
    // and right of x 885 — sample one of each against the lerp of the steady
    // frames' own bars, and the centre against the steady picture.
    check(closeTo(half.mid.at(2, 270),
                  blendExpected(half.from.at(2, 270), half.to.at(2, 270), half.alpha), 6),
          "mid-fade: the bar column is the lerped FILL, not the cover crop's picture");
    check(closeTo(half.mid.at(955, 270),
                  blendExpected(half.from.at(955, 270), half.to.at(955, 270), half.alpha), 6),
          "mid-fade: and so is the other bar");
    check(closeTo(half.mid.at(480, 270),
                  blendExpected(half.from.at(480, 270), half.to.at(480, 270), half.alpha), 6),
          "mid-fade: the centre column is the lerped PICTURE");
}

/* --- task 6.17c2 (review R17): the blur fit — a blurred COVER backdrop of
 *     the picture at the reference's 0.3 under the CONTAIN-fitted picture,
 *     built once per image, faded as the group it steady-draws ------------ */

// The reference's constants (docs/reference.md: "Media fit ... blur (blur
// amount 6, opacity 0.3)"), mirrored in SceneText.cpp's makeBlurredBackdrop
// — the fixture computes its expectation from the arithmetic, so a drift in
// either shows as a failure rather than a shared misreading.
constexpr std::uint8_t kTestBackdropAlpha = 77;  // round(0.3 * 255)

SkColor blendOver(SkColor dst, SkColor src, std::uint8_t srcAlpha) {
    const auto channel = [srcAlpha](int d, int s) {
        return static_cast<uint8_t>((d * (255 - srcAlpha) + s * srcAlpha + 127) / 255);
    };
    return SkColorSetRGB(channel(SkColorGetR(dst), SkColorGetR(src)),
                         channel(SkColorGetG(dst), SkColorGetG(src)),
                         channel(SkColorGetB(dst), SkColorGetB(src)));
}

// A flat, WIDE (1920x540) picture: contain leaves 270-canvas-px bars, and
// every backdrop pixel is one known colour, so the 0.3 composite is checkable
// as arithmetic rather than as "blurrier than before". The TALL variant is
// the same claim rotated: its bars are the sides, and on a portrait window
// they reach the outside-canvas letterbox the clip must keep black.
bool writeFlatRectPng(const std::string& path, int width, int height, SkColor color,
                      std::string* error) {
    auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));
    if (!surface) {
        if (error) {
            *error = "flat picture surface failed";
        }
        return false;
    }
    SkPaint paint;
    paint.setColor(color);
    surface->getCanvas()->drawRect(SkRect::MakeIWH(width, height), paint);
    return worker::writePngRaster(surface.get(), path, error);
}

sk_sp<SkImage> decodeFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
    auto codec = SkCodec::MakeFromData(SkData::MakeWithCopy(bytes.data(), bytes.size()));
    if (!codec) {
        return nullptr;
    }
    const SkImageInfo info =
        codec->getInfo().makeColorType(kN32_SkColorType).makeAlphaType(kPremul_SkAlphaType);
    SkBitmap bitmap;
    if (!bitmap.tryAllocPixels(info) ||
        codec->getPixels(info, bitmap.getPixels(), bitmap.rowBytes()) != SkCodec::kSuccess) {
        return nullptr;
    }
    return SkImages::RasterFromBitmap(bitmap);
}

// The backdrop by the book: the same construction as the worker's
// makeBlurredBackdrop (cover dest with the 24 px overscan, sigma 6 in canvas
// space, alpha baked at 77) — duplicated HERE on purpose: a measurement of
// the rule that borrows the production code cannot catch the rule misread.
sk_sp<SkImage> referenceBackdrop(const SkImage& source, float denominator = 1.0f,
                                 int* buildMs = nullptr) {
    const auto start = std::chrono::steady_clock::now();
    const int w = std::max(1, static_cast<int>(worker::kSceneCanvasWidth / denominator));
    const int h = std::max(1, static_cast<int>(worker::kSceneCanvasHeight / denominator));
    auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(w, h));
    if (!surface) {
        return nullptr;
    }
    const float scale =
        std::max(worker::kSceneCanvasWidth / static_cast<float>(source.width()),
                 worker::kSceneCanvasHeight / static_cast<float>(source.height()));
    const float dw = static_cast<float>(source.width()) * scale / denominator;
    const float dh = static_cast<float>(source.height()) * scale / denominator;
    SkRect dest = SkRect::MakeLTRB((w - dw) * 0.5f, (h - dh) * 0.5f, (w + dw) * 0.5f,
                                   (h + dh) * 0.5f);
    dest.outset(24.0f / denominator, 24.0f / denominator);
    SkCanvas* canvas = surface->getCanvas();
    canvas->clear(SK_ColorTRANSPARENT);
    SkPaint paint;
    paint.setAlpha(77);
    paint.setImageFilter(SkImageFilters::Blur(6.0f / denominator, 6.0f / denominator, nullptr));
    canvas->drawImageRect(sk_ref_sp(&source),
                          SkRect::MakeIWH(source.width(), source.height()), dest,
                          SkSamplingOptions(SkFilterMode::kLinear), &paint,
                          SkCanvas::kFast_SrcRectConstraint);
    if (buildMs != nullptr) {
        *buildMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - start)
                                        .count());
    }
    return surface->makeImageSnapshot();
}

// Worst channel deviation between two backdrops, compared the way they are
// DRAWN: composited over the scene's fill at canvas size, so a smaller
// approximation is upsampled by the same draw that the audience's frame
// would use, and the comparison is of opaque pixels rather than of alpha
// hairlines. (Comparing raw extents instead is what reports 235/255 for a
// picture that is one flat colour: mismatched readPixels extents are an
// API failure, not a measurement.)
int worstChannelDifference(const SkImage& full, const SkImage& approx, SkColor fill) {
    const auto flatten = [fill](const SkImage& image) {
        auto surface = SkSurfaces::Raster(
            SkImageInfo::MakeN32Premul(static_cast<int>(worker::kSceneCanvasWidth),
                                       static_cast<int>(worker::kSceneCanvasHeight)));
        if (!surface) {
            return SkBitmap{};
        }
        SkCanvas* canvas = surface->getCanvas();
        canvas->clear(fill);
        canvas->drawImageRect(sk_ref_sp(&image),
                              SkRect::MakeIWH(image.width(), image.height()),
                              SkRect::MakeWH(worker::kSceneCanvasWidth,
                                             worker::kSceneCanvasHeight),
                              SkSamplingOptions(SkFilterMode::kLinear), nullptr,
                              SkCanvas::kFast_SrcRectConstraint);
        SkBitmap bitmap;
        if (!bitmap.tryAllocPixels(SkImageInfo::MakeN32Premul(surface->width(),
                                                              surface->height())) ||
            !surface->readPixels(SkImageInfo::MakeN32Premul(surface->width(),
                                                            surface->height()),
                                 bitmap.getPixels(), bitmap.rowBytes(), 0, 0)) {
            return SkBitmap{};
        }
        return bitmap;
    };
    const SkBitmap a = flatten(full), b = flatten(approx);
    if (a.empty() || b.empty()) {
        return 255;
    }
    int worst = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            const SkColor ca = a.getColor(x, y), cb = b.getColor(x, y);
            for (int ch = 0; ch < 3; ++ch) {
                const int va = (ca >> (16 - 8 * ch)) & 0xFF, vb = (cb >> (16 - 8 * ch)) & 0xFF;
                worst = std::max(worst, std::abs(va - vb));
            }
        }
    }
    return worst;
}

void testBackgroundFitBlurSteady() {
    std::string error;
    if (!check(writeFlatRectPng("scene-render-blur-wide.png", 1920, 540,
                                SkColorSetRGB(224, 48, 48), &error) &&
                   writeFlatRectPng("scene-render-blur-tall.png", 540, 1920,
                                    SkColorSetRGB(224, 48, 48), &error) &&
                   writeBandPng("scene-render-blur-bands.png", 1920, 540, true, &error),
               "flat blur pictures written: " + error)) {
        return;
    }
    const SkColor picture = SkColorSetRGB(224, 48, 48);
    const SkColor fill = SkColorSetRGB(0x10, 0x20, 0x30);
    const SkColor expectedBar = blendOver(fill, picture, kTestBackdropAlpha);
    // The composite must MOVE the fill hard or this fixture proves nothing.
    check(differs(expectedBar, fill, 40),
          "a red picture at 0.3 over the dark fill is visibly not the fill");

    scene::Scene blur = flatImageScene("scene-render-blur-wide.png", 0x102030u);
    blur.slideId = "blur-steady";
    blur.background.fit = openshow::domain::BackgroundFit::Blur;
    Frame frame;
    if (!check(renderFixture(blur, "blur-wide-steady", &frame, &error),
               "blurred-backdrop background rendered: " + error)) {
        return;
    }
    // The flat picture over a flat fill blurs to one flat colour, so the
    // bars are arithmetic: cover-fit red at alpha 77 over the fill.
    check(closeTo(frame.at(480, 2), expectedBar, 12),
          "blur: the top bar is the fill under the 0.3 backdrop");
    check(closeTo(frame.at(480, 130), expectedBar, 12), "blur: the bar runs to its edge");
    check(closeTo(frame.at(480, 536), expectedBar, 12), "blur: the bottom bar is the backdrop");
    // ...and the contained picture stays EXACTLY the picture — the blur is
    // behind it, not over it.
    check(closeTo(frame.at(480, 270), picture, 8),
          "blur: the centre is the sharp picture, not the blur");
    check(closeTo(frame.at(50, 270), picture, 8), "blur: and so is its left edge");
    check(differs(frame.at(480, 100), frame.at(480, 270), 24),
          "blur: bar and picture are visibly different regions");

    // And the backdrop really is BLURRED, not just attenuated: a banded
    // picture cover-fits to 2x with its red|green boundary landing at device
    // x 156 (canvas 313), so a sharp backdrop would paint that pixel — and
    // its bilinear half-pixel either side — as (nearly) pure band colour,
    // while the reference's blur mixes the boundary across a dozen canvas
    // pixels. The sampled pixel must be far from BOTH pure composites.
    scene::Scene bands = flatImageScene("scene-render-blur-bands.png", 0x102030u);
    bands.slideId = "blur-bands";
    bands.background.fit = openshow::domain::BackgroundFit::Blur;
    Frame bandFrame;
    if (!check(renderFixture(bands, "blur-bands-steady", &bandFrame, &error),
               "banded blurred-backdrop background rendered: " + error)) {
        return;
    }
    const SkColor sharpRed = blendOver(fill, SkColorSetRGB(235, 40, 40), kTestBackdropAlpha);
    const SkColor sharpGreen = blendOver(fill, SkColorSetRGB(40, 200, 40), kTestBackdropAlpha);
    check(differs(bandFrame.at(156, 60), sharpRed, 16) &&
              differs(bandFrame.at(156, 60), sharpGreen, 16),
          "blur: the bar at the picture's band boundary is neither pure band — the sigma is real");

    // A TALL picture on a portrait OUTPUT window: the side bars inside the
    // authored canvas carry the blurred backdrop, and the letterbox outside
    // the canvas stays the surface's black — the blur must not smear past
    // the clip the cover fit was clipped to. A 540x960 snapshot makes the
    // window's aspect differ from the authored canvas', which is when
    // outside-canvas pixels exist at all.
    scene::Scene tall = flatImageScene("scene-render-blur-tall.png", 0x102030u);
    tall.slideId = "blur-tall";
    tall.background.fit = openshow::domain::BackgroundFit::Blur;
    worker::SnapshotOptions portrait;
    portrait.words = false;
    Frame portraitFrame;
    const std::string portraitPath = "scene-render-blur-tall-portrait.png";
    if (!check(worker::renderSceneSnapshot(tall, portraitPath, 540, 960, portrait, &error) &&
                   loadPng(portraitPath, &portraitFrame, &error),
               "blurred tall background on a portrait window rendered: " + error)) {
        return;
    }
    // scale 0.28: the canvas occupies device y 328..632; its left column at
    // the tall picture's contained-bar region samples the backdrop's
    // red-dominated left edge — the same flat composite as the wide bars.
    check(closeTo(portraitFrame.at(2, 480), expectedBar, 12),
          "blur portrait: the in-canvas side bar is the fill under the 0.3 backdrop");
    check(closeTo(portraitFrame.at(2, 2), SK_ColorBLACK, 4),
          "blur portrait: outside the authored canvas stays black — the clip");
    check(closeTo(portraitFrame.at(536, 956), SK_ColorBLACK, 4),
          "blur portrait: and so is the far corner");

    // An uncached picture cannot blur: blur degrades to exactly the contain
    // draw, bars and all — the same cold-file behaviour R12 pinned, and it
    // never touches the disk from the draw.
    scene::Scene cold = blur;
    cold.background.assetPath = "scene-render-blur-no-such-file.png";
    Frame coldFrame;
    if (!check(renderFixture(cold, "blur-cold", &coldFrame, &error),
               "blur with an unreadable file rendered: " + error)) {
        return;
    }
    check(closeTo(coldFrame.at(480, 2), fill),
          "blur degrades to contain when the picture cannot be shown: plain fill bars");
}

void testBackgroundFitBlurCache() {
    // The once-per-image rule R12's budget made a requirement: the first
    // blurred draw pays the build, every later one takes a map probe, and
    // the backdrop's bytes join the budget they cannot escape.
    std::string error;
    if (!check(writeBandPng("scene-render-blur-cache.png", 1920, 540, true, &error),
               "blur cache fixture written: " + error)) {
        return;
    }
    const std::string path = "scene-render-blur-cache.png";
    worker::SceneImageCache cache;
    check(cache.loadFor(path) != nullptr, "the blur cache decoded its picture");
    const std::size_t afterImage = cache.stats().decodedBytes;
    check(cache.stats().backdropComputations == 0 && cache.stats().backdrops == 0,
          "nothing blurs until a blurred draw asks");

    const sk_sp<SkImage> first = cache.backdropFor(path);
    check(first != nullptr && first->width() == 1920 && first->height() == 1080,
          "the backdrop is the authored canvas, cover-filled");
    check(cache.stats().backdropComputations == 1, "the first blurred draw builds it once");
    (void)cache.backdropFor(path);
    (void)cache.backdropFor(path);
    check(cache.stats().backdropComputations == 1,
          "later blurred draws take the memo, not the blur");
    check(cache.stats().backdrops == 1, "the held-backdrop count agrees");
    check(cache.stats().decodedBytes == afterImage + 1920u * 1080u * 4u,
          "the backdrop's bytes joined the budget — the promise now covers it");

    cache.invalidate(path);
    check(cache.stats().decodedBytes == 0,
          "invalidation takes the backdrop's bytes with the image's");
    check(cache.backdropFor(path) == nullptr,
          "a dropped image has no backdrop to remember — it is not re-decoded here");

    // A replaced image drops its derived backdrop too (the install path's
    // half of the same accounting rule): the new entry's bytes are its own,
    // not the old picture's plus an orphaned blur.
    const sk_sp<SkImage> original = cache.loadFor(path);
    check(original != nullptr && cache.backdropFor(path) != nullptr,
          "the replacement stage loads a picture and blurs it once");
    const std::size_t withBackdrop = cache.stats().decodedBytes;
    const sk_sp<SkImage> smaller = decodeFile("scene-render-blur-tall.png");
    check(smaller != nullptr, "the replacement picture decoded");
    if (smaller != nullptr) {
        cache.install(path, smaller);
        const std::size_t replaced = cache.stats().decodedBytes;
        check(replaced == static_cast<std::size_t>(smaller->width()) *
                              static_cast<std::size_t>(smaller->height()) * 4u,
              "installing over a blurred path counts ONLY the new picture — the "
              "old backdrop's bytes left with it");
        check(cache.stats().backdrops == 0, "and the stale backdrop is gone, not inherited");
    }

    // Budget eviction carries a backdrop's bytes with its image (the third
    // half of the same accounting): a small cache whose budget fits exactly
    // one picture plus one backdrop must land on the new picture's bytes
    // alone after the blurred older entry is evicted — not on a total still
    // holding what the cache itself forgot.
    worker::SceneImageCache tight(540u * 1920u * 4u + 1920u * 1080u * 4u);
    check(tight.loadFor("scene-render-blur-wide.png") != nullptr &&
              tight.backdropFor("scene-render-blur-wide.png") != nullptr,
          "the tight cache took a picture and its backdrop");
    check(tight.loadFor("scene-render-blur-tall.png") != nullptr,
          "and loaded the picture that forces eviction");
    check(tight.stats().evictions == 1, "one entry went");
    check(tight.stats().decodedBytes == 540u * 1920u * 4u,
          "and the budget total forgot its backdrop with it — no orphaned bytes");
    check(tight.stats().backdrops == 0, "the evicted backdrop is gone from the count too");

    // The downscale-first approximation versus the by-the-book build,
    // measured so the ledger records a decision, not a hunch.
    const sk_sp<SkImage> source = decodeFile(path);
    check(source != nullptr, "the approximation measure decoded its fixture");
    if (source == nullptr) {
        return;
    }
    int fullMs = 0, approxMs = 0;
    const sk_sp<SkImage> full = referenceBackdrop(*source, 1.0f, &fullMs);
    const sk_sp<SkImage> half = referenceBackdrop(*source, 2.0f, &approxMs);
    check(full != nullptr && half != nullptr, "both backdrop variants built");
    if (full == nullptr || half == nullptr) {
        return;
    }
    const int worst =
        worstChannelDifference(*full, *half, SkColorSetRGB(0x10, 0x20, 0x30));
    std::printf("measured: blur backdrop build 1920x1080 %d ms; half-size approximation %d ms; "
                "worst channel difference %d/255\n",
                fullMs, approxMs, worst);
    check(worst <= 64,
          "the half-size backdrop approximates the full one within 64/255 (measured worst: " +
              std::to_string(worst) + ") — recorded for the ledger");
}

void testBackgroundFitBlurFade() {
    // The blur background is ONE group — backdrop and picture inside it —
    // so a fade of it is the same per-pixel lerp every other background
    // obeys (R13's rule, which the blurred group is no exception to). The
    // bars mid-fade carry the lerped fill-plus-backdrop, not the fill
    // alone: the 0.3 blurred cover is part of what the scene IS.
    std::string error;
    // The outgoing frame's solid colour is deliberately FAR from the
    // incoming picture's red: if it matched, the dest-rect check would pass
    // for a fade shader that forgot the backdrop underneath (the picture's
    // own alpha-over-something lands on the same number). Blue keeps the
    // lerp honest on every channel.
    const scene::Scene blueFill = [] {
        scene::Scene subject;
        subject.showId = "show1";
        subject.slideId = "blur-fade-from";
        subject.background.color = 0x2030E0u;
        return subject;
    }();
    scene::Scene blurStill = flatImageScene("scene-render-blur-wide.png", 0x102030u);
    blurStill.slideId = "blur-fade-to";
    blurStill.background.fit = openshow::domain::BackgroundFit::Blur;
    BlendCase half;
    if (!renderBlendCase(blueFill, blurStill, 0.5f, "blur", &half, &error)) {
        return;
    }
    const BlendError e = measureBlend(half, 6);
    report("fill to blurred-backdrop still, blend 0.5", e);
    check(e.overTolerance == 0,
          "a blurred background fades as the group it steady-draws (" + describe(e) + ")");
    // And the mid frame really is the BLURRED group: its bar is the
    // lerped backdrop-over-fill composite, measurably off the plain
    // fill-to-fill lerp a degraded contain draw would produce.
    const SkColor barWanted =
        blendExpected(half.from.at(480, 60), half.to.at(480, 60), half.alpha);
    const SkColor degraded =
        blendExpected(half.from.at(480, 60), SkColorSetRGB(0x10, 0x20, 0x30), half.alpha);
    check(closeTo(half.mid.at(480, 60), barWanted, 6) && differs(barWanted, degraded, 8),
          "mid-fade: the bar is the lerped backdrop, not a contain frame's plain fill");
    // The self-fade — the same blurred slide replacing itself across a
    // lyric change — must not move a single pixel beyond rounding.
    BlendCase itself;
    if (renderBlendCase(blurStill, blurStill, 0.5f, "blur-itself", &itself, &error)) {
        const BlendError e2 = measureBlend(itself, 6);
        report("blurred still to itself, blend 0.5", e2);
        check(e2.overTolerance == 0,
              "a blurred slide fading to itself is the frame it already showed (" +
                  describe(e2) + ")");
    }
    // The dest rect's NESTING — `picture over (backdrop over fill)` — is
    // invisible through an opaque picture (SrcOver with an opaque src
    // ignores its dst), so the promise needs R13's translucent fixture:
    // through a picture at alpha 128 the backdrop shows ~50%, and a fade
    // shader that forgot it under the picture reads the FILL there
    // instead. At blend 0.75 the mid frame rides the group at 0.75, so
    // the missing backdrop lands ~12/255 on the covered pixels (the
    // difference peaks at 7.8 at blend 0.5 — which is why the opaque
    // fixtures above cannot see this case at any blend).
    if (!check(writeFlatPng("scene-render-blur-alpha.png", SkColorSetRGB(224, 48, 48),
                            &error, 128),
               "translucent blur still written: " + error)) {
        return;
    }
    scene::Scene blurAlpha = flatImageScene("scene-render-blur-alpha.png", 0x102030u);
    blurAlpha.slideId = "blur-fade-alpha";
    blurAlpha.background.fit = openshow::domain::BackgroundFit::Blur;
    BlendCase alphaHalf;
    if (renderBlendCase(blueFill, blurAlpha, 0.75f, "blur-alpha", &alphaHalf, &error)) {
        const BlendError e3 = measureBlend(alphaHalf, 5);
        report("blue fill to translucent blurred still, blend 0.75", e3);
        check(e3.overTolerance == 0,
              "a translucent blurred picture fades over the backdrop that is really"
              " under it — not over the bare fill (" + describe(e3) + ")");
        // And the steady incoming frame proves the fixture is the
        // translucent case: the fill and backdrop show through the
        // picture (an opaque one would erase them).
        check(differs(alphaHalf.to.at(480, 270), SkColorSetRGB(224, 48, 48), 8),
              "the translucent blurred still really does let its stack through");
    }
}

/* --- task 6.17c3 (review R17): the background GRADIENT — the style's second
   colour, painting the canvas from the first at the top to the second at the
   bottom. Every fill is now a shader, so the fixtures measure the ramp where
   each fill site draws it: the bare canvas, the contain letterbox, the blur
   backdrop's bars, and a fade that must carry it as one group. --- */

// The ramp by arithmetic, at a device row: with the 0.5 output scale the
// pixel centre of device row y sits at authored y = 2y + 1, so the ramp's
// t there is (2y+1)/1080 and its value is the BYTE-wise lerp — the ramp is
// interpolated in gamma-encoded sRGB (Skia Interpolation::kSRGB), which is
// what a browser does with the same two colours and is what pins the
// midpoint to an exact number below.
SkColor gradientAt(SkColor top, SkColor bottom, int deviceY) {
    const int t = 2 * deviceY + 1;
    const auto channel = [t](int a, int b) {
        return static_cast<uint8_t>((a * (1080 - t) + b * t + 540) / 1080);
    };
    return SkColorSetARGB(0xFF, channel(SkColorGetR(top), SkColorGetR(bottom)),
                          channel(SkColorGetG(top), SkColorGetG(bottom)),
                          channel(SkColorGetB(top), SkColorGetB(bottom)));
}

void testBackgroundGradientFill() {
    // A gradient with no picture: the whole canvas is the ramp. Endpoints,
    // the exact encoded midpoint, one-colour rows and a monotonic column —
    // plus the flat regression: the same scene with the second colour
    // taken away is one colour again, edge to edge.
    std::string error;
    scene::Scene graded;
    graded.showId = "show1";
    graded.slideId = "grad-fill";
    graded.background.color = 0x102030u;
    graded.background.gradient = 0xE09040u;
    graded.background.hasGradient = true;
    Frame frame;
    if (!check(renderFixture(graded, "gradient-fill", &frame, &error),
               "graded background rendered: " + error)) {
        return;
    }
    const SkColor top = SkColorSetRGB(0x10, 0x20, 0x30);
    const SkColor bottom = SkColorSetRGB(0xE0, 0x90, 0x40);
    check(closeTo(frame.at(480, 1), gradientAt(top, bottom, 1), 2),
          "gradient: the first device row is the top colour");
    check(closeTo(frame.at(480, 538), gradientAt(top, bottom, 538), 2),
          "gradient: the last device row is the bottom colour");
    // The midpoint spelled out in bytes, because "the shader interpolated
    // something" is not the assertion — GAMMA-ENCODED sRGB is. A linear
    // ramp would put the same pair's midpoint near 161 in the red channel;
    // the encoded mean is 120. 41 units of daylight separate the two
    // promises, so this line survives no future colour-management drift.
    check(closeTo(frame.at(480, 270), SkColorSetRGB(0x78, 0x58, 0x38), 2),
          "gradient: mid-canvas is the BYTE mean of the pair — the browser's ramp");
    check(!differs(frame.at(2, 270), frame.at(958, 270)),
          "gradient: one row is one colour — the axis is vertical");
    const int rHigh = SkColorGetR(frame.at(480, 100));
    const int rMid = SkColorGetR(frame.at(480, 270));
    const int rLow = SkColorGetR(frame.at(480, 440));
    check(rHigh < rMid && rMid < rLow,
          "gradient: the ramp runs top-to-bottom (the bottom colour is the second"
          " one), not the other way");
    // Flat regression: strip the flag and the pre-6.17c3 draw returns.
    scene::Scene flat = graded;
    flat.background.hasGradient = false;
    flat.slideId = "grad-flat";
    Frame flatFrame;
    if (check(renderFixture(flat, "gradient-flat", &flatFrame, &error),
              "the same scene without the second colour renders: " + error)) {
        check(closeTo(flatFrame.at(480, 1), top, 2) &&
                  closeTo(flatFrame.at(480, 270), top, 2) &&
                  closeTo(flatFrame.at(480, 538), top, 2),
              "flat is still flat: no ramp leaked into the scene that never asked");
    }
}

void testBackgroundGradientContain() {
    // The letterboxed bars show the RAMP, not one colour: the wide banded
    // picture contained in the canvas leaves top and bottom bars, and the
    // bar at the top of the canvas must be the ramp's top while the bar at
    // its bottom is the ramp's bottom. A flat fill cannot tell those two
    // rows apart; the scene draws them two hundred units apart.
    std::string error;
    if (!check(writeBandPng("scene-render-gradient-fit.png", 1920, 540, true, &error),
               "wide gradient-fit image written: " + error)) {
        return;
    }
    scene::Scene graded = bandImageScene("scene-render-gradient-fit.png", {});
    graded.slideId = "grad-contain";
    graded.background.fit = openshow::domain::BackgroundFit::Contain;
    graded.background.gradient = 0xE09040u;
    graded.background.hasGradient = true;
    Frame frame;
    if (!check(renderFixture(graded, "gradient-contain", &frame, &error),
               "contained gradient background rendered: " + error)) {
        return;
    }
    const SkColor top = SkColorSetRGB(0x10, 0x20, 0x30);
    const SkColor bottom = SkColorSetRGB(0xE0, 0x90, 0x40);
    check(closeTo(frame.at(480, 2), gradientAt(top, bottom, 2), 2),
          "contain+gradient: the top bar is the ramp's top");
    check(closeTo(frame.at(480, 130), gradientAt(top, bottom, 130), 2),
          "contain+gradient: and the ramp keeps grading down the bar");
    check(closeTo(frame.at(480, 536), gradientAt(top, bottom, 536), 2),
          "contain+gradient: the bottom bar is the ramp's bottom");
    check(differs(frame.at(480, 2), frame.at(480, 536), 40),
          "the two bars are visibly DIFFERENT — this is the claim a flat fill"
          " could never make");
    // Between them the picture is untouched: the same thirds cover and
    // contain have always pinned, at the same mapping.
    check(closeTo(frame.at(50, 270), SkColorSetRGB(235, 40, 40)),
          "contain+gradient: the picture's left third is still the picture");
    check(closeTo(frame.at(480, 270), SkColorSetRGB(40, 200, 40)),
          "contain+gradient: and its centre is unaffected by what runs under it");
}

void testBackgroundGradientBlur() {
    // Blur's bars are the ramp UNDER the 0.3 backdrop. The flat red picture
    // blurs to one flat red, so every bar pixel is arithmetic again:
    // blendOver(ramp(row), red, 77). If the bars' fill had stayed a flat
    // colour, the top and bottom bars would agree and both checks below
    // would fail together.
    std::string error;
    if (!check(writeFlatRectPng("scene-render-gradient-blur.png", 1920, 540,
                                SkColorSetRGB(224, 48, 48), &error),
               "flat blur picture written: " + error)) {
        return;
    }
    scene::Scene graded = flatImageScene("scene-render-gradient-blur.png", 0x102030u);
    graded.slideId = "grad-blur";
    graded.background.fit = openshow::domain::BackgroundFit::Blur;
    graded.background.gradient = 0xE09040u;
    graded.background.hasGradient = true;
    Frame frame;
    if (!check(renderFixture(graded, "gradient-blur", &frame, &error),
               "blurred graded background rendered: " + error)) {
        return;
    }
    const SkColor top = SkColorSetRGB(0x10, 0x20, 0x30);
    const SkColor bottom = SkColorSetRGB(0xE0, 0x90, 0x40);
    const SkColor picture = SkColorSetRGB(224, 48, 48);
    check(closeTo(frame.at(480, 2),
                  blendOver(gradientAt(top, bottom, 2), picture, kTestBackdropAlpha), 12),
          "blur+gradient: the top bar is the ramp's top under the 0.3 backdrop");
    check(closeTo(frame.at(480, 536),
                  blendOver(gradientAt(top, bottom, 536), picture, kTestBackdropAlpha), 12),
          "blur+gradient: the bottom bar is the ramp's bottom under it");
    check(differs(frame.at(480, 2), frame.at(480, 536), 24),
          "blur+gradient: the backdrop does not blur the ramp OUT of the bars");
    check(closeTo(frame.at(480, 270), picture, 8),
          "blur+gradient: the contained picture stays exactly the picture");
}

void testBackgroundGradientFade() {
    // The gradient rides the fade GROUP (the same R13 rule the flat fill,
    // the contain letterbox and the blur backdrop each obey): a fade from
    // a flat fill INTO a contained graded still must be the per-pixel lerp
    // of the two steady frames across the WHOLE canvas — bars included,
    // because the bars' fill is the same shader the steady draw uses. And
    // a graded scene fading to ITSELF — the lyric change over one graded
    // background — is the frame the audience already saw: no dip toward
    // any flat colour.
    std::string error;
    // Self-contained, like every fixture here: the picture is written
    // before it is used, whatever order main() runs the tests in.
    if (!check(writeFlatRectPng("scene-render-gradient-blur.png", 1920, 540,
                                SkColorSetRGB(224, 48, 48), &error),
               "graded-fade picture written: " + error)) {
        return;
    }
    const scene::Scene flatFill = [] {
        scene::Scene subject;
        subject.showId = "show1";
        subject.slideId = "grad-fade-from";
        subject.background.color = 0x2030E0u;  // far from both ramp ends
        return subject;
    }();
    scene::Scene gradedStill = flatImageScene("scene-render-gradient-blur.png", 0x102030u);
    gradedStill.slideId = "grad-fade-to";
    gradedStill.background.fit = openshow::domain::BackgroundFit::Contain;
    gradedStill.background.gradient = 0xE09040u;
    gradedStill.background.hasGradient = true;
    BlendCase half;
    if (renderBlendCase(flatFill, gradedStill, 0.5f, "gradient", &half, &error)) {
        const BlendError e = measureBlend(half, 6);
        report("flat fill to contained graded still, blend 0.5", e);
        check(e.overTolerance == 0,
              "a graded background fades as the group it steady-draws (" + describe(e) + ")");
        // The mid frame at a bar row rides the RAMP, not a flat fill: the
        // top bar and the bottom bar of the mid frame are two different
        // lerped colours, ~100 units apart. A fade path that pinned the
        // bars to background.color would make them equal and fail here.
        const SkColor topBar = half.mid.at(480, 60);
        const SkColor bottomBar = half.mid.at(480, 480);
        check(differs(topBar, bottomBar, 40),
              "mid-fade: the bars keep the ramp (top lerp vs bottom lerp are"
              " visibly different colours)");
    }
    BlendCase itself;
    if (renderBlendCase(gradedStill, gradedStill, 0.5f, "gradient-itself", &itself, &error)) {
        const BlendError e = measureBlend(itself, 6);
        report("graded still to itself, blend 0.5", e);
        check(e.overTolerance == 0,
              "a graded slide fading to itself is the frame it already showed (" +
                  describe(e) + ")");
    }
    // And the DEST rect's under-layer, where only a translucent picture can
    // see it (c2's blur-alpha discipline, re-run for the ramp): the steady
    // frame shows the gradient THROUGH the picture, so a fade path that
    // pinned the dest's background to a flat colour lands tens of units off
    // the self-fade here — the opaque fixtures above cannot see this case
    // at any blend, SrcOver with an opaque src ignores its dst.
    if (!check(writeFlatPng("scene-render-gradient-alpha.png", SkColorSetRGB(224, 48, 48),
                            &error, 128),
               "translucent graded still written: " + error)) {
        return;
    }
    scene::Scene gradedAlpha =
        flatImageScene("scene-render-gradient-alpha.png", 0x102030u);
    gradedAlpha.slideId = "grad-fade-alpha";
    gradedAlpha.background.fit = openshow::domain::BackgroundFit::Contain;
    gradedAlpha.background.gradient = 0xE09040u;
    gradedAlpha.background.hasGradient = true;
    BlendCase alphaHalf;
    if (renderBlendCase(gradedAlpha, gradedAlpha, 0.75f, "gradient-alpha", &alphaHalf, &error)) {
        const BlendError e = measureBlend(alphaHalf, 5);
        report("graded translucent still to itself, blend 0.75", e);
        check(e.overTolerance == 0,
              "the fade's dest rect carries the ramp UNDER the picture — the steady"
              " frame shows it there, and the mid frame must agree (" + describe(e) + ")");
        check(differs(alphaHalf.to.at(480, 270), SkColorSetRGB(224, 48, 48), 8),
              "the translucent still really does let its graded stack through");
    }
}

void testTranslucentPictureBlend() {
    // A picture with per-pixel alpha is where a group differs most sharply
    // from two attenuated draws: the fill shows through the picture in the
    // steady frame, so the fade must carry (picture over fill) at the blend —
    // not fill-at-blend plus picture-at-blend, which double-counts the fill
    // and under-weights the picture. Measured on the shipped code: 22/255.
    std::string error;
    if (!check(writeFlatPng("scene-render-flat-alpha-blue.png", SkColorSetRGB(48, 48, 224),
                            &error, 128),
               "translucent still written: " + error)) {
        return;
    }
    const scene::Scene translucentOverGreen =
        flatImageScene("scene-render-flat-alpha-blue.png", 0x28C828u);
    const scene::Scene opaqueRed = flatImageScene("scene-render-flat-red.png", 0x28C828u);

    BlendCase half;
    if (!renderBlendCase(opaqueRed, translucentOverGreen, 0.5f, "alpha", &half, &error)) {
        return;
    }
    const BlendError e = measureBlend(half, 3);
    report("translucent picture over fill, blend 0.5", e);
    check(e.overTolerance == 0,
          "a translucent picture fades as its own group, picture over fill (" + describe(e) + ")");
    // The steady incoming frame shows the fill through the picture: without
    // that, this fixture would be the opaque case again and the group would
    // have nothing to prove.
    check(differs(half.to.at(480, 270), SkColorSetRGB(48, 48, 224), 16),
          "the translucent still really does let its fill through when steady");
}

void testSameFrameEveryStageIsSteadyPicture() {
    // Both ends of a self-fade, and every stage between, are the one steady
    // frame. This is the "the audience must never see a lyric change dip"
    // rule stated at the extremes too: blend 0 and 1 are the same picture as
    // 0.5 here, so nothing in the fade can be quietly swapping pictures.
    std::string error;
    const scene::Scene still = flatImageScene("scene-render-flat-blue.png", 0x28C828u);
    Frame steady;
    worker::SnapshotOptions noWords;
    noWords.words = false;
    if (!check(renderFixture(still, "self-steady", noWords, &steady, &error),
               "self-fade steady frame rendered: " + error)) {
        return;
    }
    for (float blend : {0.0f, 0.35f, 0.5f, 0.75f, 1.0f}) {
        worker::SnapshotOptions stage = noWords;
        stage.fadeFrom = &still;
        stage.mediaBlend = blend;
        Frame frame;
        if (!check(renderFixture(still, "self-" + std::to_string(blend), stage, &frame, &error),
                   "self-fade stage rendered: " + error)) {
            return;
        }
        BlendError e;
        for (int y = 0; y < frame.height; ++y) {
            for (int x = 0; x < frame.width; ++x) {
                const int dev =
                    std::max(std::abs(static_cast<int>(SkColorGetR(frame.at(x, y))) -
                                      static_cast<int>(SkColorGetR(steady.at(x, y)))),
                             std::max(std::abs(static_cast<int>(SkColorGetG(frame.at(x, y))) -
                                               static_cast<int>(SkColorGetG(steady.at(x, y)))),
                                      std::abs(static_cast<int>(SkColorGetB(frame.at(x, y))) -
                                               static_cast<int>(SkColorGetB(steady.at(x, y))))));
                if (dev > e.worst) {
                    e.worst = dev;
                    e.worstX = x;
                    e.worstY = y;
                }
                if (dev > 3) {
                    ++e.overTolerance;
                }
            }
        }
        report("self-fade stage " + std::to_string(blend), e);
        check(e.overTolerance == 0, "self-fade at " + std::to_string(blend) +
                                        " is the steady frame, undimmed (" + describe(e) + ")");    }
}

/* --- task 6.11b (review R13): the text fade without its layer ----------- */

// Renders the words of `from` at the text blend over `to`'s settled
// background the way a GROUP says it: the layer holds the words at the fade's
// alpha and the alpha is the layer's. That is what production did until this
// task (SceneText.cpp called saveLayer), and what it no longer may — the
// composition rule and the allocation say so, and `render_layer_guard`
// enforces them. So it is reproduced HERE, in the fixture, as the reference
// the layer-free production path is measured against. A test may use the
// thing production is forbidden: that is how the replacement is proven
// equivalent rather than merely plausible.
bool renderWordsLayerGroup(const worker::SceneTextEngine& engine, const scene::Scene& from,
                           const scene::Scene& to, float textBlend, const std::string& name,
                           Frame* frame, std::string* error) {
    auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(kWidth, kHeight));
    if (!surface) {
        *error = "reference surface failed";
        return false;
    }
    SkCanvas* canvas = surface->getCanvas();
    canvas->clear(SK_ColorBLACK);
    canvas->scale(kScale, kScale);
    // mediaBlend 1.0: the background has arrived, only the words are dissolving.
    SkPaint fill;
    fill.setColor(SkColorSetARGB(0xFF, (to.background.color >> 16) & 0xFF,
                                 (to.background.color >> 8) & 0xFF, to.background.color & 0xFF));
    canvas->drawRect(SkRect::MakeWH(worker::kSceneCanvasWidth, worker::kSceneCanvasHeight), fill);
    const uint8_t alpha = static_cast<uint8_t>((1.0f - textBlend) * 255.0f + 0.5f);
    SkPaint group;
    group.setAlpha(alpha);
    canvas->saveLayer(SkRect::MakeWH(worker::kSceneCanvasWidth, worker::kSceneCanvasHeight),
                      &group);
    engine.drawLayout(canvas, engine.layoutScene(from), nullptr);
    canvas->restore();
    const std::string path = "scene-render-" + name + ".png";
    if (!worker::writePngRaster(surface.get(), path, error)) {
        return false;
    }
    return loadPng(path, frame, error);
}

void testTextFadeParity() {
    // The words at half strength are the words at half strength — but WHICH
    // half is a real question, because a group layer attenuates the second
    // antialiased coverage by the first in full, while the paint-time alpha
    // production uses now attenuates it by the first scaled by the fade.
    // Overlapping glyph coverage is therefore the only place the two can
    // part, and by how much is measured here rather than assumed: a single
    // line never overlaps and matches within rounding, a tight three-line
    // stanza is allowed a few units on its edge pixels and nothing more.
    //
    // Ink placement, weight and colour are the assertions that matter to an
    // audience, and both cases make them: the fade must be a real fade
    // (measured against a frame with no fade at all), and it must not move or
    // resize a word by a pixel.
    std::string error;
    worker::SceneTextEngine engine;
    if (!check(engine.create(&error), "text engine for the parity fixtures: " + error)) {
        return;
    }
    scene::Scene to;
    to.showId = "show1";
    to.slideId = "words-to";
    to.background.color = 0x1E5AC8u;  // the settled background the words sit on

    int measuredSingle = 0;
    int measuredStanza = 0;
    struct Case {
        const char* name;
        std::vector<std::string> lines;
        int tolerance;
        int* record;
    };
    for (const Case& c : {Case{"single", {"GLORY"}, 2, &measuredSingle},
                          Case{"stanza",
                               {"Praise the king of glory,", "lift up your heads,",
                                "and the gates of understanding"},
                               12, &measuredStanza}}) {
        scene::Scene from = singleItemScene(c.lines, 96.0);
        from.background.color = to.background.color;
        const SkColor backdrop =
            SkColorSetARGB(0xFF, (to.background.color >> 16) & 0xFF,
                           (to.background.color >> 8) & 0xFF, to.background.color & 0xFF);

        worker::SnapshotOptions stage;
        stage.fadeFrom = &from;
        stage.mediaBlend = 1.0f;   // background settled; words only
        stage.textBlend = 0.5f;
        Frame production;
        Frame reference;
        if (!check(renderFixture(to, "wordsfade-" + std::string(c.name), stage, &production,
                                 &error),
                   "word fade stage rendered: " + error) ||
            !check(renderWordsLayerGroup(engine, from, to, stage.textBlend,
                                         "wordsfade-" + std::string(c.name) + "-group",
                                         &reference, &error),
                   "word fade group reference rendered: " + error)) {
            continue;
        }
        int worst = 0;
        int differing = 0;
        int inked = 0;
        for (int y = 0; y < production.height; ++y) {
            for (int x = 0; x < production.width; ++x) {
                const SkColor a = production.at(x, y);
                const SkColor b = reference.at(x, y);
                const int dev =
                    std::max(std::abs(static_cast<int>(SkColorGetR(a)) - static_cast<int>(SkColorGetR(b))),
                             std::max(std::abs(static_cast<int>(SkColorGetG(a)) -
                                               static_cast<int>(SkColorGetG(b))),
                                      std::abs(static_cast<int>(SkColorGetB(a)) -
                                               static_cast<int>(SkColorGetB(b)))));
                worst = std::max(worst, dev);
                if (dev != 0) {
                    ++differing;
                }
                if (differs(a, backdrop)) {
                    ++inked;
                }
            }
        }
        *c.record = worst;
        check(worst <= c.tolerance,
              std::string(c.name) + ": the layer-free fade matches the group it replaces within " +
                  std::to_string(c.tolerance) + "/255 (worst " + std::to_string(worst) + ", " +
                  std::to_string(differing) + " of " +
                  std::to_string(production.width * production.height) + " pixels differ)");
        // The fade is real and it is the same words in the same place: the
        // frame has ink, and the ink is where the settled words are.
        check(inked > 500, std::string(c.name) + ": half-strength words are still on screen (" +
                               std::to_string(inked) + " px)");
        const Ink stageInk = measureInk(production);
        Frame settled;
        worker::SnapshotOptions full;
        full.fadeFrom = &from;
        full.mediaBlend = 1.0f;
        full.textBlend = 0.0f;  // the outgoing words at full strength
        if (check(renderFixture(to, "wordsfade-" + std::string(c.name) + "-full", full, &settled,
                                &error),
                  "full-strength words rendered: " + error)) {
            const Ink fullInk = measureInk(settled);
            check(std::abs(stageInk.left - fullInk.left) <= 2 &&
                      std::abs(stageInk.right - fullInk.right) <= 2 &&
                      std::abs(stageInk.top - fullInk.top) <= 2 &&
                      std::abs(stageInk.bottom - fullInk.bottom) <= 2,
                  std::string(c.name) + ": the fade dims the words, it does not move them (mid [" +
                      std::to_string(stageInk.left) + "," + std::to_string(stageInk.top) + "]-[" +
                      std::to_string(stageInk.right) + "," + std::to_string(stageInk.bottom) +
                      "] vs [" + std::to_string(fullInk.left) + "," +
                      std::to_string(fullInk.top) + "]-[" + std::to_string(fullInk.right) + "," +
                      std::to_string(fullInk.bottom) + "])");
            check(stageInk.count < fullInk.count,
                  std::string(c.name) + ": half strength really is dimmer (" +
                      std::to_string(stageInk.count) + " of " + std::to_string(fullInk.count) +
                      " pixels reach the ink threshold)");
        }
    }
    std::printf(
        "measured: text fade parity with the group it replaced — single line worst %d/255, "
        "three-line stanza worst %d/255\n",
        measuredSingle, measuredStanza);
}

/* --- task 6.11b (review R13): what the two fades cost ------------------- */

// The low-end case is the CPU raster backend: no GPU, no bandwidth, and the
// audience's frame budget spent in software. That makes it the honest place
// to record what a fade costs, and the two mechanisms disagree in exactly
// the way this measures: a group layer allocates a full-canvas target for
// every frame of every transition, and the layer-free forms do not.
//
// The numbers are recorded for the ledger (PLAN.md task 6.11b) and asserted
// only loosely: what is being pinned is "the replacement is not slower than
// the thing it replaced, by more than measurement noise", not a microsecond
// figure a different machine would fail.
void testFadeCostMeasured() {
    std::string error;
    worker::SceneTextEngine engine;
    if (!engine.create(&error)) {
        check(false, "text engine for the cost fixtures: " + error);
        return;
    }
    if (!check(writeFlatPng("scene-render-flat-red.png", SkColorSetRGB(224, 48, 48), &error) &&
                   writeFlatPng("scene-render-flat-blue.png", SkColorSetRGB(48, 48, 224), &error),
               "cost fixtures' stills written: " + error)) {
        return;
    }
    const scene::Scene outgoing = flatImageScene("scene-render-flat-red.png", 0x28C828u);
    scene::Scene incoming = flatImageScene("scene-render-flat-blue.png", 0x28C828u);
    scene::SceneItem stanza;
    stanza.lines = {"Praise the king of glory,", "lift up your heads,",
                    "and the gates of understanding"};
    stanza.fontSize = 96.0;
    stanza.color = 0xFFFFFFu;
    incoming.items.push_back(std::move(stanza));

    // Full authored resolution: the low-end box is showing 1920x1080, not the
    // fixtures' 960x540 thumbnail.
    auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(1920, 1080));
    if (!check(bool(surface), "cost fixture surface")) {
        return;
    }
    SkCanvas* canvas = surface->getCanvas();
    worker::SceneImageCache images;
    images.loadFor(outgoing.background.assetPath);
    images.loadFor(incoming.background.assetPath);
    const auto outgoingImage = images.imageFor(outgoing.background.assetPath);
    const worker::SceneTextEngine::SceneLayout layout = engine.layoutScene(incoming);
    const uint8_t half = 128;

    const auto timeFrames = [canvas](const char* label, int iterations,
                                     const std::function<void()>& body) {
        body();  // warm the caches the first frame would pay for
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < iterations; ++i) {
            body();
        }
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count();
        const double us = static_cast<double>(ns) / 1000.0 / iterations;
        std::printf("measured: %-34s %8.0f us/frame\n", label, us);
        return us;
    };

    constexpr int kIterations = 40;
    const double textSteady = timeFrames(
        "words, steady", kIterations, [&] {
            canvas->clear(SK_ColorBLACK);
            engine.drawLayout(canvas, layout, nullptr);
        });
    const double textFaded = timeFrames(
        "words, fade (alpha in the paint)", kIterations, [&] {
            SkPaint fade;
            fade.setAlpha(half);
            canvas->clear(SK_ColorBLACK);
            engine.drawLayout(canvas, layout, &fade);
        });
    const double textLayer = timeFrames(
        "words, fade (group layer)", kIterations, [&] {
            SkPaint fade;
            fade.setAlpha(half);
            canvas->clear(SK_ColorBLACK);
            canvas->saveLayer(SkRect::MakeWH(1920, 1080), &fade);
            engine.drawLayout(canvas, layout, nullptr);
            canvas->restore();
        });
    const double backgroundSteady = timeFrames(
        "background, steady", kIterations,
        [&] { canvas->clear(SK_ColorBLACK); worker::drawSceneBackground(canvas, incoming, &images, nullptr); });
    const double backgroundFaded = timeFrames(
        "background, fade (one shader)", kIterations, [&] {
            SkPaint fade;
            fade.setAlpha(half);
            canvas->clear(SK_ColorBLACK);
            worker::drawSceneBackground(canvas, incoming, &images, &fade);
        });
    const double backgroundLegacy = timeFrames(
        "background, fade (fill then image)", kIterations, [&] {
            SkPaint fade;
            fade.setAlpha(half);
            canvas->clear(SK_ColorBLACK);
            // The pre-6.11b two-draw path, reproduced for its cost (its
            // maths are the fixtures above, and they are wrong).
            const uint32_t color = incoming.background.color;
            SkPaint fill;
            fill.setColor(SkColorSetARGB(0xFF, (color >> 16) & 0xFF, (color >> 8) & 0xFF,
                                         color & 0xFF));
            fill.setAlpha(half);
            canvas->drawRect(SkRect::MakeWH(1920, 1080), fill);
            const SkRect src = SkRect::MakeIWH(outgoingImage->width(), outgoingImage->height());
            canvas->save();
            canvas->clipRect(SkRect::MakeWH(1920, 1080));
            canvas->drawImageRect(outgoingImage, src, SkRect::MakeWH(1920, 1080),
                                  SkSamplingOptions(SkFilterMode::kLinear), &fade,
                                  SkCanvas::kFast_SrcRectConstraint);
            canvas->restore();
        });

    check(textFaded <= textLayer * 1.5 + 200.0,
          "the layer-free word fade is not slower than the layer it replaced (" +
              std::to_string(textFaded) + " vs " + std::to_string(textLayer) + " us)");
    check(backgroundFaded <= backgroundLegacy * 1.5 + 200.0,
          "the shader group is not slower than the two draws it replaced (" +
              std::to_string(backgroundFaded) + " vs " + std::to_string(backgroundLegacy) + " us)");
    // A transition that costs several times a settled frame is the thing a
    // low-end PC feels; 4x on the CPU raster backend is the point where the
    // ledger would call this an M6 defect rather than a detail.
    check(textFaded < textSteady * 4.0 + 200.0, "words fade within 4x of a settled words frame");
    check(backgroundFaded < backgroundSteady * 4.0 + 200.0,
          "background fade within 4x of a settled background frame");
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
    testAuthoredBoxStackRenders();
    testTypographyAlignsAndSpaces();
    testWireSceneRendersIdentically();
    testDemoSlidesRender();
    testImageBackgroundCover();
    testBackgroundFitContain();
    testBackgroundFitBlurSteady();
    testBackgroundFitBlurCache();
    testMissingBackgroundFile();
    testForegroundImageItems();
    testForegroundImageFade();
    testCrossfadeStages();
    testImageCrossfadeBlend();
    testBackgroundFitContainFade();
    testBackgroundFitBlurFade();
    testBackgroundGradientFill();
    testBackgroundGradientContain();
    testBackgroundGradientBlur();
    testBackgroundGradientFade();
    testTranslucentPictureBlend();
    testSameFrameEveryStageIsSteadyPicture();
    testTextFadeParity();
    testFadeCostMeasured();
    testSceneLayerToggles();
    testWordsImagesFilterTheStack();

    if (failures == 0) {
        std::printf("scene_render_tests: all fixtures passed\n");
        return 0;
    }
    std::fprintf(stderr, "scene_render_tests: %d failure(s)\n", failures);
    return 1;
}
