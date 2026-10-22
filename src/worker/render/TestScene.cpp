#include "TestScene.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <initializer_list>
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
#include "include/effects/SkGradient.h"
#include "include/encode/SkPngEncoder.h"
#include "include/ports/SkFontMgr_fontconfig.h"
#include "include/ports/SkFontScanner_FreeType.h"
#include "include/private/base/SkTArray.h"
#include "src/base/SkUTF.h"
#include "modules/skparagraph/include/ParagraphBuilder.h"
#include "modules/skparagraph/include/ParagraphStyle.h"
#include "modules/skparagraph/include/TextStyle.h"
#include "modules/skunicode/include/SkUnicode.h"
#include "modules/skunicode/include/SkUnicode_icu.h"
#include "modules/skunicode/src/SkUnicode_hardcoded.h"

namespace openshow::worker {

namespace {

// The hard-coded SkUnicode backend implements the character properties but
// leaves breaking, bidi and upper-casing abstract (full ICU is blocked on an
// ICU version mismatch; see PROGRESS.md). These implementations cover the
// ASCII rules the test scene needs: whitespace, newlines as hard breaks, a
// soft break after whitespace, and a grapheme start per code point.
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
        // Single-level text in the task 1.1 scene (no mixed directions).
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
        // Single direction (LTR) in the test scene: the identity mapping.
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

// The authored-canvas regions each layer draws into. --check-layers asserts
// the diff of every single-layer toggle stays inside its region, so moving
// content means updating these bounds.
constexpr SkRect kPhotoRegion = SkRect::MakeLTRB(130.0f, 240.0f, 985.0f, 775.0f);
constexpr SkRect kWordsRegion = SkRect::MakeLTRB(1025.0f, 205.0f, 1855.0f, 600.0f);
constexpr SkRect kAuxBadge = SkRect::MakeLTRB(1560.0f, 40.0f, 1880.0f, 112.0f);
constexpr SkRect kAuxRegion = SkRect::MakeLTRB(1545.0f, 25.0f, 1895.0f, 125.0f);
constexpr SkRect kClockRegion = SkRect::MakeLTRB(130.0f, 965.0f, 1500.0f, 1035.0f);

// Bottom of the stack: the gradient background. FreeShow puts video here
// too; the M5 background layer will replace this shader with decoded frames
// through the same draw() entry point.
class BackgroundLayer final : public RenderLayer {
public:
    LayerId id() const override { return LayerId::kBackground; }

    void draw(const FrameContext& ctx) override {
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
};

// Slide images: the PNG-round-tripped photo with a border and caption.
class ImageLayer final : public RenderLayer {
public:
    ImageLayer(sk_sp<SkImage> photo, sk_sp<SkTypeface> regular)
        : fPhoto(std::move(photo)), fRegular(std::move(regular)) {}

    LayerId id() const override { return LayerId::kImages; }

    void draw(const FrameContext& ctx) override {
        if (!fPhoto) {
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
};

// Words: the SkParagraph heading, exactly what M3 lyrics will use.
class WordsLayer final : public RenderLayer {
public:
    explicit WordsLayer(std::unique_ptr<skia::textlayout::Paragraph> title)
        : fTitle(std::move(title)) {}

    LayerId id() const override { return LayerId::kWords; }

    void draw(const FrameContext& ctx) override {
        if (fTitle) {
            fTitle->paint(ctx.canvas, 1040, 220);
        }
    }

private:
    std::unique_ptr<skia::textlayout::Paragraph> fTitle;
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
    sk_sp<SkTypeface> bold = matchTypeface(fFontMgr.get(), "Liberation Sans",
                                           SkFontStyle::Bold(),
                                           {"/usr/share/fonts/liberation-sans-fonts/LiberationSans-Bold.ttf",
                                            "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf"});
    if (!fRegular) {
        // fontconfig default sans when the named matches both failed.
        fRegular = fFontMgr->matchFamilyStyle(nullptr, SkFontStyle::Normal());
    }
    if (!bold) {
        bold = fRegular;
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

    std::unique_ptr<skia::textlayout::Paragraph> title =
        makeTitle(fFontCollection, fUnicode, bold, fRegular);
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
    fLayers.add(std::make_unique<BackgroundLayer>());
    fLayers.add(std::make_unique<ImageLayer>(std::move(photo), fRegular));
    fLayers.add(std::make_unique<WordsLayer>(std::move(title)));
    fLayers.add(std::make_unique<AuxLayer>(fRegular));
    fLayers.add(std::make_unique<ClockLayer>(fRegular));
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

    SkPixmap pixmap;
    if (!surface->peekPixels(&pixmap)) {
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

namespace {

struct RasterFrame {
    int width = 1920;
    int height = 1080;
    std::vector<SkColor> pixels;

    bool render(TestScene& scene, std::string* error) {
        auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));
        if (!surface) {
            if (error) {
                *error = "raster surface allocation failed";
            }
            return false;
        }
        // Fixed wall clock and status line: two frames with the same layer
        // configuration must come out pixel identical.
        scene.draw(surface->getCanvas(), width, height, 0.0, std::time_t{1234567890}, "layers");
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

}  // namespace openshow::worker

// Compatibility definition: libskshaper's default shaping entry point calls
// SkUnicodes::ICU::Make(), and the prebuilt libskunicode_icu expects ICU v77
// versioned symbols this Fedora (ICU 78) does not provide (see PROGRESS.md).
// Supplying the hard-coded backend here keeps the linker happy and hands the
// shaper the same Unicode provider the paragraph uses. Retire this when the
// ICU match is resolved for M3.
namespace SkUnicodes::ICU {
sk_sp<SkUnicode> Make() { return sk_make_sp<openshow::worker::HardCodedUnicode>(); }
}  // namespace SkUnicodes::ICU
