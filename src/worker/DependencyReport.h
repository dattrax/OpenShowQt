#pragma once
/*
 * The --check-deps report (task 6.18b2), shared by the production worker and
 * the dev harness so the two binaries can never drift apart in what they
 * report about the build.
 */

#include <cstdio>

#include <GLFW/glfw3.h>
#include <pipewire/pipewire.h>
#include <vulkan/vulkan.h>

// FFmpeg headers are C headers without extern "C" guards; media/FFmpeg.h is
// the module's single wrapped include point (task 1.5a).
#include "media/FFmpeg.h"

#include "include/core/SkGraphics.h"

#include "openshow/protocol/protocol.h"

namespace openshow::worker {

inline void printVersion(const char* label, unsigned int packed) {
    std::printf("%-14s %u.%u.%u\n", label,
                AV_VERSION_MAJOR(packed),
                AV_VERSION_MINOR(packed),
                AV_VERSION_MICRO(packed));
}

// Task 5.1a: --check-deps prints the libraries this binary linked and exits
// without touching a display, a GPU or a PipeWire server.
inline int reportDependencies() {
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

}  // namespace openshow::worker
