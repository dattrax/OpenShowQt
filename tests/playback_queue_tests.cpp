// Task 5.1 acceptance: fixtures decode through the production PlaybackPipeline
// without the queue growing. The CPU-mode scenarios pin the ring high-water,
// the parked-on-a-frozen-clock stall and the bounded stop with no GPU at all;
// the GPU scenarios run the same pipeline over a headless Vulkan device and
// prove commits wait on a stuck presenter instead of buffering ahead.
// Fixtures are generated with the ffmpeg CLI; exits 77 (skipped) when the
// CLI or a usable Vulkan device is missing.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include "playback/PlaybackPipeline.h"
#include "output/VulkanContext.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkImage.h"
#include "include/core/SkSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/vk/GrVkDirectContext.h"

namespace {

using openshow::worker::PlaybackPipeline;
using openshow::worker::VideoSubmission;

bool check(bool ok, const char* message) {
    std::fprintf(ok ? stdout : stderr, "%s: %s\n", ok ? "ok" : "FAIL", message);
    return ok;
}

bool generate(const std::string& command) {
    return std::system(command.c_str()) == 0;
}

template <typename Predicate>
bool waitFor(Predicate predicate, int timeoutMs) {
    for (int waited = 0; waited < timeoutMs; waited += 20) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return predicate();
}

// What a sink records while its lambda plays the session's part. With
// `stuck` the sink only accepts submissions — tokens are never presented,
// so the GPU slots stay held downstream.
struct SinkState {
    mutable std::mutex mutex;
    uint64_t count = 0;
    uint64_t lastSequence = 0;
    bool sequencesMonotone = true;
    bool allImagesValid = true;
    // The session-emulator's held frame: each arrival replaces it, and
    // the replaced lease is given up exactly as OutputSession does (the
    // 6.6a seam protocol: onPresented obligates, onReplaced releases).
    bool hasHeld = false;
    unsigned heldToken = 0;
    uint64_t heldSequence = 0;
    unsigned replaces = 0;
};

// present=true: acquire the submission's image (where the pipeline has a
// GPU mode), hand the presentation obligation back, and give up the lease
// of the frame this one replaces — as a frontend that presents every
// frame would.
auto makeSink(PlaybackPipeline& pipeline, SinkState& state, bool present) {
    return [&pipeline, &state, present](const VideoSubmission& submission) {
        SkImage* image = nullptr;
        if (present) {
            unsigned replacedToken = 0;
            uint64_t replacedSequence = 0;
            bool replaced = false;
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                if (state.hasHeld && state.heldToken != submission.token) {
                    replacedToken = state.heldToken;
                    replacedSequence = state.heldSequence;
                    replaced = true;
                }
            }
            if (replaced) {
                pipeline.onReplaced(replacedToken, replacedSequence);
                std::lock_guard<std::mutex> lock(state.mutex);
                ++state.replaces;
            }
            image = pipeline.acquireImage(submission.token, submission.sequence);
        }
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            ++state.count;
            state.sequencesMonotone =
                state.sequencesMonotone && submission.sequence > state.lastSequence;
            state.lastSequence = submission.sequence;
            if (present) {
                state.hasHeld = true;
                state.heldToken = submission.token;
                state.heldSequence = submission.sequence;
            }
            if (present && image == nullptr) {
                state.allImagesValid = false;
            }
        }
        if (present) {
            // Nothing sampled the image, so the obligation is empty; the
            // lease still stands until the next submission replaces it.
            pipeline.onPresented(submission.token, VK_NULL_HANDLE);
        }
    };
}

// --- CPU mode: the bounded queue with no GPU at all ---

// A healthy drain to EOF keeps the ring at or below its configured depth.
bool cpuHealthyDrain(const std::string& path) {
    PlaybackPipeline pipeline;
    PlaybackPipeline::Options options;
    options.path = path;
    options.ringDepth = 3;
    std::string error;
    if (!check(pipeline.openCpu(options, &error), "the pipeline opens the fixture (CPU mode)")) {
        std::fprintf(stderr, "  open: %s\n", error.c_str());
        return false;
    }
    SinkState state;
    if (!check(pipeline.start(makeSink(pipeline, state, true), nullptr, &error),
               "decode and pacing start")) {
        std::fprintf(stderr, "  start: %s\n", error.c_str());
        return false;
    }
    // Drain means paced-out, not just decoded: EOF can settle while frames
    // are still flowing to the sink.
    const bool drained = waitFor([&] { return pipeline.stats().submitted >= 60; }, 15000);
    const auto stats = pipeline.stats();
    pipeline.requestStop();
    pipeline.join();

    bool ok = drained && stats.submitted == 60;
    ok = check(ok, "all 60 frames of the 2 s x 30 fps fixture pace out") && ok;
    ok = check(stats.eof && stats.decoded == 60, "the decode ends cleanly at EOF") && ok;
    ok = check(stats.presents == 60, "every frame was handed back") && ok;
    ok = check(stats.ringPublished == 60 && stats.ringConsumed == 60,
               "the ring carried every frame") && ok;
    ok = check(stats.ringPeakFilled <= 3, "the queue never grew past its three slots") && ok;
    ok = check(stats.decodeErrors == 0 && stats.fatalError.empty(),
               "clean decode, no fatal error") && ok;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        ok = check(state.sequencesMonotone && state.count == 60,
                   "submissions carry the decoder's monotone sequence") && ok;
    }
    std::printf("ok: cpu drain peak=%llu of %u slots, drift mean %+.1fms\n",
                static_cast<unsigned long long>(stats.ringPeakFilled), stats.ringDepth,
                stats.driftMeanMs);
    return ok;
}

// The acceptance sentence's hard half: while the clock is frozen the
// producer parks — the queue neither grows nor races ahead — and one
// released clock later everything still arrives.
bool cpuFrozenClockStall(const std::string& path) {
    PlaybackPipeline pipeline;
    PlaybackPipeline::Options options;
    options.path = path;
    options.ringDepth = 2;
    std::string error;
    if (!check(pipeline.openCpu(options, &error),
               "the pipeline opens the 1080p fixture (depth 2)")) {
        return false;
    }
    std::atomic<double> fakeClock{0.0};
    SinkState state;
    if (!pipeline.start(makeSink(pipeline, state, true), [&] { return fakeClock.load(); },
                        &error)) {
        std::fprintf(stderr, "  start: %s\n", error.c_str());
        return false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    const auto stalledA = pipeline.stats();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    const auto stalledB = pipeline.stats();
    bool ok = true;
    ok = check(stalledA.submitted >= 1 && stalledA.submitted <= 3,
               "the first frame goes out; pacing holds the next against the frozen clock") && ok;
    ok = check(stalledB.submitted == stalledA.submitted,
               "the pipeline is parked: no further submissions while the clock waits") && ok;
    // Published (= decoded) may lead what the pacing thread polled by at
    // most the ring depth plus the one frame in flight with it.
    ok = check(stalledB.ringPeakFilled <= 2 &&
                   stalledB.decoded <= stalledB.submitted + 2 + 1,
               "nothing grew: the high-water is the configured depth and the "
               "producer stalls on the full ring") && ok;
    ok = check(stalledB.decoded < 100, "the decoder did not race through the clip") && ok;
    std::printf("ok: parked at submitted=%llu decoded=%llu peak=%llu of %u\n",
                static_cast<unsigned long long>(stalledB.submitted),
                static_cast<unsigned long long>(stalledB.decoded),
                static_cast<unsigned long long>(stalledB.ringPeakFilled), stalledB.ringDepth);

    // Release the clock: the whole clip still has to arrive, in order.
    fakeClock.store(1.0e6);
    const bool delivered = waitFor([&] { return pipeline.stats().submitted >= 100; }, 20000);
    const auto stats = pipeline.stats();
    ok = check(delivered && stats.submitted == 100 && stats.eof && stats.decoded == 100,
               "after the stall every frame of the 100-frame clip is delivered") && ok;
    ok = check(stats.ringPeakFilled <= 2 && stats.decodeErrors == 0 &&
                   stats.fatalError.empty(),
               "the queue never outgrew its depth across the stall") && ok;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        ok = check(state.sequencesMonotone && state.count == 100,
                   "100 in-order submissions across the stall") && ok;
    }

    // Stop once everything has quietened down: teardown must be prompt.
    const auto stopStart = std::chrono::steady_clock::now();
    pipeline.requestStop();
    pipeline.join();
    const auto stopMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - stopStart)
                            .count();
    ok = check(stopMs < 1000, "stop from a parked state is prompt") && ok;
    return ok;
}

// --- GPU mode: the same contract with real commits and presenters ---

// A healthy GPU run: every frame goes through the direct decode → commit →
// wrapped YCbCr image path once, within the bounded slots.
bool gpuHealthyDrain(const std::string& path, openshow::worker::VulkanContext& vk,
                     GrDirectContext* gr) {
    PlaybackPipeline pipeline;
    PlaybackPipeline::Options options;
    options.path = path;
    PlaybackPipeline::Gpu gpu;
    gpu.device = vk.device();
    gpu.physicalDevice = vk.physicalDevice();
    gpu.graphicsQueueIndex = vk.queueIndex();
    gpu.uploadQueueIndex = vk.uploadQueueIndex();
    gpu.uploadQueue = vk.uploadQueue();
    gpu.context = gr;
    std::string error;
    if (!check(pipeline.openGpu(gpu, options, &error), "the pipeline opens (GPU mode)")) {
        std::fprintf(stderr, "  open: %s\n", error.c_str());
        return false;
    }
    SinkState state;
    if (!pipeline.start(makeSink(pipeline, state, true), nullptr, &error)) {
        std::fprintf(stderr, "  start: %s\n", error.c_str());
        return false;
    }
    const bool drained = waitFor([&] { return pipeline.stats().submitted >= 24; }, 20000);
    const auto stats = pipeline.stats();
    pipeline.requestStop();
    pipeline.join();

    bool ok = drained && stats.submitted == 24;
    ok = check(ok, "the 24-frame fixture paces out (GPU mode)") && ok;
    ok = check(stats.eof && stats.decoded == 24 && stats.presents == 24 &&
                   state.allImagesValid,
               "every submission carried a wrapped YCbCr image and came back") && ok;
    ok = check(stats.ringPeakFilled <= 4 && stats.gpuSlotsCreated == 4,
               "four presentation slots, never more in flight") && ok;
    ok = check(stats.gpuUploads == 24 && stats.directFrames == 24 &&
                   stats.copiedFrames == 0 && stats.downloadedFrames == 0,
               "direct decode end to end: one image copy per frame, zero CPU copies") && ok;
    ok = check(stats.decodeErrors == 0 && stats.fatalError.empty(),
               "clean GPU-mode decode, no fatal error") && ok;
    std::printf("ok: gpu drain peak=%llu of %u slots, %llu uploads, %llu wraps\n",
                static_cast<unsigned long long>(stats.ringPeakFilled), stats.ringDepth,
                static_cast<unsigned long long>(stats.gpuUploads),
                static_cast<unsigned long long>(stats.gpuWraps));
    return ok;
}

// A stuck presenter holds every token: commits to a held slot wait (the
// producer parks there), the ring stays at depth, and the shutdown wakes
// the parked commit instead of deadlocking.
bool gpuStuckPresenter(const std::string& path, openshow::worker::VulkanContext& vk,
                       GrDirectContext* gr) {
    PlaybackPipeline pipeline;
    PlaybackPipeline::Options options;
    options.path = path;
    PlaybackPipeline::Gpu gpu;
    gpu.device = vk.device();
    gpu.physicalDevice = vk.physicalDevice();
    gpu.graphicsQueueIndex = vk.queueIndex();
    gpu.uploadQueueIndex = vk.uploadQueueIndex();
    gpu.uploadQueue = vk.uploadQueue();
    gpu.context = gr;
    std::string error;
    if (!check(pipeline.openGpu(gpu, options, &error), "the stuck scenario opens (GPU mode)")) {
        return false;
    }
    SinkState state;
    // The stuck sink accepts submissions but never presents or drops them.
    if (!pipeline.start(
            [&state](const VideoSubmission& submission) {
                std::lock_guard<std::mutex> lock(state.mutex);
                ++state.count;
            },
            nullptr, &error)) {
        std::fprintf(stderr, "  start: %s\n", error.c_str());
        return false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    const auto stalledA = pipeline.stats();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    const auto stalledB = pipeline.stats();
    bool ok = true;
    // The single decode producer re-claims the ring slot the pacing thread
    // released while its GPU twin is still held, so it parks there — the
    // pipeline halts within its bounds, it does not buffer ahead. How many
    // tokens got out before the first reused slot is a race, not a
    // contract; that it stopped is.
    ok = check(stalledA.submitted >= 1 && stalledB.submitted <= 5,
               "frames go out, then the pacing waits for a held slot") && ok;
    ok = check(stalledB.submitted == stalledA.submitted,
               "with every slot held, submissions stop — nothing buffers ahead") && ok;
    ok = check(stalledB.ringPeakFilled <= 4,
               "the ring stayed at its four presentation slots") && ok;
    ok = check(stalledB.gpuWaitsForFreeSlot >= 1,
               "a commit actually waited for a held slot") && ok;
    ok = check(stalledB.presents == 0 && stalledB.dropped == 0 &&
                   stalledB.fatalError.empty(),
               "a wait is not an error: nothing failed, nothing was released") && ok;
    std::printf("ok: stuck presenter parked at submitted=%llu peak=%llu of %u, slot-waits=%llu\n",
                static_cast<unsigned long long>(stalledB.submitted),
                static_cast<unsigned long long>(stalledB.ringPeakFilled), stalledB.ringDepth,
                static_cast<unsigned long long>(stalledB.gpuWaitsForFreeSlot));

    // The teardown ladder wakes the commit parked on a held slot.
    const auto stopStart = std::chrono::steady_clock::now();
    pipeline.requestStop();
    pipeline.join();
    const auto stopMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - stopStart)
                            .count();
    ok = check(stopMs < 2000, "the shutdown wakes the commit parked on a held slot") && ok;
    pipeline.close();
    return ok;
}

// --- task 6.6a (review R6): the displayed frame's lease ---

// Draw a wrapped slot image into a CPU-readback surface: two readbacks of
// a held frame must agree byte for byte — same image, same content, no
// producer rewrite slipping in between.
std::vector<uint8_t> readBackPixels(GrDirectContext* gr, SkImage* image) {
    if (image == nullptr) {
        return {};
    }
    const SkISize size = image->dimensions();
    sk_sp<SkSurface> surface = SkSurfaces::RenderTarget(
        gr, skgpu::Budgeted::kNo,
        SkImageInfo::Make(size, kRGBA_8888_SkColorType, kPremul_SkAlphaType));
    if (!surface) {
        return {};
    }
    surface->getCanvas()->clear(SK_ColorBLACK);
    surface->getCanvas()->drawImage(image, 0, 0);
    std::vector<uint8_t> pixels(static_cast<size_t>(size.width()) * size.height() * 4);
    const SkImageInfo info = SkImageInfo::Make(size, kRGBA_8888_SkColorType,
                                               kPremul_SkAlphaType);
    if (!surface->readPixels(info, pixels.data(), size.width() * 4, 0, 0)) {
        return {};
    }
    return pixels;
}

// The acceptance sentence of task 6.6a, deterministically: a frame the
// session holds (frozen clock — the parked-decoder analogue of pause and
// of the final frame) must survive every attempted slot reuse by the
// producer AND keep sampling identically through dirty and preview-only
// redraws, until the session gives the lease up. The lease rules make
// this three observable facts: the producer parks on the held slot
// (published stops, slot-waits climb), acquire of the held generation is
// stable, and a release naming the wrong generation frees nothing.
bool gpuHeldFrameLease(const std::string& path, openshow::worker::VulkanContext& vk,
                       GrDirectContext* gr) {
    PlaybackPipeline pipeline;
    PlaybackPipeline::Options options;
    options.path = path;
    PlaybackPipeline::Gpu gpu;
    gpu.device = vk.device();
    gpu.physicalDevice = vk.physicalDevice();
    gpu.graphicsQueueIndex = vk.queueIndex();
    gpu.uploadQueueIndex = vk.uploadQueueIndex();
    gpu.uploadQueue = vk.uploadQueue();
    gpu.context = gr;
    std::string error;
    if (!check(pipeline.openGpu(gpu, options, &error),
               "the held-frame scenario opens (GPU mode)")) {
        std::fprintf(stderr, "  open: %s\n", error.c_str());
        return false;
    }

    // A session-emulating sink: every arrival is drawn (acquire + the
    // onPresented obligation), and the frame it replaces gives its lease
    // up — the 6.6a protocol, in the order the session's run loop uses.
    struct Held {
        std::mutex mutex;
        bool hasFirst = false;
        unsigned firstToken = 0;
        uint64_t firstSequence = 0;
        bool hasHeld = false;
        unsigned heldToken = 0;
        uint64_t heldSequence = 0;
        bool allAcquiresValid = true;
    };
    Held held;
    std::atomic<double> fakeClock{0.0};
    if (!pipeline.start(
            [&](const VideoSubmission& submission) {
                unsigned replacedToken = 0;
                uint64_t replacedSequence = 0;
                bool replaced = false;
                {
                    std::lock_guard<std::mutex> lock(held.mutex);
                    if (held.hasHeld && held.heldToken != submission.token) {
                        replacedToken = held.heldToken;
                        replacedSequence = held.heldSequence;
                        replaced = true;
                    }
                }
                if (replaced) {
                    pipeline.onReplaced(replacedToken, replacedSequence);
                }
                SkImage* image =
                    pipeline.acquireImage(submission.token, submission.sequence);
                std::lock_guard<std::mutex> lock(held.mutex);
                if (!held.hasFirst) {
                    held.hasFirst = true;
                    held.firstToken = submission.token;
                    held.firstSequence = submission.sequence;
                }
                held.hasHeld = true;
                held.heldToken = submission.token;
                held.heldSequence = submission.sequence;
                if (image == nullptr) {
                    held.allAcquiresValid = false;
                }
                pipeline.onPresented(submission.token, VK_NULL_HANDLE);
            },
            [&] { return fakeClock.load(); }, &error)) {
        std::fprintf(stderr, "  start: %s\n", error.c_str());
        return false;
    }

    bool ok = true;
    // The frozen clock lets exactly the first frame through; the producer
    // publishes its three siblings, then attempts the held slot's reuse
    // and parks there.
    const bool parked = waitFor(
        [&] {
            const auto stats = pipeline.stats();
            return stats.ringPublished >= 4 && stats.gpuWaitsForFreeSlot >= 1;
        },
        5000);
    ok = check(parked, "the producer publishes what it can and attempts the held slot")
           && ok;

    unsigned firstToken = 0;
    uint64_t firstSequence = 0;
    {
        std::lock_guard<std::mutex> lock(held.mutex);
        firstToken = held.firstToken;
        firstSequence = held.firstSequence;
    }
    const auto stalledA = pipeline.stats();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    const auto stalledB = pipeline.stats();
    ok = check(stalledB.submitted == 1 && stalledA.submitted == 1,
               "the frozen clock holds at one submission") && ok;
    ok = check(stalledB.ringPublished == 4 && stalledA.ringPublished == 4,
               "the held frame's slot turns every reuse attempt into a park: "
               "nothing further publishes") && ok;
    ok = check(stalledB.gpuWaitsForFreeSlot > stalledA.gpuWaitsForFreeSlot,
               "the decoder's reuse attempts keep waiting on the lease") && ok;

    // Dirty and preview-style redraws of the held frame: the same
    // generation acquires the same image and samples the same pixels.
    SkImage* imageA = pipeline.acquireImage(firstToken, firstSequence);
    SkImage* imageB = pipeline.acquireImage(firstToken, firstSequence);
    ok = check(imageA != nullptr && imageA == imageB,
               "redraws of the held generation acquire one stable image") && ok;
    const std::vector<uint8_t> pixelsA = readBackPixels(gr, imageA);
    const std::vector<uint8_t> pixelsB = readBackPixels(gr, imageB);
    ok = check(!pixelsA.empty() && pixelsA == pixelsB,
               "the held frame samples identically across redraws — no rewrite slipped in")
           && ok;

    // A stale generation never samples the slot (R6's unchecked reacquire),
    // and a release naming a generation that is not the lease frees
    // nothing: the producer stays parked exactly where it was.
    ok = check(pipeline.acquireImage(firstToken, firstSequence + 7) == nullptr,
               "an acquire naming a different generation gets no image") && ok;
    const auto beforeStale = pipeline.stats();
    ok = check(beforeStale.staleAcquires >= 1, "the stale acquire was counted") && ok;
    pipeline.onReplaced(firstToken, firstSequence + 7);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const auto afterStale = pipeline.stats();
    ok = check(afterStale.staleReleases >= 1, "the unproved release was counted") && ok;
    ok = check(afterStale.ringPublished == 4 &&
                   afterStale.gpuWaitsForFreeSlot > beforeStale.gpuWaitsForFreeSlot,
               "a release without the lease proof freed nothing: still parked") && ok;

    // The valid handoff: releasing the held generation through the
    // session's own protocol unblocks the producer and every frame of the
    // clip flows, each with a valid image, to the end.
    fakeClock.store(1.0e6);
    const bool drained = waitFor([&] { return pipeline.stats().submitted >= 24; }, 10000);
    const auto done = pipeline.stats();
    ok = check(drained && done.submitted == 24 && done.eof,
               "the proved release lets the whole clip flow after the stall") && ok;
    {
        std::lock_guard<std::mutex> lock(held.mutex);
        ok = check(held.allAcquiresValid,
                   "every submission through the stall carried its own valid image") && ok;
    }
    ok = check(done.decodeErrors == 0 && done.fatalError.empty(),
               "the held-frame scenario is clean") && ok;

    pipeline.requestStop();
    pipeline.join();

    // After the run: the final frame still answers its own generation,
    // and the FIRST frame's generation — its slot long since rewritten —
    // is now refused. That is the R6 bug's mirror image: the old session
    // would have sampled a later frame under the old token.
    unsigned lastToken = 0;
    uint64_t lastSequence = 0;
    {
        std::lock_guard<std::mutex> lock(held.mutex);
        lastToken = held.heldToken;
        lastSequence = held.heldSequence;
    }
    ok = check(pipeline.acquireImage(lastToken, lastSequence) != nullptr,
               "the final frame stays sampled while it is held") && ok;
    ok = check(pipeline.acquireImage(firstToken, firstSequence) == nullptr,
               "a generation whose slot was rewritten and re-leased can never "
               "sample again") && ok;
    pipeline.close();
    return ok;
}

}  // namespace

int main() {
    namespace fs = std::filesystem;

    if (std::system("ffmpeg -version >/dev/null 2>&1") != 0) {
        std::printf("SKIP: the ffmpeg CLI is not available\n");
        return 77;
    }
    const fs::path dir = "playback-fixtures";
    std::error_code ec;
    fs::create_directories(dir, ec);

    // 2 s x 30 fps (60 frames) and the 4 s x 1080p25 video-only clip
    // (100 frames) follow the media_tests recipes; the GPU fixture is the
    // direct-decode tests' 130x98 twelve-fps generator, 24 frames.
    const std::string small = (dir / "stall-2s.mp4").string();
    const std::string hd = (dir / "stall-hd.mp4").string();
    const std::string gpu = (dir / "gpu-24f.mp4").string();
    if (!generate("ffmpeg -hide_banner -loglevel error -nostdin -y "
                  "-f lavfi -i testsrc2=size=320x240:rate=30:duration=2 "
                  "-c:v libx264 -preset ultrafast -pix_fmt yuv420p -an " + small) ||
        !generate("ffmpeg -hide_banner -loglevel error -nostdin -y "
                  "-f lavfi -i testsrc2=size=1920x1080:rate=25:duration=4 "
                  "-c:v libx264 -preset ultrafast -pix_fmt yuv420p -an " + hd) ||
        !generate("ffmpeg -hide_banner -loglevel error -nostdin -y -f lavfi -i "
                  "\"nullsrc=s=130x98:r=12,geq=lum='32+mod(N*7,180)':cb='64+mod(N*5,128)':"
                  "cr='80+mod(N*3,112)'\" -frames:v 24 "
                  "-c:v libx264 -preset fast -g 24 " + gpu)) {
        std::printf("SKIP: the ffmpeg CLI could not generate the fixtures\n");
        return 77;
    }

    bool ok = true;
    ok = cpuHealthyDrain(small) && ok;
    ok = cpuFrozenClockStall(hd) && ok;

    // GPU scenarios share one headless device, like gpu_decode_tests.
    openshow::worker::VulkanContext vk;
    if (!vk.createHeadless()) {
        std::printf("SKIP: no Vulkan device for the GPU scenarios "
                    "(the CPU scenarios above still ran)\n");
        return ok ? 77 : 1;
    }
    auto gr = GrDirectContexts::MakeVulkan(vk.backendContext());
    if (!gr) {
        std::printf("SKIP: no GrDirectContext for the GPU scenarios\n");
        return ok ? 77 : 1;
    }
    ok = gpuHealthyDrain(gpu, vk, gr.get()) && ok;
    ok = gpuStuckPresenter(gpu, vk, gr.get()) && ok;
    ok = gpuHeldFrameLease(gpu, vk, gr.get()) && ok;
    gr.reset();
    vk.destroy();
    ok = check(vk.validationErrors() == 0, "no Vulkan validation or synchronization errors") && ok;

    return ok ? 0 : 1;
}
