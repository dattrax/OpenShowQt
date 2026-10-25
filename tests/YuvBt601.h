#pragma once

/*
 * Independently specified BT.601 limited-range YUV->RGB expectations for the
 * GPU tests (task 6.2a, review R9). Until now the GPU tests asked Skia's
 * private src/core/SkYUVMath.h for the same matrix its sampler uses — a
 * private header in test code, and circular as an oracle. Instead the
 * expectation is written here from the standard itself: ITU-R BT.601
 * (Kr = 0.299, Kb = 0.114), 8-bit narrow range (Y in 16..235, Cb/Cr in
 * 16..240) expanded to full range for display, in its natural offset form
 *
 *     R = S (Y - 16/255) + a_vr (V - 128/255)
 *     G = S (Y - 16/255) - a_ub (U - 128/255) - a_ur (V - 128/255)
 *     B = S (Y - 16/255) + a_bu (U - 128/255)
 *
 * with S = 255/219 and the published coefficient set of that transform
 * (the decimal form carried by FFmpeg's legacy conversion tables and
 * libyuv's BT.601 constants). The GPU side renders through the Vulkan
 * driver's own REC601/LIMITED sampler conversion, so pixel expectations
 * here and the sampled colours under test have no shared source.
 */

namespace openshow::test {

inline constexpr double kBt601LimitedScale = 255.0 / 219.0;  // S
inline constexpr double kBt601VR = 1.596027;                 // a_vr (scaled)
inline constexpr double kBt601UB = 0.391762;                 // a_ub (scaled)
inline constexpr double kBt601UR = 0.812968;                 // a_ur (scaled)
inline constexpr double kBt601BU = 2.017232;                 // a_bu (scaled)

// Fills the classic 4x5 colour-matrix layout (rows R, G, B, alpha; each
// row is coefficients on Y, U, V, A followed by the offset), so a test
// computes expected = row . (Y/255, U/255, V/255, 1, 1). The offsets fold
// the luma footroom and the 128 chroma bias into that layout.
inline void bt601LimitedYuvToRgb(float m[20]) {
    constexpr double kLumaFoot = kBt601LimitedScale * 16.0 / 255.0;
    constexpr double kHalf = 128.0 / 255.0;
    for (int row = 0; row < 20; ++row) {
        m[row] = 0.0f;
    }
    m[0] = static_cast<float>(kBt601LimitedScale);
    m[2] = static_cast<float>(kBt601VR);
    m[4] = static_cast<float>(-(kBt601VR * kHalf + kLumaFoot));
    m[5] = static_cast<float>(kBt601LimitedScale);
    m[6] = static_cast<float>(-kBt601UB);
    m[7] = static_cast<float>(-kBt601UR);
    m[9] = static_cast<float>(kBt601UB * kHalf + kBt601UR * kHalf - kLumaFoot);
    m[10] = static_cast<float>(kBt601LimitedScale);
    m[11] = static_cast<float>(kBt601BU);
    m[14] = static_cast<float>(-(kBt601BU * kHalf + kLumaFoot));
    m[18] = 1.0f;
}

}  // namespace openshow::test
