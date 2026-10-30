#include "Swapchain.h"

#include <algorithm>
#include <cstdio>

#include "include/core/SkColorSpace.h"
#include "include/core/SkSurfaceProps.h"
#include "include/gpu/GpuTypes.h"
#include "include/gpu/ganesh/GrBackendSemaphore.h"
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/vk/GrVkBackendSemaphore.h"
#include "include/gpu/ganesh/vk/GrVkBackendSurface.h"
#include "include/gpu/ganesh/vk/GrVkTypes.h"
#include "include/gpu/vk/VulkanMutableTextureState.h"
#include "output/VulkanQueueGuard.h"

namespace openshow::worker {

void Swapchain::Image::destroy(VkDevice device) {
    fSurface.reset();
    if (device != VK_NULL_HANDLE) {
        if (fRenderComplete != VK_NULL_HANDLE) {
            vkDestroySemaphore(device, fRenderComplete, nullptr);
            fRenderComplete = VK_NULL_HANDLE;
        }
        if (fPresentReady != VK_NULL_HANDLE) {
            vkDestroySemaphore(device, fPresentReady, nullptr);
            fPresentReady = VK_NULL_HANDLE;
        }
        if (fFrameFence != VK_NULL_HANDLE) {
            vkDestroyFence(device, fFrameFence, nullptr);
            fFrameFence = VK_NULL_HANDLE;
        }
    }
}

bool Swapchain::create(const CreateInfo& info) {
    fDevice = info.device;
    fPhysicalDevice = info.physicalDevice;
    fSurface = info.surface;
    fQueue = info.queue;
    fContext = info.context;
    fPresentQueueIndex = info.presentQueueIndex;

    VkSurfaceCapabilitiesKHR capabilities = {};
    VkResult err = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(info.physicalDevice, info.surface,
                                                             &capabilities);
    if (err != VK_SUCCESS) {
        std::fprintf(stderr, "openshow-worker: surface capabilities failed: %d\n", err);
        return false;
    }

    uint32_t formatCount = 0;
    err = vkGetPhysicalDeviceSurfaceFormatsKHR(info.physicalDevice, info.surface, &formatCount,
                                               nullptr);
    if (err != VK_SUCCESS || formatCount == 0) {
        std::fprintf(stderr, "openshow-worker: surface has no formats: %d\n", err);
        return false;
    }
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(info.physicalDevice, info.surface, &formatCount,
                                         formats.data());

    VkSurfaceFormatKHR chosenFormat = formats[0];
    for (const VkSurfaceFormatKHR& format : formats) {
        if (format.format == VK_FORMAT_B8G8R8A8_UNORM || format.format == VK_FORMAT_R8G8B8A8_UNORM) {
            chosenFormat = format;
            break;
        }
    }
    // Match the Skia color type to the chosen format's channel order, not
    // just to the UNORM names: a Wayland surface may expose only SRGB
    // variants, and pairing B8G8R8A8_SRGB with kRGBA_8888 makes
    // SkSurfaces::WrapBackendRenderTarget reject the wrap.
    const SkColorType colorType =
        (chosenFormat.format == VK_FORMAT_B8G8R8A8_UNORM ||
         chosenFormat.format == VK_FORMAT_B8G8R8A8_SRGB)
            ? kBGRA_8888_SkColorType
            : kRGBA_8888_SkColorType;

    VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == 0xFFFFFFFFu) {
        // Undefined extent: fall back to the caller's window size. Under
        // Wayland the compositor reports a real extent only after it has
        // configured the surface; the size chosen here is provisional and the
        // first acquire/present returns VK_ERROR_OUT_OF_DATE_KHR once the real
        // size is known, which the frame loop answers by recreating.
        if (info.width <= 0 || info.height <= 0) {
            std::fprintf(stderr, "openshow-worker: surface extent undefined and no fallback size\n");
            return false;
        }
        extent.width = static_cast<uint32_t>(info.width);
        extent.height = static_cast<uint32_t>(info.height);
    }
    extent.width = std::max(capabilities.minImageExtent.width,
                            std::min(capabilities.maxImageExtent.width, extent.width));
    extent.height = std::max(capabilities.minImageExtent.height,
                             std::min(capabilities.maxImageExtent.height, extent.height));

    uint32_t imageCount = info.imageCount;
    if (imageCount < capabilities.minImageCount) {
        imageCount = capabilities.minImageCount;
    }
    if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount) {
        imageCount = capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = info.surface;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = chosenFormat.format;
    createInfo.imageColorSpace = chosenFormat.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    // Skia's check_image_info rejects any wrapped image whose usage lacks
    // BOTH transfer bits, so the swapchain must request TRANSFER_SRC too
    // (this is what sloppy-ui does; leaving it out made every
    // WrapBackendRenderTarget return null).
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                            VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    createInfo.preTransform = capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    createInfo.clipped = VK_TRUE;

    err = vkCreateSwapchainKHR(fDevice, &createInfo, nullptr, &fSwapchain);
    if (err != VK_SUCCESS) {
        std::fprintf(stderr, "openshow-worker: vkCreateSwapchainKHR failed: %d\n", err);
        return false;
    }

    uint32_t swapchainImageCount = 0;
    err = vkGetSwapchainImagesKHR(fDevice, fSwapchain, &swapchainImageCount, nullptr);
    if (err != VK_SUCCESS || swapchainImageCount == 0) {
        std::fprintf(stderr, "openshow-worker: vkGetSwapchainImagesKHR failed: %d\n", err);
        return false;
    }
    std::vector<VkImage> vkImages(swapchainImageCount);
    vkGetSwapchainImagesKHR(fDevice, fSwapchain, &swapchainImageCount, vkImages.data());

    VkFenceCreateInfo acquireFenceInfo = {};
    acquireFenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    err = vkCreateFence(fDevice, &acquireFenceInfo, nullptr, &fAcquireFence);
    if (err != VK_SUCCESS) {
        std::fprintf(stderr, "openshow-worker: acquire fence failed: %d\n", err);
        return false;
    }

    fImages.resize(swapchainImageCount);
    const VkSemaphoreCreateInfo semaphoreInfo = {
        VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, nullptr};
    const VkFenceCreateInfo frameFenceInfo = {
        VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT};

    for (uint32_t i = 0; i < swapchainImageCount; ++i) {
        Image& image = fImages[i];
        if (vkCreateSemaphore(fDevice, &semaphoreInfo, nullptr, &image.fRenderComplete) !=
                VK_SUCCESS ||
            vkCreateSemaphore(fDevice, &semaphoreInfo, nullptr, &image.fPresentReady) !=
                VK_SUCCESS ||
            vkCreateFence(fDevice, &frameFenceInfo, nullptr, &image.fFrameFence) != VK_SUCCESS) {
            std::fprintf(stderr, "openshow-worker: per-image sync creation failed\n");
            return false;
        }

        GrVkImageInfo imageInfo;
        imageInfo.fImage = vkImages[i];
        imageInfo.fImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.fFormat = chosenFormat.format;
        imageInfo.fImageUsageFlags = createInfo.imageUsage;
        imageInfo.fSampleCount = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.fLevelCount = 1;
        imageInfo.fCurrentQueueFamily = info.presentQueueIndex;
        imageInfo.fSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.fProtected = skgpu::Protected::kNo;
        GrBackendRenderTarget backendTarget =
            GrBackendRenderTargets::MakeVk(static_cast<int>(extent.width),
                                           static_cast<int>(extent.height), imageInfo);

        SkSurfaceProps props(0, kRGB_H_SkPixelGeometry);
        image.fSurface = SkSurfaces::WrapBackendRenderTarget(
            info.context, backendTarget, kTopLeft_GrSurfaceOrigin, colorType,
            SkColorSpace::MakeSRGB(), &props);
        if (!image.fSurface) {
            std::fprintf(stderr,
                         "openshow-worker: WrapBackendRenderTarget failed for image %u "
                         "(format %d colorspace %d, colorType %d, extent %ux%u, target valid %d)\n",
                         i, static_cast<int>(chosenFormat.format),
                         static_cast<int>(chosenFormat.colorSpace), static_cast<int>(colorType),
                         extent.width, extent.height, backendTarget.isValid() ? 1 : 0);
            for (const VkSurfaceFormatKHR& format : formats) {
                std::fprintf(stderr, "openshow-worker:   surface offered format %d colorspace %d\n",
                             static_cast<int>(format.format),
                             static_cast<int>(format.colorSpace));
            }
            return false;
        }
    }

    fWidth = static_cast<int>(extent.width);
    fHeight = static_cast<int>(extent.height);
    std::fprintf(stderr, "openshow-worker: swapchain %dx%d, %u images, format %d\n",
                 fWidth, fHeight, swapchainImageCount, static_cast<int>(chosenFormat.format));
    return true;
}

void Swapchain::destroy() {
    // The same ordering as every other teardown on this device (task 6.22b,
    // review A2): vkDeviceWaitIdle needs the device's queues externally
    // synchronised, and a resize can reap the swapchain while a video
    // producer is inside its guarded commit. frame() already holds this
    // recursive guard, so the presenting path is unchanged.
    const std::lock_guard queueLock(graphicsQueueMutex());
    if (fDevice != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(fDevice);
    }
    for (Image& image : fImages) {
        image.destroy(fDevice);
    }
    fImages.clear();
    if (fAcquireFence != VK_NULL_HANDLE) {
        vkDestroyFence(fDevice, fAcquireFence, nullptr);
        fAcquireFence = VK_NULL_HANDLE;
    }
    if (fSwapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(fDevice, fSwapchain, nullptr);
        fSwapchain = VK_NULL_HANDLE;
    }
    fDevice = VK_NULL_HANDLE;
    fPhysicalDevice = VK_NULL_HANDLE;
    fSurface = VK_NULL_HANDLE;
    fQueue = VK_NULL_HANDLE;
    fContext = nullptr;
    fWidth = 0;
    fHeight = 0;
}

bool Swapchain::extentOutOfDate(int* outWidth, int* outHeight) const {
    if (fSurface == VK_NULL_HANDLE || fPhysicalDevice == VK_NULL_HANDLE) {
        return false;
    }
    VkSurfaceCapabilitiesKHR capabilities = {};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(fPhysicalDevice, fSurface, &capabilities) !=
        VK_SUCCESS) {
        return false;
    }
    const uint32_t width = capabilities.currentExtent.width;
    const uint32_t height = capabilities.currentExtent.height;
    if (width == 0xFFFFFFFFu || height == 0xFFFFFFFFu) {
        return false;  // not (re)configured yet
    }
    if (outWidth) {
        *outWidth = static_cast<int>(width);
    }
    if (outHeight) {
        *outHeight = static_cast<int>(height);
    }
    return static_cast<int>(width) != fWidth || static_cast<int>(height) != fHeight;
}

bool Swapchain::acquire(uint32_t* outImageIndex, VkResult* outResult) {
    const VkResult err = vkAcquireNextImageKHR(fDevice, fSwapchain, UINT64_MAX, VK_NULL_HANDLE,
                                               fAcquireFence, outImageIndex);
    if (outResult) {
        *outResult = err;
    }
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
        return false;
    }
    if (err != VK_SUCCESS) {
        std::fprintf(stderr, "openshow-worker: vkAcquireNextImageKHR failed: %d\n", err);
        return false;
    }

    vkWaitForFences(fDevice, 1, &fAcquireFence, VK_TRUE, UINT64_MAX);
    vkResetFences(fDevice, 1, &fAcquireFence);

    Image& image = fImages[*outImageIndex];
    vkWaitForFences(fDevice, 1, &image.fFrameFence, VK_TRUE, UINT64_MAX);
    vkResetFences(fDevice, 1, &image.fFrameFence);
    return true;
}

bool Swapchain::flushAndPresent(SkSurface* surface, uint32_t imageIndex, VkResult* outResult,
                                VkSemaphore extraSignal) {
    Image& image = fImages[imageIndex];

    // Hand the image from Skia to the presentation queue: flush with the
    // layout transition baked in and a timeline semaphore to gate present.
    // Callers may attach one more signal of their own (task 1.5e: a video
    // done-semaphore covering the draw that sampled a GPU slot) — every
    // failure path below still flushes first, so attached semaphores are
    // signalled regardless of the return value.
    GrBackendSemaphore signals[2] = {GrBackendSemaphores::MakeVk(image.fRenderComplete),
                                     GrBackendSemaphore{}};
    size_t signalCount = 1;
    if (extraSignal != VK_NULL_HANDLE) {
        signals[1] = GrBackendSemaphores::MakeVk(extraSignal);
        signalCount = 2;
    }
    GrFlushInfo flushInfo;
    flushInfo.fNumSemaphores = signalCount;
    flushInfo.fSignalSemaphores = signals;
    skgpu::MutableTextureState presentState = skgpu::MutableTextureStates::MakeVulkan(
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, fPresentQueueIndex);
    fContext->flush(surface, flushInfo, &presentState);
    fContext->submit();

    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &image.fRenderComplete;
    submitInfo.pWaitDstStageMask = &waitStage;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &image.fPresentReady;
    VkResult err = vkQueueSubmit(fQueue, 1, &submitInfo, image.fFrameFence);
    if (err != VK_SUCCESS) {
        std::fprintf(stderr, "openshow-worker: present-chain submit failed: %d\n", err);
        if (outResult) {
            *outResult = err;
        }
        return false;
    }

    VkPresentInfoKHR presentInfo = {};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &image.fPresentReady;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &fSwapchain;
    presentInfo.pImageIndices = &imageIndex;

    err = vkQueuePresentKHR(fQueue, &presentInfo);
    if (outResult) {
        *outResult = err;
    }
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
        return false;
    }
    if (err != VK_SUCCESS) {
        std::fprintf(stderr, "openshow-worker: vkQueuePresentKHR failed: %d\n", err);
        return false;
    }
    return true;
}

SkSurface* Swapchain::surface(uint32_t index) {
    return index < fImages.size() ? fImages[index].fSurface.get() : nullptr;
}

}  // namespace openshow::worker
