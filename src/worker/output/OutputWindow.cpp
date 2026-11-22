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

void OutputWindow::shutdownGlfw() {
    if (!gGlfwInitialised) {
        return;
    }
    glfwTerminate();
    gGlfwInitialised = false;
}

bool OutputWindow::open(const Options& options) {
    if (!initGlfw()) {
        return false;
    }

    fWidth = options.width;
    fHeight = options.height;

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    // The operator UI owns input focus during a service. GLFW otherwise
    // minimizes a fullscreen window when it loses focus, hiding the audience
    // output whenever the operator clicks the separate frontend window.
    glfwWindowHint(GLFW_AUTO_ICONIFY, GLFW_FALSE);

    fWindow = glfwCreateWindow(options.width, options.height, options.title,
                               nullptr, nullptr);
    if (!fWindow) {
        std::fprintf(stderr, "openshow-worker: glfwCreateWindow failed\n");
        return false;
    }
    glfwSetWindowUserPointer(fWindow, this);
    glfwSetFramebufferSizeCallback(fWindow, &OutputWindow::onFramebufferSize);

    // Map straight onto the selected panel. The window was created hidden
    // and glfwSetWindowMonitor() places it while hidden, so it is never
    // briefly shown on the operator's display and then moved — the
    // show-then-fullscreen sequence the stage delivery plan requires this
    // path to abandon. On Wayland the fullscreen state travels with the
    // first map, which is why the monitor is set before glfwShowWindow.
    if (options.monitor >= 0) {
        glfwShowWindow(fWindow);
        // Map first, then request fullscreen on the TARGET panel. GLFW's
        // Wayland backend applies the toplevel state when the surface maps;
        // setting the monitor while hidden leaves the new toplevel at its
        // creation size on current KWin. The window is assigned to its target
        // in the same event turn as the map, and the compositor never puts it
        // on the operator's display in between.
        if (!setMonitor(options.monitor, 600)) {
            close();
            return false;
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

bool OutputWindow::setMonitor(int index, int settleMs) {
    if (!fWindow) {
        return false;
    }

    int targetWidth = 0;
    int targetHeight = 0;
    if (index >= 0) {
        int monitorCount = 0;
        GLFWmonitor** monitors = glfwGetMonitors(&monitorCount);
        if (!monitors || index >= monitorCount) {
            std::fprintf(stderr, "openshow-worker: monitor %d requested, %d present\n",
                         index, monitorCount);
            return false;
        }
        GLFWmonitor* monitor = monitors[index];
        const GLFWvidmode* mode = glfwGetVideoMode(monitor);
        if (!mode) {
            std::fprintf(stderr, "openshow-worker: monitor %d reports no mode\n", index);
            return false;
        }
        glfwSetWindowMonitor(fWindow, monitor, 0, 0, mode->width, mode->height,
                             mode->refreshRate);
        std::fprintf(stderr,
                     "openshow-worker: output %s monitor %d (%s) %dx%d@%dHz\n",
                     fMonitor >= 0 ? "switched to" : "fullscreen on", index,
                     glfwGetMonitorName(monitor), mode->width, mode->height,
                     mode->refreshRate);
        targetWidth = mode->width;
        targetHeight = mode->height;
    } else {
        // Restore the borderless window at the size it opened with.
        glfwSetWindowMonitor(fWindow, nullptr, 100, 100, fWidth, fHeight, 0);
        std::fprintf(stderr, "openshow-worker: output returned to windowed %dx%d\n", fWidth,
                     fHeight);
    }
    fMonitor = index;
    fResized = true;

    // Pump until the compositor's configure lands. Fullscreen ends when the
    // buffer reaches the panel mode; windowed restore ends when the buffer
    // merely changes (fractional scaling means it is rarely fWidth x fHeight).
    int lastWidth = 0;
    int lastHeight = 0;
    glfwGetFramebufferSize(fWindow, &lastWidth, &lastHeight);
    const int steps = settleMs > 0 ? settleMs / 10 : 0;
    for (int i = 0; i < steps; ++i) {
        glfwWaitEventsTimeout(0.01);
        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(fWindow, &width, &height);
        const bool arrived =
            index >= 0 ? (width == targetWidth && height == targetHeight)
                       : (width != lastWidth || height != lastHeight);
        if (arrived) {
            break;
        }
    }
    return true;
}

void OutputWindow::close() {
    if (fWindow) {
        glfwDestroyWindow(fWindow);
        fWindow = nullptr;
    }
    // GLFW's lifetime belongs to the PROCESS, not this window (stage
    // delivery FO.5a.1/FO.1e.1): closing the audience or the stage must not
    // terminate the other one's event source. shutdownGlfw() is called once
    // by the process owner after every window is gone.
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
    shutdownGlfw();
    return true;
}

}  // namespace openshow::worker
