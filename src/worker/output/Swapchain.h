/*
 * Swapchain: Vulkan swapchain plus one Skia Ganesh surface per image.
 * Adapted from the proven /home/jim/sloppy-ui classes (acquire fence,
 * per-image frame fence and two semaphores); FIFO present only for now.
 */

#pragma once

#include <vector>

#include <vulkan/vulkan.h>

#include "include/core/SkSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"

namespace openshow::worker {

class Swapchain {
public:
    struct CreateInfo {
        VkDevice device = VK_NULL_HANDLE;
        VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VkQueue queue = VK_NULL_HANDLE;
        uint32_t presentQueueIndex = 0;
        GrDirectContext* context = nullptr;
        uint32_t imageCount = 3;
        // Fallback size used when the surface reports an undefined extent
        // (VK_WHOLE_SIZE), which happens under Wayland before the compositor
        // has configured the surface for its new (fullscreen) mode.
        int width = 1280;
        int height = 720;
    };

    Swapchain() = default;
    ~Swapchain() { destroy(); }

    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    bool create(const CreateInfo& info);
    void destroy();

    // Waits for the image and its previous frame to be free. Returns false on
    // VK_ERROR_OUT_OF_DATE_KHR / VK_SUBOPTIMAL_KHR; the caller recreates.
    bool acquire(uint32_t* outImageIndex, VkResult* outResult = nullptr);

    // Flushes Skia work for the image, transitioning it to
    // VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, then submits and presents.
    bool flushAndPresent(SkSurface* surface, uint32_t imageIndex,
                         VkResult* outResult = nullptr);

    SkSurface* surface(uint32_t index);
    uint32_t imageCount() const { return static_cast<uint32_t>(fImages.size()); }
    int width() const { return fWidth; }
    int height() const { return fHeight; }

private:
    struct Image {
        VkSemaphore fRenderComplete = VK_NULL_HANDLE;
        VkSemaphore fPresentReady = VK_NULL_HANDLE;
        VkFence fFrameFence = VK_NULL_HANDLE;
        sk_sp<SkSurface> fSurface;

        void destroy(VkDevice device);
    };

    VkDevice fDevice = VK_NULL_HANDLE;
    VkQueue fQueue = VK_NULL_HANDLE;
    VkSwapchainKHR fSwapchain = VK_NULL_HANDLE;
    VkFence fAcquireFence = VK_NULL_HANDLE;
    GrDirectContext* fContext = nullptr;
    uint32_t fPresentQueueIndex = 0;
    int fWidth = 0;
    int fHeight = 0;
    std::vector<Image> fImages;
};

}  // namespace openshow::worker
