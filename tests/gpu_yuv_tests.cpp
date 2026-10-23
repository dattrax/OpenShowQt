/*
 * Task 1.5d: the YUV GPU upload path, proven headless.
 *
 * Creates a surface-less Vulkan device (the same VulkanContext the windowed
 * worker uses, headless mode), uploads synthetic I420 frames through
 * GpuYuvUploader (staging → device-local R8/R8G8 images, transition to
 * shader-read, one persistent SkImage wrap per slot), draws the wrapped
 * image 1:1 into a GPU render target, reads it back, and compares probe
 * pixels against the very YUV→RGB matrix Skia applies (SkColorMatrix_YUV2RGB)
 * within a ±3 tolerance for dithering. Upload timings are reported from the
 * uploader's stats, split into staging (memcpy) and GPU (submit + fence).
 *
 * Exits 77 (ctest "skipped") when no Vulkan device or context can be
 * created — the headless preset runs on machines with and without GPUs.
 */

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImage.h"
#include "include/core/SkSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/vk/GrVkDirectContext.h"

#include "src/core/SkYUVMath.h"  // SkColorMatrix_YUV2RGB (expected values)

#include "output/VulkanContext.h"
#include "render/YuvGpu.h"

namespace {

constexpr int kW = 64;
constexpr int kH = 64;
constexpr int kTolerance = 3;  // dither + chroma filtering slack

bool gFailed = false;

bool check(bool condition, const char* what) {
    if (!condition) {
        gFailed = true;
    }
    std::printf("%s: %s\n", condition ? "ok" : "FAIL", what);
    return condition;
}

// I420, compact rows: Y is w*h, UV is (w/2)*(h/2) pairs.
struct I420 {
    std::vector<uint8_t> y;
    std::vector<uint8_t> uv;

    I420() : y(kW * kH), uv((kW / 2) * (kH / 2) * 2) {}

    void setUv(int uvX, int uvY, uint8_t u, uint8_t v) {
        const size_t index = (static_cast<size_t>(uvY) * (kW / 2) + uvX) * 2;
        uv[index] = u;
        uv[index + 1] = v;
    }
};

// variant 0: horizontal luma ramp, variant 1: all-bright, variant 2: all-dark.
// Four horizontal chroma bands: neutral, red-ish, blue-ish, cyan-dim.
I420 makePattern(int variant) {
    I420 frame;
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            int luma;
            switch (variant) {
                case 1: luma = 235; break;
                case 2: luma = 16; break;
                default: luma = 16 + (x * 219 + (kW - 1) / 2) / (kW - 1); break;
            }
            frame.y[static_cast<size_t>(y) * kW + x] = static_cast<uint8_t>(luma);
        }
    }
    for (int uvY = 0; uvY < kH / 2; ++uvY) {
        const int band = uvY / (kH / 8);  // 4 bands of 16 luma rows
        uint8_t u = 128, v = 128;
        switch (band) {
            case 1: v = 235; break;  // red
            case 2: u = 235; break;  // blue
            case 3: u = 16; break;   // dim cyan
            default: break;
        }
        for (int uvX = 0; uvX < kW / 2; ++uvX) {
            frame.setUv(uvX, uvY, u, v);
        }
    }
    return frame;
}

int bandAt(int y) {
    return y / 16;
}

uint8_t lumaAt(int variant, int x) {
    switch (variant) {
        case 1: return 235;
        case 2: return 16;
        default: return static_cast<uint8_t>(16 + (x * 219 + (kW - 1) / 2) / (kW - 1));
    }
}

struct Rgb {
    int r, g, b;
};

Rgb expected(int variant, int x, int y) {
    const uint8_t luma = lumaAt(variant, x);
    const int band = bandAt(y);
    const int u = band == 1 ? 128 : band == 2 ? 235 : band == 3 ? 16 : 128;
    const int v = band == 1 ? 235 : 128;
    float m[20];
    SkColorMatrix_YUV2RGB(kRec601_Limited_SkYUVColorSpace, m);
    const float in[4] = {luma / 255.0f, u / 255.0f, v / 255.0f, 1.0f};
    Rgb out;
    out.r = 0;
    out.g = 0;
    out.b = 0;
    auto row = [&](int r) {
        const float value = m[r * 5 + 0] * in[0] + m[r * 5 + 1] * in[1] + m[r * 5 + 2] * in[2] +
                            m[r * 5 + 3] * in[3] + m[r * 5 + 4];
        return static_cast<int>(std::lround(value * 255.0f));
    };
    out.r = std::max(0, std::min(255, row(0)));
    out.g = std::max(0, std::min(255, row(1)));
    out.b = std::max(0, std::min(255, row(2)));
    return out;
}

std::vector<uint8_t> drawAndRead(GrDirectContext* gr, SkImage* image) {
    sk_sp<SkSurface> surface = SkSurfaces::RenderTarget(
        gr, skgpu::Budgeted::kNo,
        SkImageInfo::Make(kW, kH, kRGBA_8888_SkColorType, kPremul_SkAlphaType,
                          SkColorSpace::MakeSRGB()));
    if (!surface) {
        return {};
    }
    surface->getCanvas()->clear(SK_ColorBLACK);
    surface->getCanvas()->drawImage(image, 0, 0);
    std::vector<uint8_t> pixels(static_cast<size_t>(kW) * kH * 4);
    const SkImageInfo info =
        SkImageInfo::Make(kW, kH, kRGBA_8888_SkColorType, kPremul_SkAlphaType,
                          SkColorSpace::MakeSRGB());
    if (!surface->readPixels(info, pixels.data(), kW * 4, 0, 0)) {
        return {};
    }
    return pixels;
}

bool probe(const std::vector<uint8_t>& pixels, int x, int y, const Rgb& want,
           const char* label) {
    const size_t offset = (static_cast<size_t>(y) * kW + x) * 4;
    const int got[3] = {pixels[offset], pixels[offset + 1], pixels[offset + 2]};
    const int delta[3] = {std::abs(got[0] - want.r), std::abs(got[1] - want.g),
                          std::abs(got[2] - want.b)};
    char message[256];
    std::snprintf(message, sizeof(message),
                  "%s at (%d,%d): got rgb(%d,%d,%d) want rgb(%d,%d,%d)", label, x, y, got[0],
                  got[1], got[2], want.r, want.g, want.b);
    return check(delta[0] <= kTolerance && delta[1] <= kTolerance && delta[2] <= kTolerance,
                 message);
}

}  // namespace

int main() {
    openshow::worker::VulkanContext vk;
    if (!vk.createHeadless()) {
        std::fprintf(stderr, "gpu_yuv_tests: no Vulkan device; skipping\n");
        return 77;
    }
    sk_sp<GrDirectContext> gr = GrDirectContexts::MakeVulkan(vk.backendContext());
    if (!gr) {
        std::fprintf(stderr, "gpu_yuv_tests: GrDirectContexts::MakeVulkan failed; skipping\n");
        return 77;
    }

    openshow::worker::GpuYuvUploader uploader;
    std::string error;
    if (!uploader.create(vk.device(), vk.physicalDevice(), vk.queueIndex(), vk.uploadQueueIndex(),
                         vk.uploadQueue(), gr.get(), kW, kH, kRec601_Limited_SkYUVColorSpace,
                         &error)) {
        std::fprintf(stderr, "gpu_yuv_tests: uploader.create failed: %s\n", error.c_str());
        return 1;
    }
    std::printf("upload queue %s (graphics family %u, upload family %u)\n",
                vk.hasDedicatedUploadQueue() ? "dedicated" : "shared with graphics",
                vk.queueIndex(), vk.uploadQueueIndex());

    // ---- pixel correctness through the wrapped image ---------------------
    struct VariantCase {
        int variant;
        const char* label;
    };
    const VariantCase cases[] = {
        {0, "luma ramp"},
        {1, "all-bright"},
        {2, "all-dark"},
    };
    int expectedUploads = 0;
    for (const VariantCase& testCase : cases) {
        const I420 frame = makePattern(testCase.variant);
        openshow::worker::GpuYuvUploader::Frame uploaded;
        if (!uploader.upload(frame.y.data(), kW, frame.uv.data(), kW,
                             static_cast<uint64_t>(expectedUploads + 1), 0.0, &uploaded)) {
            std::fprintf(stderr, "gpu_yuv_tests: upload failed (%s)\n", testCase.label);
            return 1;
        }
        ++expectedUploads;
        check(uploaded.image != nullptr && uploaded.slot < openshow::worker::GpuYuvUploader::kSlots,
              "upload returns a wrapped image and a slot");

        const std::vector<uint8_t> pixels = drawAndRead(gr.get(), uploaded.image.get());
        if (pixels.empty()) {
            std::fprintf(stderr, "gpu_yuv_tests: readback failed (%s)\n", testCase.label);
            return 1;
        }
        // One probe per chroma band; chroma is constant inside each band, so
        // the band centre is independent of siting and filtering details.
        probe(pixels, 8, 8, expected(testCase.variant, 8, 8), testCase.label);
        probe(pixels, 24, 24, expected(testCase.variant, 24, 24), testCase.label);
        probe(pixels, 40, 40, expected(testCase.variant, 40, 40), testCase.label);
        probe(pixels, 56, 56, expected(testCase.variant, 56, 56), testCase.label);

        // Structural guard (ramp variant only): there the luma term leaves
        // headroom, so band 1 must read red-dominant and band 2 blue. In the
        // saturated all-bright variant both channels pin at 255 and the
        // comparison is meaningless by design.
        if (testCase.variant == 0) {
            const size_t red = (static_cast<size_t>(24) * kW + 24) * 4;
            const size_t blue = (static_cast<size_t>(40) * kW + 40) * 4;
            check(pixels[red] > pixels[red + 2] + 40, "chroma band 1 reads red-dominant");
            check(pixels[blue + 2] > pixels[blue] + 40, "chroma band 2 reads blue-dominant");
        }

        uploader.release(uploaded.slot);
    }

    // ---- ring behaviour: depth is bounded and enforced -------------------
    // Fill all kSlots without releasing: the next upload must refuse rather
    // than overwrite a slot the consumer still holds.
    const I420 frame = makePattern(0);
    for (int i = 0; i < openshow::worker::GpuYuvUploader::kSlots; ++i) {
        openshow::worker::GpuYuvUploader::Frame uploaded;
        if (!uploader.upload(frame.y.data(), kW, frame.uv.data(), kW, 100 + i, 0.0, &uploaded)) {
            std::fprintf(stderr, "gpu_yuv_tests: fill upload %d failed\n", i);
            return 1;
        }
        ++expectedUploads;
    }
    openshow::worker::GpuYuvUploader::Frame overflow;
    check(!uploader.upload(frame.y.data(), kW, frame.uv.data(), kW, 999, 0.0, &overflow),
          "a full ring refuses new uploads (never grows, never overwrites)");
    check(uploader.stats().uploads == static_cast<uint64_t>(expectedUploads),
          "a refused upload is not counted");

    uploader.releaseAll();
    openshow::worker::GpuYuvUploader::Frame afterRelease;
    check(uploader.upload(frame.y.data(), kW, frame.uv.data(), kW, 1000, 0.0, &afterRelease),
          "releaseAll makes room again");
    ++expectedUploads;
    uploader.release(afterRelease.slot);

    // ---- instrumented upload cost ----------------------------------------
    const auto& stats = uploader.stats();
    std::printf("uploads=%llu slotsCreated=%llu waitsForFreeSlot=%llu stage=%.3fms gpu=%.3fms\n",
                static_cast<unsigned long long>(stats.uploads),
                static_cast<unsigned long long>(stats.slotsCreated),
                static_cast<unsigned long long>(stats.waitsForFreeSlot), stats.totalStageMs,
                stats.totalGpuMs);
    check(stats.uploads == static_cast<uint64_t>(expectedUploads), "upload count matches");
    check(stats.slotsCreated == openshow::worker::GpuYuvUploader::kSlots,
          "exactly one GPU allocation per slot (reused frames rewrap nothing)");

    uploader.destroy();
    gr.reset();
    return gFailed ? 1 : 0;
}
