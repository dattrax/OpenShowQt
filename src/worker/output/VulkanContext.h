/*
 * VulkanContext: instance, surface, device and the Skia Vulkan backend
 * context for the worker's output window. Follows the proven pattern from
 * /home/jim/sloppy-ui (vk-bootstrap for setup, graphics queue that presents).
 *
 * Task 1.5d added two things: createHeadless() (no surface at all, for the
 * offscreen GPU tests) and an optional dedicated upload queue. When the GPU
 * offers a transfer-capable family other than the graphics family (RADV
 * does: family 0 gfx, family 1 compute+transfer), the device is built with
 * both and the uploader may copy on its own queue while Skia draws. On
 * single-family GPUs (llvmpipe) the upload queue is the graphics queue and
 * callers skip cross-queue handling. Images intended for the uploader are
 * created with CONCURRENT sharing over both families.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <vulkan/vulkan.h>

#include "include/gpu/vk/VulkanExtensions.h"

struct GLFWwindow;
namespace skgpu { struct VulkanBackendContext; }

namespace openshow::worker {

class VulkanContext {
public:
    // Constructor and destructor are out of line: fPhysical is a pimpl whose
    // type is only complete in the .cpp.
    VulkanContext();
    ~VulkanContext();

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    // Windowed: instance, GLFW surface, device (graphics queue presents).
    bool create(GLFWwindow* window);
    // Offscreen: instance and device only; nothing can present.
    bool createHeadless();
    void destroy();

    VkInstance instance() const { return fInstance; }
    VkPhysicalDevice physicalDevice() const { return fPhysicalDevice; }
    VkDevice device() const { return fDevice; }
    VkQueue queue() const { return fQueue; }
    uint32_t queueIndex() const { return fQueueIndex; }
    VkSurfaceKHR surface() const { return fSurface; }
    const std::string& deviceName() const { return fDeviceName; }

    // Copy/upload queue: a distinct transfer family when the GPU has one,
    // otherwise the graphics queue itself.
    VkQueue uploadQueue() const { return fUploadQueue; }
    uint32_t uploadQueueIndex() const { return fUploadQueueIndex; }
    bool hasDedicatedUploadQueue() const { return fUploadQueueIndex != fQueueIndex; }

    // Filled by create(); valid until destroy().
    skgpu::VulkanBackendContext& backendContext() { return *fBackendContext; }

private:
    bool createInstance(bool withWindowExtensions);
    bool selectPhysicalDevice();
    bool createDevice();
    int pickUploadFamily() const;  // -1 when no separate family exists

    VkInstance fInstance = VK_NULL_HANDLE;
    VkSurfaceKHR fSurface = VK_NULL_HANDLE;
    VkPhysicalDevice fPhysicalDevice = VK_NULL_HANDLE;
    VkDevice fDevice = VK_NULL_HANDLE;
    VkQueue fQueue = VK_NULL_HANDLE;
    uint32_t fQueueIndex = 0;
    VkQueue fUploadQueue = VK_NULL_HANDLE;
    uint32_t fUploadQueueIndex = 0;
    std::string fDeviceName;
    skgpu::VulkanBackendContext* fBackendContext = nullptr;
    // The backend context stores raw pointers to these, so they must live as
    // long as the context does. Skia reads the enabled device features and the
    // extension list straight out of them.
    skgpu::VulkanExtensions fExtensions;
    VkPhysicalDeviceFeatures2 fDeviceFeatures2 = {};
    // Survive between create() phases so the queue/feature handover to Skia
    // can read them after the device exists.
    class PhysicalDeviceStorage;
    std::unique_ptr<PhysicalDeviceStorage> fPhysical;
};

}  // namespace openshow::worker
