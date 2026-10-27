/*
 * Production scene text layout — see SceneText.h.
 *
 * The font assembly (fontconfig + FreeType scanner + named-family fallbacks)
 * and the SkUnicode backend graduate here from TestScene (task 3.1): the
 * engine is the single font setup the worker has, so a fixture rendered
 * through layoutScene()/renderSceneSnapshot exercises exactly what the
 * audience layers draw. The Unicode provider is the public ICU-backed
 * module since task 6.2b — the homemade hard-coded backend and the
 * SkUnicodes::ICU::Make() compatibility shim are gone with review R9.
 */

#include "render/SceneText.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>

#include "include/codec/SkCodec.h"
#include "include/core/SkBitmap.h"
#include "include/core/SkBlurTypes.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkData.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkImage.h"
#include "include/core/SkMaskFilter.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkRect.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkShader.h"
#include "include/core/SkSurface.h"
#include "include/core/SkTextBlob.h"
#include "include/encode/SkPngEncoder.h"
#include "include/ports/SkFontMgr_fontconfig.h"
#include "include/ports/SkFontScanner_FreeType.h"
#include "modules/skparagraph/include/FontCollection.h"
#include "modules/skparagraph/include/ParagraphBuilder.h"
#include "modules/skparagraph/include/ParagraphPainter.h"
#include "modules/skparagraph/include/ParagraphStyle.h"
#include "modules/skparagraph/include/TextStyle.h"
#include "modules/skunicode/include/SkUnicode.h"
#include "modules/skunicode/include/SkUnicode_icu.h"

namespace openshow::worker {

namespace {

// The fallback line box: a SceneItem that carries no line spacing (a
// hand-built item, or a NaN the wire would have refused) lays out at the
// domain's default, so it looks exactly as scenes did before task 4.4.
constexpr float kDefaultLineSpacing =
    static_cast<float>(openshow::domain::kDefaultLineSpacing);

}  // namespace

namespace {

sk_sp<SkTypeface> matchTypeface(SkFontMgr* mgr, const char* family, const SkFontStyle& style,
                                std::initializer_list<const char*> fallbackFiles) {
    if (mgr) {
        sk_sp<SkTypeface> typeface = mgr->matchFamilyStyle(family, style);
        if (typeface) {
            return typeface;
        }
        for (const char* path : fallbackFiles) {
            typeface = mgr->makeFromFile(path);
            if (typeface) {
                return typeface;
            }
        }
    }
    // Last resort before giving up: load the file through an empty manager.
    sk_sp<SkFontMgr> loader = SkFontMgr::RefEmpty();
    for (const char* path : fallbackFiles) {
        sk_sp<SkData> data = SkData::MakeFromFileName(path);
        if (data) {
            sk_sp<SkTypeface> typeface = loader->makeFromData(std::move(data));
            if (typeface) {
                return typeface;
            }
        }
    }
    return nullptr;
}

}  // namespace

SceneTextEngine::SceneTextEngine() = default;
SceneTextEngine::~SceneTextEngine() = default;

bool SceneTextEngine::create(std::string* error) {
    // System fonts through fontconfig with the internal FreeType scanner.
    // SkFontMgr::RefDefault() is gone from this Skia revision, so the manager
    // is assembled explicitly.
    std::unique_ptr<SkFontScanner> scanner = SkFontScanner_Make_FreeType();
    if (scanner) {
        fFontMgr = SkFontMgr_New_FontConfig(nullptr, std::move(scanner));
    }
    if (!fFontMgr) {
        fFontMgr = SkFontMgr::RefEmpty();
        if (error) {
            error->append("fontconfig font manager unavailable, using fallbacks; ");
        }
    }

    fRegular = matchTypeface(fFontMgr.get(), "Liberation Sans", SkFontStyle::Normal(),
                             {"/usr/share/fonts/liberation-sans-fonts/LiberationSans-Regular.ttf",
                              "/usr/share/fonts/dejavu/DejaVuSans.ttf"});
    fBold = matchTypeface(fFontMgr.get(), "Liberation Sans", SkFontStyle::Bold(),
                          {"/usr/share/fonts/liberation-sans-fonts/LiberationSans-Bold.ttf",
                           "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf"});
    if (!fRegular) {
        // fontconfig default sans when the named matches both failed.
        fRegular = fFontMgr->matchFamilyStyle(nullptr, SkFontStyle::Normal());
    }
    if (!fBold) {
        fBold = fRegular;
    }
    if (!fRegular) {
        if (error) {
            *error = "no usable typeface";
        }
        return false;
    }

    fFontCollection = sk_make_sp<skia::textlayout::FontCollection>();
    fFontCollection->setDynamicFontManager(fFontMgr);
    // Real Unicode: UAX #14 line breaking, UAX #29 grapheme clustering and
    // UBA bidi from the public ICU-backed module (task 6.2b). The worker's
    // Skia is built with skia_use_system_icu=true, so libskunicode_icu and
    // the system libicuuc agree on the versioned symbols.
    fUnicode = SkUnicodes::ICU::Make();
    if (!fUnicode) {
        if (error) {
            *error = "SkUnicode ICU backend unavailable";
        }
        return false;
    }
    return true;
}

namespace {

float effectiveSize(const openshow::scene::SceneItem& item, float scale) {
    const float base =
        item.fontSize > 0.0 ? static_cast<float>(item.fontSize) : kSceneDefaultFontSize;
    return base * scale;
}

}  // namespace

SceneTextEngine::LaidOutItem SceneTextEngine::layout(const openshow::scene::SceneItem& item,
                                                     float scale, float maxWidth) const {
    LaidOutItem out;
    out.fontSize = effectiveSize(item, scale);

    std::string joined;
    for (std::size_t i = 0; i < item.lines.size(); ++i) {
        if (i > 0) {
            joined.push_back('\n');
        }
        joined += item.lines[i];
    }
    if (item.lines.empty()) {
        return out;  // hasText stays false; nothing to draw
    }
    out.hasText = true;

    skia::textlayout::ParagraphStyle paragraphStyle;
    // Where the lines sit inside the box (task 4.4); anything unknown
    // falls back to centred, the contract both the domain and the wire
    // enforce, so this only guards a hand-built SceneItem.
    skia::textlayout::TextAlign align = skia::textlayout::TextAlign::kCenter;
    if (item.align == "left") {
        align = skia::textlayout::TextAlign::kLeft;
    } else if (item.align == "right") {
        align = skia::textlayout::TextAlign::kRight;
    }
    paragraphStyle.setTextAlign(align);
    paragraphStyle.setTextDirection(skia::textlayout::TextDirection::kLtr);

    skia::textlayout::TextStyle textStyle;
    textStyle.setFontSize(out.fontSize);
    // Explicit line box so stanza rhythm does not depend on which font the
    // platform resolves. This fork's setHeight is a *multiplier* of the font
    // size (Run::calculateMetrics: runHeight = multiplier * fontSize) with
    // half-leading so text sits centred in its box — verified by probe. The
    // multiplier is the item's line spacing (task 4.4), clamped to the band
    // the layout assumes so a rogue value cannot collapse or explode a box.
    float lineSpacing = static_cast<float>(item.lineSpacing);
    if (!(lineSpacing >= 0.25f)) {  // also catches NaN
        lineSpacing = kDefaultLineSpacing;
    } else if (lineSpacing > 8.0f) {
        lineSpacing = 8.0f;
    }
    textStyle.setHeight(lineSpacing);
    textStyle.setHeightOverride(true);
    textStyle.setHalfLeading(true);
    textStyle.setColor(SkColorSetARGB(0xFF, (item.color >> 16) & 0xFF, (item.color >> 8) & 0xFF,
                                     item.color & 0xFF));
    // Named first (if any), our default family second: FontCollection
    // resolves names through fontconfig, and per-run fallback covers glyphs
    // the named family lacks.
    std::vector<SkString> families;
    if (!item.fontFamily.empty()) {
        families.emplace_back(item.fontFamily.c_str());
    }
    SkString defaultFamily;
    fRegular->getFamilyName(&defaultFamily);
    if (defaultFamily.size() > 0) {
        families.push_back(defaultFamily);
    }
    textStyle.setFontFamilies(std::move(families));
    paragraphStyle.setTextStyle(textStyle);

    auto builder = skia::textlayout::ParagraphBuilder::make(paragraphStyle, fFontCollection,
                                                            fUnicode);
    builder->pushStyle(textStyle);
    builder->addText(joined.data(), joined.size());
    out.paragraph = builder->Build();
    if (!out.paragraph) {
        out.hasText = false;
        return out;
    }
    out.paragraph->layout(maxWidth);
    out.height = out.paragraph->getHeight();
    out.lineCount = static_cast<int>(out.paragraph->lineNumber());
    return out;
}

SceneTextEngine::SceneLayout SceneTextEngine::layoutScene(
    const openshow::scene::Scene& scene) const {
    SceneLayout out;
    out.items.reserve(scene.items.size());
    for (const auto& item : scene.items) {
        // The authored box. A hand-built item with a degenerate box is
        // clamped to one pixel rather than allowed to loop the fit pass.
        const float boxX = static_cast<float>(item.x);
        const float boxY = static_cast<float>(item.y);
        const float boxWidth = std::max(static_cast<float>(item.width), 1.0f);
        const float boxHeight = std::max(static_cast<float>(item.height), 1.0f);

        if (item.isImage) {
            // A foreground picture (task 6.17b): no shaping and no fit
            // loop — the authored box is where the picture is stretched,
            // so layout contributes only the shared degenerate-box clamp.
            // It stays IN this list at its stack position, which is what
            // makes draw order survive layout untouched.
            LaidOutItem laid;
            laid.isImage = true;
            laid.assetPath = item.assetPath;
            laid.boxX = boxX;
            laid.boxY = boxY;
            laid.boxWidth = boxWidth;
            laid.boxHeight = boxHeight;
            out.items.push_back(std::move(laid));
            continue;
        }

        // Per-item shrinkToFit (FreeShow's textFit): wrapping changes with
        // the scale, so height is not linear in it — lay out, shrink,
        // re-lay out. Three shrinks converge long before the cap; the
        // floor keeps even an impossible box readable (kSceneMinFontSize).
        const float base = effectiveSize(item, 1.0f);
        const float minScale = base > 0.0f ? kSceneMinFontSize / base : 1.0f;
        float scale = 1.0f;
        for (int pass = 0;; ++pass) {
            LaidOutItem laid = layout(item, scale, boxWidth);
            laid.boxX = boxX;
            laid.boxY = boxY;
            laid.boxWidth = boxWidth;
            laid.boxHeight = boxHeight;
            const bool fits = !laid.hasText || laid.height <= boxHeight;
            const float next =
                fits || pass >= 3 ? scale : std::max(scale * boxHeight / laid.height, minScale);
            const bool keep = next >= scale;  // done, or the floor stopped the shrink
            scale = next;
            if (keep) {
                out.items.push_back(std::move(laid));
                break;
            }
        }
    }
    return out;
}

namespace {

// The fade's dissolve, applied DURING text drawing (task 6.11b, review R13).
//
// This fork's `Paragraph::paint(SkCanvas*, x, y)` takes no paint, so the
// obvious way to dissolve a block of words was a `saveLayer()` over the boxes
// and the alpha on the layer. That is an intermediate render target in the
// middle of the composition pass — the rule this renderer has held since M1
// ("one pass, no render-to-texture") — and a full-canvas allocation every
// frame of every fade. `Paragraph::paint(ParagraphPainter*, x, y)` is the
// public way in: the paragraph hands every draw it makes to a painter of the
// caller's choosing, so the alpha rides each draw instead of a target behind
// it. Everything here is the canvas call the stock painter would have made,
// with the alpha folded into the paint first.
//
// What that changes, measurably: where two glyphs' antialiased coverage
// overlaps (adjacent lines, tight leading), a group layer attenuates the
// second coverage by the first in full while paint-time alpha attenuates it
// by the first *scaled by the fade*, so a mid-fade frame can read a few
// units brighter on edge pixels — measured at worst 10/255 with three
// overlapping lines, and within rounding (1/255) wherever coverage does not
// overlap. Ink placement, weight and colour are untouched.
class GroupAlphaPainter final : public skia::textlayout::ParagraphPainter {
public:
    GroupAlphaPainter(SkCanvas* canvas, uint8_t alpha) : fCanvas(canvas), fAlpha(alpha) {}

    void drawTextBlob(const sk_sp<SkTextBlob>& blob, SkScalar x, SkScalar y,
                      const SkPaintOrID& paint) override {
        if (!holdsSkPaint(paint)) {
            return;
        }
        fCanvas->drawTextBlob(blob, x, y, faded(std::get<SkPaint>(paint)));
    }

    void drawTextShadow(const sk_sp<SkTextBlob>& blob, SkScalar x, SkScalar y, SkColor color,
                        SkScalar blurSigma) override {
        SkPaint paint;
        paint.setColor(scaled(color));
        if (blurSigma != 0.0f) {
            paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, blurSigma, false));
        }
        fCanvas->drawTextBlob(blob, x, y, paint);
    }

    void drawRect(const SkRect& rect, const SkPaintOrID& paint) override {
        if (!holdsSkPaint(paint)) {
            return;
        }
        fCanvas->drawRect(rect, faded(std::get<SkPaint>(paint)));
    }

    void drawFilledRect(const SkRect& rect, const DecorationStyle& style) override {
        SkPaint paint = faded(style.skPaint());
        paint.setStroke(false);
        fCanvas->drawRect(rect, paint);
    }

    void drawPath(const SkPath& path, const DecorationStyle& style) override {
        fCanvas->drawPath(path, faded(style.skPaint()));
    }

    void drawLine(SkScalar x0, SkScalar y0, SkScalar x1, SkScalar y1,
                  const DecorationStyle& style) override {
        fCanvas->drawLine(x0, y0, x1, y1, faded(style.skPaint()));
    }

    void clipRect(const SkRect& rect) override { fCanvas->clipRect(rect); }
    void translate(SkScalar dx, SkScalar dy) override { fCanvas->translate(dx, dy); }
    void save() override { fCanvas->save(); }
    void restore() override { fCanvas->restore(); }

private:
    // Registered paints are the Flutter-side flow (a PaintID the painter owns).
    // We never register one — every style this engine builds carries its own
    // SkPaint — so a variant that is not an SkPaint has no colour to draw with.
    static bool holdsSkPaint(const SkPaintOrID& paint) {
        return std::holds_alternative<SkPaint>(paint);
    }

    // alpha' = round(alpha * fade / 255): the same scale for every channel of
    // an alpha value, with the +127 so a full-strength draw stays full.
    static uint8_t scaled(int alpha, uint8_t fade) {
        return static_cast<uint8_t>((alpha * fade + 127) / 255);
    }

    SkColor scaled(SkColor color) const {
        return SkColorSetARGB(scaled(SkColorGetA(color), fAlpha), SkColorGetR(color),
                              SkColorGetG(color), SkColorGetB(color));
    }

    SkPaint faded(SkPaint paint) const {
        paint.setAlpha(scaled(static_cast<int>(paint.getAlpha()), fAlpha));
        return paint;
    }

    SkCanvas* fCanvas;
    uint8_t fAlpha;
};

}  // namespace

void SceneTextEngine::drawLayout(SkCanvas* canvas, const SceneLayout& layout,
                                 const SkPaint* paint, SceneImageCache* images) const {
    // Centred: the paragraph aligns its lines inside boxWidth, and the block
    // sits at the box's vertical middle.
    const auto centerY = [](const LaidOutItem& item) {
        return item.boxY + std::max(0.0f, (item.boxHeight - item.height) * 0.5f);
    };
    // A foreground picture (task 6.17b) draws beside the paragraphs, in the
    // stack's array order, so the composition honours the order the operator
    // authored: later items over earlier ones, either kind. LOOKUP ONLY —
    // the cache rule since 6.10: a picture the session has not decoded is
    // simply not drawn and the frame shows what is under it rather than
    // paying for a disk read at frame rate. The fade's alpha rides the paint
    // (no render-to-texture, same rule as the text above and the background
    // below), so a crossfading stack dissolves its words and pictures as
    // one. A null cache means "no pictures here": the dev paths that build
    // layouts without a renderer still draw every text item exactly as
    // before.
    const auto drawImageItem = [&](const LaidOutItem& item, std::uint8_t alpha) {
        if (images == nullptr) {
            return;
        }
        const sk_sp<SkImage> image = images->imageFor(item.assetPath);
        if (image == nullptr) {
            return;
        }
        SkPaint imagePaint;
        imagePaint.setAlpha(alpha);
        canvas->drawImageRect(image, SkRect::MakeIWH(image->width(), image->height()),
                              SkRect::MakeXYWH(item.boxX, item.boxY, item.boxWidth,
                                               item.boxHeight),
                              SkSamplingOptions(SkFilterMode::kLinear), &imagePaint,
                              SkCanvas::kFast_SrcRectConstraint);
    };
    if (paint == nullptr || paint->getAlpha() >= 255) {
        for (const auto& item : layout.items) {
            if (item.isImage) {
                drawImageItem(item, 255);
            } else if (item.paragraph && item.hasText) {
                item.paragraph->paint(canvas, item.boxX, centerY(item));
            }
        }
        return;
    }
    GroupAlphaPainter painter(canvas, paint->getAlpha());
    for (const auto& item : layout.items) {
        if (item.isImage) {
            drawImageItem(item, paint->getAlpha());
        } else if (item.paragraph && item.hasText) {
            item.paragraph->paint(&painter, item.boxX, centerY(item));
        }
    }
}

namespace {

// The one decode both entry points share (task 6.7c): file -> SkCodec ->
// premul bitmap. `error`, when asked for, names the reason a null means
// "no picture" — file unreadable, not an image, or a codec that gave up.
//
// Task 6.10 (review R12) added the two gates that stand BEFORE any
// allocation: the file's own size (from its stat, so a 2 GB file is
// refused without being read) and the dimensions the header declares
// (from the codec's encoded info, so a 20000x20000 PNG is refused on its
// numbers rather than by an allocation that may or may not fail and, when
// it succeeds, leaves the machine 1.6 GB poorer).
//
// Task 6.16b added the optional cap. `maxW`/`maxH` of 0 mean "native";
// otherwise the caller gets an image that FITS that box, no larger. The two
// gates above still judge the file's declared numbers, so a 20000x20000 PNG
// is still an incident and not a thumbnail. Getting inside the box is a
// two-step: the codec is asked for the closest size it can produce natively
// (cheap, and exact for JPEG), and anything still over the box afterwards —
// which is every PNG, since PNG has no scaled mode — is resampled here. The
// caller therefore never has to resize, and, more importantly, never has to
// CACHE a picture larger than the one it asked for.
sk_sp<SkImage> decodeImageFile(const std::string& path, std::string* error,
                               std::uint32_t maxW = 0, std::uint32_t maxH = 0) {
    auto fail = [&](std::string message) -> sk_sp<SkImage> {
        if (error) {
            *error = std::move(message);
        }
        return nullptr;
    };
    std::error_code ec;
    const auto fileSize = std::filesystem::file_size(path, ec);
    if (ec) {
        return fail("cannot open '" + path + "' for reading");
    }
    if (fileSize > SceneImageCache::kMaxEncodedFileBytes) {
        return fail("'" + path + "' is " + std::to_string(fileSize / (1024u * 1024u)) +
                    " MiB, over the " +
                    std::to_string(SceneImageCache::kMaxEncodedFileBytes / (1024u * 1024u)) +
                    " MiB asset file limit");
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return fail("cannot open '" + path + "' for reading");
    }
    const std::string bytes((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
    if (bytes.empty()) {
        return fail("'" + path + "' is empty");
    }
    sk_sp<SkData> data = SkData::MakeWithCopy(bytes.data(), bytes.size());
    auto codec = SkCodec::MakeFromData(std::move(data));
    if (!codec) {
        return fail("'" + path + "' is not a readable image");
    }
    const SkISize encoded = codec->getInfo().dimensions();
    if (encoded.width() > SceneImageCache::kMaxImageDimension ||
        encoded.height() > SceneImageCache::kMaxImageDimension) {
        return fail("image is " + std::to_string(encoded.width()) + "x" +
                    std::to_string(encoded.height()) + ", over the " +
                    std::to_string(SceneImageCache::kMaxImageDimension) +
                    " pixel dimension limit");
    }
    const std::uint64_t pixels =
        static_cast<std::uint64_t>(encoded.width()) * static_cast<std::uint64_t>(encoded.height());
    if (pixels > SceneImageCache::kMaxDecodedPixels) {
        return fail("image is " + std::to_string(encoded.width()) + "x" +
                    std::to_string(encoded.height()) + ", over the " +
                    std::to_string(SceneImageCache::kMaxDecodedPixels / 1000000u) +
                    " megapixel limit");
    }
    const SkImageInfo full = codec->getInfo().makeColorType(kN32_SkColorType)
                                 .makeAlphaType(kPremul_SkAlphaType);
    SkISize wanted = full.dimensions();
    if (maxW > 0 && maxH > 0 &&
        (wanted.width() > static_cast<int>(maxW) || wanted.height() > static_cast<int>(maxH))) {
        // The codec's own suggestion for the nearest scale it can do
        // natively, which is the cheap half of the box. It may still
        // overshoot: a JPEG decodes at 1/2, 1/4, 1/8 and lands a step above
        // the box, a PNG declines to scale at all. The resample below is what
        // makes the box a promise rather than a preference, so taking the
        // codec's offer here is purely about how much memory the decode has
        // to touch on its way to that.
        const float scale =
            std::min(static_cast<float>(maxW) / static_cast<float>(wanted.width()),
                     static_cast<float>(maxH) / static_cast<float>(wanted.height()));
        const SkISize suggested = codec->getScaledDimensions(scale);
        if (suggested.width() > 0 && suggested.height() > 0) {
            wanted = suggested;
        }
    }
    const SkImageInfo info = full.makeWH(wanted.width(), wanted.height());
    SkBitmap bitmap;
    if (!bitmap.tryAllocPixels(info)) {
        return fail("no pixels for a " + std::to_string(info.width()) + "x" +
                    std::to_string(info.height()) + " image");
    }
    const SkCodec::Result result = codec->getPixels(info, bitmap.getPixels(), bitmap.rowBytes());
    if (result != SkCodec::kSuccess) {
        return fail("image decode failed (SkCodec result " + std::to_string(result) + ")");
    }
    sk_sp<SkImage> image = SkImages::RasterFromBitmap(bitmap);
    if (image == nullptr) {
        return fail("the decoded pixels would not become an image");
    }
    // THE SECOND HALF OF THE CAP, and the part a codec cannot give you.
    // `getScaledDimensions` answers with the nearest scale the codec does
    // natively — for a PNG that is its native size, because PNG has no
    // scaled mode at all. A 1600x900 backdrop asked for as a 160x90 picture
    // would then be CACHED at 5.8 MB, and a slide list's whole point is that
    // thirty of those are held at once. So the box is enforced here rather
    // than suggested: any decode that is still too big is resampled into
    // raster pixels at the box's aspect, and that is what the caller keeps.
    // The transient native decode is not wasted work the gates did not price:
    // it is bounded by the same file-size, dimension and megapixel ceilings
    // every other decode answers to, and it is freed on the way out.
    if (maxW > 0 && maxH > 0 &&
        (image->width() > static_cast<int>(maxW) || image->height() > static_cast<int>(maxH))) {
        const float scale =
            std::min(static_cast<float>(maxW) / static_cast<float>(image->width()),
                     static_cast<float>(maxH) / static_cast<float>(image->height()));
        const int boxW = std::max(1, static_cast<int>(image->width() * scale));
        const int boxH = std::max(1, static_cast<int>(image->height() * scale));
        const SkImageInfo small = full.makeWH(boxW, boxH);
        sk_sp<SkSurface> surface = SkSurfaces::Raster(small);
        if (surface == nullptr) {
            // No resample, and the oversized picture must not be kept under a
            // caller that asked for a bounded one: this is a failure, honestly
            // reported, not a big entry that happens to work.
            return fail("no pixels to scale a " + std::to_string(boxW) + "x" +
                        std::to_string(boxH) + " image into");
        }
        surface->getCanvas()->clear(SK_ColorTRANSPARENT);
        surface->getCanvas()->drawImageRect(image, SkRect::MakeIWH(boxW, boxH),
                                            SkSamplingOptions(SkFilterMode::kLinear));
        sk_sp<SkImage> scaled = surface->makeImageSnapshot();
        if (scaled == nullptr) {
            return fail("the scaled image could not be made");
        }
        image = std::move(scaled);
    }
    if (error) {
        error->clear();
    }
    return image;
}

// Decoded pixel bytes an entry accounts for: the N32 raster a load
// allocates. A remembered failure accounts for nothing.
std::size_t imageBytes(const sk_sp<SkImage>& image) {
    if (image == nullptr) {
        return 0;
    }
    const SkImageInfo info = image->imageInfo();
    const std::size_t rowBytes =
        static_cast<std::size_t>(info.width()) * static_cast<std::size_t>(info.bytesPerPixel());
    return rowBytes * static_cast<std::size_t>(info.height());
}

}  // namespace

sk_sp<SkImage> SceneImageCache::decodeAtMost(const std::string& path, std::uint32_t maxW,
                                             std::uint32_t maxH, std::string* error) {
    return decodeImageFile(path, error, maxW, maxH);
}

sk_sp<SkImage> SceneImageCache::install(const std::string& path, sk_sp<SkImage> image) {
    // The loader's way in for pixels a lookup could not have produced (a
    // scaled thumbnail decode). Accounting is the load's accounting — the
    // bytes count against this cache's budget, a null is a remembered
    // failure the draw path will not retry at frame rate — and the new
    // entry is the one eviction is told to leave alone, so installing a
    // picture that is about to be drawn cannot forget itself first.
    const std::size_t bytes = imageBytes(image);
    std::lock_guard<std::mutex> lock(fMutex);
    const auto existing = fEntries.find(path);
    if (existing != fEntries.end()) {
        fBytes -= existing->second.bytes;
        fEntries.erase(existing);
    }
    Entry& entry = fEntries[path];
    entry.image = image;
    entry.bytes = bytes;
    entry.seen = ++fClock;
    fBytes += bytes;
    evictLocked(path);
    return image;
}

sk_sp<SkImage> SceneImageCache::imageFor(const std::string& path) {
    // Pure lookup — the draw path's entry point (task 6.10, review R12).
    // A miss answers null and the colour fill stands; nothing here opens a
    // file, so the worst a cold or evicted asset can cost the frame loop
    // is a mutex and a map probe.
    std::lock_guard<std::mutex> lock(fMutex);
    const auto existing = fEntries.find(path);
    if (existing == fEntries.end()) {
        return nullptr;
    }
    existing->second.seen = ++fClock;
    return existing->second.image;
}

bool SceneImageCache::has(const std::string& path) {
    std::lock_guard<std::mutex> lock(fMutex);
    const auto existing = fEntries.find(path);
    return existing != fEntries.end() && existing->second.image != nullptr;
}

sk_sp<SkImage> SceneImageCache::loadFor(const std::string& path) {
    // The prefetch is refreshFor without the error report: neither ever
    // re-decodes a SUCCESS, and both treat anything else — a remembered
    // failure, or the empty entry a pin created before its load — as not
    // yet answered. loadFor once returned the empty placeholder as if it
    // were one, which made a pin-before-load (exactly what prepare does)
    // poison its own prefetch; that is why the two share this one body.
    return refreshFor(path, nullptr);
}

sk_sp<SkImage> SceneImageCache::refreshFor(const std::string& path, std::string* error) {
    // Always decode a FAILURE or an empty (pin-only) entry, then let a
    // success overwrite it. A cached SUCCESS is returned without
    // re-decoding: the bytes are the same file, and re-decoding a 4K still
    // on every prepare would defeat the preload this path exists to do.
    {
        std::lock_guard<std::mutex> lock(fMutex);
        const auto existing = fEntries.find(path);
        if (existing != fEntries.end() && existing->second.image != nullptr) {
            if (error) {
                error->clear();
            }
            return existing->second.image;
        }
    }
    sk_sp<SkImage> image = decodeImageFile(path, error);
    std::lock_guard<std::mutex> lock(fMutex);
    const auto existing = fEntries.find(path);
    if (existing == fEntries.end()) {
        Entry entry;
        entry.image = image;
        entry.bytes = imageBytes(image);
        entry.seen = ++fClock;
        fBytes += entry.bytes;
        const auto inserted = fEntries.emplace(path, std::move(entry)).first;
        const sk_sp<SkImage> kept = inserted->second.image;
        evictLocked(path);
        return kept;
    }
    if (image != nullptr && existing->second.image == nullptr) {
        existing->second.image = image;  // repaired: the draw path picks it up now
        existing->second.bytes = imageBytes(image);
        fBytes += existing->second.bytes;  // the failure placeholder weighed nothing
        const sk_sp<SkImage> kept = existing->second.image;
        evictLocked(path);
        return kept;
    }
    return existing->second.image;
}

void SceneImageCache::pin(const std::string& path) {
    std::lock_guard<std::mutex> lock(fMutex);
    ++fEntries[path].pins;  // a pin may precede the load: prepare then prefetch
    ++fPins;
}

void SceneImageCache::unpin(const std::string& path) {
    std::lock_guard<std::mutex> lock(fMutex);
    const auto existing = fEntries.find(path);
    if (existing != fEntries.end()) {
        if (existing->second.pins > 0) {
            --existing->second.pins;
        }
        if (existing->second.image == nullptr && existing->second.pins == 0) {
            fEntries.erase(existing);  // an unpinned failure is pure weight
        }
    }
    if (fPins > 0) {
        --fPins;
    }
}

void SceneImageCache::syncPinSet(const std::vector<std::string>& paths) {
    std::lock_guard<std::mutex> lock(fMutex);
    releaseAllPinsLocked();
    // One reference per DISTINCT named path: a shared background carried
    // by three scenes is one asset to evict, and syncing the same set
    // twice (prepare, then present, then a commit) must not drift the
    // count upward. The release above already dropped everything the set
    // does not name, so what remains is exactly one each.
    std::vector<std::string> distinct = paths;
    std::sort(distinct.begin(), distinct.end());
    distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());
    for (const std::string& path : distinct) {
        ++fEntries[path].pins;
        ++fPins;
    }
    // Only now resettle the budget: with the keep set pinned again, so a
    // sync NEVER evicts an image belonging to a scene the session still
    // holds (running this inside the release, while every pin was zero,
    // meant a seventh 4K prepare could quietly strip the six prepared
    // scenes down to placeholders mid-show).
    evictLocked("");
}

void SceneImageCache::invalidate(const std::string& path) {
    std::lock_guard<std::mutex> lock(fMutex);
    const auto existing = fEntries.find(path);
    if (existing == fEntries.end()) {
        return;
    }
    fBytes -= existing->second.bytes;
    fPins -= existing->second.pins;
    fEntries.erase(existing);
}

SceneImageCache::Stats SceneImageCache::stats() const {
    std::lock_guard<std::mutex> lock(fMutex);
    Stats out;
    out.decodedBytes = fBytes;
    out.entries = fEntries.size();
    out.pins = fPins;
    out.evictions = fEvictions;
    out.budget = fBudget;
    return out;
}

void SceneImageCache::evictLocked(std::string_view protect) {
    // Forget unpinned entries, least-recently-seen first, until the decoded
    // bytes fit the budget or nothing can go. A pinned entry is content the
    // operator can put on air at any moment — it is kept, and the excess it
    // causes is reported in stats() rather than paid for with a colour fill
    // mid-show. An entry still drawn elsewhere survives in its caller's
    // sk_sp regardless: eviction frees the CACHE's reference, not the
    // frame's.
    while (fBytes > fBudget) {
        auto victim = fEntries.end();
        for (auto candidate = fEntries.begin(); candidate != fEntries.end(); ++candidate) {
            if (candidate->second.pins != 0 || candidate->first == protect ||
                candidate->second.bytes == 0) {
                continue;
            }
            if (victim == fEntries.end() || candidate->second.seen < victim->second.seen) {
                victim = candidate;
            }
        }
        if (victim == fEntries.end()) {
            return;  // everything left is pinned, protected, or free
        }
        fBytes -= victim->second.bytes;
        fEntries.erase(victim);
        ++fEvictions;
    }
}

void SceneImageCache::releaseAllPinsLocked() {
    // Release EVERY pin — no eviction here (the caller re-pins its set and
    // evicts afterwards, so a sync cannot strip what the session still
    // holds). The paths this drops are the ones a churned show should be
    // forgetting; syncPinSet re-takes the survivors immediately, and
    // keeping in-set pins instead of clearing them would make each entry's
    // count depend on how many prepares had run since the last clear —
    // which is the drift the re-derivation exists to prevent.
    std::vector<std::string> release;
    for (const auto& [path, entry] : fEntries) {
        if (entry.pins != 0) {
            release.push_back(path);
        }
    }
    for (const std::string& path : release) {
        const auto existing = fEntries.find(path);
        if (existing == fEntries.end()) {
            continue;
        }
        // ALL of them: the set is re-derived, not tallied. An entry that
        // somehow carried two references from two syncs (the caller's
        // re-derivation racing its own repeat call) must end this sync at
        // zero, or "who is pinned" would depend on how many prepares
        // happened since the last one — which is the drift this primitive
        // exists to prevent.
        fPins -= existing->second.pins;
        existing->second.pins = 0;
        if (existing->second.image == nullptr) {
            fEntries.erase(existing);  // an unpinned failure is pure weight
        }
    }
}

namespace {

// The scene's background picture, or null when there is no picture: nothing
// flagged, a video with no frame offered yet (task 5.5), or a still the cache
// does not hold (task 6.10: the draw path never decodes). Null means the
// colour fill stands alone — the same degradation as a missing file, and the
// session reports it.
sk_sp<SkImage> backgroundPicture(const openshow::scene::Scene& scene, SceneImageCache* images,
                                 const SkImage* video) {
    if (!scene.background.hasImage) {
        return nullptr;
    }
    if (scene.background.isVideo) {
        return sk_ref_sp(const_cast<SkImage*>(video));
    }
    if (images == nullptr) {
        return nullptr;
    }
    return images->imageFor(scene.background.assetPath);
}

// Cover fit: scale to fill the authored canvas, centre, crop overflow.
SkRect coverFitDest(const SkImage& image) {
    const float scale = std::max(kSceneCanvasWidth / static_cast<float>(image.width()),
                                 kSceneCanvasHeight / static_cast<float>(image.height()));
    const float w = static_cast<float>(image.width()) * scale;
    const float h = static_cast<float>(image.height()) * scale;
    return SkRect::MakeLTRB((kSceneCanvasWidth - w) * 0.5f, (kSceneCanvasHeight - h) * 0.5f,
                            (kSceneCanvasWidth + w) * 0.5f, (kSceneCanvasHeight + h) * 0.5f);
}

}  // namespace

void drawSceneBackground(SkCanvas* canvas, const openshow::scene::Scene& scene,
                         SceneImageCache* images, const SkPaint* paint,
                         const SkImage* video) {
    const uint32_t color = scene.background.color;
    const SkColor fill =
        SkColorSetARGB(0xFF, (color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF);
    const sk_sp<SkImage> image = backgroundPicture(scene, images, video);
    const bool fading = paint != nullptr && paint->getAlpha() < 255;
    const SkRect canvasRect = SkRect::MakeWH(kSceneCanvasWidth, kSceneCanvasHeight);

    canvas->save();
    // Clip so a portrait window's letterbox bars stay black (the cover-fit
    // dest can overflow the authored canvas on one axis).
    canvas->clipRect(canvasRect);
    if (image == nullptr) {
        // Colour only: the fill IS the background, so the blend rides it directly.
        SkPaint fillPaint;
        fillPaint.setColor(fill);
        if (fading) {
            fillPaint.setAlpha(paint->getAlpha());
        }
        canvas->drawRect(canvasRect, fillPaint);
    } else if (fading) {
        // The background is ONE group — the picture over the colour fill — and
        // the media blend is that group's alpha (task 6.11b, review R13).
        //
        // The obvious two-draw version (fill at alpha, then picture at alpha)
        // is not that: the fill is counted again under the picture and the
        // audience's own picture is attenuated twice, so an opaque still at
        // blend 0.5 composites to 0.25 audience + 0.25 incoming FILL + 0.5
        // incoming picture, and a scene fading to ITSELF — a lyric change over
        // one still — visibly dips toward its fill mid fade. Measured on the
        // shipped code: 46/255 away from the defined blend for an opaque
        // picture over a saturated fill, 22/255 for a picture with per-pixel
        // alpha, and the same picture on both sides lands 46/255 off the
        // frame the audience was already shown.
        //
        // The textbook fix is a layer, which this composition pass does not
        // have. This is the same composite with the layer folded into the
        // shader: `picture over fill` evaluated per pixel, then drawn once at
        // the group alpha. Measured against a by-the-book isolated layer:
        // exact for an opaque picture (0/255 over the whole canvas), within
        // rounding for a picture with per-pixel alpha (1/255), and one draw
        // call instead of two.
        const SkRect dest = coverFitDest(*image);
        // The image shader's local matrix maps the IMAGE'S OWN PIXELS into the
        // canvas' space (the same RectToRect the canvas itself builds for
        // drawImageRect), not the unit square. Getting this wrong is invisible
        // to a flat-colour fixture and unmistakable on a banded one: a unit
        // square matrix samples the picture's top-left pixel everywhere, so
        // the incoming scene arrives as one smeared colour.
        const SkMatrix local =
            SkMatrix::RectToRectOrIdentity(SkRect::MakeIWH(image->width(), image->height()), dest);
        SkPaint group;
        group.setShader(SkShaders::Blend(
            SkBlendMode::kSrcOver, SkShaders::Color(fill),
            SkShaders::Image(image, SkTileMode::kClamp, SkTileMode::kClamp,
                             SkSamplingOptions(SkFilterMode::kLinear), &local)));
        group.setAlpha(paint->getAlpha());
        canvas->drawRect(canvasRect, group);
    } else {
        SkPaint fillPaint;
        fillPaint.setColor(fill);
        canvas->drawRect(canvasRect, fillPaint);
        canvas->drawImageRect(image, SkRect::MakeIWH(image->width(), image->height()),
                              coverFitDest(*image), SkSamplingOptions(SkFilterMode::kLinear),
                              nullptr, SkCanvas::kFast_SrcRectConstraint);
    }
    canvas->restore();
}

bool writePngRaster(SkSurface* surface, const std::string& path, std::string* error) {
    SkPixmap pixmap;
    if (!surface || !surface->peekPixels(&pixmap)) {
        if (error) {
            *error = "snapshot pixels unavailable";
        }
        return false;
    }
    sk_sp<SkData> png = SkPngEncoder::Encode(pixmap, SkPngEncoder::Options{});
    if (!png) {
        if (error) {
            *error = "snapshot PNG encode failed";
        }
        return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error) {
            *error = "cannot write " + path;
        }
        return false;
    }
    out.write(static_cast<const char*>(png->data()), static_cast<std::streamsize>(png->size()));
    out.close();
    if (!out) {
        if (error) {
            *error = "write failed for " + path;
        }
        return false;
    }
    return true;
}

bool renderSceneSnapshot(const openshow::scene::Scene& scene, const std::string& path,
                         int width, int height, const SnapshotOptions& options,
                         std::string* error) {
    SceneTextEngine engine;
    if (!engine.create(error)) {
        return false;
    }
    auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));
    if (!surface) {
        if (error) {
            *error = "raster surface allocation failed";
        }
        return false;
    }
    SkCanvas* canvas = surface->getCanvas();
    canvas->resetMatrix();
    canvas->clear(SK_ColorBLACK);

    // The output rule, identical to SceneRenderer::draw.
    const float scale = std::min(static_cast<float>(width) / kSceneCanvasWidth,
                                 static_cast<float>(height) / kSceneCanvasHeight);
    canvas->translate((static_cast<float>(width) - kSceneCanvasWidth * scale) * 0.5f,
                      (static_cast<float>(height) - kSceneCanvasHeight * scale) * 0.5f);
    canvas->scale(scale, scale);

    // The two layers, dispatched like the LayerStack would (a disabled one
    // draws nothing at all), each honouring its own crossfade track.
    SceneImageCache images;
    // Task 6.10 (review R12): imageFor() never hits the disk, so this
    // snapshot entry point — the dev CLI's --snapshot and the render
    // fixtures — loads its backgrounds up front, outside the draw, exactly
    // as the live session's prepare does for the frame loop. Task 6.17b
    // added the stack's foreground pictures to the same up-front pass;
    // loadFor on a path already in the cache is the cheap lookup it always
    // was, so a picture shared by the fade's two sides decodes once.
    const auto loadStack = [&images](const openshow::scene::Scene& one) {
        for (const openshow::scene::SceneItem& item : one.items) {
            if (item.isImage && !item.assetPath.empty()) {
                images.loadFor(item.assetPath);
            }
        }
    };
    for (const openshow::scene::Scene* one : {&scene, options.fadeFrom}) {
        if (one != nullptr && one->background.hasImage && !one->background.isVideo &&
            !one->background.assetPath.empty()) {
            images.loadFor(one->background.assetPath);
        }
        if (one != nullptr) {
            loadStack(*one);
        }
    }
    if (options.background) {
        const bool fading = options.fadeFrom != nullptr && options.mediaBlend < 1.0f;
        if (fading) {
            drawSceneBackground(canvas, *options.fadeFrom, &images, nullptr);
        }
        SkPaint fade;
        if (fading) {
            fade.setAlpha(static_cast<uint8_t>(options.mediaBlend * 255.0f + 0.5f));
        }
        drawSceneBackground(canvas, scene, &images, fading ? &fade : nullptr);
    }
    if (options.words) {
        const bool fading = options.fadeFrom != nullptr && options.textBlend < 1.0f;
        if (fading) {
            const SceneTextEngine::SceneLayout fromLayout = engine.layoutScene(*options.fadeFrom);
            SkPaint fade;
            fade.setAlpha(static_cast<uint8_t>((1.0f - options.textBlend) * 255.0f + 0.5f));
            engine.drawLayout(canvas, fromLayout, &fade, &images);
        }
        const SceneTextEngine::SceneLayout layout = engine.layoutScene(scene);
        SkPaint fade;
        if (fading) {
            fade.setAlpha(static_cast<uint8_t>(options.textBlend * 255.0f + 0.5f));
        }
        engine.drawLayout(canvas, layout, fading ? &fade : nullptr, &images);
    }
    return writePngRaster(surface.get(), path, error);
}

}  // namespace openshow::worker
