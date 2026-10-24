/*
 * Production scene text layout — see SceneText.h.
 *
 * The font assembly (fontconfig + FreeType scanner + named-family fallbacks)
 * and the SkUnicode backend graduate here from TestScene (task 3.1): the
 * engine is the single font setup the worker has, so a fixture rendered
 * through layoutScene()/renderSceneSnapshot exercises exactly what the
 * audience layers draw.
 */

#include "render/SceneText.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>

#include "include/codec/SkCodec.h"
#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkData.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkImage.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkRect.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkSurface.h"
#include "include/encode/SkPngEncoder.h"
#include "include/ports/SkFontMgr_fontconfig.h"
#include "include/ports/SkFontScanner_FreeType.h"
#include "include/private/base/SkTArray.h"
#include "src/base/SkUTF.h"
#include "modules/skparagraph/include/FontCollection.h"
#include "modules/skparagraph/include/ParagraphBuilder.h"
#include "modules/skparagraph/include/ParagraphStyle.h"
#include "modules/skparagraph/include/TextStyle.h"
#include "modules/skunicode/include/SkUnicode.h"
#include "modules/skunicode/src/SkUnicode_hardcoded.h"

namespace openshow::worker {

namespace {

// Line boxes are lineHeight = fontSize * this factor, so stanza rhythm does
// not depend on which font the platform resolves.
constexpr float kLineHeightFactor = 1.25f;

}  // namespace

// The hard-coded SkUnicode backend implements the character properties but
// leaves breaking, bidi and upper-casing abstract (full ICU is blocked on an
// ICU version mismatch; see PROGRESS.md). These implementations cover the
// rules scenes need: whitespace, newlines as hard breaks, a soft break after
// whitespace (which is what makes word wrapping work), and a grapheme start
// per code point. Named (not anonymous): the SkUnicodes::ICU::Make() shim at
// the bottom of this file hands libskshaper the same provider.
class HardCodedUnicode final : public SkUnicodeHardCodedCharProperties {
public:
    SkString toUpper(const SkString& text) override {
        SkString upper(text);
        char* data = upper.data();
        for (size_t i = 0; i < upper.size(); ++i) {
            data[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(data[i])));
        }
        return upper;
    }

    SkString toUpper(const SkString& text, const char*) override { return toUpper(text); }

    std::unique_ptr<SkBidiIterator> makeBidiIterator(const uint16_t[], int,
                                                     SkBidiIterator::Direction) override {
        return nullptr;
    }

    std::unique_ptr<SkBidiIterator> makeBidiIterator(const char[], int,
                                                     SkBidiIterator::Direction) override {
        return nullptr;
    }

    std::unique_ptr<SkBreakIterator> makeBreakIterator(const char[], BreakType) override {
        return nullptr;
    }

    std::unique_ptr<SkBreakIterator> makeBreakIterator(BreakType) override { return nullptr; }

    bool getBidiRegions(const char[], int utf8Units, TextDirection direction,
                        std::vector<BidiRegion>* results) override {
        if (!results) {
            return false;
        }
        // Single-level text (no mixed directions) through task 3.1.
        results->clear();
        results->emplace_back(0, static_cast<Position>(utf8Units),
                              direction == TextDirection::kRTL ? BidiLevel(1) : BidiLevel(0));
        return true;
    }

    bool getWords(const char[], int, const char*, std::vector<Position>*) override {
        return false;
    }

    bool getUtf8Words(const char[], int, const char*, std::vector<Position>*) override {
        return false;
    }

    bool getSentences(const char[], int, const char*, std::vector<Position>*) override {
        return false;
    }

    void reorderVisual(const BidiLevel[], int levelCount, int32_t* reorderMap) override {
        // Single direction (LTR): the identity mapping.
        for (int i = 0; i < levelCount; ++i) {
            reorderMap[i] = i;
        }
    }

    bool computeCodeUnitFlags(char utf8[], int utf8Units, bool,
                              skia_private::TArray<CodeUnitFlags, true>* results) override {
        if (!results) {
            return false;
        }
        results->clear();
        results->push_back_n(utf8Units + 1, CodeUnitFlags::kNoCodeUnitFlag);
        const char* current = utf8;
        const char* end = utf8 + utf8Units;
        bool previousWasWhitespace = false;
        bool pendingHardBreak = false;
        while (current < end) {
            const int before = static_cast<int>(current - utf8);
            SkUnichar unichar = SkUTF::NextUTF8(&current, end);
            if (unichar < 0) {
                unichar = 0xFFFD;
            }
            const int after = static_cast<int>(current - utf8);
            // A hard break belongs to the position *after* the newline so the
            // newline stays trailing whitespace of its own line, matching
            // SkUnicode_icu's UTF-8 path.
            if (pendingHardBreak) {
                (*results)[before] = static_cast<CodeUnitFlags>(
                    (*results)[before] | CodeUnitFlags::kHardLineBreakBefore);
            }
            const bool whitespace = isWhitespace(unichar);
            int flags = CodeUnitFlags::kGraphemeStart;
            if (isControl(unichar)) {
                flags |= CodeUnitFlags::kControl;
            }
            if (isTabulation(unichar)) {
                flags |= CodeUnitFlags::kTabulation;
            }
            if (whitespace) {
                flags |= CodeUnitFlags::kPartOfWhiteSpaceBreak;
                for (int i = before; i < after; ++i) {
                    (*results)[i] = static_cast<CodeUnitFlags>(
                        (*results)[i] | CodeUnitFlags::kPartOfWhiteSpaceBreak);
                }
            }
            if (!whitespace && previousWasWhitespace) {
                flags |= CodeUnitFlags::kSoftLineBreakBefore;
            }
            (*results)[before] = static_cast<CodeUnitFlags>((*results)[before] | flags);
            pendingHardBreak = isHardBreak(unichar);
            previousWasWhitespace = whitespace;
        }
        // ICU reports a hard break after a trailing newline and a final soft
        // break at the end of the text.
        CodeUnitFlags tail = CodeUnitFlags::kSoftLineBreakBefore;
        if (pendingHardBreak) {
            tail = static_cast<CodeUnitFlags>(tail | CodeUnitFlags::kHardLineBreakBefore);
        }
        (*results)[utf8Units] = static_cast<CodeUnitFlags>((*results)[utf8Units] | tail);
        return true;
    }

    bool computeCodeUnitFlags(char16_t utf16[], int utf16Units, bool,
                              skia_private::TArray<CodeUnitFlags, true>* results) override {
        if (!results) {
            return false;
        }
        results->clear();
        results->push_back_n(utf16Units + 1, CodeUnitFlags::kNoCodeUnitFlag);
        const char16_t* current = utf16;
        const char16_t* end = utf16 + utf16Units;
        bool previousWasWhitespace = false;
        bool pendingHardBreak = false;
        while (current < end) {
            const int before = static_cast<int>(current - utf16);
            SkUnichar unichar = SkUTF::NextUTF16(reinterpret_cast<const uint16_t**>(&current),
                                                 reinterpret_cast<const uint16_t*>(end));
            if (unichar < 0) {
                unichar = 0xFFFD;
            }
            const int after = static_cast<int>(current - utf16);
            if (pendingHardBreak) {
                (*results)[before] = static_cast<CodeUnitFlags>(
                    (*results)[before] | CodeUnitFlags::kHardLineBreakBefore);
            }
            const bool whitespace = isWhitespace(unichar);
            int flags = CodeUnitFlags::kGraphemeStart;
            if (isControl(unichar)) {
                flags |= CodeUnitFlags::kControl;
            }
            if (isTabulation(unichar)) {
                flags |= CodeUnitFlags::kTabulation;
            }
            if (whitespace) {
                flags |= CodeUnitFlags::kPartOfWhiteSpaceBreak;
                for (int i = before; i < after; ++i) {
                    (*results)[i] = static_cast<CodeUnitFlags>(
                        (*results)[i] | CodeUnitFlags::kPartOfWhiteSpaceBreak);
                }
            }
            if (!whitespace && previousWasWhitespace) {
                flags |= CodeUnitFlags::kSoftLineBreakBefore;
            }
            (*results)[before] = static_cast<CodeUnitFlags>((*results)[before] | flags);
            pendingHardBreak = isHardBreak(unichar);
            previousWasWhitespace = whitespace;
        }
        CodeUnitFlags tail = CodeUnitFlags::kSoftLineBreakBefore;
        if (pendingHardBreak) {
            tail = static_cast<CodeUnitFlags>(tail | CodeUnitFlags::kHardLineBreakBefore);
        }
        (*results)[utf16Units] = static_cast<CodeUnitFlags>((*results)[utf16Units] | tail);
        return true;
    }
};

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
    // Full ICU-backed SkUnicode needs an ICU matching the Skia build (it
    // expects v77 symbols, this Fedora has 78); the hard-coded backend is
    // enough for the task 1.1 text and the swap is recorded in PROGRESS.md.
    fUnicode = sk_make_sp<HardCodedUnicode>();
    return true;
}

namespace {

float effectiveSize(const openshow::scene::TextItem& item, float scale) {
    const float base =
        item.fontSize > 0.0 ? static_cast<float>(item.fontSize) : kSceneDefaultFontSize;
    return base * scale;
}

}  // namespace

SceneTextEngine::LaidOutItem SceneTextEngine::layout(const openshow::scene::TextItem& item,
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
    paragraphStyle.setTextAlign(skia::textlayout::TextAlign::kCenter);
    paragraphStyle.setTextDirection(skia::textlayout::TextDirection::kLtr);

    skia::textlayout::TextStyle textStyle;
    textStyle.setFontSize(out.fontSize);
    // Explicit line box so stanza rhythm does not depend on which font the
    // platform resolves. This fork's setHeight is a *multiplier* of the font
    // size (Run::calculateMetrics: runHeight = multiplier * fontSize) with
    // half-leading so text sits centred in its box — verified by probe.
    textStyle.setHeight(kLineHeightFactor);
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
    if (scene.items.empty()) {
        return out;
    }

    const float maxWidth = kSceneCanvasWidth - 2.0f * kSceneMarginX;
    const float maxHeight = kSceneCanvasHeight - 2.0f * kSceneMarginY;

    // The fit pass scales every item by one factor so relative sizes survive.
    float maxBase = 0.0f;
    for (const auto& item : scene.items) {
        maxBase = std::max(maxBase, effectiveSize(item, 1.0f));
    }
    const float gapBase = 0.4f * maxBase;
    const float minScale = maxBase > 0.0f ? kSceneMinFontSize / maxBase : 1.0f;

    float scale = 1.0f;
    // Wrapping changes with the scale, so height is not linear in it: lay
    // out, shrink, re-lay out; three iterations converge long before this
    // cap in practice, and the last accepted layout is always returned.
    for (int pass = 0; pass < 4; ++pass) {
        out.items.clear();
        out.blockHeight = 0.0f;
        out.scale = scale;
        out.gap = gapBase * scale;
        for (const auto& item : scene.items) {
            LaidOutItem laid = layout(item, scale, maxWidth);
            out.blockHeight += laid.height;
            out.items.push_back(std::move(laid));
        }
        if (out.items.size() > 1) {
            out.blockHeight += out.gap * static_cast<float>(out.items.size() - 1);
        }
        if (out.blockHeight <= maxHeight || out.blockHeight <= 0.0f) {
            break;
        }
        const float next = std::max(scale * maxHeight / out.blockHeight, minScale);
        if (next >= scale) {
            break;  // at the floor already; shrink no further
        }
        scale = next;
    }
    return out;
}

void SceneTextEngine::drawLayout(SkCanvas* canvas, const SceneLayout& layout,
                                 const SkPaint* paint) const {
    const float top = (kSceneCanvasHeight - layout.blockHeight) * 0.5f;
    // Paragraph::paint in this fork takes no paint, so the fade's group
    // alpha wraps the block in a saveLayer — Skia's own "transparency
    // straight into the paint". The layer is word-bounds only and exists
    // only while a fade is in flight; steady frames never take it.
    const bool group = paint != nullptr && paint->getAlpha() < 255;
    if (group) {
        canvas->saveLayer(SkRect::MakeLTRB(kSceneMarginX - 8.0f, top - 8.0f,
                                           kSceneCanvasWidth - kSceneMarginX + 8.0f,
                                           top + layout.blockHeight + 8.0f),
                          paint);
    }
    float y = top;
    for (const auto& item : layout.items) {
        if (item.paragraph && item.hasText) {
            item.paragraph->paint(canvas, kSceneMarginX, y);
        }
        y += item.height + layout.gap;
    }
    if (group) {
        canvas->restore();
    }
}

sk_sp<SkImage> SceneImageCache::imageFor(const std::string& path) {
    const auto existing = fImages.find(path);
    if (existing != fImages.end()) {
        return existing->second;  // cached, or a cached failure (null)
    }
    sk_sp<SkImage> image;
    std::ifstream in(path, std::ios::binary);
    if (in) {
        const std::string bytes((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
        sk_sp<SkData> data = SkData::MakeWithCopy(bytes.data(), bytes.size());
        auto codec = SkCodec::MakeFromData(std::move(data));
        if (codec) {
            const SkImageInfo info = codec->getInfo().makeColorType(kN32_SkColorType)
                                         .makeAlphaType(kPremul_SkAlphaType);
            SkBitmap bitmap;
            if (bitmap.tryAllocPixels(info) &&
                codec->getPixels(info, bitmap.getPixels(), bitmap.rowBytes()) ==
                    SkCodec::kSuccess) {
                image = SkImages::RasterFromBitmap(bitmap);
            }
        }
    }
    fImages.emplace(path, image);
    return image;
}

void drawSceneBackground(SkCanvas* canvas, const openshow::scene::Scene& scene,
                         SceneImageCache* images, const SkPaint* paint) {
    const uint32_t color = scene.background.color;
    SkPaint fill;
    fill.setColor(SkColorSetARGB(0xFF, (color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF));
    if (paint != nullptr) {
        fill.setAlpha(paint->getAlpha());
    }
    canvas->drawRect(SkRect::MakeWH(kSceneCanvasWidth, kSceneCanvasHeight), fill);
    if (!scene.background.hasImage || images == nullptr) {
        return;
    }
    sk_sp<SkImage> image = images->imageFor(scene.background.assetPath);
    if (!image) {
        return;  // missing or undecodable: the colour fill stands alone
    }
    // Cover fit: scale to fill the authored canvas, centre, crop overflow
    // (clip so a portrait window's letterbox bars stay black).
    const float scale = std::max(kSceneCanvasWidth / static_cast<float>(image->width()),
                                 kSceneCanvasHeight / static_cast<float>(image->height()));
    const float w = static_cast<float>(image->width()) * scale;
    const float h = static_cast<float>(image->height()) * scale;
    const SkRect dest = SkRect::MakeLTRB((kSceneCanvasWidth - w) * 0.5f,
                                         (kSceneCanvasHeight - h) * 0.5f,
                                         (kSceneCanvasWidth + w) * 0.5f,
                                         (kSceneCanvasHeight + h) * 0.5f);
    canvas->save();
    canvas->clipRect(SkRect::MakeWH(kSceneCanvasWidth, kSceneCanvasHeight));
    SkPaint imagePaint;
    if (paint != nullptr) {
        imagePaint.setAlpha(paint->getAlpha());
    }
    canvas->drawImageRect(image, SkRect::MakeIWH(image->width(), image->height()), dest,
                          SkSamplingOptions(SkFilterMode::kLinear),
                          paint != nullptr ? &imagePaint : nullptr,
                          SkCanvas::kFast_SrcRectConstraint);
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

    // The output rule, identical to TestScene::draw.
    const float scale = std::min(static_cast<float>(width) / kSceneCanvasWidth,
                                 static_cast<float>(height) / kSceneCanvasHeight);
    canvas->translate((static_cast<float>(width) - kSceneCanvasWidth * scale) * 0.5f,
                      (static_cast<float>(height) - kSceneCanvasHeight * scale) * 0.5f);
    canvas->scale(scale, scale);

    // The two layers, dispatched like the LayerStack would (a disabled one
    // draws nothing at all), each honouring its own crossfade track.
    SceneImageCache images;
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
            engine.drawLayout(canvas, fromLayout, &fade);
        }
        const SceneTextEngine::SceneLayout layout = engine.layoutScene(scene);
        SkPaint fade;
        if (fading) {
            fade.setAlpha(static_cast<uint8_t>(options.textBlend * 255.0f + 0.5f));
        }
        engine.drawLayout(canvas, layout, fading ? &fade : nullptr);
    }
    return writePngRaster(surface.get(), path, error);
}

}  // namespace openshow::worker

// Compatibility definition: libskshaper's default shaping entry point calls
// SkUnicodes::ICU::Make(), and the prebuilt libskunicode_icu expects ICU v77
// versioned symbols this Fedora (ICU 78) does not provide (see PROGRESS.md).
// Supplying the hard-coded backend here keeps the linker happy and hands the
// shaper the same Unicode provider the paragraph uses. Retire this when the
// ICU match is resolved.
namespace SkUnicodes::ICU {
sk_sp<SkUnicode> Make() { return sk_make_sp<openshow::worker::HardCodedUnicode>(); }
}  // namespace SkUnicodes::ICU
