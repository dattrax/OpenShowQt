/*
 * ND.6: the live camera hub, proven headless. A fake helper emits the real
 * UYVY shape; the hub converts to NV12, uploads through the same
 * GpuYuvUploader FFmpeg playback uses, and wraps the slot as an SkImage.
 * Also covers the deterministic source-lost path. Exits 77 when no Vulkan
 * device is available, like the other GPU tests.
 */

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "include/core/SkImage.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/vk/GrVkDirectContext.h"

#include "live/live_source_hub.h"
#include "output/VulkanContext.h"

namespace {

bool gFailed = false;

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        gFailed = true;
    }
    return condition;
}

openshow::live::HelperOptions fakeOptions(bool loseSource) {
    openshow::live::HelperOptions options;
    options.executable = OPENSHOW_FAKE_HELPER_BIN;
    options.arguments = {"--fake", "--fake-format", "uyvy", "--fake-static"};
    if (loseSource) {
        options.arguments.push_back("--fake-lose-source-ms");
        options.arguments.push_back("120");
    }
    return options;
}

openshow::live::HelperOptions fakeCrashOptions() {
    openshow::live::HelperOptions options;
    options.executable = OPENSHOW_FAKE_HELPER_BIN;
    options.arguments = {"--fake", "--fake-format", "uyvy", "--fake-static",
                         "--fake-crash-ms", "150"};
    return options;
}

int waitForSlot(openshow::live::LiveSourceHub& hub, std::string* error,
                int timeoutMs = 3000) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        const int slot = hub.pump(error);
        if (slot >= 0) {
            return slot;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return -1;
}

}  // namespace

int main() {
    openshow::worker::VulkanContext vk;
    if (!vk.createHeadless()) {
        std::fprintf(stderr, "live_hub_tests: no Vulkan device; skipping\n");
        return 77;
    }
    sk_sp<GrDirectContext> gr = GrDirectContexts::MakeVulkan(vk.backendContext());
    if (!gr) {
        std::fprintf(stderr, "live_hub_tests: GrDirectContexts::MakeVulkan failed; skipping\n");
        return 77;
    }

    openshow::live::LiveSourceHub::Gpu gpu;
    gpu.device = vk.device();
    gpu.physicalDevice = vk.physicalDevice();
    gpu.graphicsQueueIndex = vk.queueIndex();
    gpu.uploadQueueIndex = vk.uploadQueueIndex();
    gpu.uploadQueue = vk.uploadQueue();
    gpu.context = gr.get();

    std::string error;
    openshow::live::LiveSourceHub hub;
    check(hub.start("Fake camera 1", gpu, &error, fakeOptions(false)),
          "live hub: starts against the fake helper");
    check(hub.active(), "live hub: active after open");

    const int first = waitForSlot(hub, &error);
    check(first >= 0, "live hub: a frame is uploaded");
    SkImage* image = hub.image(first, &error);
    check(image != nullptr && image->width() == 320 && image->height() == 180,
          "live hub: the wrapped image is the fake's 320x180 frame");
    hub.presented(first, VK_NULL_HANDLE);

    const int second = waitForSlot(hub, &error);
    check(second >= 0, "live hub: a newer frame is uploaded");
    check(second != first, "live hub: the next frame uses another slot");
    SkImage* secondImage = hub.image(second, &error);
    check(secondImage != nullptr, "live hub: the newer image wraps");
    hub.presented(second, VK_NULL_HANDLE);
    hub.stop();
    check(!hub.active(), "live hub: stop leaves it inactive");

    // The deterministic source-loss path: the hub stops and reports why.
    openshow::live::LiveSourceHub loss;
    error.clear();
    check(loss.start("Fake camera 1", gpu, &error, fakeOptions(true)),
          "live hub: starts with a losing source");
    std::string lossError;
    bool lost = false;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        const int slot = loss.pump(&lossError);
        if (slot < 0 && !loss.active() && !lossError.empty()) {
            lost = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(lost, "live hub: source-lost stops the hub with a reason");
    loss.stop();

    // ND.8: a helper that EXITS is a typed fault, not "no new frames". The
    // fake process exits after 150 ms without a close handshake; the hub must
    // stop and hand back the reason rather than keep presenting a dead frame.
    openshow::live::LiveSourceHub crash;
    error.clear();
    check(crash.start("Fake camera 1", gpu, &error, fakeCrashOptions()),
          "live hub: starts with a crashing helper");
    std::string crashError;
    bool crashed = false;
    const auto crashDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < crashDeadline) {
        const int slot = crash.pump(&crashError);
        if (slot < 0 && !crash.active() && !crashError.empty()) {
            crashed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(crashed, "live hub: a helper crash stops the hub with a reason");
    crash.stop();

    if (gFailed) {
        std::fprintf(stderr, "live_hub_tests: FAILED\n");
        return 1;
    }
    std::printf("live_hub_tests: all checks passed\n");
    return 0;
}
