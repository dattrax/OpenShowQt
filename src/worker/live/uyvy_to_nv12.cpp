#include "live/uyvy_to_nv12.h"

#include <algorithm>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define OPENSHOW_LIVE_X86 1
#endif

namespace openshow::live {

namespace {

bool fail(std::string* error, const char* message) {
    if (error) {
        *error = message;
    }
    return false;
}

struct Geometry {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t chromaRows = 0;
};

// The reference path. Also what runs on non-x86 and on x86 without SSSE3.
void convertScalar(const std::uint8_t* source, std::uint32_t stride,
                   const Geometry& geometry, Nv12Frame* out) {
    const std::uint32_t width = geometry.width;
    const std::uint32_t height = geometry.height;
    for (std::uint32_t row = 0; row < height; ++row) {
        const std::uint8_t* src = source + static_cast<std::size_t>(row) * stride;
        std::uint8_t* dst = out->y.data() + static_cast<std::size_t>(row) * width;
        for (std::uint32_t x = 0; x < width; ++x) {
            dst[x] = src[x * 2 + 1];
        }
    }

    for (std::uint32_t row = 0; row < height; row += 2) {
        const std::uint8_t* row0 = source + static_cast<std::size_t>(row) * stride;
        const std::uint8_t* row1 = row + 1 < height ? row0 + stride : row0;
        std::uint8_t* dst = out->uv.data() + static_cast<std::size_t>(row / 2) * width;
        for (std::uint32_t x = 0; x < width; x += 2) {
            const std::uint32_t u =
                (static_cast<std::uint32_t>(row0[x * 2]) + row1[x * 2] + 1) >> 1;
            const std::uint32_t v =
                (static_cast<std::uint32_t>(row0[x * 2 + 2]) + row1[x * 2 + 2] + 1) >> 1;
            dst[x] = static_cast<std::uint8_t>(u);
            dst[x + 1] = static_cast<std::uint8_t>(v);
        }
    }
}

#if defined(OPENSHOW_LIVE_X86) && (defined(__GNUC__) || defined(__clang__))

__attribute__((target("ssse3"))) void convertSsse3(const std::uint8_t* source,
                                                   std::uint32_t stride,
                                                   const Geometry& geometry, Nv12Frame* out) {
    const std::uint32_t width = geometry.width;
    const std::uint32_t height = geometry.height;

    // UYVY packs [U Y0 V Y1]. The Y mask keeps bytes 1,3,...,15; the high half
    // is zeroed so the result can be stored as one 8-byte luma run.
    const __m128i yMask = _mm_setr_epi8(1, 3, 5, 7, 9, 11, 13, 15,
                                        -1, -1, -1, -1, -1, -1, -1, -1);
    for (std::uint32_t row = 0; row < height; ++row) {
        const std::uint8_t* src = source + static_cast<std::size_t>(row) * stride;
        std::uint8_t* dst = out->y.data() + static_cast<std::size_t>(row) * width;
        std::uint32_t x = 0;
        for (; x + 8 <= width; x += 8) {
            const __m128i packed =
                _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + x * 2));
            const __m128i luma = _mm_shuffle_epi8(packed, yMask);
            _mm_storel_epi64(reinterpret_cast<__m128i*>(dst + x), luma);
        }
        for (; x < width; ++x) {
            dst[x] = src[x * 2 + 1];
        }
    }

    // U bytes are at 0,4,8,12 and V at 2,6,10,14 in each 16-byte block; avg
    // rounds (a+b+1)>>1, and unpacklo interleaves U,V,U,V.
    const __m128i uMask = _mm_setr_epi8(0, 4, 8, 12, -1, -1, -1, -1,
                                        -1, -1, -1, -1, -1, -1, -1, -1);
    const __m128i vMask = _mm_setr_epi8(2, 6, 10, 14, -1, -1, -1, -1,
                                        -1, -1, -1, -1, -1, -1, -1, -1);
    for (std::uint32_t row = 0; row < height; row += 2) {
        const std::uint8_t* row0 = source + static_cast<std::size_t>(row) * stride;
        const std::uint8_t* row1 = row + 1 < height ? row0 + stride : row0;
        std::uint8_t* dst = out->uv.data() + static_cast<std::size_t>(row / 2) * width;
        std::uint32_t x = 0;
        for (; x + 8 <= width; x += 8) {
            const __m128i a =
                _mm_loadu_si128(reinterpret_cast<const __m128i*>(row0 + x * 2));
            const __m128i b =
                _mm_loadu_si128(reinterpret_cast<const __m128i*>(row1 + x * 2));
            const __m128i u = _mm_avg_epu8(_mm_shuffle_epi8(a, uMask),
                                           _mm_shuffle_epi8(b, uMask));
            const __m128i v = _mm_avg_epu8(_mm_shuffle_epi8(a, vMask),
                                           _mm_shuffle_epi8(b, vMask));
            _mm_storel_epi64(reinterpret_cast<__m128i*>(dst + x),
                             _mm_unpacklo_epi8(u, v));
        }
        for (; x < width; x += 2) {
            const std::uint32_t u =
                (static_cast<std::uint32_t>(row0[x * 2]) + row1[x * 2] + 1) >> 1;
            const std::uint32_t v =
                (static_cast<std::uint32_t>(row0[x * 2 + 2]) + row1[x * 2 + 2] + 1) >> 1;
            dst[x] = static_cast<std::uint8_t>(u);
            dst[x + 1] = static_cast<std::uint8_t>(v);
        }
    }
}

bool ssse3Available() {
    static const bool available = __builtin_cpu_supports("ssse3");
    return available;
}

#endif

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

    Geometry geometry;
    geometry.width = source.width;
    geometry.height = source.height;
    geometry.chromaRows = (source.height + 1) / 2;

    out->width = source.width;
    out->height = source.height;
    out->sequence = source.sequence;
    out->timestampNs = source.timestampNs;
    out->y.resize(static_cast<std::size_t>(geometry.width) * geometry.height);
    out->uv.resize(static_cast<std::size_t>(geometry.width) * geometry.chromaRows);

#if defined(OPENSHOW_LIVE_X86) && (defined(__GNUC__) || defined(__clang__))
    if (ssse3Available()) {
        convertSsse3(source.pixels.data(), source.stride, geometry, out);
        return true;
    }
#endif
    convertScalar(source.pixels.data(), source.stride, geometry, out);
    return true;
}

}  // namespace openshow::live
