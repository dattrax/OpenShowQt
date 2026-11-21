/*
 * Service-card thumbnails kept with the service (TC.2): the Qt-free half.
 *
 * The worker paints a slide's card, the frontend keeps the finished PNG beside
 * the authoritative shows in service.json, and the next run can show the same
 * picture without asking the renderer for it. This header is the value
 * contract both halves share: the entry records, their bounds, the checksum
 * and Base64 helpers, and the deterministic fingerprint that decides whether a
 * stored picture still describes the scene it is listed under.
 *
 * It is deliberately Qt-free and free of the document model: a cache record is
 * derived data, never part of Documents or its history. The service codec
 * (storage.h) and the catalog codec each carry a ThumbnailCache beside their
 * authored content, and the runtime overlay (src/ui/ThumbnailStore.h) turns
 * entries into QImages.
 *
 * The bounds — native renders up to 512x288, 256 records, 32 KiB per PNG,
 * 1.5 MiB encoded, which scene members enter the fingerprint — are written
 * down in docs/ui/thumbnail-cache-plan.md.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace openshow::domain {

// The block's OWN version, independent of the service manifest's format: the
// block is derived data and travels with the authored document rather than
// defining it.
inline constexpr unsigned long long kThumbnailCacheFormat = 1;

// One finished card picture, as it lives in JSON.
struct ThumbnailCacheEntry {
    std::string showId;      // the owning show (empty for a Library variant)
    std::string slideId;     // the owning slide; occurrences share this record
    std::string fingerprint; // renderFingerprint(): 64 lowercase hex
    std::string renderer;    // rendererIdentity() at render time
    std::uint32_t width = 0; // actual render width, at most 512
    std::uint32_t height = 0;  // actual render height, at most 288
    std::uint64_t pngBytes = 0;  // decoded PNG length, before Base64
    std::string checksum;    // sha256Hex(png bytes), 64 lowercase hex
    std::string pngBase64;   // the picture itself
};

struct ThumbnailCache {
    unsigned long long format = kThumbnailCacheFormat;
    std::vector<ThumbnailCacheEntry> entries;
};

// The maximum 16:9 extent. Records keep their native resolution and may serve
// smaller Show/Edit cards by downscaling; larger requests need a fresh render.
inline constexpr std::uint32_t kThumbnailWidth = 512;
inline constexpr std::uint32_t kThumbnailHeight = 288;
// Show/Edit operator cards render once at this size and scale for display.
// Keep the larger codec envelope above so existing larger records still load.
inline constexpr std::uint32_t kOperatorThumbnailWidth = 320;
inline constexpr std::uint32_t kOperatorThumbnailHeight = 180;
inline constexpr std::uint32_t kMinThumbnailWidth = 2;
inline constexpr std::uint32_t kMinThumbnailHeight = 2;
inline constexpr const char* kThumbnailPolicy = "card-surface/2";

// Bounds checked before anything is allocated or decoded.
inline constexpr std::size_t kMaxThumbnailCacheEntries = 256;
inline constexpr std::uint64_t kMaxThumbnailPngBytes = 32 * 1024;
inline constexpr std::uint64_t kMaxThumbnailEncodedBytes = 1536 * 1024;

// --- hashing and encodings -------------------------------------------------

// Lowercase hexadecimal SHA-256 of the given bytes.
std::string sha256Hex(std::string_view bytes);

// Standard-alphabet Base64 with padding, and the strict inverse. The encoder
// never wraps lines; the decoder accepts only that spelling (no whitespace, no
// URL alphabet), because a record is a value rather than a payload to guess at.
std::string base64Encode(std::string_view bytes);
bool base64Decode(std::string_view text, std::string* out);
// Length the encoder produces, and a charset/padding check that does not
// allocate — the parse path judges a record with these, never by decoding it.
std::size_t base64EncodedSize(std::size_t rawBytes);
bool base64IsCanonical(std::string_view text, std::size_t* decodedBytes);

// The checksum an entry carries: sha256Hex of the PNG bytes.
std::string pngChecksum(std::string_view png);

// PNG signature + IHDR dimensions, without decoding the picture. False for
// anything that is not a PNG or whose header is too short.
bool pngDimensions(std::string_view png, std::uint32_t* width, std::uint32_t* height);

// --- render identity -------------------------------------------------------

// Build signature plus the process's Fontconfig configuration, face mappings
// and font file stamps. Cached once per process, like the worker's font manager.
// Failure to identify the environment disables persistence (empty identity).
std::string rendererIdentity();
// Recompute for an isolated Fontconfig environment; also used by regressions.
std::string fontEnvironmentIdentity();

// One referenced asset's identity: the id the scene names, the byte length and
// a probe hash over the first 4 KiB, the last 4 KiB and the size. Content
// identity rather than path identity, so save-adopt and package relocation
// still hit; an in-place replacement that changes either end or the size
// misses. Reading the probe is the caller's memoization problem (path, size
// and mtime identify an unchanged file).
struct ThumbnailAssetStamp {
    std::string assetId;
    std::uint64_t size = 0;
    std::string probe;  // 64 lowercase hex
};
bool thumbnailAssetStamp(const std::filesystem::path& path, const std::string& assetId,
                         ThumbnailAssetStamp* out, std::string* error);

// The compiled scene with every "assetPath", "showId" and "slideId" member
// emptied, re-serialized. A package-local path is where bytes happen to live
// and the ids are the service's names: none of them decides a pixel, yet a
// re-preparation mints new ids and a save-adopt moves every path. Asset
// identity travels in ThumbnailAssetStamp, never in the scene text.
bool normalizeSceneForFingerprint(std::string_view sceneJson, std::string* out,
                                  std::string* error);

// Whether a still of this scene may be kept at all: a video or camera
// background is live content, and a persisted still of it would be a lie.
bool scenePersistable(std::string_view sceneJson);

// A deterministic hash of everything that decides the pixels: the normalized
// scene, the referenced asset identities (sorted by id), the renderer, the
// static thumbnail policy and the bucket extent.
std::string renderFingerprint(std::string_view sceneJson,
                              const std::vector<ThumbnailAssetStamp>& assets,
                              std::string_view renderer = {}, std::string_view policy = {},
                              std::uint32_t width = kThumbnailWidth,
                              std::uint32_t height = kThumbnailHeight);

// --- validation ------------------------------------------------------------

// Bounds and spelling of ONE record, without decoding its PNG. An invalid
// record is a miss and never an authored-content failure.
bool validateThumbnailEntry(const ThumbnailCacheEntry& entry, std::string* error);

// The same over a block: format supported, entry count and total encoded bytes
// inside their bounds, each record valid. Duplicate (slideId, fingerprint)
// pairs are refused: one slide's one picture has one home.
bool validateThumbnailCache(const ThumbnailCache& cache, std::string* error);

// The record a row asks for, or nullptr.
const ThumbnailCacheEntry* findThumbnail(const ThumbnailCache& cache, std::string_view slideId,
                                         std::string_view fingerprint);

// --- Library render variants (TC.9) ---------------------------------------
//
// A shared-catalog song can be played under several looks, and a prepared
// service slide's ids are new every time. A variant therefore carries no
// service owner at all: identity is the fingerprint alone (the canonical scene,
// the referenced asset bytes, the renderer and the bucket). The same pixel
// record is validated with the same bounds; only the ownership differs.
inline constexpr std::size_t kMaxThumbnailVariantsPerItem = 6;
inline constexpr std::uint64_t kMaxThumbnailVariantBytesPerItem = 192 * 1024;

bool validThumbnailVariant(const ThumbnailCacheEntry& entry, std::string* error);
// Keep the newest valid variants under the count/byte ceilings, returning
// them in oldest-to-newest order. Returns how many were removed.
std::size_t sanitizeThumbnailVariants(std::vector<ThumbnailCacheEntry>* variants);
const ThumbnailCacheEntry* findThumbnailVariant(
    const std::vector<ThumbnailCacheEntry>& variants, std::string_view fingerprint);

// Drops malformed records in place and keeps the rest, in file order, within
// the count and total-byte bounds (later records drop first). Returns how many
// records were removed. This is the tolerant parse a hand-edited or
// older-build block gets: a bad record costs its own picture, never the load.
std::size_t sanitizeThumbnailCache(ThumbnailCache* cache);

}  // namespace openshow::domain
