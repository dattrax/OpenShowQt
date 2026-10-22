#include "OutputWindow.h"

#include <cstdio>

#include <GLFW/glfw3.h>

namespace openshow::worker {

namespace {

// True once GLFW has been initialised in this process.
bool gGlfwInitialised = false;

}  // namespace

bool OutputWindow::initGlfw() {
    if (gGlfwInitialised) {
        return true;
    }
    // Target native Wayland; fall back to whatever GLFW picks (X11) so the
    // worker still starts on an X session.
    glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_WAYLAND);
    if (!glfwInit()) {
        glfwInitHint(GLFW_PLATFORM, GLFW_ANY_PLATFORM);
        if (!glfwInit()) {
            std::fprintf(stderr, "openshow-worker: glfwInit failed\n");
            return false;
        }
    }
    gGlfwInitialised = true;
    std::fprintf(stderr, "openshow-worker: GLFW platform %s\n", [] {
        switch (glfwGetPlatform()) {
            case GLFW_PLATFORM_WAYLAND: return "wayland";
            case GLFW_PLATFORM_X11: return "x11";
            case GLFW_PLATFORM_NULL: return "null";
            default: return "unknown";
        }
    }());
    return true;
}

OutputWindow::~OutputWindow() {
    close();
}

bool OutputWindow::open(const Options& options) {
    if (!initGlfw()) {
        return false;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    fWindow = glfwCreateWindow(options.width, options.height, options.title,
                               nullptr, nullptr);
    if (!fWindow) {
        std::fprintf(stderr, "openshow-worker: glfwCreateWindow failed\n");
        return false;
    }
    fOwnsGlfw = true;
    glfwSetWindowUserPointer(fWindow, this);
    glfwSetFramebufferSizeCallback(fWindow, &OutputWindow::onFramebufferSize);

    if (options.monitor >= 0) {
        int monitorCount = 0;
        GLFWmonitor** monitors = glfwGetMonitors(&monitorCount);
        if (!monitors || options.monitor >= monitorCount) {
            std::fprintf(stderr,
                         "openshow-worker: monitor %d requested, %d present\n",
                         options.monitor, monitorCount);
            close();
            return false;
        }
        GLFWmonitor* monitor = monitors[options.monitor];
        const GLFWvidmode* mode = glfwGetVideoMode(monitor);
        if (!mode) {
            std::fprintf(stderr, "openshow-worker: monitor %d reports no mode\n",
                         options.monitor);
            close();
            return false;
        }
        // Map the window first, then request fullscreen: GLFW's Wayland
        // backend forwards the fullscreen state when the surface maps, and
        // KWin then configures the surface for the monitor mode.
        glfwShowWindow(fWindow);
        glfwSetWindowMonitor(fWindow, monitor, 0, 0, mode->width, mode->height,
                             mode->refreshRate);
        std::fprintf(stderr,
                     "openshow-worker: fullscreen on monitor %d (%s) %dx%d@%dHz\n",
                     options.monitor, glfwGetMonitorName(monitor),
                     mode->width, mode->height, mode->refreshRate);
        // Let the compositor's configure arrive before the caller creates
        // the Vulkan surface, up to ~600 ms; the render loop's extent check
        // catches the late case regardless.
        for (int i = 0; i < 60; ++i) {
            glfwWaitEventsTimeout(0.01);
            int width = 0;
            int height = 0;
            glfwGetFramebufferSize(fWindow, &width, &height);
            if (width == mode->width && height == mode->height) {
                break;
            }
        }
    } else {
        glfwShowWindow(fWindow);
        // Give the initial configure a short head start (~150 ms).
        for (int i = 0; i < 15; ++i) {
            glfwWaitEventsTimeout(0.01);
        }
    }
    fResized = true;
    return true;
}

void OutputWindow::close() {
    if (fWindow) {
        glfwDestroyWindow(fWindow);
        fWindow = nullptr;
    }
    if (fOwnsGlfw) {
        glfwTerminate();
        fOwnsGlfw = false;
        gGlfwInitialised = false;
    }
}

bool OutputWindow::shouldClose() const {
    return !fWindow || glfwWindowShouldClose(fWindow);
}

bool OutputWindow::framebufferSize(int* width, int* height) const {
    if (!fWindow) {
        return false;
    }
    glfwGetFramebufferSize(fWindow, width, height);
    return *width > 0 && *height > 0;
}

bool OutputWindow::framebufferResized() const {
    return fResized;
}

void OutputWindow::resetFramebufferResize() {
    fResized = false;
}

void OutputWindow::onFramebufferSize(GLFWwindow* window, int, int) {
    auto* self = static_cast<OutputWindow*>(glfwGetWindowUserPointer(window));
    if (self) {
        self->fResized = true;
    }
}

bool OutputWindow::printMonitors() {
    if (!initGlfw()) {
        return false;
    }
    int count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    GLFWmonitor* primary = glfwGetPrimaryMonitor();
    std::printf("monitors: %d (primary index reported below as *)\n", count);
    for (int i = 0; i < count; ++i) {
        const GLFWvidmode* mode = glfwGetVideoMode(monitors[i]);
        int x = 0;
        int y = 0;
        glfwGetMonitorPos(monitors[i], &x, &y);
        std::printf("  [%d]%s %s at %d,%d", i, monitors[i] == primary ? "*" : " ",
                    glfwGetMonitorName(monitors[i]), x, y);
        if (mode) {
            std::printf(" mode %dx%d@%dHz", mode->width, mode->height,
                        mode->refreshRate);
        }
        float xs = 0.0f;
        float ys = 0.0f;
        glfwGetMonitorContentScale(monitors[i], &xs, &ys);
        std::printf(" content-scale %.3f\n", xs);
        if (ys != xs) {
            std::printf("      (uneven scale y=%.3f)\n", ys);
        }
    }
    glfwTerminate();
    gGlfwInitialised = false;
    return true;
}

}  // namespace openshow::worker
