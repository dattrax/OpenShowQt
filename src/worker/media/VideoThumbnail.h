/*
 * VideoThumbnail: one first frame, scaled and PNG-encoded, for the Media Bin's
 * video cards (MB.6).
 *
 * Bounded by construction: it opens the container, decodes until the first
 * frame arrives (at most 128 packets), scales that one image to fit the
 * requested box, and returns a base64 PNG. No audio, no seek, no loop, no
 * cache. The public type speaks plain C++ (FFmpeg and libpng stay in the
 * .cpp), matching the rest of the media module.
 */

#pragma once

#include <string>

namespace openshow::media {

// Decodes the first video frame of `path`, scales it to fit inside
// maxWidth x maxHeight (aspect preserved, never upscaled), and encodes it as
// PNG. On success fills *base64Png with the base64 text and *width/*height
// with the thumbnail's pixel size. On failure fills *error and returns false.
bool videoThumbnailBase64(const std::string& path, int maxWidth, int maxHeight,
                          std::string* base64Png, int* width, int* height,
                          std::string* error);

}  // namespace openshow::media
