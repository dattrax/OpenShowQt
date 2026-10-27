/*
 * The development side of the compositor (task 6.1b): the 1.x demo
 * picture as a SceneRenderer decoration, the CPU snapshot, and the pixel
 * assertion harnesses behind --check-layers, --check-transition and
 * --check-production. The composition itself — layers, fade state
 * machine, output rule — is render/SceneRenderer.cpp; every frame this
 * file asserts against is drawn by that same production code path.
 */

#include "TestScene.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "include/codec/SkCodec.h"
#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkData.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkRect.h"
#include "include/core/SkSurface.h"
#include "include/core/SkTypeface.h"
#include "include/effects/SkGradient.h"
#include "include/encode/SkPngEncoder.h"
#include "modules/skparagraph/include/Paragraph.h"
#include "modules/skparagraph/include/ParagraphBuilder.h"
#include "modules/skparagraph/include/ParagraphStyle.h"
#include "modules/skparagraph/include/TextStyle.h"

namespace openshow::worker {

namespace {

// The authored-canvas regions each layer draws into. --check-layers asserts
// the diff of every single-layer toggle stays inside its region, so moving
// content means updating these bounds. (The layers themselves keep their
// own drawing geometry in SceneRenderer.cpp; these are the harness's view
// of where that content is allowed to appear.)
constexpr SkRect kPhotoRegion = SkRect::MakeLTRB(130.0f, 240.0f, 985.0f, 775.0f);
constexpr SkRect kWordsRegion = SkRect::MakeLTRB(1025.0f, 205.0f, 1855.0f, 600.0f);
constexpr SkRect kAuxRegion = SkRect::MakeLTRB(1545.0f, 25.0f, 1895.0f, 125.0f);
constexpr SkRect kClockRegion = SkRect::MakeLTRB(130.0f, 965.0f, 1500.0f, 1035.0f);

std::unique_ptr<skia::textlayout::Paragraph> makeTitle(
    const sk_sp<skia::textlayout::FontCollection>& fontCollection,
    const sk_sp<SkUnicode>& unicode, const sk_sp<SkTypeface>& bold,
    const sk_sp<SkTypeface>& regular) {
    skia::textlayout::ParagraphStyle paragraphStyle;

    auto builder = skia::textlayout::ParagraphBuilder::make(paragraphStyle, fontCollection,
                                                            unicode);

    skia::textlayout::TextStyle heading;
    heading.setTypeface(bold);
    heading.setFontSize(110.0f);
    heading.setColor(SkColorSetRGB(0xFF, 0xFF, 0xFF));
    builder->pushStyle(heading);
    builder->addText("OpenShowQt");

    skia::textlayout::TextStyle detail;
    detail.setTypeface(regular);
    detail.setFontSize(40.0f);
    detail.setColor(SkColorSetRGB(0xC5, 0xCF, 0xE4));
    builder->pushStyle(detail);
    // Second line: what this frame proves. Third line: shaping and fallback
    // sample (Greek, an em dash and accented Latin through harfbuzz).
    builder->addText("\nTask 1.1 \xc2\xb7 GLFW + Vulkan + Skia \xc2\xb7 text via SkParagraph"
                     "\n\xce\xa9 \xce\xb2 \xcf\x80 \xc2\xbd \xe2\x80\x94 caf\xc3\xa9 na\xc3\xafve");

    std::unique_ptr<skia::textlayout::Paragraph> paragraph = builder->Build();
    if (paragraph) {
        paragraph->layout(800.0f);
    }
    return paragraph;
}

// A small procedural landscape, drawn on the CPU, encoded to PNG and
// decoded again through SkCodec so the real image path is exercised.
bool createPhoto(sk_sp<SkImage>* photo, std::string* error) {
    constexpr int kPhotoWidth = 1280;
    constexpr int kPhotoHeight = 720;
    auto raster = SkSurfaces::Raster(
        SkImageInfo::MakeN32Premul(kPhotoWidth, kPhotoHeight));
    SkCanvas* canvas = raster->getCanvas();

    const SkColor skyTop = SkColorSetRGB(0x16, 0x22, 0x4E);
    const SkColor skyBottom = SkColorSetRGB(0xF2, 0x9B, 0x54);
    const SkPoint skyPoints[2] = {{0, 0}, {0, SkIntToScalar(kPhotoHeight)}};
    const SkColor4f skyColors[2] = {SkColor4f::FromColor(skyTop), SkColor4f::FromColor(skyBottom)};
    SkPaint sky;
    sky.setShader(SkShaders::LinearGradient(
        skyPoints, SkGradient{SkGradient::Colors(skyColors, SkTileMode::kClamp),
                              SkGradient::Interpolation{}}));
    canvas->drawRect(SkRect::MakeIWH(kPhotoWidth, kPhotoHeight), sky);

    SkPaint sun;
    sun.setColor(SkColorSetRGB(0xFF, 0xE3, 0xA0));
    sun.setAntiAlias(true);
    canvas->drawCircle(880, 470, 90, sun);

    SkPaint hillBack;
    hillBack.setColor(SkColorSetRGB(0x2A, 0x47, 0x36));
    hillBack.setAntiAlias(true);
    SkPath back = SkPathBuilder{}
                      .moveTo(0, 620)
                      .cubicTo(320, 430, 620, 560, 1280, 480)
                      .lineTo(1280, 720)
                      .close()
                      .detach();
    canvas->drawPath(back, hillBack);

    SkPaint hillFront;
    hillFront.setColor(SkColorSetRGB(0x14, 0x28, 0x1E));
    hillFront.setAntiAlias(true);
    SkPath front = SkPathBuilder{}
                       .moveTo(0, 720)
                       .cubicTo(400, 520, 900, 700, 1280, 590)
                       .lineTo(1280, 720)
                       .close()
                       .detach();
    canvas->drawPath(front, hillFront);

    SkPixmap pixmap;
    if (!raster->peekPixels(&pixmap)) {
        if (error) {
            *error = "test image pixels unavailable";
        }
        return false;
    }
    sk_sp<SkData> png = SkPngEncoder::Encode(pixmap, SkPngEncoder::Options{});
    if (!png) {
        if (error) {
            *error = "test image PNG encode failed";
        }
        return false;
    }

    std::unique_ptr<SkCodec> codec = SkCodec::MakeFromData(png);
    if (!codec) {
        if (error) {
            *error = "SkCodec could not read the test PNG";
        }
        return false;
    }
    const SkImageInfo info =
        codec->getInfo().makeColorType(kN32_SkColorType).makeAlphaType(kPremul_SkAlphaType);
    SkBitmap bitmap;
    bitmap.allocPixels(info);
    const SkCodec::Result result = codec->getPixels(info, bitmap.getPixels(), bitmap.rowBytes());
    if (result != SkCodec::kSuccess) {
        if (error) {
            *error = "SkCodec PNG decode failed";
        }
        return false;
    }
    *photo = SkImages::RasterFromBitmap(bitmap);
    return *photo != nullptr;
}

struct RasterFrame {
    int width = 1920;
    int height = 1080;
    std::vector<SkColor> pixels;

    bool render(SceneRenderer& scene, std::string* error, double timeSeconds = 0.0) {
        auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));
        if (!surface) {
            if (error) {
                *error = "raster surface allocation failed";
            }
            return false;
        }
        // Fixed wall clock and status line: two frames with the same layer
        // configuration must come out pixel identical.
        scene.drawLive(surface->getCanvas(), width, height, timeSeconds,
                       std::time_t{1234567890}, "layers");
        pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height));
        const bool ok = surface->readPixels(SkImageInfo::MakeN32Premul(width, height),
                                            pixels.data(),
                                            static_cast<size_t>(width) * sizeof(SkColor), 0, 0);
        if (!ok && error) {
            *error = "layer selftest readPixels failed";
        }
        return ok;
    }

    SkColor at(int x, int y) const { return pixels[static_cast<size_t>(y) * width + x]; }
};

// Channel-wise closeness for the transition checks: rasters read straight
// back carry no PNG colour-management drift, so tolerances only have to
// cover Skia's fixed-point blend rounding.
bool channelsMatch(SkColor a, SkColor b, int tolerance) {
    const auto away = [](int x, int y) { return x > y ? x - y : y - x; };
    return away(SkColorGetR(a), SkColorGetR(b)) <= tolerance &&
           away(SkColorGetG(a), SkColorGetG(b)) <= tolerance &&
           away(SkColorGetB(a), SkColorGetB(b)) <= tolerance;
}

// Bounding box of differing pixels; false when the frames are identical.
bool diffBounds(const RasterFrame& a, const RasterFrame& b, SkIRect* bounds) {
    int left = a.width;
    int top = a.height;
    int right = -1;
    int bottom = -1;
    for (int y = 0; y < a.height; ++y) {
        for (int x = 0; x < a.width; ++x) {
            if (a.at(x, y) != b.at(x, y)) {
                left = std::min(left, x);
                top = std::min(top, y);
                right = std::max(right, x);
                bottom = std::max(bottom, y);
            }
        }
    }
    if (right < 0) {
        return false;
    }
    *bounds = SkIRect::MakeLTRB(left, top, right + 1, bottom + 1);
    return true;
}

bool contains(const SkIRect& outer, const SkIRect& inner) {
    return inner.left() >= outer.left() && inner.top() >= outer.top() &&
           inner.right() <= outer.right() && inner.bottom() <= outer.bottom();
}

std::string describe(const SkIRect& r) {
    return std::to_string(r.left()) + "," + std::to_string(r.top()) + " " +
           std::to_string(r.width()) + "x" + std::to_string(r.height());
}

bool checkLayerRegion(SceneRenderer& scene, const RasterFrame& baseline, LayerId id,
                      const SkRect& allowed, RasterFrame* scratch, std::string* error) {
    scene.setLayerEnabled(id, true);
    if (!scratch->render(scene, error)) {
        scene.setLayerEnabled(id, false);
        return false;
    }
    SkIRect bounds;
    const bool differs = diffBounds(baseline, *scratch, &bounds);
    scene.setLayerEnabled(id, false);
    if (!differs) {
        if (error) {
            *error = std::string(layerName(id)) + " layer drew nothing while enabled";
        }
        return false;
    }
    const SkIRect allowedRect =
        SkIRect::MakeLTRB(static_cast<int>(std::floor(allowed.fLeft)),
                          static_cast<int>(std::floor(allowed.fTop)),
                          static_cast<int>(std::ceil(allowed.fRight)),
                          static_cast<int>(std::ceil(allowed.fBottom)));
    if (!contains(allowedRect, bounds)) {
        if (error) {
            *error = std::string(layerName(id)) + " layer drew outside its region: diff " +
                     describe(bounds) + " not inside " + describe(allowedRect);
        }
        return false;
    }
    return true;
}

}  // namespace

SceneRenderer::DemoBuilder demoBuilder() {
    return [](const SceneTextEngine& engine, DemoDecoration* out, std::string* error) {
        std::shared_ptr<skia::textlayout::Paragraph> title =
            makeTitle(engine.fontCollection(), engine.unicode(), engine.bold(),
                      engine.regular());
        if (!title) {
            if (error) {
                *error = "SkParagraph title layout failed";
            }
            return false;
        }
        sk_sp<SkImage> photo;
        if (!createPhoto(&photo, error)) {
            return false;
        }
        // The paint closure owns the laid-out paragraph; the words layer
        // calls it with the canvas already in authored coordinates.
        out->titlePaint = [p = std::move(title)](SkCanvas* canvas) { p->paint(canvas, 1040, 220); };
        out->photo = std::move(photo);
        out->gradient = true;
        return true;
    };
}

bool renderTestSnapshot(const std::string& path, int width, int height,
                        const std::vector<std::pair<LayerId, bool>>& layerOverrides,
                        std::string* error) {
    SceneRenderer scene;
    if (!scene.create(error, demoBuilder())) {
        return false;
    }
    scene.applyLayerOverrides(layerOverrides);
    auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));
    if (!surface) {
        if (error) {
            *error = "raster surface allocation failed";
        }
        return false;
    }
    scene.drawLive(surface->getCanvas(), width, height, 0.0, 0, "snapshot (CPU raster)");
    return writePngRaster(surface.get(), path, error);
}

bool verifyLayerComposition(std::string* error) {
    // The decorated renderer: the region claims describe where the full
    // development picture's content — demo assets included — may appear.
    SceneRenderer scene;
    if (!scene.create(error, demoBuilder())) {
        return false;
    }

    RasterFrame frame;

    // 1. Every layer off: nothing must be dispatched, so the frame is the
    //    plain black clear of the compositor.
    for (uint32_t i = 0; i < static_cast<uint32_t>(LayerId::kCount); ++i) {
        scene.setLayerEnabled(static_cast<LayerId>(i), false);
    }
    if (!frame.render(scene, error)) {
        return false;
    }
    for (int y = 0; y < frame.height; y += 7) {
        for (int x = 0; x < frame.width; x += 7) {
            if (frame.at(x, y) != SK_ColorBLACK) {
                if (error) {
                    *error = "all layers off is not pure black: a disabled layer still drew";
                }
                return false;
            }
        }
    }

    // 2. Background alone: the gradient must cover the whole canvas.
    scene.setLayerEnabled(LayerId::kBackground, true);
    if (!frame.render(scene, error)) {
        return false;
    }
    if (frame.at(960, 540) == SK_ColorBLACK || frame.at(1800, 60) == SK_ColorBLACK ||
        frame.at(80, 1000) == SK_ColorBLACK) {
        if (error) {
            *error = "background gradient does not cover the canvas";
        }
        return false;
    }

    // 3. Each remaining layer, on its own, paints only inside its region.
    //    At 1920x1080 the output-rule scale is 1, so authored coordinates
    //    equal pixel coordinates.
    RasterFrame probe;
    if (!checkLayerRegion(scene, frame, LayerId::kImages, kPhotoRegion, &probe, error)) {
        return false;
    }
    if (!checkLayerRegion(scene, frame, LayerId::kWords, kWordsRegion, &probe, error)) {
        return false;
    }
    if (!checkLayerRegion(scene, frame, LayerId::kAux, kAuxRegion, &probe, error)) {
        return false;
    }
    if (!checkLayerRegion(scene, frame, LayerId::kClock, kClockRegion, &probe, error)) {
        return false;
    }

    // 4. Toggles round-trip: back to the same configuration twice yields
    //    byte-identical frames, so off layers leave no trace behind.
    scene.setLayerEnabled(LayerId::kImages, true);
    scene.setLayerEnabled(LayerId::kWords, true);
    scene.setLayerEnabled(LayerId::kClock, true);
    scene.setLayerEnabled(LayerId::kAux, false);
    RasterFrame first;
    if (!first.render(scene, error)) {
        return false;
    }
    if (!probe.render(scene, error)) {
        return false;
    }
    SkIRect bounds;
    if (diffBounds(first, probe, &bounds)) {
        if (error) {
            *error = "two renders of the same layer configuration differ: " + describe(bounds);
        }
        return false;
    }
    return true;
}

bool verifySceneTransition(std::string* error) {
    SceneRenderer scene;
    if (!scene.create(error, demoBuilder())) {
        return false;
    }
    // Background and words only: the footer clock would count as ink, and
    // the demo photo is not part of any scene.
    scene.setLayerEnabled(LayerId::kClock, false);

    const SkColor red = SkColorSetRGB(0xE0, 0x30, 0x30);
    const SkColor blue = SkColorSetRGB(0x30, 0x30, 0xE0);
    auto stage = [](uint32_t color, const char* line) {
        auto subject = std::make_shared<openshow::scene::Scene>();
        subject->showId = "check";
        subject->slideId = "transition";
        subject->background.color = color;
        if (line != nullptr) {
            openshow::scene::SceneItem item;
            item.lines = {line};
            item.fontSize = 300.0;
            item.color = 0xFFFFFFu;
            subject->items.push_back(std::move(item));
        }
        return subject;
    };

    RasterFrame frame;
    const auto fail = [&](const char* what) {
        if (error) {
            *error = what;
        }
        return false;
    };
    const auto inkCount = [&]() {
        const SkColor background = frame.at(2, 2);
        int count = 0;
        for (int y = 0; y < frame.height; y += 2) {
            for (int x = 0; x < frame.width; x += 2) {
                if (!channelsMatch(frame.at(x, y), background, 16)) {
                    ++count;
                }
            }
        }
        return count;
    };

    // 1. The first presentation pops in: with nothing to fade from, the
    //    very first frame already carries the scene's background.
    scene.setActiveScene(stage(0xE03030u, "ONE"));  // fade requested, none possible
    if (!frame.render(scene, error, 0.0)) {
        return false;
    }
    if (!channelsMatch(frame.at(2, 2), red, 4)) {
        return fail("first presentation did not pop in");
    }
    if (inkCount() < 5000) {
        return fail("first presentation lost the words");
    }

    // 2. A fade is measured from the first frame it draws...
    const auto blueScene = stage(0x3030E0u, nullptr);
    scene.setActiveScene(blueScene);
    if (!frame.render(scene, error, 10.0)) {
        return false;
    }
    if (!channelsMatch(frame.at(2, 2), red, 4)) {
        return fail("fade dropped its first frame");
    }
    //    ...so the media track is halfway at +0.4s of its 800ms, while the
    //    old words (500ms text track, 80% through) are still faintly on
    //    screen over the blending background.
    if (!frame.render(scene, error, 10.4)) {
        return false;
    }
    const SkColor midpoint =
        SkColorSetRGB((0xE0 + 0x30) / 2, 0x30, (0x30 + 0xE0) / 2);
    if (!channelsMatch(frame.at(2, 2), midpoint, 6)) {
        return fail("media blend is not halfway at +0.4s");
    }
    if (inkCount() == 0) {
        return fail("words vanished before the text track completed");
    }
    //    At +0.5s the text track is finished — no trace of the old words —
    //    while the media track is still travelling.
    if (!frame.render(scene, error, 10.5)) {
        return false;
    }
    if (inkCount() != 0) {
        return fail("text track did not complete at 0.5s");
    }
    if (channelsMatch(frame.at(2, 2), blue, 12)) {
        return fail("media track completed before its 0.8s");
    }
    //    +0.8s: both tracks are done and the fade releases the old scene.
    if (!frame.render(scene, error, 10.9)) {
        return false;
    }
    if (!channelsMatch(frame.at(2, 2), blue, 4)) {
        return fail("media track did not complete at 0.8s");
    }
    if (!frame.render(scene, error, 20.0)) {
        return false;
    }
    if (!channelsMatch(frame.at(2, 2), blue, 4) || inkCount() != 0) {
        return fail("a completed fade left a trace behind");
    }

    // 3. A cut swaps in one frame.
    scene.setActiveScene(stage(0xE03030u, "ONE"), SceneTransitionKind::kCut);
    if (!frame.render(scene, error, 21.0)) {
        return false;
    }
    if (!channelsMatch(frame.at(2, 2), red, 4) || inkCount() < 5000) {
        return fail("cut did not swap the scene in one frame");
    }

    // 4. A cut cancels a fade in flight: the next frame is the cut scene
    //    exactly, not a blend.
    scene.setActiveScene(blueScene);  // fade starts
    if (!frame.render(scene, error, 30.0) || !frame.render(scene, error, 30.3)) {
        return false;
    }
    scene.setActiveScene(stage(0xE03030u, "ONE"), SceneTransitionKind::kCut);
    if (!frame.render(scene, error, 30.4)) {
        return false;
    }
    if (!channelsMatch(frame.at(2, 2), red, 4)) {
        return fail("cut did not cancel a fade in flight");
    }
    return true;
}

bool verifyProductionCompositor(std::string* error) {
    const auto fail = [&](const char* what) {
        if (error) {
            *error = what;
        }
        return false;
    };

    // 1. Layer defaults. The scene tabs are the production composition;
    //    aux and the clock/diagnostics footer start disabled (R1), and a
    //    renderer with no decoration is production by construction.
    SceneRenderer production;
    if (!production.create(error)) {
        return false;
    }
    if (!production.production()) {
        return fail("undecorated renderer does not report production");
    }
    if (!production.layerEnabled(LayerId::kBackground) ||
        !production.layerEnabled(LayerId::kImages) ||
        !production.layerEnabled(LayerId::kWords)) {
        return fail("production scene layers do not start enabled");
    }
    if (production.layerEnabled(LayerId::kAux) || production.layerEnabled(LayerId::kClock)) {
        return fail("production aux/clock layers do not start disabled");
    }

    // 2. Empty black initial scene: with nothing presented every pixel is
    //    black — the footer/clock band included, which is exactly where
    //    the review found diagnostics painted onto the audience output.
    RasterFrame frame;
    if (!frame.render(production, error)) {
        return false;
    }
    for (int y = 0; y < frame.height; ++y) {
        for (int x = 0; x < frame.width; ++x) {
            if (frame.at(x, y) != SK_ColorBLACK) {
                return fail("production start-up frame is not pure black");
            }
        }
    }

    // 3. A presented scene composes identical pixels in either renderer
    //    (clock off on the development side, since that is the production
    //    default): the audience scene is the proven layer path, and
    //    output/preview sharing one compositor means they carry the same
    //    scene by construction.
    auto subject = std::make_shared<openshow::scene::Scene>();
    subject->showId = "check";
    subject->slideId = "production";
    subject->background.color = 0xE03030u;  // scene colours are 0xRRGGBB
    openshow::scene::SceneItem item;
    item.lines = {"PROD"};
    item.fontSize = 300.0;
    item.color = 0xFFFFFFu;
    subject->items.push_back(std::move(item));

    const SkIRect footer = SkIRect::MakeLTRB(0, static_cast<int>(kClockRegion.fTop) - 5,
                                             frame.width, frame.height);
    production.setActiveScene(subject, SceneTransitionKind::kCut);
    if (!frame.render(production, error, 5.0)) {
        return false;
    }
    if (!channelsMatch(frame.at(2, 2), SkColorSetRGB(0xE0, 0x30, 0x30), 4)) {
        return fail("production presentation lost the scene background");
    }
    {  // the words made it, and the footer band stays scene-colour
        int ink = 0;
        for (int y = 0; y < frame.height; y += 2) {
            for (int x = 0; x < frame.width; x += 2) {
                const SkColor c = frame.at(x, y);
                if (SkColorGetR(c) > 235 && SkColorGetG(c) > 235 && SkColorGetB(c) > 235) {
                    ++ink;
                }
            }
        }
        if (ink < 100) {
            return fail("production presentation lost the scene words");
        }
        const SkColor red = SkColorSetRGB(0xE0, 0x30, 0x30);
        for (int y = footer.top(); y < footer.bottom(); y += 3) {
            for (int x = footer.left(); x < footer.right(); x += 3) {
                if (!channelsMatch(frame.at(x, y), red, 4)) {
                    return fail("production draws diagnostics into the footer band");
                }
            }
        }
    }

    SceneRenderer dev;
    if (!dev.create(error, demoBuilder())) {
        return false;
    }
    if (dev.production()) {
        return fail("a decorated renderer reports itself as production");
    }
    dev.setLayerEnabled(LayerId::kClock, false);  // production's default
    dev.setActiveScene(subject, SceneTransitionKind::kCut);
    RasterFrame devFrame;
    if (!devFrame.render(dev, error, 5.0)) {
        return false;
    }
    SkIRect bounds;
    if (diffBounds(frame, devFrame, &bounds)) {
        if (error) {
            *error = "production and development scene composition differ: diff " +
                     describe(bounds);
        }
        return false;
    }

    // 4. The demo picture remains fully available when a decoration is
    //    attached: gradient, photo, title and footer all paint again.
    SceneRenderer demoPicture;
    if (!demoPicture.create(error, demoBuilder())) {
        return false;
    }
    RasterFrame demoStart;
    if (!demoStart.render(demoPicture, error)) {
        return false;
    }
    if (demoStart.at(960, 540) == SK_ColorBLACK || demoStart.at(1800, 60) == SK_ColorBLACK) {
        return fail("demo decoration lost its gradient backdrop");
    }
    if (demoStart.at(500, 500) == SK_ColorBLACK) {
        return fail("demo decoration lost the demo photo");
    }
    {  // some ink in the title and footer regions proves text came back
        int words = 0;
        int footerInk = 0;
        for (int y = 0; y < demoStart.height; ++y) {
            for (int x = 0; x < demoStart.width; ++x) {
                const SkColor c = demoStart.at(x, y);
                if (c != SK_ColorBLACK && y >= 205 && y < 600 && x >= 1025) {
                    ++words;
                }
                if (c != SK_ColorBLACK && y >= 965 && y < 1035 && x >= 130 && x < 1500) {
                    ++footerInk;
                }
            }
        }
        if (words == 0) {
            return fail("demo decoration lost the demo title");
        }
        if (footerInk == 0) {
            return fail("demo decoration lost the status/clock footer");
        }
    }
    return true;
}

}  // namespace openshow::worker
