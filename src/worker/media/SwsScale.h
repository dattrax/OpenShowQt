/*
 * SwsScale: the one place the media module builds a swscale context.
 *
 * Phone and camera clips commonly decode to the deprecated full-range YUV
 * formats (yuvj420p and friends), or to yuv420p carrying colour_range=jpeg.
 * The legacy swscale API cannot read the range from the frame, so it logs
 * "deprecated pixel format used, make sure you did set range correctly" for
 * every context built from those formats — and, for the non-J spelling,
 * silently treats full-range bytes as limited-range. Both decode paths (the
 * thumbnail's RGB still and the frame ring's NV12 upload) go through here:
 * the deprecated format is normalised to its non-J spelling and the input
 * range is stated explicitly, so the picture keeps the range it was encoded
 * with and the console stays quiet.
 */

#pragma once

#include "FFmpeg.h"

namespace openshow::media {

// The J formats are the deprecated full-range spellings of their non-J
// counterparts. Returns the modern spelling and tells the caller whether the
// input is full range by definition.
inline AVPixelFormat normaliseDeprecatedYuvj(AVPixelFormat format, bool* fullRange) {
    switch (format) {
    case AV_PIX_FMT_YUVJ420P:
        *fullRange = true;
        return AV_PIX_FMT_YUV420P;
    case AV_PIX_FMT_YUVJ422P:
        *fullRange = true;
        return AV_PIX_FMT_YUV422P;
    case AV_PIX_FMT_YUVJ444P:
        *fullRange = true;
        return AV_PIX_FMT_YUV444P;
    case AV_PIX_FMT_YUVJ440P:
        *fullRange = true;
        return AV_PIX_FMT_YUV440P;
    default:
        *fullRange = false;
        return format;
    }
}

// Builds a context for one conversion. `sourceFullRange` is the frame's own
// colour_range answer (frame->color_range == AVCOL_RANGE_JPEG); a deprecated
// J format forces it true. The destination's range and coefficient table are
// whatever swscale chose, so only the input side changes.
inline SwsContext* swsContextFor(int srcWidth, int srcHeight, AVPixelFormat srcFormat,
                                 bool sourceFullRange, int dstWidth, int dstHeight,
                                 AVPixelFormat dstFormat, int flags) {
    bool deprecatedFullRange = false;
    const AVPixelFormat normalised =
        normaliseDeprecatedYuvj(srcFormat, &deprecatedFullRange);
    SwsContext* context = sws_getContext(srcWidth, srcHeight, normalised, dstWidth,
                                         dstHeight, dstFormat, flags, nullptr,
                                         nullptr, nullptr);
    if (context != nullptr && (sourceFullRange || deprecatedFullRange)) {
        int* inputTable = nullptr;
        int* outputTable = nullptr;
        int inputRange = 0;
        int outputRange = 0;
        int brightness = 0;
        int contrast = 0;
        int saturation = 0;
        if (sws_getColorspaceDetails(context, &inputTable, &inputRange, &outputTable,
                                     &outputRange, &brightness, &contrast,
                                     &saturation) >= 0) {
            // 1 = full/JPEG range on the input; everything else is kept.
            sws_setColorspaceDetails(context, inputTable, 1, outputTable, outputRange,
                                     brightness, contrast, saturation);
        }
    }
    return context;
}

}  // namespace openshow::media
