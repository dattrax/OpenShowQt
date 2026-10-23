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

// FFmpeg headers are C headers without extern "C" guards, so C++ callers have
// to wrap them or the symbols will not resolve at link time.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/version.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

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
                 "  --disable-layer NAME composition layer off: background, images,\n"
                 "                       words, aux or clock (repeatable)\n"
                 "  --enable-layer NAME  the converse, e.g. --enable-layer aux\n",
                 program, program, program, program, program);
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

}  // namespace

int main(int argc, char** argv) {
    installStopHandlers();
    const char* socketPath = nullptr;
    const char* snapshotPath = nullptr;
    bool checkDependencies = false;
    bool renderTest = false;
    bool listMonitors = false;
    bool checkLayers = false;
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
