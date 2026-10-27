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
#include "include/core/SkImageInfo.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRect.h"
#include "include/core/SkSurface.h"

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
    domain::TextBox title;
    title.lines = {"A TITLE STRIP"};
    title.x = 160.0;
    title.y = 60.0;
    title.width = 1600.0;
    title.height = 180.0;
    check(!docs.addTextBox(show, slide, title, &error).empty(),
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
        // literally: one item carrying the slide's lines at TextItem's own
        // defaults, because those defaults ARE the legacy lyric box (pinned in
        // scene_tests). If migration or compilation ever shifts a default by a
        // pixel, this equality is what breaks.
        scene::Scene oldShape;
        oldShape.showId = "show1";
        oldShape.slideId = "slide1";
        scene::TextItem only;
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
    scene::TextItem stanza;
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
    testMissingBackgroundFile();
    testCrossfadeStages();
    testImageCrossfadeBlend();
    testTranslucentPictureBlend();
    testSameFrameEveryStageIsSteadyPicture();
    testTextFadeParity();
    testFadeCostMeasured();
    testSceneLayerToggles();

    if (failures == 0) {
        std::printf("scene_render_tests: all fixtures passed\n");
        return 0;
    }
    std::fprintf(stderr, "scene_render_tests: %d failure(s)\n", failures);
    return 1;
}
