/*
 * openshow-worker: the Qt-free presentation output process.
 *
 * Task 1.1 adds --render-test: a GLFW window, a vk-bootstrap Vulkan device
 * and Skia Ganesh presenting the task 1.1 test scene. --snapshot draws the
 * same scene on the CPU with no display at all. Task 1.2 adds --socket: a
 * JSON-RPC server thread steering the same OutputSession a frontend
 * spawns us for; the worker exits when its frontend goes away.
 *
 * Exit codes: 0 success, 1 runtime failure, 2 bad usage, 3 feature not built.
 */

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <sys/resource.h>

#include <GLFW/glfw3.h>
#include <pipewire/version.h>
#include <vulkan/vulkan.h>

// FFmpeg headers are C headers without extern "C" guards; media/FFmpeg.h is
// the module's single wrapped include point (task 1.5a).
#include "media/FFmpeg.h"
#include "media/AudioClock.h"
#include "media/AudioFeeder.h"
#include "media/PipeWireSink.h"
#include "media/VideoFrameSource.h"

#include "include/core/SkGraphics.h"
#include "include/core/SkImageInfo.h"

#include "openshow/domain/version.h"
#include "openshow/protocol/protocol.h"

#include "output/OutputWindow.h"
#include "render/Layer.h"
#include "render/TestScene.h"
#include "render/YuvGpu.h"
#include "rpc/RpcServer.h"
#include "session/OutputSession.h"

namespace {

// SIGTERM/SIGINT ask the process to go away; the frame loops notice the
// flag between frames and take the normal clean-exit path (server stopped,
// socket unlinked, exit 0) instead of dying mid-present.
std::atomic<bool> gStopRequested{false};

void handleStopSignal(int) { gStopRequested.store(true); }

void installStopHandlers() {
    struct sigaction action = {};
    action.sa_handler = handleStopSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    sigaction(SIGTERM, &action, nullptr);
    sigaction(SIGINT, &action, nullptr);
}

void printUsage(const char* program) {
    std::fprintf(stderr,
                 "Usage: %s [--version] [--check-deps] [--socket PATH]\n"
                 "       %s --render-test [--monitor N] [--seconds S] [--idle-check]\n"
                 "       %s --av-bench FILE [--monitor N] [--seconds S] [--no-audio]\n"
                 "                                  [--preview]\n"
                 "       %s --snapshot PATH [--width N] [--height N]\n"
                 "       %s --list-monitors\n"
                 "       %s --check-layers\n"
                 "       %s --pw-check [--seconds S]\n"
                 "  --version            print the application version and exit\n"
                 "  --check-deps         report the libraries this build linked and exit\n"
                 "  --socket PATH        connect to the frontend over a Unix socket\n"
                 "  --render-test        open the output window and draw the test scene\n"
                 "  --av-bench FILE      decode FILE to the GPU and pace presents\n"
                 "                       against the audio clock (10 s by default)\n"
                 "  --no-audio           with --av-bench: pace on the wall clock\n"
                 "  --preview            with --av-bench: run the 1.4 preview stream\n"
                 "                       at 640x360@15 with a built-in consumer\n"
                 "  --vulkan-validation  with the Vulkan modes: run with the Khronos\n"
                 "                       validation layer and log its warnings and\n"
                 "                       errors (also OPENSHOW_VULKAN_VALIDATION=1,\n"
                 "                       which additionally covers the GPU tests)\n"
                 "  --monitor N          destination override: fullscreen on monitor N\n"
                 "                       (socket mode defaults to the second display)\n"
                 "  --seconds S          with --render-test, exit after S seconds\n"
                 "  --idle-check         with --render-test: verify a static scene\n"
                 "                       presents nothing and a command wakes the loop\n"
                 "  --snapshot PATH      render the test scene to a PNG (no display needed)\n"
                 "  --list-monitors      print the GLFW monitor list and exit\n"
                 "  --check-layers       verify layer composition on the CPU and exit\n"
                 "  --pw-check           feed a quiet tone through PipeWire and report the\n"
                 "                       sink's timing numbers (audible, ~5 s by default)\n"
                 "  --disable-layer NAME composition layer off: background, images,\n"
                 "                       words, aux or clock (repeatable)\n"
                 "  --enable-layer NAME  the converse, e.g. --enable-layer aux\n",
                 program, program, program, program, program, program, program);
}

void printVersion(const char* label, unsigned int packed) {
    std::printf("%-14s %u.%u.%u\n", label,
                AV_VERSION_MAJOR(packed),
                AV_VERSION_MINOR(packed),
                AV_VERSION_MICRO(packed));
}

int reportDependencies() {
    int glfwMajor = 0;
    int glfwMinor = 0;
    int glfwPatch = 0;
    glfwGetVersion(&glfwMajor, &glfwMinor, &glfwPatch);
    std::printf("%-14s %d.%d.%d (%s)\n", "GLFW", glfwMajor, glfwMinor, glfwPatch,
                glfwGetVersionString());

    std::printf("%-14s header %u\n", "Vulkan", VK_HEADER_VERSION);

    SkGraphics::Init();
    std::printf("%-14s linked, font cache limit %zu KiB\n", "Skia",
                SkGraphics::GetFontCacheLimit() / 1024);

    std::printf("%-14s %s\n", "PipeWire", pw_get_library_version());

    printVersion("libavcodec", avcodec_version());
    printVersion("libavformat", avformat_version());
    printVersion("libswscale", swscale_version());
    printVersion("libswresample", swresample_version());

    std::printf("%-14s %s\n", "protocol", openshow::protocol::offerPayload().c_str());
    return 0;
}

// Task 1.1: window, Vulkan device, Skia context and the test scene on screen.
// Since 1.2a this is a thin development driver: the window, device, swapchain
// and frame loop live in OutputSession, the runtime seam that the 1.2 RPC
// server will drive.
//
// --idle-check (task 1.5e acceptance) watches that loop from a second
// thread: after a warm-up the present counter must stay flat, process CPU
// time during the quiet window must be negligible, and a layer toggle must
// wake a present again within half a second — the event-driven loop either
// holds all three or the check exits non-zero.
int runRenderTest(int monitorIndex, double seconds,
                  const std::vector<std::pair<openshow::worker::LayerId, bool>>& layerOverrides,
                  bool idleCheck, bool vulkanValidation) {
    openshow::worker::OutputSession::Options options;
    options.title = "OpenShowQt render test (task 1.1)";
    options.monitor = monitorIndex;
    options.diagnostics = true;
    options.vulkanValidation = vulkanValidation;

    openshow::worker::OutputSession session;
    std::string error;
    if (!session.open(options, &error)) {
        std::fprintf(stderr, "openshow-worker: render test failed: %s\n", error.c_str());
        return 1;
    }
    for (const auto& [id, enabled] : layerOverrides) {
        session.setLayerEnabled(openshow::worker::layerName(id), enabled, nullptr);
    }

    auto processMs = [] {
        rusage usage = {};
        getrusage(RUSAGE_SELF, &usage);
        return (static_cast<double>(usage.ru_utime.tv_sec) + usage.ru_utime.tv_usec * 1e-6) +
               (static_cast<double>(usage.ru_stime.tv_sec) + usage.ru_stime.tv_usec * 1e-6);
    };

    std::thread monitor;
    std::atomic<bool> idleFailed{false};
    if (idleCheck) {
        monitor = std::thread([&session, &idleFailed, processMs] {
            std::this_thread::sleep_for(std::chrono::seconds(2));  // warm-up
            const unsigned long long frames0 = session.status().frames;
            const double cpu0 = processMs();
            const auto wall0 = std::chrono::steady_clock::now();
            std::this_thread::sleep_for(std::chrono::seconds(5));
            const unsigned long long frames1 = session.status().frames;
            const double cpuMs = (processMs() - cpu0) * 1e3;
            const double wallMs = std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - wall0)
                                      .count();
            bool woke = false;
            if (frames1 == frames0) {
                // Not presenting is only half of event-driven: a command
                // has to wake the loop for the next vsync.
                session.setLayerEnabled("clock", false, nullptr);
                const auto wakeDeadline =
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
                while (session.status().frames == frames1 &&
                       std::chrono::steady_clock::now() < wakeDeadline) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                woke = session.status().frames > frames1;
            }
            const bool ok = frames1 == frames0 && cpuMs <= wallMs * 0.05 && woke;
            std::fprintf(stderr,
                         "openshow-worker: idle-check: %llu presents in %.1fs idle, "
                         "cpu %.1fms (%.2f%%), toggle wake %s -> %s\n",
                         frames1 - frames0, wallMs / 1e3, cpuMs, cpuMs / wallMs * 100.0,
                         woke ? "ok" : "MISSING", ok ? "PASS" : "FAIL");
            idleFailed = !ok;
        });
    }

    // The check needs its 2 + 5 seconds plus the wake window; an explicit
    // --seconds still wins.
    const double runSeconds = idleCheck && seconds <= 0.0 ? 9.0 : seconds;
    const unsigned long long frameCount = session.run(runSeconds, &gStopRequested);
    if (monitor.joinable()) {
        monitor.join();
    }
    std::fprintf(stderr, "openshow-worker: render test presented %llu frames\n", frameCount);
    return idleFailed.load() ? 1 : 0;
}

// Task 1.5c: live PipeWire evidence. Feeds a quiet 440 Hz tone through the
// bounded AudioRing and reports the sink's numbers: negotiated rate, callback
// p99, underruns and clock-vs-wall drift. The underrun verdict is taken over
// the window after a 0.5 s warm-up (the first callbacks race the ring fill);
// totals are printed too, so nothing is hidden. Exit 0 only if a fed run
// behaved: callbacks arrived, the measured window underran nothing, and the
// delivered-frame clock stayed within 50 ms of the wall.
int runPipeWireCheck(double seconds) {
    openshow::media::PipeWireSink sink;
    std::string error;
    if (!sink.start(48000, &error)) {
        std::fprintf(stderr, "openshow-worker: pw-check could not start: %s\n", error.c_str());
        return 1;
    }
    // If negotiation ever failed, fall back rather than feed 0-frame chunks
    // (a silent no-op that would fake a healthy run).
    const uint32_t rate = sink.negotiatedRate() != 0 ? sink.negotiatedRate() : 48000;

    std::atomic<bool> feeding{true};
    std::thread feeder([&] {
        const size_t chunkFrames = rate / 100;  // 10 ms
        std::vector<float> chunk(chunkFrames * 2, 0.0f);
        double phase = 0.0;
        const double step = 2.0 * 3.14159265358979 * 440.0 / static_cast<double>(rate);
        while (feeding.load()) {
            for (size_t i = 0; i < chunkFrames; ++i) {
                const float value = static_cast<float>(0.02 * std::sin(phase));
                chunk[i * 2] = value;
                chunk[i * 2 + 1] = value;
                phase += step;
                if (phase > 2.0 * 3.14159265358979) {
                    phase -= 2.0 * 3.14159265358979;
                }
            }
            // Paced by consumption like a resample thread: back off while
            // the ring is full rather than spin.
            size_t written = 0;
            while (written < chunkFrames && feeding.load()) {
                written += sink.feed(chunk.data() + written * 2, chunkFrames - written);
                if (written < chunkFrames) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        }
    });

    const double warmup = seconds > 1.5 ? 0.5 : 0.0;
    std::this_thread::sleep_for(std::chrono::duration<double>(warmup));
    const auto steadyStart = sink.stats();
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::duration<double>(seconds - warmup);
    while (!gStopRequested && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    const auto end = sink.stats();
    feeding.store(false);
    feeder.join();
    sink.stop();

    const uint64_t windowUnderruns = end.underrunEvents - steadyStart.underrunEvents;
    const double windowSeconds = end.wallSeconds - steadyStart.wallSeconds;
    const double windowClock = end.clockSeconds - steadyStart.clockSeconds;
    const double driftMs = (windowSeconds - windowClock) * 1000.0;

    std::printf(
        "pw-check: rate=%u callbacks=%llu p99=%.2fms underruns=%llu window=%llu "
        "(%llu frames) clock=%.3fs/%.3fs drift=%+.1fms total\n",
        end.negotiatedRate, static_cast<unsigned long long>(end.callbacks), end.callbackP99Ms,
        static_cast<unsigned long long>(end.underrunEvents),
        static_cast<unsigned long long>(windowUnderruns),
        static_cast<unsigned long long>(end.underrunFrames), end.clockSeconds, end.wallSeconds,
        (end.wallSeconds - end.clockSeconds) * 1000.0);

    const bool ok = end.callbacks > 0 && windowUnderruns == 0 && windowSeconds > 0.5 &&
                    std::fabs(driftMs) < 50.0;
    if (!ok) {
        std::fprintf(stderr, "openshow-worker: pw-check failed its acceptance numbers\n");
    }
    return ok ? 0 : 1;
}

// Task 1.5e A/V bench: the stream's probe hint decides the conversion the
// compositing shader applies to uploaded YUV. Unknown color spaces follow
// the height convention (720 rows and up are usually Rec.709); this Skia
// revision has no Rec.601 full-range entry, where JPEG's full-range
// mapping is the matching one.
SkYUVColorSpace pickYuvColorSpace(const openshow::media::VideoStreamInfo& info) {
    const bool fullRange =
        info.colorRange == "jpeg" || info.colorRange == "pc" || info.colorRange == "full";
    bool bt709 = info.colorSpace == "bt709";
    const bool bt601 = info.colorSpace == "bt601" || info.colorSpace == "smpte240m";
    if (!bt709 && !bt601) {
        bt709 = info.height >= 720;
    }
    if (bt709) {
        return fullRange ? kRec709_Full_SkYUVColorSpace : kRec709_Limited_SkYUVColorSpace;
    }
    return fullRange ? kJPEG_Full_SkYUVColorSpace : kRec601_Limited_SkYUVColorSpace;
}

// Counts what the scheduler did with the frames the upload thread offered:
// drift of the presentation instant against the video clock, deadlines
// missed (M5's metric), and drops. onDropped can arrive on the upload
// thread (submitVideo replacing an undrawn pending slot) while onPresented
// is render-thread, so the counters carry a mutex.
class BenchPresenter final : public openshow::worker::OutputSession::VideoPresenter {
public:
    // A present is "missed" when the clock already ran 25 ms past the
    // frame's pts: twice a 40 fps frame interval, so wake-up jitter stays
    // out of the count while real stalls do not.
    static constexpr double kMissedWindowMs = 25.0;

    explicit BenchPresenter(openshow::worker::GpuYuvUploader* uploader) : fUploader(uploader) {}

    void setClock(std::function<double()> clock) { fClock = std::move(clock); }

    // The upload thread records a slot's pts before submitting it. The slot
    // cannot be rewritten before its release (onPresented/onDropped), and
    // the uploader's in-use protocol orders that release against the next
    // upload, so the entry lives exactly as long as any reader can need it.
    void recordPts(unsigned slot, double pts) { fPtsBySlot[slot] = pts; }

    SkImage* acquireImage(unsigned token) override {
        // Borrowed: the uploader holds the wrapper's own reference until
        // it is destroyed, so the raw pointer outlives every draw.
        return fUploader->imageForSlot(token, nullptr).get();
    }

    void onPresented(unsigned token, VkSemaphore doneVk) override {
        const double pts = token < kSlotTableSize ? fPtsBySlot[token] : 0.0;
        const double driftMs = ((fClock ? fClock() : 0.0) - pts) * 1000.0;
        {
            std::lock_guard<std::mutex> lock(fStatsMutex);
            ++fPresents;
            fDriftSumMs += driftMs;
            if (driftMs > fDriftMaxMs) {
                fDriftMaxMs = driftMs;
            }
            if (driftMs > kMissedWindowMs) {
                ++fMissed;
            }
        }
        fUploader->release(token, doneVk);
    }

    void onDropped(unsigned token) override {
        {
            std::lock_guard<std::mutex> lock(fStatsMutex);
            ++fDrops;
        }
        fUploader->release(token, VK_NULL_HANDLE);  // never sampled
    }

    struct Report {
        uint64_t presents = 0;
        uint64_t missed = 0;
        uint64_t drops = 0;
        double driftMeanMs = 0.0;
        double driftMaxMs = 0.0;
    };
    Report report() const {
        std::lock_guard<std::mutex> lock(fStatsMutex);
        Report out;
        out.presents = fPresents;
        out.missed = fMissed;
        out.drops = fDrops;
        out.driftMeanMs =
            fPresents != 0 ? fDriftSumMs / static_cast<double>(fPresents) : 0.0;
        out.driftMaxMs = fDriftMaxMs;
        return out;
    }

private:
    static constexpr unsigned kSlotTableSize = 8;

    openshow::worker::GpuYuvUploader* fUploader;
    std::function<double()> fClock;
    double fPtsBySlot[kSlotTableSize] = {};
    mutable std::mutex fStatsMutex;
    uint64_t fPresents = 0;
    uint64_t fMissed = 0;
    uint64_t fDrops = 0;
    double fDriftSumMs = 0.0;
    double fDriftMaxMs = 0.0;
};

// Decode FILE, upload its frames to the GPU ring and let OutputSession's
// scheduler pace presents against the audio sink's own clock (wall clock
// with --no-audio or when FILE has no decodable audio). Reports the numbers
// M5 is judged on: missed deadlines under 1%, drift under 50 ms.
int runAvBench(const std::string& path, int monitorIndex, double seconds, bool useAudio,
               bool preview, bool vulkanValidation) {
    openshow::media::MediaInfo info;
    std::string error;
    if (openshow::media::probeMedia(path, &info, &error) != openshow::media::ProbeStatus::kOk) {
        std::fprintf(stderr, "openshow-worker: av-bench cannot read %s: %s\n", path.c_str(),
                     error.c_str());
        return 1;
    }

    openshow::worker::OutputSession::Options options;
    options.title = "OpenShowQt A/V bench (task 1.5e)";
    options.monitor = monitorIndex;
    options.vulkanValidation = vulkanValidation;

    // Session first, uploader second: the reverse destruction order keeps
    // the GrDirectContext alive for uploader.destroy()'s SkImage drops.
    openshow::worker::OutputSession session;
    openshow::worker::GpuYuvUploader uploader;
    if (!session.open(options, &error)) {
        std::fprintf(stderr, "openshow-worker: av-bench session: %s\n", error.c_str());
        return 1;
    }

    openshow::media::VideoFrameSource source;
    if (!source.open(path, 6, &error)) {
        std::fprintf(stderr, "openshow-worker: av-bench video: %s\n", error.c_str());
        return 1;
    }
    const openshow::media::VideoStreamInfo& video = source.streamInfo();

    openshow::media::PipeWireSink sink;
    openshow::media::AudioFileFeeder feeder;
    bool audio = false;
    if (useAudio) {
        if (!info.audioStreams.empty() && info.audioStreams.front().decoderAvailable) {
            if (sink.start(48000, &error)) {
                const uint32_t rate =
                    sink.negotiatedRate() != 0 ? sink.negotiatedRate() : 48000;
                if (feeder.open(path, rate, &error)) {
                    audio = feeder.start(
                        [&sink](const float* frames, size_t count) {
                            return sink.feed(frames, count);
                        },
                        true, &error);
                }
            }
            if (!audio) {
                std::fprintf(stderr,
                             "openshow-worker: av-bench: audio unavailable (%s), "
                             "pacing on the wall clock\n",
                             error.c_str());
            }
        } else {
            std::fprintf(stderr, "openshow-worker: av-bench: no decodable audio stream, "
                                 "pacing on the wall clock\n");
        }
    }

    // The scheduler and the drift stats read the same function: on the
    // audio path the sink's stream time, otherwise one wall clock anchored
    // here so drift compares against the scheduler's actual notion of now.
    const auto clockStart = std::chrono::steady_clock::now();
    std::function<double()> clock;
    if (audio) {
        clock = [&sink] { return sink.clock().clockSeconds(); };
    } else {
        clock = [clockStart] {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - clockStart)
                .count();
        };
    }
    session.setVideoClock(clock);

    BenchPresenter presenter(&uploader);
    presenter.setClock(clock);
    session.setVideoPresenter(&presenter);

    // Optional 1.4 preview stream under load, with a built-in consumer that
    // pops readys and releases immediately like a front-row frontend: the
    // point is exercising the publish path (draw + readPixels) at preview
    // fps while the audience output clocks video.
    std::atomic<bool> previewStop{false};
    std::atomic<uint64_t> previewConsumed{0};
    std::thread previewThread;
    bool previewRunning = false;
    if (preview) {
        openshow::worker::OutputSession::PreviewInfo previewInfo;
        if (!session.startPreview(640, 360, 15, &previewInfo, &error)) {
            std::fprintf(stderr, "openshow-worker: av-bench preview: %s (continuing without)\n",
                         error.c_str());
        } else {
            previewRunning = true;
            previewThread = std::thread([&] {
                openshow::worker::OutputSession::PreviewReady ready;
                while (!previewStop.load()) {
                    if (session.takePreviewReady(&ready)) {
                        session.releasePreviewSlot(ready.slot, ready.seq);
                        previewConsumed.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                }
            });
        }
    }

    if (!source.startDecode(true, &error)) {
        std::fprintf(stderr, "openshow-worker: av-bench decode: %s\n", error.c_str());
        return 1;
    }

    // Decode ring -> GPU ring. upload() applies backpressure itself while
    // every slot is in use, which is exactly the pacing this thread wants;
    // it returns the slot index, and recordPts precedes submitVideo so the
    // render thread can never observe a submission without its pts.
    bool uploaderFailed = false;
    uint64_t uploadRefusals = 0;  // upload thread writes; read after join
    // Supply pacing: upload no further ahead of the clock than the session
    // can hold. The seam keeps one pending frame and drops the replaced
    // one, so the lookahead must stay under a frame interval — otherwise
    // every deadline is superseded before it arrives and nothing presents.
    const double supplyLookahead =
        video.frameRate > 0.0
            ? std::min(0.05, std::max(0.004, 0.4 / video.frameRate))
            : 0.015;
    const double clipSeconds = info.durationSeconds;
    // The upload loop ends on a stop signal or on the bench's own teardown
    // (run() returning at its deadline is neither: a stopped ring just
    // makes poll() return false, which must end the loop, not spin it).
    std::atomic<bool> benchStop{false};
    std::thread uploadThread([&] {
        openshow::media::VideoFrame frame;
        // The ring's frames are planar I420; the GPU slots are a Y (R8)
        // plus interleaved-UV (R8G8) layout, so fold U and V together
        // here — a 960x540 interleave costs microseconds. (M2 can skip it
        // by asking swscale for NV12 output directly.)
        std::vector<uint8_t> uvScratch;
        // pts continuity across decoder loops: the container pts restart at
        // 0, while the presentation clock runs on; keep a monotonic
        // effective pts so drift means what M5 needs it to mean.
        double lastPts = -1.0;
        double loopBase = 0.0;
        while (!benchStop.load() && !gStopRequested.load()) {
            if (!source.ring().poll(&frame, std::chrono::milliseconds(100))) {
                continue;  // timeout now; the teardown's stop() ends the loop
            }
            bool ok = true;
            if (uploader.width() == 0) {
                const auto handles = session.gpuHandles();
                ok = uploader.create(handles.device, handles.physicalDevice,
                                     handles.graphicsQueueIndex, handles.uploadQueueIndex,
                                     handles.uploadQueue, handles.context, frame.width,
                                     frame.height, pickYuvColorSpace(video), &error);
                if (!ok) {
                    std::fprintf(stderr, "openshow-worker: av-bench gpu: %s\n", error.c_str());
                    uploaderFailed = true;
                }
            }
            unsigned slot = 0;
            // Hold the frame near its presentation time before shipping it
            // to the GPU (small sleeps; stop cuts them short).
            if (lastPts >= 0.0 && frame.ptsSeconds + 0.5 < lastPts && clipSeconds > 0.0) {
                loopBase += clipSeconds;  // the decoder rewound; pts restarts
            }
            lastPts = frame.ptsSeconds;
            const double effectivePts = loopBase + frame.ptsSeconds;
            while (!benchStop.load() && !gStopRequested.load() &&
                   clock() < effectivePts - supplyLookahead) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            if (ok && !gStopRequested.load()) {
                const int chromaWidth = frame.width / 2;
                const int chromaRows = frame.height / 2;
                uvScratch.resize(static_cast<size_t>(chromaWidth) * 2u *
                                 static_cast<size_t>(chromaRows));
                for (int row = 0; row < chromaRows; ++row) {
                    const uint8_t* uRow =
                        frame.data[1] + static_cast<size_t>(row) * frame.linesize[1];
                    const uint8_t* vRow =
                        frame.data[2] + static_cast<size_t>(row) * frame.linesize[2];
                    uint8_t* uvRow = uvScratch.data() + static_cast<size_t>(row) * chromaWidth * 2u;
                    for (int col = 0; col < chromaWidth; ++col) {
                        uvRow[col * 2] = uRow[col];
                        uvRow[col * 2 + 1] = vRow[col];
                    }
                }
                ok = uploader.upload(frame.data[0], frame.linesize[0], uvScratch.data(),
                                     chromaWidth * 2, frame.sequence, frame.ptsSeconds, &slot);
                if (ok) {
                    presenter.recordPts(slot, effectivePts);
                    openshow::worker::OutputSession::VideoSubmission submission;
                    submission.token = slot;
                    submission.ptsSeconds = effectivePts;
                    submission.sequence = frame.sequence;
                    session.submitVideo(submission);
                } else {
                    ++uploadRefusals;  // counted: silence here hides real bugs
                }
            }
            source.ring().release(frame.slot);
            if (uploaderFailed) {
                gStopRequested.store(true);  // the run loop unwinds on this
                break;
            }
        }
    });

    const unsigned long long presented = session.run(seconds, &gStopRequested);

    // Snapshot the sink before stopping it (the stats keep counting while
    // the stream lives, and the meaningful window ends with run()).
    uint64_t sinkCallbacks = 0;
    uint64_t sinkUnderruns = 0;
    double sinkDriftMs = 0.0;
    if (audio) {
        const auto stats = sink.stats();
        sinkCallbacks = stats.callbacks;
        sinkUnderruns = stats.underrunEvents;
        sinkDriftMs = (stats.wallSeconds - stats.clockSeconds) * 1000.0;
    }

    // Teardown order: producers first (benchStop ends the upload loop — the
    // deadline path leaves no other signal; ring stop would only spin it),
    // then audio, GPU objects last while the context is still open.
    benchStop.store(true);
    source.ring().stop();
    source.requestStop();
    uploadThread.join();
    feeder.requestStop();
    feeder.join();
    if (audio) {
        sink.stop();
    }
    previewStop.store(true);
    if (previewThread.joinable()) {
        previewThread.join();
    }
    if (previewRunning) {
        session.stopPreview();
    }
    const auto previewStatus = session.status().preview;
    session.setVideoPresenter(nullptr);
    session.setVideoClock(nullptr);
    uploader.destroy();

    if (uploaderFailed) {
        return 1;
    }

    const auto report = presenter.report();
    const auto gpu = uploader.stats();
    const auto decoded = source.stats();
    std::printf(
        "av-bench %s: presents=%llu missed(>%.0fms)=%llu drops=%llu "
        "drift mean=%+.1fms max=%.1fms\n",
        path.c_str(), static_cast<unsigned long long>(report.presents),
        BenchPresenter::kMissedWindowMs, static_cast<unsigned long long>(report.missed),
        static_cast<unsigned long long>(report.drops), report.driftMeanMs, report.driftMaxMs);
    std::printf("  decoded=%llu loops=%llu decode-errors=%llu stage=%.3fms gpu=%.3fms per upload\n",
                static_cast<unsigned long long>(decoded.framesProduced),
                static_cast<unsigned long long>(decoded.loops),
                static_cast<unsigned long long>(decoded.decodeErrors),
                gpu.uploads != 0 ? gpu.totalStageMs / static_cast<double>(gpu.uploads) : 0.0,
                gpu.uploads != 0 ? gpu.totalGpuMs / static_cast<double>(gpu.uploads) : 0.0);
    std::printf("  gpu slots=%llu wraps=%llu refusals=%llu, sink callbacks=%llu underruns=%llu drift=%+.1fms\n",
                static_cast<unsigned long long>(gpu.slotsCreated),
                static_cast<unsigned long long>(gpu.wraps),
                static_cast<unsigned long long>(uploadRefusals),
                static_cast<unsigned long long>(sinkCallbacks),
                static_cast<unsigned long long>(sinkUnderruns), sinkDriftMs);
    std::printf("  session presented %llu frames total\n", presented);
    if (preview) {
        std::printf("  preview consumed=%llu published=%llu dropped=%llu\n",
                    static_cast<unsigned long long>(previewConsumed.load()),
                    static_cast<unsigned long long>(previewStatus.published),
                    static_cast<unsigned long long>(previewStatus.dropped));
    }

    if (report.presents == 0) {
        std::fprintf(stderr, "openshow-worker: av-bench presented nothing\n");
        return 1;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    installStopHandlers();
    const char* socketPath = nullptr;
    const char* snapshotPath = nullptr;
    bool checkDependencies = false;
    bool renderTest = false;
    bool listMonitors = false;
    bool checkLayers = false;
    bool pwCheck = false;
    const char* avBenchPath = nullptr;
    bool idleCheck = false;
    bool noAudio = false;
    bool benchPreview = false;
    bool vulkanValidation = false;
    int monitorIndex = openshow::worker::OutputSession::kAutoSecondDisplay;
    bool monitorGiven = false;
    double seconds = 0.0;
    int snapshotWidth = 1920;
    int snapshotHeight = 1080;
    std::vector<std::pair<openshow::worker::LayerId, bool>> layerOverrides;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--version") {
            std::printf("%s\n", openshow::domain::applicationVersion());
            return 0;
        }
        if (argument == "--check-deps") {
            checkDependencies = true;
        } else if (argument == "--render-test") {
            renderTest = true;
        } else if (argument == "--list-monitors") {
            listMonitors = true;
        } else if (argument == "--check-layers") {
            checkLayers = true;
        } else if (argument == "--pw-check") {
            pwCheck = true;
        } else if (argument == "--av-bench" && i + 1 < argc) {
            avBenchPath = argv[++i];
        } else if (argument == "--idle-check") {
            idleCheck = true;
        } else if (argument == "--no-audio") {
            noAudio = true;
        } else if (argument == "--preview") {
            benchPreview = true;
        } else if (argument == "--vulkan-validation") {
            vulkanValidation = true;
        } else if (argument == "--socket" && i + 1 < argc) {
            socketPath = argv[++i];
        } else if (argument == "--monitor" && i + 1 < argc) {
            monitorIndex = std::atoi(argv[++i]);
            monitorGiven = true;
        } else if (argument == "--seconds" && i + 1 < argc) {
            seconds = std::atof(argv[++i]);
        } else if (argument == "--snapshot" && i + 1 < argc) {
            snapshotPath = argv[++i];
        } else if (argument == "--width" && i + 1 < argc) {
            snapshotWidth = std::atoi(argv[++i]);
        } else if (argument == "--height" && i + 1 < argc) {
            snapshotHeight = std::atoi(argv[++i]);
        } else if ((argument == "--disable-layer" || argument == "--enable-layer") &&
                   i + 1 < argc) {
            openshow::worker::LayerId layerId;
            if (!openshow::worker::layerIdFromName(argv[i + 1], &layerId)) {
                std::fprintf(stderr, "openshow-worker: unknown layer '%s'\n", argv[i + 1]);
                return 2;
            }
            ++i;
            layerOverrides.emplace_back(layerId, argument == "--enable-layer");
        } else {
            printUsage(argv[0]);
            return 2;
        }
    }

    if (checkDependencies) {
        reportDependencies();
    }

    if (listMonitors) {
        return openshow::worker::OutputWindow::printMonitors() ? 0 : 1;
    }

    if (checkLayers) {
        std::string error;
        if (!openshow::worker::verifyLayerComposition(&error)) {
            std::fprintf(stderr, "openshow-worker: layer check failed: %s\n", error.c_str());
            return 1;
        }
        std::printf("layer composition checks passed\n");
        return 0;
    }

    if (pwCheck) {
        return runPipeWireCheck(seconds > 0.0 ? seconds : 5.0);
    }

    if (snapshotPath) {
        std::string error;
        if (!openshow::worker::renderTestSnapshot(snapshotPath, snapshotWidth, snapshotHeight,
                                                  layerOverrides, &error)) {
            std::fprintf(stderr, "openshow-worker: snapshot failed: %s\n", error.c_str());
            return 1;
        }
        std::printf("wrote %s (%dx%d)\n", snapshotPath, snapshotWidth, snapshotHeight);
        return 0;
    }

    if (avBenchPath != nullptr) {
        // Task 1.5e development bench: same session an operator drives, plus
        // the decode -> GPU upload pipeline paced by the audio clock.
        return runAvBench(avBenchPath, monitorGiven ? monitorIndex : -1,
                          seconds > 0.0 ? seconds : 10.0, !noAudio, benchPreview,
                          vulkanValidation);
    }

    if (renderTest) {
        // The development driver keeps the explicit destination; without
        // --monitor it shows the borderless window on the operator panel.
        return runRenderTest(monitorGiven ? monitorIndex : -1, seconds, layerOverrides,
                             idleCheck, vulkanValidation);
    }

    if (socketPath) {
        // Task 1.2: bind first so a bad path fails without touching the
        // display, then open the output session, then serve the frontend.
        // The worker lives only while connected: disconnect or "shutdown"
        // ends run() and we exit cleanly.
        openshow::worker::OutputSession session;
        openshow::worker::RpcServer::Options rpc;
        rpc.socketPath = socketPath;
        rpc.workerVersion = openshow::domain::applicationVersion();
        openshow::worker::RpcServer server(session, std::move(rpc));
        std::string error;
        if (!server.listen(&error)) {
            std::fprintf(stderr, "openshow-worker: could not listen on %s: %s\n",
                         socketPath, error.c_str());
            return 1;
        }
        openshow::worker::OutputSession::Options options;
        options.title = "OpenShowQt output";
        options.monitor = monitorIndex;
        options.vulkanValidation = vulkanValidation;
        if (!session.open(options, &error)) {
            std::fprintf(stderr, "openshow-worker: %s\n", error.c_str());
            return 1;
        }
        if (!server.start(&error)) {
            std::fprintf(stderr, "openshow-worker: could not start the RPC server: %s\n",
                         error.c_str());
            return 1;
        }
        std::fprintf(stderr, "openshow-worker: listening on %s, waiting for the frontend\n",
                     socketPath);
        const unsigned long long frameCount = session.run(seconds, &gStopRequested);
        server.stop();
        std::fprintf(stderr, "openshow-worker: shut down after %llu frames\n", frameCount);
        return 0;
    }

    if (!checkDependencies) {
        printUsage(argv[0]);
        return 2;
    }

    return 0;
}
