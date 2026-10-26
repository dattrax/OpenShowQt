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
#include "modules/skparagraph/include/FontCollection.h"
#include "modules/skparagraph/include/ParagraphBuilder.h"
#include "modules/skparagraph/include/ParagraphStyle.h"
#include "modules/skparagraph/include/TextStyle.h"
#include "modules/skunicode/include/SkUnicode.h"
#include "modules/skunicode/include/SkUnicode_icu.h"

namespace openshow::worker {

namespace {

// The fallback line box: a TextItem that carries no line spacing (a
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
    // Where the lines sit inside the box (task 4.4); anything unknown
    // falls back to centred, the contract both the domain and the wire
    // enforce, so this only guards a hand-built TextItem.
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

void SceneTextEngine::drawLayout(SkCanvas* canvas, const SceneLayout& layout,
                                 const SkPaint* paint) const {
    // The fade's group alpha wraps every occupied box — Skia's
    // "transparency straight into the paint" for a fork whose
    // Paragraph::paint takes no paint. Steady frames never take the layer.
    const bool group = paint != nullptr && paint->getAlpha() < 255;
    if (group) {
        SkRect bounds = SkRect::MakeEmpty();
        bool any = false;
        for (const auto& item : layout.items) {
            if (!item.paragraph || !item.hasText) {
                continue;
            }
            const SkRect box = SkRect::MakeLTRB(item.boxX, item.boxY,
                                                item.boxX + item.boxWidth,
                                                item.boxY + item.boxHeight);
            if (any) {
                bounds.join(box);
            } else {
                bounds = box;
                any = true;
            }
        }
        if (!any) {
            return;
        }
        bounds.outset(8.0f, 8.0f);
        canvas->saveLayer(bounds, paint);
    }
    for (const auto& item : layout.items) {
        if (item.paragraph && item.hasText) {
            // Centred: the paragraph aligns its lines inside boxWidth, and
            // the block sits at the box's vertical middle.
            const float y = item.boxY + std::max(0.0f, (item.boxHeight - item.height) * 0.5f);
            item.paragraph->paint(canvas, item.boxX, y);
        }
    }
    if (group) {
        canvas->restore();
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
sk_sp<SkImage> decodeImageFile(const std::string& path, std::string* error) {
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
    const SkImageInfo info = codec->getInfo().makeColorType(kN32_SkColorType)
                                 .makeAlphaType(kPremul_SkAlphaType);
    SkBitmap bitmap;
    if (!bitmap.tryAllocPixels(info)) {
        return fail("no pixels for a " + std::to_string(info.width()) + "x" +
                    std::to_string(info.height()) + " image");
    }
    const SkCodec::Result result = codec->getPixels(info, bitmap.getPixels(), bitmap.rowBytes());
    if (result != SkCodec::kSuccess) {
        return fail("image decode failed (SkCodec result " + std::to_string(result) + ")");
    }
    if (error) {
        error->clear();
    }
    return SkImages::RasterFromBitmap(bitmap);
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
    while (fBytes > kDecodedImageBudget) {
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

void drawSceneBackground(SkCanvas* canvas, const openshow::scene::Scene& scene,
                         SceneImageCache* images, const SkPaint* paint,
                         const SkImage* video) {
    const uint32_t color = scene.background.color;
    SkPaint fill;
    fill.setColor(SkColorSetARGB(0xFF, (color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF));
    if (paint != nullptr) {
        fill.setAlpha(paint->getAlpha());
    }
    canvas->drawRect(SkRect::MakeWH(kSceneCanvasWidth, kSceneCanvasHeight), fill);
    if (!scene.background.hasImage) {
        return;
    }
    // Task 5.5: the media kind decides where the picture comes from —
    // the offered video frame, or the cached decode of the image. Either
    // absent leaves the colour fill standing alone (a video with no
    // frame yet, or one that failed to open, is the same degradation as
    // a missing image; the session reports the failure).
    sk_sp<SkImage> image;
    if (scene.background.isVideo) {
        if (video == nullptr) {
            return;
        }
        image = sk_ref_sp(const_cast<SkImage*>(video));
    } else {
        if (images == nullptr) {
            return;
        }
        image = images->imageFor(scene.background.assetPath);
        if (!image) {
            return;  // missing or undecodable: the colour fill stands alone
        }
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
    // as the live session's prepare does for the frame loop.
    for (const openshow::scene::Scene* one : {&scene, options.fadeFrom}) {
        if (one != nullptr && one->background.hasImage && !one->background.isVideo &&
            !one->background.assetPath.empty()) {
            images.loadFor(one->background.assetPath);
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
