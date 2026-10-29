/*
 * openshow-worker: the Qt-free presentation output process.
 *
 * This is the binary a frontend spawns, and it has exactly one job: serve
 * the JSON-RPC protocol on the Unix socket it is handed (task 1.2), driving
 * one OutputSession for the life of that connection and exiting when the
 * frontend goes away.
 *
 * Task 6.18b2 moved the development drivers that grew up around it —
 * --render-test, --snapshot, the --check-* composition checks, --pw-check,
 * --av-bench, --list-monitors — into the separate openshow-worker-dev
 * executable. They are harnesses that exercise the same OutputSession
 * without a frontend; keeping them here only widened what a deployment
 * ships and has to trust. OutputSession.cpp and the output/render modules
 * compile into both binaries; the demo scene and the checks are no longer
 * reachable from this one.
 *
 * Exit codes: 0 success, 1 runtime failure, 2 bad usage.
 */

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "DependencyReport.h"

#include "openshow/domain/version.h"
#include "rpc/RpcServer.h"
#include "session/OutputSession.h"

namespace {

// SIGTERM/SIGINT ask the process to go away; the frame loop notices the
// flag between frames and takes the normal clean-exit path (server stopped,
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
                 "Usage: %s --socket PATH [--monitor N] [--seconds S] [--vulkan-validation]\n"
                 "       %s --version | --check-deps\n"
                 "\n"
                 "The presentation output process a frontend spawns. The development\n"
                 "drivers (--render-test, --snapshot, --check-layers, --check-transition,\n"
                 "--check-production, --pw-check, --av-bench, --list-monitors) live in\n"
                 "the separate openshow-worker-dev executable (task 6.18b2).\n"
                 "\n"
                 "  --version            print the application version and exit\n"
                 "  --check-deps         report the libraries this build linked and exit\n"
                 "  --socket PATH        connect to the frontend over a Unix socket\n"
                 "  --monitor N          destination override: fullscreen on monitor N\n"
                 "                       (default: the second display)\n"
                 "  --seconds S          exit after S seconds\n"
                 "  --vulkan-validation  run with the Khronos validation layer and log its\n"
                 "                       warnings and errors (also\n"
                 "                       OPENSHOW_VULKAN_VALIDATION=1)\n",
                 program, program);
}

}  // namespace

int main(int argc, char** argv) {
    installStopHandlers();
    const char* socketPath = nullptr;
    bool checkDependencies = false;
    int monitorIndex = openshow::worker::OutputSession::kAutoSecondDisplay;
    bool vulkanValidation = false;
    double seconds = 0.0;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--version") {
            std::printf("%s\n", openshow::domain::applicationVersion());
            return 0;
        }
        if (argument == "--check-deps") {
            checkDependencies = true;
        } else if (argument == "--socket" && i + 1 < argc) {
            socketPath = argv[++i];
        } else if (argument == "--monitor" && i + 1 < argc) {
            monitorIndex = std::atoi(argv[++i]);
        } else if (argument == "--seconds" && i + 1 < argc) {
            seconds = std::atof(argv[++i]);
        } else if (argument == "--vulkan-validation") {
            vulkanValidation = true;
        } else {
            printUsage(argv[0]);
            return 2;
        }
    }

    if (checkDependencies) {
        openshow::worker::reportDependencies();
    }

    if (!socketPath) {
        if (!checkDependencies) {
            printUsage(argv[0]);
        }
        return checkDependencies ? 0 : 2;
    }

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
    // Task 6.1b (review R1): no demo builder — the audience gets the
    // undecorated production compositor: black before the first
    // presentation, clock/aux and the diagnostic footer off, so
    // startup, clear-all and fresh workers are uniformly black and
    // preview mirrors the output.
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
    // Task 6.9a, ordered deliberately: stopping the server first means no
    // prepare can be submitted any more, and closing the session while the
    // server object is still alive lets the prepare executor's final
    // completions (stop() retires a decode mid-file) land in a server that
    // can still own the frame. The destructor's close() is then a no-op.
    session.close();
    std::fprintf(stderr, "openshow-worker: shut down after %llu frames\n", frameCount);
    return 0;
}
