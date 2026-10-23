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
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <GLFW/glfw3.h>
#include <pipewire/version.h>
#include <vulkan/vulkan.h>

// FFmpeg headers are C headers without extern "C" guards; media/FFmpeg.h is
// the module's single wrapped include point (task 1.5a).
#include "media/FFmpeg.h"
#include "media/PipeWireSink.h"

#include "include/core/SkGraphics.h"

#include "openshow/domain/version.h"
#include "openshow/protocol/protocol.h"

#include "output/OutputWindow.h"
#include "render/Layer.h"
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
                 "       %s --render-test [--monitor N] [--seconds S]\n"
                 "       %s --snapshot PATH [--width N] [--height N]\n"
                 "       %s --list-monitors\n"
                 "       %s --check-layers\n"
                 "       %s --pw-check [--seconds S]\n"
                 "  --version            print the application version and exit\n"
                 "  --check-deps         report the libraries this build linked and exit\n"
                 "  --socket PATH        connect to the frontend over a Unix socket\n"
                 "  --render-test        open the output window and draw the test scene\n"
                 "  --monitor N          destination override: fullscreen on monitor N\n"
                 "                       (socket mode defaults to the second display)\n"
                 "  --seconds S          with --render-test, exit after S seconds\n"
                 "  --snapshot PATH      render the test scene to a PNG (no display needed)\n"
                 "  --list-monitors      print the GLFW monitor list and exit\n"
                 "  --check-layers       verify layer composition on the CPU and exit\n"
                 "  --pw-check           feed a quiet tone through PipeWire and report the\n"
                 "                       sink's timing numbers (audible, ~5 s by default)\n"
                 "  --disable-layer NAME composition layer off: background, images,\n"
                 "                       words, aux or clock (repeatable)\n"
                 "  --enable-layer NAME  the converse, e.g. --enable-layer aux\n",
                 program, program, program, program, program, program);
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
int runRenderTest(int monitorIndex, double seconds,
                  const std::vector<std::pair<openshow::worker::LayerId, bool>>& layerOverrides) {
    openshow::worker::OutputSession::Options options;
    options.title = "OpenShowQt render test (task 1.1)";
    options.monitor = monitorIndex;
    options.diagnostics = true;

    openshow::worker::OutputSession session;
    std::string error;
    if (!session.open(options, &error)) {
        std::fprintf(stderr, "openshow-worker: render test failed: %s\n", error.c_str());
        return 1;
    }
    for (const auto& [id, enabled] : layerOverrides) {
        session.setLayerEnabled(openshow::worker::layerName(id), enabled, nullptr);
    }

    const unsigned long long frameCount = session.run(seconds, &gStopRequested);
    std::fprintf(stderr, "openshow-worker: render test presented %llu frames\n", frameCount);
    return 0;
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

    if (renderTest) {
        // The development driver keeps the explicit destination; without
        // --monitor it shows the borderless window on the operator panel.
        return runRenderTest(monitorGiven ? monitorIndex : -1, seconds, layerOverrides);
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
