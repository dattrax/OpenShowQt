/*
 * openshow-worker: the Qt-free presentation output process.
 *
 * Task 0.4 only proves that the Qt-free target configures, links and runs
 * against Skia, GLFW, Vulkan, FFmpeg and PipeWire. The GLFW window and Skia
 * rendering arrive in task 1.1, the RPC handshake in 1.2.
 *
 * Exit codes: 0 success, 2 bad usage, 3 requested feature not built yet.
 */

#include <cstdio>
#include <cstring>
#include <string>

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

namespace {

void printUsage(const char* program) {
    std::fprintf(stderr,
                 "Usage: %s [--version] [--check-deps] [--socket PATH]\n"
                 "  --version     print the application version and exit\n"
                 "  --check-deps  report the libraries this build linked and exit\n"
                 "  --socket PATH connect to the frontend over a Unix socket\n",
                 program);
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

}  // namespace

int main(int argc, char** argv) {
    const char* socketPath = nullptr;
    bool checkDependencies = false;

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
        } else {
            printUsage(argv[0]);
            return 2;
        }
    }

    if (checkDependencies) {
        reportDependencies();
    }

    if (socketPath) {
        std::fprintf(stderr,
                     "openshow-worker: the socket transport is task 1.2 and is not "
                     "implemented yet (was asked for %s)\n",
                     socketPath);
        return 3;
    }

    if (!checkDependencies) {
        printUsage(argv[0]);
        return 2;
    }

    return 0;
}
