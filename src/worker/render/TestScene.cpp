#include "TestScene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "include/codec/SkCodec.h"
#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkData.h"
#include "include/core/SkFont.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkRRect.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkSurface.h"
#include "include/core/SkTextBlob.h"
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
// content means updating these bounds.
constexpr SkRect kPhotoRegion = SkRect::MakeLTRB(130.0f, 240.0f, 985.0f, 775.0f);
constexpr SkRect kWordsRegion = SkRect::MakeLTRB(1025.0f, 205.0f, 1855.0f, 600.0f);
constexpr SkRect kAuxBadge = SkRect::MakeLTRB(1560.0f, 40.0f, 1880.0f, 112.0f);
constexpr SkRect kAuxRegion = SkRect::MakeLTRB(1545.0f, 25.0f, 1895.0f, 125.0f);
constexpr SkRect kClockRegion = SkRect::MakeLTRB(130.0f, 965.0f, 1500.0f, 1035.0f);

// Bottom of the stack: the gradient background, or the presented scene's
// colour fill (and cover-fitted image, task 3.2) while one is on air. When
// a crossfade is in flight the outgoing scene draws first, then the on-air
// scene dissolves over it at the media blend — the whole background fading
// as one, with the transparency straight in the paint (no render-to-
// texture). FreeShow puts video here too; the M5 background layer will
// replace this shader with decoded frames through the same draw() entry.
class BackgroundLayer final : public RenderLayer {
public:
    BackgroundLayer(const ActiveScene* active, SceneImageCache* images,
                    const SceneTransition* transition)
        : fActive(active), fImages(images), fTransition(transition) {}

    LayerId id() const override { return LayerId::kBackground; }

    void draw(const FrameContext& ctx) override {
        if (*fActive) {
            if (fTransition->fading()) {
                drawSceneBackground(ctx.canvas, *fTransition->fFrom, fImages, nullptr);
                SkPaint fade;
                fade.setAlpha(static_cast<uint8_t>(fTransition->fMediaBlend * 255.0f + 0.5f));
                drawSceneBackground(ctx.canvas, **fActive, fImages, &fade);
            } else {
                drawSceneBackground(ctx.canvas, **fActive, fImages, nullptr);
            }
            return;
        }
        const SkPoint points[2] = {{0, 0}, {0, TestScene::kCanvasHeight}};
        const SkColor4f colors[2] = {
            SkColor4f::FromColor(SkColorSetRGB(0x14, 0x16, 0x1F)),
            SkColor4f::FromColor(SkColorSetRGB(0x24, 0x2C, 0x46))};
        SkPaint background;
        background.setShader(SkShaders::LinearGradient(
            points, SkGradient{SkGradient::Colors(colors, SkTileMode::kClamp),
                               SkGradient::Interpolation{}}));
        ctx.canvas->drawRect(SkRect::MakeWH(TestScene::kCanvasWidth, TestScene::kCanvasHeight),
                             background);
    }

private:
    const ActiveScene* fActive;            // TestScene-owned; outlives the layer
    SceneImageCache* fImages;              // TestScene-owned image cache
    const SceneTransition* fTransition;    // TestScene-owned fade state
};

// Slide images: the PNG-round-tripped photo with a border and caption. The
// demo picture only — a presented scene stands it down until real slide
// images arrive with the scene (task 3.2).
class ImageLayer final : public RenderLayer {
public:
    ImageLayer(sk_sp<SkImage> photo, sk_sp<SkTypeface> regular, const ActiveScene* active)
        : fPhoto(std::move(photo)), fRegular(std::move(regular)), fActive(active) {}

    LayerId id() const override { return LayerId::kImages; }

    void draw(const FrameContext& ctx) override {
        if (*fActive || !fPhoto) {
            return;
        }
        const float photoWidth = 820.0f;
        const float photoHeight =
            photoWidth * static_cast<float>(fPhoto->height()) / static_cast<float>(fPhoto->width());
        const SkRect photoRect = SkRect::MakeXYWH(140, 250, photoWidth, photoHeight);
        ctx.canvas->drawImageRect(fPhoto,
                                  SkRect::MakeIWH(fPhoto->width(), fPhoto->height()), photoRect,
                                  SkSamplingOptions(SkFilterMode::kLinear), nullptr,
                                  SkCanvas::kFast_SrcRectConstraint);
        SkPaint border;
        border.setStyle(SkPaint::kStroke_Style);
        border.setStrokeWidth(3.0f);
        border.setColor(SkColorSetRGB(0x49, 0x57, 0x7A));
        ctx.canvas->drawRect(photoRect, border);

        SkFont captionFont(fRegular, 26.0f);
        captionFont.setEdging(SkFont::Edging::kAntiAlias);
        const std::string_view captionText =
            "image: raster \xe2\x86\x92 PNG \xe2\x86\x92 SkCodec \xe2\x86\x92 Ganesh texture";
        sk_sp<SkTextBlob> caption = SkTextBlob::MakeFromText(
            captionText.data(), captionText.size(), captionFont, SkTextEncoding::kUTF8);
        SkPaint captionPaint;
        captionPaint.setColor(SkColorSetRGB(0x9A, 0xA6, 0xC0));
        if (caption) {
            ctx.canvas->drawTextBlob(caption, 140, 250 + photoHeight + 40, captionPaint);
        }
    }

private:
    sk_sp<SkImage> fPhoto;
    sk_sp<SkTypeface> fRegular;
    const ActiveScene* fActive;  // TestScene-owned; outlives the layer
};

// Words: while a scene is on air, its text laid out through
// SceneTextEngine (wrap, centre, fallback, fit — task 3.1); otherwise the
// demo SkParagraph title. Layouts are cached per scene pointer (two slots,
// so a crossfade never re-lays-out either side); during the fade the two
// scenes dissolve — outgoing at 1 - textBlend, incoming at textBlend,
// the classic lyric cross-dissolve (docs/reference.md §7 text transition).
class WordsLayer final : public RenderLayer {
public:
    WordsLayer(std::unique_ptr<skia::textlayout::Paragraph> title,
               const SceneTextEngine* engine, const ActiveScene* active,
               const SceneTransition* transition)
        : fTitle(std::move(title)), fEngine(engine), fActive(active),
          fTransition(transition) {}

    LayerId id() const override { return LayerId::kWords; }

    void draw(const FrameContext& ctx) override {
        if (*fActive) {
            const bool fading = fTransition->fading();
            if (fading) {
                const SceneTextEngine::SceneLayout& from = layoutFor(&fSlotA, fTransition->fFrom);
                SkPaint fade;
                fade.setAlpha(
                    static_cast<uint8_t>((1.0f - fTransition->fTextBlend) * 255.0f + 0.5f));
                fEngine->drawLayout(ctx.canvas, from, &fade);
            }
            const SceneTextEngine::SceneLayout& current = layoutFor(&fSlotB, *fActive);
            if (fading) {
                SkPaint fade;
                fade.setAlpha(static_cast<uint8_t>(fTransition->fTextBlend * 255.0f + 0.5f));
                fEngine->drawLayout(ctx.canvas, current, &fade);
            } else {
                fEngine->drawLayout(ctx.canvas, current);
            }
            return;
        }
        if (fTitle) {
            fTitle->paint(ctx.canvas, 1040, 220);
        }
    }

private:
    // One layout cache slot. The scenes cross-fading through here are the
    // two this layer was handed, so a two-slot cache (this struct twice)
    // covers every frame of a fade without rebuilding a layout.
    struct LayoutSlot {
        ActiveScene scene;  // the scene fLayout was built from, if any
        SceneTextEngine::SceneLayout layout;
    };

    // Returns the layout for `scene`, (re)building `slot` when it holds a
    // different scene. The slots live inside this layer; the returned
    // reference stays valid until the next call that rebuilds that slot.
    const SceneTextEngine::SceneLayout& layoutFor(LayoutSlot* slot, const ActiveScene& scene) {
        if (slot->scene != scene) {
            slot->layout = fEngine->layoutScene(*scene);
            slot->scene = scene;
        }
        return slot->layout;
    }

    std::unique_ptr<skia::textlayout::Paragraph> fTitle;
    const SceneTextEngine* fEngine;
    const ActiveScene* fActive;          // TestScene-owned; outlives the layer
    const SceneTransition* fTransition;  // TestScene-owned fade state
    LayoutSlot fSlotA;                   // holds the outgoing scene during a fade
    LayoutSlot fSlotB;                   // holds the on-air scene
};

// Aux: placeholder for future overlay content, off by default. A badge so
// enabling it is visibly verifiable.
class AuxLayer final : public RenderLayer {
public:
    explicit AuxLayer(sk_sp<SkTypeface> regular) : fRegular(std::move(regular)) {}

    LayerId id() const override { return LayerId::kAux; }

    void draw(const FrameContext& ctx) override {
        SkPaint badge;
        badge.setColor(SkColorSetARGB(0x40, 0x6A, 0x4D, 0xD9));
        badge.setAntiAlias(true);
        ctx.canvas->drawRRect(SkRRect::MakeRectXY(kAuxBadge, 16.0f, 16.0f), badge);
        SkPaint outline;
        outline.setStyle(SkPaint::kStroke_Style);
        outline.setStrokeWidth(2.0f);
        outline.setColor(SkColorSetARGB(0x99, 0x7B, 0x5F, 0xF0));
        outline.setAntiAlias(true);
        ctx.canvas->drawRRect(SkRRect::MakeRectXY(kAuxBadge, 16.0f, 16.0f), outline);

        SkFont font(fRegular, 40.0f);
        font.setEdging(SkFont::Edging::kAntiAlias);
        const std::string_view text = "aux layer";
        sk_sp<SkTextBlob> blob =
            SkTextBlob::MakeFromText(text.data(), text.size(), font, SkTextEncoding::kUTF8);
        SkPaint paint;
        paint.setColor(SkColorSetRGB(0xD8, 0xCE, 0xF7));
        if (blob) {
            ctx.canvas->drawTextBlob(blob, kAuxBadge.fLeft + 26.0f, kAuxBadge.fBottom - 32.0f,
                                     paint);
        }
    }

private:
    sk_sp<SkTypeface> fRegular;
};

// Clock: diagnostics plus the live wall clock so a running loop visibly
// presents new frames. The audience clock in FreeShow is this slot.
class ClockLayer final : public RenderLayer {
public:
    explicit ClockLayer(sk_sp<SkTypeface> regular) : fRegular(std::move(regular)) {}

    LayerId id() const override { return LayerId::kClock; }

    void draw(const FrameContext& ctx) override {
        char clock[16] = "";
        std::tm local{};
        if (localtime_r(&ctx.wallClock, &local) != nullptr) {
            std::strftime(clock, sizeof(clock), "%H:%M:%S", &local);
        }
        const std::string footer = std::string(ctx.statusLine ? ctx.statusLine : "") +
                                   "  \xc2\xb7  " + clock;
        SkFont footerFont(fRegular, 26.0f);
        footerFont.setEdging(SkFont::Edging::kAntiAlias);
        sk_sp<SkTextBlob> footerBlob = SkTextBlob::MakeFromText(
            footer.c_str(), footer.size(), footerFont, SkTextEncoding::kUTF8);
        SkPaint footerPaint;
        footerPaint.setColor(SkColorSetRGB(0x83, 0x8F, 0xA8));
        if (footerBlob) {
            ctx.canvas->drawTextBlob(footerBlob, 140, 1010, footerPaint);
        }
    }

private:
    sk_sp<SkTypeface> fRegular;
};

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

}  // namespace

TestScene::~TestScene() = default;

bool TestScene::create(std::string* error) {
    // One font setup for the whole worker (task 3.1): the engine assembles
    // the fontconfig manager, typefaces and FontCollection, and the
    // production layers lay scene text out through it too.
    fText = std::make_unique<SceneTextEngine>();
    if (!fText->create(error)) {
        return false;
    }

    std::unique_ptr<skia::textlayout::Paragraph> title =
        makeTitle(fText->fontCollection(), fText->unicode(), fText->bold(), fText->regular());
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

    // One layer per composition tab, stacked bottom to top in the order the
    // compositor will dispatch them (PLAN.md "Output layer model"). Audio is
    // not here: it is a playback source, never a composition item.
    fLayers.add(std::make_unique<BackgroundLayer>(&fActiveScene, &fImages, &fTransition));
    fLayers.add(std::make_unique<ImageLayer>(std::move(photo), fText->regular(), &fActiveScene));
    fLayers.add(
        std::make_unique<WordsLayer>(std::move(title), fText.get(), &fActiveScene, &fTransition));
    fLayers.add(std::make_unique<AuxLayer>(fText->regular()));
    fLayers.add(std::make_unique<ClockLayer>(fText->regular()));
    // Aux is not part of the agreed use case and starts off; the clock stays
    // on in this development scene as the live-frame indicator.
    fLayers.setEnabled(LayerId::kAux, false);
    return true;
}

bool TestScene::createPhoto(sk_sp<SkImage>* photo, std::string* error) {
    // A small procedural landscape, drawn on the CPU, encoded to PNG and
    // decoded again through SkCodec so the real image path is exercised.
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

namespace {

// Crossfade durations, matching FreeShow's two transition-tab defaults
// (docs/reference.md §7): text 500ms, media 800ms. A presented scene's
// words dissolve on the faster clock, its background on the slower one.
constexpr double kTextFadeSeconds = 0.5;
constexpr double kMediaFadeSeconds = 0.8;

float fadeBlend(double elapsed, double duration) {
    if (elapsed <= 0.0) {
        return 0.0f;
    }
    if (elapsed >= duration) {
        return 1.0f;
    }
    return static_cast<float>(elapsed / duration);
}

}  // namespace

void TestScene::setActiveScene(ActiveScene scene, SceneTransitionKind kind) {
    if (scene == fActiveScene) {
        return;  // already on air; a re-present of the same revision is a no-op
    }
    // A cut, or the first presentation (nothing to fade from), swaps in one
    // frame and clears any fade in flight.
    if (kind == SceneTransitionKind::kCut || !fActiveScene) {
        fActiveScene = std::move(scene);
        fTransition = SceneTransition{};
        fTransitionRunning = false;
        return;
    }
    // A fade keeps the outgoing scene alive as fFrom and starts the clock on
    // the next draw (advanceTransition), so the blend is measured in the same
    // monotonic timeSeconds the loop drives.
    fTransition.fFrom = fActiveScene;
    fTransition.fMediaBlend = 0.0f;
    fTransition.fTextBlend = 0.0f;
    fActiveScene = std::move(scene);
    fTransitionRunning = true;
    fTransitionStart = -1.0;
}

void TestScene::advanceTransition(double timeSeconds) {
    if (!fTransitionRunning) {
        return;
    }
    if (fTransitionStart < 0.0) {
        fTransitionStart = timeSeconds;  // first frame of the fade
    }
    const double elapsed = timeSeconds - fTransitionStart;
    fTransition.fTextBlend = fadeBlend(elapsed, kTextFadeSeconds);
    fTransition.fMediaBlend = fadeBlend(elapsed, kMediaFadeSeconds);
    if (fTransition.fMediaBlend >= 1.0f && fTransition.fTextBlend >= 1.0f) {
        fTransition = SceneTransition{};  // done: drop the outgoing scene
        fTransitionRunning = false;
    }
}

void TestScene::draw(SkCanvas* canvas, int widthPx, int heightPx, double timeSeconds,
                     const std::string& statusLine) {
    draw(canvas, widthPx, heightPx, timeSeconds, std::time(nullptr), statusLine);
}

void TestScene::draw(SkCanvas* canvas, int widthPx, int heightPx, double timeSeconds,
                     std::time_t wallClock, const std::string& statusLine) {
    // The swapchain surface hands out the same canvas every frame and Skia
    // keeps its matrix: without this reset the output-rule scale below
    // compounds frame after frame and the scene collapses into the top-left
    // corner (seen live 2026-10-22; CPU snapshots rendered one frame per
    // process, so they hid it).
    canvas->resetMatrix();
    canvas->clear(SK_ColorBLACK);

    // The output rule: scale the authored 1920x1080 canvas uniformly and
    // centre it, exactly like FreeShow's zoom = boxWidth / 1920.
    const float scale =
        std::min(static_cast<float>(widthPx) / kCanvasWidth,
                 static_cast<float>(heightPx) / kCanvasHeight);
    canvas->translate((static_cast<float>(widthPx) - kCanvasWidth * scale) * 0.5f,
                      (static_cast<float>(heightPx) - kCanvasHeight * scale) * 0.5f);
    canvas->scale(scale, scale);

    // Advance any crossfade in flight before dispatching, so every layer
    // this frame sees one consistent blend (steady frames pay nothing).
    advanceTransition(timeSeconds);

    // Single pass, no render-to-texture: the layer stack dispatches each
    // enabled layer into this canvas in z order and skips disabled ones.
    const FrameContext ctx{canvas, timeSeconds, wallClock, statusLine.c_str()};
    fLayers.draw(ctx);
}

bool renderTestSnapshot(const std::string& path, int width, int height,
                        const std::vector<std::pair<LayerId, bool>>& layerOverrides,
                        std::string* error) {
    TestScene scene;
    if (!scene.create(error)) {
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
    scene.draw(surface->getCanvas(), width, height, 0.0, "snapshot (CPU raster)");
    return writePngRaster(surface.get(), path, error);
}

namespace {

struct RasterFrame {
    int width = 1920;
    int height = 1080;
    std::vector<SkColor> pixels;

    bool render(TestScene& scene, std::string* error, double timeSeconds = 0.0) {
        auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));
        if (!surface) {
            if (error) {
                *error = "raster surface allocation failed";
            }
            return false;
        }
        // Fixed wall clock and status line: two frames with the same layer
        // configuration must come out pixel identical.
        scene.draw(surface->getCanvas(), width, height, timeSeconds, std::time_t{1234567890},
                   "layers");
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

bool checkLayerRegion(TestScene& scene, const RasterFrame& baseline, LayerId id,
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

bool verifyLayerComposition(std::string* error) {
    TestScene scene;
    if (!scene.create(error)) {
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
    TestScene scene;
    if (!scene.create(error)) {
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
            openshow::scene::TextItem item;
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

}  // namespace openshow::worker
