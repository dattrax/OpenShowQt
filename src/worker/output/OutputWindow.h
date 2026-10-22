/*
 * OutputWindow: the GLFW window the audience output (and task 1.1's render
 * test) presents into. No client API; VulkanContext creates the surface.
 *
 * The window is always borderless (GLFW_DECORATED false). Under Wayland
 * decorations are the compositor's, and a fullscreen toplevel is borderless
 * by definition, so "fullscreen on monitor N" is the reliable audience mode.
 */

#pragma once

struct GLFWwindow;

namespace openshow::worker {

class OutputWindow {
public:
    struct Options {
        int width = 1280;
        int height = 720;
        const char* title = "OpenShowQt output";
        // -1 keeps the borderless windowed mode. >=0 calls
        // glfwSetWindowMonitor() fullscreen on that monitor at its current mode.
        int monitor = -1;
    };

    OutputWindow() = default;
    ~OutputWindow();

    OutputWindow(const OutputWindow&) = delete;
    OutputWindow& operator=(const OutputWindow&) = delete;

    bool open(const Options& options);
    void close();

    GLFWwindow* handle() const { return fWindow; }
    bool shouldClose() const;

    // Physical (buffer) pixels of the window; may differ from logical size
    // under Wayland fractional scaling.
    bool framebufferSize(int* width, int* height) const;

    // True once since the last resetFramebufferResize(); drives swapchain
    // rebuilds alongside VK_ERROR_OUT_OF_DATE_KHR.
    bool framebufferResized() const;
    void resetFramebufferResize();

    // Prints the GLFW monitor list (name, resolution, refresh) to stdout.
    // Initialises GLFW if needed. Returns false when there is no display.
    static bool printMonitors();

private:
    static void onFramebufferSize(GLFWwindow* window, int width, int height);
    static bool initGlfw();

    GLFWwindow* fWindow = nullptr;
    bool fOwnsGlfw = false;
    bool fResized = true;
};

}  // namespace openshow::worker
