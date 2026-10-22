/*
 * VulkanContext: instance, surface, device and the Skia Vulkan backend
 * context for the worker's output window. Follows the proven pattern from
 * /home/jim/sloppy-ui (vk-bootstrap for setup, graphics queue that presents).
 */

#pragma once

#include <memory>
#include <string>

#include <vulkan/vulkan.h>

#include "include/gpu/vk/VulkanExtensions.h"

struct GLFWwindow;
namespace skgpu { struct VulkanBackendContext; }

namespace openshow::worker {

class VulkanContext {
public:
    VulkanContext() = default;
    ~VulkanContext() { destroy(); }

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    bool create(GLFWwindow* window);
    void destroy();

    VkInstance instance() const { return fInstance; }
    VkPhysicalDevice physicalDevice() const { return fPhysicalDevice; }
    VkDevice device() const { return fDevice; }
    VkQueue queue() const { return fQueue; }
    uint32_t queueIndex() const { return fQueueIndex; }
    VkSurfaceKHR surface() const { return fSurface; }
    const std::string& deviceName() const { return fDeviceName; }

    // Filled by create(); valid until destroy().
    skgpu::VulkanBackendContext& backendContext() { return *fBackendContext; }

private:
    VkInstance fInstance = VK_NULL_HANDLE;
    VkSurfaceKHR fSurface = VK_NULL_HANDLE;
    VkPhysicalDevice fPhysicalDevice = VK_NULL_HANDLE;
    VkDevice fDevice = VK_NULL_HANDLE;
    VkQueue fQueue = VK_NULL_HANDLE;
    uint32_t fQueueIndex = 0;
    std::string fDeviceName;
    skgpu::VulkanBackendContext* fBackendContext = nullptr;
    // The backend context stores raw pointers to these, so they must live as
    // long as the context does. Skia reads the enabled device features and the
    // extension list straight out of them.
    skgpu::VulkanExtensions fExtensions;
    VkPhysicalDeviceFeatures2 fDeviceFeatures2 = {};
};

}  // namespace openshow::worker
