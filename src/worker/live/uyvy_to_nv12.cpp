#include "live/uyvy_to_nv12.h"

namespace openshow::live {

namespace {

bool fail(std::string* error, const char* message) {
    if (error) {
        *error = message;
    }
    return false;
}

}  // namespace

bool convertUyvyToNv12(const Frame& source, Nv12Frame* out, std::string* error) {
    if (!out) {
        return fail(error, "uyvy: no destination");
    }
    if (source.format != static_cast<std::uint32_t>(FrameFormat::UYVY)) {
        return fail(error, "uyvy: the frame is not UYVY");
    }
    if (source.width == 0 || source.height == 0) {
        return fail(error, "uyvy: the frame has no size");
    }
    if ((source.width % 2) != 0) {
        return fail(error, "uyvy: an odd width has no chroma pairs");
    }
    const std::uint64_t minStride = static_cast<std::uint64_t>(source.width) * 2;
    if (source.stride < minStride) {
        return fail(error, "uyvy: the stride is smaller than the pixels");
    }
    const std::uint64_t needed =
        static_cast<std::uint64_t>(source.stride) * source.height;
    if (source.pixels.size() < needed) {
        return fail(error, "uyvy: the pixel buffer is smaller than the frame");
    }

    const std::uint32_t width = source.width;
    const std::uint32_t height = source.height;
    const std::uint32_t chromaRows = (height + 1) / 2;

    out->width = width;
    out->height = height;
    out->sequence = source.sequence;
    out->timestampNs = source.timestampNs;
    out->y.resize(static_cast<std::size_t>(width) * height);
    out->uv.resize(static_cast<std::size_t>(width) * chromaRows);

    const std::uint8_t* sourceBase = source.pixels.data();
    std::uint8_t* yBase = out->y.data();
    for (std::uint32_t row = 0; row < height; ++row) {
        const std::uint8_t* src = sourceBase + static_cast<std::size_t>(row) * source.stride;
        std::uint8_t* dst = yBase + static_cast<std::size_t>(row) * width;
        // UYVY packs [U Y0 V Y1]: luma is the odd byte of each pair.
        for (std::uint32_t x = 0; x < width; ++x) {
            dst[x] = src[x * 2 + 1];
        }
    }

    std::uint8_t* uvBase = out->uv.data();
    for (std::uint32_t row = 0; row < height; row += 2) {
        const std::uint8_t* row0 = sourceBase + static_cast<std::size_t>(row) * source.stride;
        const std::uint8_t* row1 =
            row + 1 < height ? row0 + source.stride : row0;
        std::uint8_t* dst = uvBase + static_cast<std::size_t>(row / 2) * width;
        for (std::uint32_t x = 0; x < width; x += 2) {
            // Chroma is co-sited horizontally; NV12 halves the vertical
            // resolution, so average the two source rows with rounding.
            const std::uint32_t u =
                (static_cast<std::uint32_t>(row0[x * 2]) + row1[x * 2] + 1) >> 1;
            const std::uint32_t v =
                (static_cast<std::uint32_t>(row0[x * 2 + 2]) + row1[x * 2 + 2] + 1) >> 1;
            dst[x] = static_cast<std::uint8_t>(u);
            dst[x + 1] = static_cast<std::uint8_t>(v);
        }
    }
    return true;
}

}  // namespace openshow::live
