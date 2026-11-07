/*
 * UYVY (packed 4:2:2) to NV12 (semi-planar 4:2:0) for live sources (ND.4/ND.6).
 *
 * NDI|HX and full NDI hand the receiver packed UYVY (4:2:2, two bytes per
 * pixel), which is the standard receive API's YUV output. The compressed H.264
 * or HEVC stream is usually 4:2:0; the runtime expands it on the way out, so
 * no RGB conversion happens in the SDK but the vertical chroma rows are
 * upsampled. RADV on the target GPU cannot sample a 4:2:2 Vulkan format, so
 * the worker converts to NV12 and feeds the same GpuYuvUploader the FFmpeg
 * video path uses. The conversion is a Y-plane deinterleave and a vertical
 * chroma average; it allocates nothing when the caller reuses the output.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "live/live_source.h"

namespace openshow::live {

struct Nv12Frame {
    std::vector<std::uint8_t> y;   // width * height
    std::vector<std::uint8_t> uv;  // width * ((height + 1) / 2), U/V interleaved
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t sequence = 0;
    std::uint64_t timestampNs = 0;

    bool empty() const { return y.empty(); }
    // The existing uploader's argument shape: whole rows, no padding.
    int yRowBytes() const { return static_cast<int>(width); }
    int uvRowBytes() const { return static_cast<int>(width); }
};

// Converts a UYVY frame. A 4:2:2 source has chroma for every row; NV12 keeps
// one chroma row per two luma rows, so each output chroma sample is the
// rounded average of the two source rows. An odd final row is averaged with
// itself. `source.stride` may include padding.
bool convertUyvyToNv12(const Frame& source, Nv12Frame* out, std::string* error);

}  // namespace openshow::live
