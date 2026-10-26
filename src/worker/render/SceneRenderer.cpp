/*
 * SceneRenderer: the production compositor (task 6.1b). The scene layers,
 * the transition state machine and the output-rule dispatch graduate here
 * from TestScene (review R1): this file is the audience composition path,
 * free of demo assets, snapshot helpers and assertion harnesses — those
 * stayed in the development source (render/TestScene.cpp).
 */

#include "render/SceneRenderer.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <memory>
#include <string_view>
#include <utility>

#include "include/core/SkColor.h"
#include "include/core/SkFont.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRRect.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkTextBlob.h"
#include "include/core/SkTypeface.h"
#include "include/effects/SkGradient.h"

namespace openshow::worker {

namespace {

// The authored-canvas region the aux badge draws into. --check-layers
// asserts the diff of its toggle stays inside this (widened) region.
constexpr SkRect kAuxBadge = SkRect::MakeLTRB(1560.0f, 40.0f, 1880.0f, 112.0f);

// Bottom of the stack: the presented scene's colour fill and cover-fitted
// media (still image task 3.2, video frames task 5.5). When a crossfade is
// in flight the outgoing scene draws first, then the on-air scene
// dissolves over it at the media blend — the whole background fading as
// one, with the transparency straight in the paint (no render-to-texture).
//
// Task 5.5's rule: a background that involves video CROSSES OVER ON THE
// MEDIA TRACK ONLY AS A CUT — either side being video means the media
// blend snaps to the on-air scene. Two identical video frames do not
// crossfade with themselves (a lyric change over the same background
// video just keeps rolling — the M3 continuity rule), and swapping
// between different videos (or a video and a still) cuts the picture
// rather than compositing two live GPU slots at once. The text track
// fades independently either way: the audience cross-dissolves words
// while the background plays on.
//
// With nothing presented a production renderer draws nothing — the frame's
// black clear is the start-up and clear-all picture (6.1a). Only a
// decorated development renderer paints the 1.x gradient backdrop there.
class BackgroundLayer final : public RenderLayer {
public:
    BackgroundLayer(SceneImageCache* images, const DemoDecoration* demo)
        : fImages(images), fDemo(demo) {}

    LayerId id() const override { return LayerId::kBackground; }

    void draw(const FrameContext& ctx) override {
        const SceneFrame& frame = *ctx.frame;
        if (frame.scene) {
            const bool videoInvolved =
                frame.scene->background.isVideo ||
                (frame.fadeFrom != nullptr && frame.fadeFrom->background.isVideo);
            if (frame.fadeFrom != nullptr && !videoInvolved) {
                drawSceneBackground(ctx.canvas, *frame.fadeFrom, fImages, nullptr);
                SkPaint fade;
                fade.setAlpha(static_cast<uint8_t>(frame.mediaBlend * 255.0f + 0.5f));
                drawSceneBackground(ctx.canvas, *frame.scene, fImages, &fade);
            } else {
                // Steady state, and the video media cut: on-air only,
                // full strength.
                drawSceneBackground(ctx.canvas, *frame.scene, fImages, nullptr,
                                    ctx.backgroundVideo);
            }
            return;
        }
        if (!fDemo->gradient) {
            return;  // production: the black clear stands (6.1a)
        }
        const SkPoint points[2] = {{0, 0}, {0, SceneRenderer::kCanvasHeight}};
        const SkColor4f colors[2] = {
            SkColor4f::FromColor(SkColorSetRGB(0x14, 0x16, 0x1F)),
            SkColor4f::FromColor(SkColorSetRGB(0x24, 0x2C, 0x46))};
        SkPaint background;
        background.setShader(SkShaders::LinearGradient(
            points, SkGradient{SkGradient::Colors(colors, SkTileMode::kClamp),
                               SkGradient::Interpolation{}}));
        ctx.canvas->drawRect(SkRect::MakeWH(SceneRenderer::kCanvasWidth,
                                            SceneRenderer::kCanvasHeight),
                             background);
    }

private:
    SceneImageCache* fImages;              // SceneRenderer-owned image cache
    const DemoDecoration* fDemo;           // development backdrop, if decorated
};

// Slide images (task 6.17b will bring the authored ones): a production
// renderer draws nothing until then. The development decoration's demo
// photo with its border and caption stands in, and stands down the moment
// a presentation is on air.
class ImageLayer final : public RenderLayer {
public:
    ImageLayer(const DemoDecoration* demo, sk_sp<SkTypeface> regular)
        : fDemo(demo), fRegular(std::move(regular)) {}

    LayerId id() const override { return LayerId::kImages; }

    void draw(const FrameContext& ctx) override {
        const sk_sp<SkImage>& photo = fDemo->photo;
        if (ctx.frame->scene || !photo) {
            return;
        }
        const float photoWidth = 820.0f;
        const float photoHeight =
            photoWidth * static_cast<float>(photo->height()) / static_cast<float>(photo->width());
        const SkRect photoRect = SkRect::MakeXYWH(140, 250, photoWidth, photoHeight);
        ctx.canvas->drawImageRect(photo,
                                  SkRect::MakeIWH(photo->width(), photo->height()), photoRect,
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
    const DemoDecoration* fDemo;   // development photo, if decorated
    sk_sp<SkTypeface> fRegular;
    const ActiveScene* fActive;    // SceneRenderer-owned; outlives the layer
};

// Words: while a scene is on air, its text laid out through
// SceneTextEngine (wrap, centre, fallback, fit — task 3.1); otherwise the
// decoration's demo title, if this renderer was decorated for development.
// Layouts are cached per scene pointer (two slots, so a crossfade never
// re-lays-out either side); during the fade the two scenes dissolve —
// outgoing at 1 - textBlend, incoming at textBlend, the classic lyric
// cross-dissolve (docs/reference.md §7 text transition).
class WordsLayer final : public RenderLayer {
public:
    WordsLayer(const DemoDecoration* demo, const SceneTextEngine* engine)
        : fDemo(demo), fEngine(engine) {}

    LayerId id() const override { return LayerId::kWords; }

    void draw(const FrameContext& ctx) override {
        const SceneFrame& frame = *ctx.frame;
        if (frame.scene) {
            const bool fading = frame.fadeFrom != nullptr;
            if (fading) {
                const SceneTextEngine::SceneLayout& from = layoutFor(&fSlotA, frame.fadeFrom);
                SkPaint fade;
                fade.setAlpha(static_cast<uint8_t>((1.0f - frame.textBlend) * 255.0f + 0.5f));
                fEngine->drawLayout(ctx.canvas, from, &fade);
            }
            const SceneTextEngine::SceneLayout& current = layoutFor(&fSlotB, frame.scene);
            if (fading) {
                SkPaint fade;
                fade.setAlpha(static_cast<uint8_t>(frame.textBlend * 255.0f + 0.5f));
                fEngine->drawLayout(ctx.canvas, current, &fade);
            } else {
                fEngine->drawLayout(ctx.canvas, current);
            }
            return;
        }
        if (fDemo->titlePaint) {
            fDemo->titlePaint(ctx.canvas);
        }
    }

private:
    // One layout cache slot. A frame carries at most two scenes (on air and
    // fading out), so a two-slot cache (this struct twice) covers every
    // frame of a fade without rebuilding a layout. The key is the scene
    // itself, so two targets drawing the same captured frame share the work
    // and any frame that really changed rebuilds it.
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

    const DemoDecoration* fDemo;             // development title, if decorated
    const SceneTextEngine* fEngine;
    LayoutSlot fSlotA;                       // holds the outgoing scene during a fade
    LayoutSlot fSlotB;                       // holds the on-air scene
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

// Clock: the audience clock (docs/reference.md §7: this is FreeShow's
// clock slot) with the development diagnostics line prefixed. Production
// starts it disabled and passes the empty status line, so the footer
// region stays scene pixels; an operator who calls the clock up sees the
// time, never this process's device or frame numbers.
class ClockLayer final : public RenderLayer {
public:
    explicit ClockLayer(sk_sp<SkTypeface> regular) : fRegular(std::move(regular)) {}

    LayerId id() const override { return LayerId::kClock; }

    void draw(const FrameContext& ctx) override {
        char clock[16] = "";
        std::tm local{};
        if (localtime_r(&ctx.frame->wallClock, &local) != nullptr) {
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

SceneRenderer::~SceneRenderer() = default;

bool SceneRenderer::create(std::string* error, const DemoBuilder& demo) {
    // One font setup for the whole renderer (task 3.1): the engine
    // assembles the fontconfig manager, typefaces and FontCollection; the
    // words layer lays scene text out through it, and a development
    // decoration builds its demo title through it too.
    fText = std::make_unique<SceneTextEngine>();
    if (!fText->create(error)) {
        return false;
    }

    // Production creates nothing else: no demo photo or title exists, so
    // the renderer cannot fall back onto development content.
    if (demo && !demo(*fText, &fDemo, error)) {
        return false;
    }

    // One layer per composition tab, stacked bottom to top in the order
    // the compositor will dispatch them (PLAN.md "Output layer model").
    // Audio is not here: it is a playback source, never a composition
    // item.
    fLayers.add(std::make_unique<BackgroundLayer>(&fImages, &fDemo));
    fLayers.add(std::make_unique<ImageLayer>(&fDemo, fText->regular()));
    fLayers.add(std::make_unique<WordsLayer>(&fDemo, fText.get()));
    fLayers.add(std::make_unique<AuxLayer>(fText->regular()));
    fLayers.add(std::make_unique<ClockLayer>(fText->regular()));
    // Aux is not part of the agreed use case and starts off in both
    // compositions. The clock is off for the audience (6.1a, review R1:
    // the RPC layer toggle remains the operator's way to call it up); the
    // development picture brings it back as the 1.x live-frame indicator.
    fLayers.setEnabled(LayerId::kAux, false);
    fLayers.setEnabled(LayerId::kClock, !production());
    return true;
}

void SceneRenderer::setActiveScene(ActiveScene scene, SceneTransitionKind kind) {
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
    // A fade keeps the outgoing scene alive as fFrom and starts its clock on
    // the next captureFrame, so the blend is measured in the same monotonic
    // timeSeconds the audience loop drives — and by nothing else.
    fTransition.fFrom = fActiveScene;
    fTransition.fStartSeconds = -1.0;
    fActiveScene = std::move(scene);
    fTransitionRunning = true;
}

SceneFrame SceneRenderer::captureFrame(double timeSeconds, std::uint64_t revision,
                                       std::time_t wallClock) {
    SceneFrame frame;
    frame.scene = fActiveScene;
    frame.timeSeconds = timeSeconds;
    frame.wallClock = wallClock != 0 ? wallClock : std::time(nullptr);
    frame.revision = revision;
    if (fTransition.fFrom == nullptr) {
        return frame;  // steady state: the on-air scene, at full strength
    }
    if (fTransition.fStartSeconds < 0.0) {
        fTransition.fStartSeconds = timeSeconds;  // first frame of the fade
    }
    const double elapsed = timeSeconds - fTransition.fStartSeconds;
    frame.mediaBlend = fadeBlend(elapsed, kMediaFadeSeconds);
    frame.textBlend = fadeBlend(elapsed, kTextFadeSeconds);
    if (frame.mediaBlend >= 1.0f && frame.textBlend >= 1.0f) {
        // The fade's last blend: this frame is the on-air scene alone. The
        // state machine keeps the outgoing scene until commitFrame() credits
        // this frame with reaching the panel, so a frame that is captured
        // and then lost (backpressure, a failed submit) leaves the loop
        // presenting until the settled picture actually arrives.
        return frame;
    }
    frame.fadeFrom = fTransition.fFrom;
    return frame;
}

void SceneRenderer::commitFrame(const SceneFrame& frame) {
    if (fTransition.fFrom == nullptr) {
        return;  // nothing in flight (or already retired by an earlier commit)
    }
    // Only the frame that fades TO the scene now on air may retire it: a
    // stale capture being credited after a newer present must not end the
    // fade that newer present started.
    if (frame.scene == fActiveScene && frame.mediaBlend >= 1.0f && frame.textBlend >= 1.0f) {
        fTransition = SceneTransition{};  // done: drop the outgoing scene
        fTransitionRunning = false;
    }
}

void SceneRenderer::draw(const SceneFrame& frame, SkCanvas* canvas, int widthPx, int heightPx,
                         const std::string& statusLine, const SkImage* backgroundVideo) {
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

    // Single pass, no render-to-texture: the layer stack dispatches each
    // enabled layer into this canvas in z order and skips disabled ones.
    // Everything the layers composite comes from the captured frame, which
    // this call only reads (task 6.11a): the second, third and hundredth
    // target of a frame draw the picture the audience was owed, and leave
    // the renderer's own state exactly as they found it.
    const FrameContext ctx{canvas, &frame, statusLine.c_str(), backgroundVideo};
    fLayers.draw(ctx);
}

void SceneRenderer::drawLive(SkCanvas* canvas, int widthPx, int heightPx, double timeSeconds,
                             std::time_t wallClock, const std::string& statusLine,
                             const SkImage* backgroundVideo) {
    const SceneFrame frame = captureFrame(timeSeconds, 0, wallClock);
    draw(frame, canvas, widthPx, heightPx, statusLine, backgroundVideo);
    commitFrame(frame);
}

}  // namespace openshow::worker
