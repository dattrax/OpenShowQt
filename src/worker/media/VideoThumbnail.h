/*
 * VideoThumbnail: one first frame, scaled and PNG-encoded, for the Media Bin's
 * video cards (MB.6).
 *
 * Bounded by construction: it opens the container, decodes until the first
 * frame arrives (at most 128 packets), scales that one image to fit the
 * requested box, and encodes one PNG. No audio, no seek, no loop. The public
 * type speaks plain C++ (FFmpeg and libpng stay in the .cpp), matching the
 * rest of the media module.
 *
 * Review D7b: the bin's pixels do not travel in JSON-RPC any more. The worker
 * writes the PNG into a small on-disk cache and answers with its path — a
 * file reference is metadata, which is what the control connection carries.
 * The cache is bounded by file count, total bytes and age, and a file's name
 * carries the source's mtime and size, so a re-encoded clip gets a new entry
 * instead of a stale card.
 */

#pragma once

#include <cstdint>
#include <string>

namespace openshow::media {

// Decodes the first video frame of `path`, scales it to fit inside
// maxWidth x maxHeight (aspect preserved, never upscaled), and encodes it as
// PNG. On success fills *base64Png with the base64 text and *width/*height
// with the thumbnail's pixel size. On failure fills *error and returns false.
bool videoThumbnailBase64(const std::string& path, int maxWidth, int maxHeight,
                          std::string* base64Png, int* width, int* height,
                          std::string* error);

// The PNG bytes alone (the base64 form is this plus the encoder).
bool videoThumbnailPng(const std::string& path, int maxWidth, int maxHeight,
                       std::string* png, int* width, int* height, std::string* error);

// The Media Bin's on-disk thumbnail cache (review D7b). Decodes the first
// frame into `cacheDir` (created if needed) under a name that fingerprints the
// source's path, mtime, size and box, reuses an existing entry, and prunes the
// directory to kMaxFiles / kMaxBytes / kMaxAgeSeconds. On success fills
// *cachePath with the absolute file path. A decode failure is the same
// failure videoThumbnailPng reports.
bool videoThumbnailCachePath(const std::string& path, int maxWidth, int maxHeight,
                             const std::string& cacheDir, std::string* cachePath,
                             int* width, int* height, std::string* error);

// The budgets, exposed so a fixture can assert the pruning rule rather than
// guess it.
inline constexpr std::size_t kThumbnailCacheMaxFiles = 64;
inline constexpr std::uintmax_t kThumbnailCacheMaxBytes = 32ull * 1024ull * 1024ull;
inline constexpr std::int64_t kThumbnailCacheMaxAgeSeconds = 24 * 60 * 60;

}  // namespace openshow::media
