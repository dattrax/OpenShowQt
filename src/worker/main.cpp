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
#include <fstream>
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
#include "media/MediaProbe.h"
#include "media/AudioMixer.h"
#include "media/PipeWireSink.h"
#include "playback/AvPlayer.h"

#include "include/core/SkGraphics.h"
#include "include/core/SkImageInfo.h"

#include "openshow/domain/version.h"
#include "openshow/protocol/protocol.h"
#include "openshow/scene/scene.h"

#include "output/OutputWindow.h"
#include "playback/PlaybackPipeline.h"
#include "render/Layer.h"
#include "render/SceneText.h"
#include "render/TestScene.h"
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
                 "                                  [--second-audio FILE] [--preview]\n"
                 "                                  [--allow-video-copy] [--no-hw-decode]\n"
                 "       %s --snapshot PATH [--scene-file FILE] [--width N] [--height N]\n"
                 "       %s --list-monitors\n"
                 "       %s --check-layers\n"
                 "       %s --check-transition\n"
                 "       %s --pw-check [--seconds S]\n"
                 "  --version            print the application version and exit\n"
                 "  --check-deps         report the libraries this build linked and exit\n"
                 "  --socket PATH        connect to the frontend over a Unix socket\n"
                 "  --render-test        open the output window and draw the test scene\n"
                 "  --av-bench FILE      decode FILE to the GPU and pace presents\n"
                 "                       against the audio clock (10 s by default)\n"
                 "  --no-audio           with --av-bench: pace on the wall clock\n"
                 "  --second-audio FILE  with --av-bench: mix FILE's audio as a second\n"
                 "                       input into the one PipeWire output (5.3)\n"
                 "  --preview            with --av-bench: run the 1.4 preview stream\n"
                 "                       at 640x360@15 with a built-in consumer\n"
                 "  --vulkan-validation  with the Vulkan modes: run with the Khronos\n"
                 "                       validation layer and log its warnings and\n"
                 "                       errors (also OPENSHOW_VULKAN_VALIDATION=1,\n"
                 "                       which additionally covers the GPU tests)\n"
                 "  --allow-video-copy   permit copying fallback if direct decode is unsupported\n"
                 "  --no-hw-decode       disable VA-API in the optional copying fallback\n"
                 "  --monitor N          destination override: fullscreen on monitor N\n"
                 "                       (socket mode defaults to the second display)\n"
                 "  --seconds S          with --render-test, exit after S seconds\n"
                 "  --idle-check         with --render-test: verify a static scene\n"
                 "                       presents nothing and a command wakes the loop\n"
                 "  --snapshot PATH      render the test scene to a PNG (no display needed)\n"
                "  --scene-file FILE    with --snapshot: render the wire scene in FILE\n"
                "                       (the scene.prepare JSON object) instead of the demo\n"
                 "  --list-monitors      print the GLFW monitor list and exit\n"
                 "  --check-layers       verify layer composition on the CPU and exit\n"
                 "  --check-transition   verify scene cut/crossfade timing on the CPU\n"
                 "                       and exit (no display needed)\n"
                 "  --pw-check           feed a quiet tone through PipeWire and report the\n"
                 "                       sink's timing numbers (audible, ~5 s by default)\n"
                 "  --disable-layer NAME composition layer off: background, images,\n"
                 "                       words, aux or clock (repeatable)\n"
                 "  --enable-layer NAME  the converse, e.g. --enable-layer aux\n",
                 program, program, program, program, program, program, program, program);
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

    // The stream connects inactive (task 5.4): let the feeder put real
    // audio in the ring first, then wake the server — the pre-roll
    // discipline AvPlayer applies, so the callbacks never read a silent
    // head. resume() also anchors the clock here.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    sink.resume();

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

// Task 1.5e A/V bench, re-driven on task 5.1's PlaybackPipeline: decode,
// the GPU-backed bounded ring, the paced submission thread and the
// presenter role all live in the pipeline now — the bench keeps what is
// genuinely bench-only (audio sink and clock choice, the optional preview
// consumer, and reporting the numbers M5 is judged on). Decode FILE into
// the pipeline's GPU-backed ring and let OutputSession's scheduler pace
// presents against the audio sink's own clock (wall clock with --no-audio
// or when FILE has no decodable audio). FILE's audio and an optional
// standalone --second-audio file run as independent mixer inputs into the
// one PipeWire output (task 5.3). Reports the numbers M5 is judged
// on: missed deadlines under 1%, drift under 50 ms.
int runAvBench(const std::string& path, const std::string& secondAudio, int monitorIndex,
               double seconds, bool useAudio, bool preview, bool vulkanValidation,
               bool hardwareDecode, bool allowVideoCopy) {
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

    // Session first, pipeline second: the reverse destruction order keeps
    // the GrDirectContext alive for the pipeline's uploader teardown.
    openshow::worker::OutputSession session;
    openshow::worker::PlaybackPipeline pipeline;
    if (!session.open(options, &error)) {
        std::fprintf(stderr, "openshow-worker: av-bench session: %s\n", error.c_str());
        return 1;
    }

    const auto handles = session.gpuHandles();
    openshow::worker::PlaybackPipeline::Gpu gpu;
    gpu.device = handles.device;
    gpu.physicalDevice = handles.physicalDevice;
    gpu.graphicsQueueIndex = handles.graphicsQueueIndex;
    gpu.uploadQueueIndex = handles.uploadQueueIndex;
    gpu.uploadQueue = handles.uploadQueue;
    gpu.context = handles.context;
    openshow::worker::PlaybackPipeline::Options playback;
    playback.path = path;
    playback.loop = true;  // the bench runs until told to stop
    playback.allowVideoCopy = allowVideoCopy;
    playback.hardwareDecode = hardwareDecode;
    if (!pipeline.openGpu(gpu, playback, &error)) {
        std::fprintf(stderr, "openshow-worker: av-bench video: %s\n", error.c_str());
        return 1;
    }

    // Task 5.4: the audio path is the production AvPlayer (sink + mixer +
    // feeders, inactive-start with pre-roll and flush-at-resume); the
    // bench keeps only the clock hand-off and reporting.
    openshow::worker::AvPlayer player;
    if (useAudio) {
        const bool wantVideo =
            !info.audioStreams.empty() && info.audioStreams.front().decoderAvailable;
        openshow::worker::AvPlayer::Config audioConfig;
        audioConfig.videoAudioPath = wantVideo ? path : "";
        audioConfig.secondAudioPath = secondAudio;
        audioConfig.loop = true;  // the bench runs until told to stop
        if (!player.start(audioConfig, &error)) {
            std::fprintf(stderr, "openshow-worker: av-bench: audio player: %s, "
                                 "pacing on the monotonic clock\n",
                         error.c_str());
        }
    }
    player.attachPipeline(&pipeline);

    // The scheduler and the pipeline's drift stats read the same function:
    // the player's audio clock while live, its monotonic fallback
    // otherwise — one anchor for both so drift compares against the
    // scheduler's actual notion of now.
    const std::function<double()> clock = player.clockFn();
    session.setVideoClock(clock);
    session.setVideoPresenter(&pipeline);

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

    // Decode and paced submission: the pipeline's sink feeds the session's
    // video seam exactly as the bench's private upload thread did, and its
    // presenter role (registered above) hands slots back once the GPU has
    // sampled them.
    if (!pipeline.start(
            [&session](const openshow::worker::VideoSubmission& submission) {
                session.submitVideo(submission);
            },
            clock, &error)) {
        std::fprintf(stderr, "openshow-worker: av-bench decode: %s\n", error.c_str());
        return 1;
    }

    const unsigned long long presented = session.run(seconds, &gStopRequested);

    // Snapshot the player before stopping it (the stats keep counting
    // while the stream lives, and the meaningful window ends with run();
    // stop() also clears the feeder table, so the feeder/mixer snapshots
    // must precede it).
    const bool audio = player.audioLive();
    uint64_t sinkCallbacks = 0;
    uint64_t sinkUnderruns = 0;
    double sinkDriftMs = 0.0;
    if (audio) {
        const auto stats = player.sinkStats();
        sinkCallbacks = stats.callbacks;
        sinkUnderruns = stats.underrunEvents;
        sinkDriftMs = (stats.wallSeconds - stats.clockSeconds) * 1000.0;
    }
    const auto mixerStats = player.mixerStats();
    const auto feederStats = player.feederStats(false);
    const auto extraStats = player.feederStats(true);

    // Teardown order (the pipeline owns the producer ladder now: stop
    // flags, then the ring and decode thread, then the GPU waits): audio
    // next — the player unwinds feeders, mixer, sink — GPU objects last
    // while the context is still open.
    pipeline.requestStop();
    player.stop();
    previewStop.store(true);
    if (previewThread.joinable()) {
        previewThread.join();
    }
    if (previewRunning) {
        session.stopPreview();
    }
    const auto previewStatus = session.status().preview;
    pipeline.join();
    session.setVideoPresenter(nullptr);
    session.setVideoClock(nullptr);

    const auto report = pipeline.stats();
    std::printf(
        "av-bench %s: presents=%llu missed(>%.0fms)=%llu drops=%llu "
        "drift mean=%+.1fms max=%.1fms\n",
        path.c_str(), static_cast<unsigned long long>(report.presents), 25.0,
        static_cast<unsigned long long>(report.missed),
        static_cast<unsigned long long>(report.dropped), report.driftMeanMs, report.driftMaxMs);
    std::printf("  decoded=%llu loops=%llu decode-errors=%llu copy=%.3fms per commit\n",
                static_cast<unsigned long long>(report.decoded),
                static_cast<unsigned long long>(report.loops),
                static_cast<unsigned long long>(report.decodeErrors),
                report.gpuMsPerUpload);
    std::printf("  gpu slots=%llu wraps=%llu slot-waits=%llu, sink callbacks=%llu underruns=%llu drift=%+.1fms\n",
                static_cast<unsigned long long>(report.gpuSlotsCreated),
                static_cast<unsigned long long>(report.gpuWraps),
                static_cast<unsigned long long>(report.gpuWaitsForFreeSlot),
                static_cast<unsigned long long>(sinkCallbacks),
                static_cast<unsigned long long>(sinkUnderruns), sinkDriftMs);
    if (useAudio && mixerStats.inputs != 0) {
        std::printf("  mixer inputs=%u video written=%llu mixed=%llu starved=%llu | extra "
                    "written=%llu mixed=%llu starved=%llu | delivered=%llu retries=%llu "
                    "flushes video=%llu extra=%llu sink=%llu\n",
                    mixerStats.inputs,
                    static_cast<unsigned long long>(mixerStats.input[0].framesWritten),
                    static_cast<unsigned long long>(mixerStats.input[0].framesMixed),
                    static_cast<unsigned long long>(mixerStats.input[0].starvedBlocks),
                    mixerStats.inputs > 1
                        ? static_cast<unsigned long long>(mixerStats.input[1].framesWritten)
                        : 0ULL,
                    mixerStats.inputs > 1
                        ? static_cast<unsigned long long>(mixerStats.input[1].framesMixed)
                        : 0ULL,
                    mixerStats.inputs > 1
                        ? static_cast<unsigned long long>(mixerStats.input[1].starvedBlocks)
                        : 0ULL,
                    static_cast<unsigned long long>(mixerStats.deliveredFrames),
                    static_cast<unsigned long long>(mixerStats.writeRetries),
                    static_cast<unsigned long long>(mixerStats.input[0].flushes),
                    mixerStats.inputs > 1
                        ? static_cast<unsigned long long>(mixerStats.input[1].flushes)
                        : 0ULL,
                    static_cast<unsigned long long>(player.sinkStats().flushes));
        std::printf("  feeders video fed=%llu retries=%llu errors=%llu loops=%llu seeks=%llu "
                    "seek-discards=%llu | extra fed=%llu retries=%llu errors=%llu loops=%llu "
                    "seeks=%llu eof=%d\n",
                    static_cast<unsigned long long>(feederStats.framesFed),
                    static_cast<unsigned long long>(feederStats.writeRetries),
                    static_cast<unsigned long long>(feederStats.decodeErrors),
                    static_cast<unsigned long long>(feederStats.loops),
                    static_cast<unsigned long long>(feederStats.seeks),
                    static_cast<unsigned long long>(feederStats.seekDiscards),
                    static_cast<unsigned long long>(extraStats.framesFed),
                    static_cast<unsigned long long>(extraStats.writeRetries),
                    static_cast<unsigned long long>(extraStats.decodeErrors),
                    static_cast<unsigned long long>(extraStats.loops),
                    static_cast<unsigned long long>(extraStats.seeks),
                    extraStats.eof ? 1 : 0);
    }
    std::printf("  ring peak=%llu of %u slots (the bounded queue), session presented %llu frames total\n",
                static_cast<unsigned long long>(report.ringPeakFilled), report.ringDepth,
                presented);
    if (preview) {
        std::printf("  preview consumed=%llu published=%llu dropped=%llu\n",
                    static_cast<unsigned long long>(previewConsumed.load()),
                    static_cast<unsigned long long>(previewStatus.published),
                    static_cast<unsigned long long>(previewStatus.dropped));
    }

    std::printf("  direct=%llu downloads=%llu CPU-copies=%llu conversions=%llu "
                "vkCmdCopyImage=%llu decoder-images=%llu cached-decoder-images=%llu\n",
                static_cast<unsigned long long>(report.directFrames),
                static_cast<unsigned long long>(report.downloadedFrames),
                static_cast<unsigned long long>(report.copiedFrames),
                static_cast<unsigned long long>(report.convertedFrames),
                static_cast<unsigned long long>(report.gpuUploads),
                static_cast<unsigned long long>(report.gpuDecodeImagesCreated),
                static_cast<unsigned long long>(report.gpuCachedDecodeImages));
    if (!report.fatalError.empty()) return 1;
    if (report.presents == 0) {
        std::fprintf(stderr, "openshow-worker: av-bench presented nothing\n");
        return 1;
    }
    pipeline.close();  // frees codec-held reference images before Vulkan teardown
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    installStopHandlers();
    const char* socketPath = nullptr;
    const char* snapshotPath = nullptr;
    const char* sceneFilePath = nullptr;
    bool checkDependencies = false;
    bool renderTest = false;
    bool listMonitors = false;
    bool checkLayers = false;
    bool checkTransition = false;
    bool pwCheck = false;
    const char* avBenchPath = nullptr;
    std::string secondAudioPath;  // with --av-bench: a second mixer input (5.3)
    bool idleCheck = false;
    bool noAudio = false;
    bool benchPreview = false;
    bool vulkanValidation = false;
    bool hardwareDecode = true;
    bool allowVideoCopy = false;
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
        } else if (argument == "--check-transition") {
            checkTransition = true;
        } else if (argument == "--pw-check") {
            pwCheck = true;
        } else if (argument == "--av-bench" && i + 1 < argc) {
            avBenchPath = argv[++i];
        } else if (argument == "--second-audio" && i + 1 < argc) {
            secondAudioPath = argv[++i];
        } else if (argument == "--idle-check") {
            idleCheck = true;
        } else if (argument == "--no-audio") {
            noAudio = true;
        } else if (argument == "--preview") {
            benchPreview = true;
        } else if (argument == "--vulkan-validation") {
            vulkanValidation = true;
        } else if (argument == "--allow-video-copy") {
            allowVideoCopy = true;
        } else if (argument == "--no-hw-decode") {
            hardwareDecode = false;
        } else if (argument == "--socket" && i + 1 < argc) {
            socketPath = argv[++i];
        } else if (argument == "--monitor" && i + 1 < argc) {
            monitorIndex = std::atoi(argv[++i]);
            monitorGiven = true;
        } else if (argument == "--seconds" && i + 1 < argc) {
            seconds = std::atof(argv[++i]);
        } else if (argument == "--snapshot" && i + 1 < argc) {
            snapshotPath = argv[++i];
        } else if (argument == "--scene-file" && i + 1 < argc) {
            sceneFilePath = argv[++i];
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

    if (checkTransition) {
        std::string error;
        if (!openshow::worker::verifySceneTransition(&error)) {
            std::fprintf(stderr, "openshow-worker: transition check failed: %s\n",
                         error.c_str());
            return 1;
        }
        std::printf("scene transition checks passed\n");
        return 0;
    }

    if (pwCheck) {
        return runPipeWireCheck(seconds > 0.0 ? seconds : 5.0);
    }

    if (snapshotPath) {
        std::string error;
        bool ok = false;
        if (sceneFilePath != nullptr) {
            // Task 3.1: render a compiled wire scene through the production
            // text engine — the same layout the audience layers draw.
            std::ifstream in(sceneFilePath, std::ios::binary);
            if (!in) {
                error = std::string("cannot read ") + sceneFilePath;
            } else {
                const std::string text((std::istreambuf_iterator<char>(in)),
                                       std::istreambuf_iterator<char>());
                boost::system::error_code ec;
                const boost::json::value value = boost::json::parse(text, ec);
                if (ec) {
                    error = std::string("invalid JSON in ") + sceneFilePath + ": " + ec.message();
                } else {
                    openshow::scene::Scene scene;
                    if (!openshow::scene::fromJson(value, &scene, &error)) {
                        error = "scene file rejected: " + error;
                    } else {
                        ok = openshow::worker::renderSceneSnapshot(
                            scene, snapshotPath, snapshotWidth, snapshotHeight,
                            openshow::worker::SnapshotOptions{}, &error);
                    }
                }
            }
        } else {
            ok = openshow::worker::renderTestSnapshot(snapshotPath, snapshotWidth,
                                                      snapshotHeight, layerOverrides, &error);
        }
        if (!ok) {
            std::fprintf(stderr, "openshow-worker: snapshot failed: %s\n", error.c_str());
            return 1;
        }
        std::printf("wrote %s (%dx%d)\n", snapshotPath, snapshotWidth, snapshotHeight);
        return 0;
    }

    if (avBenchPath != nullptr) {
        // Task 1.5e development bench: same session an operator drives, plus
        // the decode -> GPU upload pipeline paced by the audio clock.
        return runAvBench(avBenchPath, secondAudioPath, monitorGiven ? monitorIndex : -1,
                          seconds > 0.0 ? seconds : 10.0, !noAudio, benchPreview,
                          vulkanValidation, hardwareDecode, allowVideoCopy);
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
        // Task 5.5: a presented scene's video background plays through the
        // session's transport. Only socket mode drives scenes from a real
        // frontend; benches and checks never present, so they keep the
        // plain seam.
        options.backgroundMedia = true;
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
