#include "VulkanContext.h"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include <GLFW/glfw3.h>
#include <VkBootstrap.h>

#include "include/gpu/vk/VulkanBackendContext.h"
#include "include/gpu/vk/VulkanMemoryAllocator.h"
#include "src/gpu/GpuTypesPriv.h"
#include "src/gpu/vk/vulkanmemoryallocator/VulkanMemoryAllocatorPriv.h"

namespace openshow::worker {

namespace {
constexpr uint32_t kApiVersion = VK_API_VERSION_1_2;
}

bool VulkanContext::create(GLFWwindow* window) {
    uint32_t glfwExtensionCount = 0;
    const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
    if (!glfwExtensions || glfwExtensionCount == 0) {
        std::fprintf(stderr, "openshow-worker: GLFW returned no Vulkan instance extensions\n");
        return false;
    }

    vkb::InstanceBuilder instanceBuilder;
    instanceBuilder.set_app_name("openshow-worker")
        .require_api_version(kApiVersion);
    for (uint32_t i = 0; i < glfwExtensionCount; ++i) {
        instanceBuilder.enable_extension(glfwExtensions[i]);
    }

    auto builtInstance = instanceBuilder.build();
    if (!builtInstance) {
        std::fprintf(stderr, "openshow-worker: Vulkan instance failed: %s\n",
                     builtInstance.error().message().c_str());
        return false;
    }
    fInstance = builtInstance->instance;

    if (glfwCreateWindowSurface(fInstance, window, nullptr, &fSurface) != VK_SUCCESS) {
        std::fprintf(stderr, "openshow-worker: glfwCreateWindowSurface failed\n");
        destroy();
        return false;
    }

    auto selectedPhysicalDevice =
        vkb::PhysicalDeviceSelector(*builtInstance).set_surface(fSurface).select();
    if (!selectedPhysicalDevice) {
        std::fprintf(stderr, "openshow-worker: no Vulkan device presents to the window: %s\n",
                     selectedPhysicalDevice.error().message().c_str());
        destroy();
        return false;
    }
    fPhysicalDevice = selectedPhysicalDevice->physical_device;

    auto builtDevice = vkb::DeviceBuilder(*selectedPhysicalDevice).build();
    if (!builtDevice) {
        std::fprintf(stderr, "openshow-worker: Vulkan device failed: %s\n",
                     builtDevice.error().message().c_str());
        destroy();
        return false;
    }
    fDevice = builtDevice->device;
    fQueue = builtDevice->get_queue(vkb::QueueType::graphics).value();
    fQueueIndex = builtDevice->get_queue_index(vkb::QueueType::graphics).value();

    VkBool32 queueCanPresent = VK_FALSE;
    if (vkGetPhysicalDeviceSurfaceSupportKHR(fPhysicalDevice, fQueueIndex, fSurface,
                                             &queueCanPresent) != VK_SUCCESS ||
        !queueCanPresent) {
        std::fprintf(stderr, "openshow-worker: graphics queue cannot present\n");
        destroy();
        return false;
    }

    VkPhysicalDeviceProperties properties = {};
    vkGetPhysicalDeviceProperties(fPhysicalDevice, &properties);
    fDeviceName = properties.deviceName;

    // The backend context Skia needs for GrDirectContexts::MakeVulkan. fGetProc
    // lets Skia resolve entry points without linking the whole loader chain.
    fBackendContext = new skgpu::VulkanBackendContext();
    fBackendContext->fInstance = fInstance;
    fBackendContext->fPhysicalDevice = fPhysicalDevice;
    fBackendContext->fDevice = fDevice;
    fBackendContext->fQueue = fQueue;
    fBackendContext->fGraphicsQueueIndex = fQueueIndex;
    fBackendContext->fMaxAPIVersion = kApiVersion;
    fBackendContext->fGetProc = [instance = fInstance](const char* name,
                                                       VkInstance instanceArgument,
                                                       VkDevice deviceArgument)
                                    -> PFN_vkVoidFunction {
        if (deviceArgument != VK_NULL_HANDLE) {
            auto getDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
                vkGetInstanceProcAddr(instance, "vkGetDeviceProcAddr"));
            return getDeviceProcAddr ? getDeviceProcAddr(deviceArgument, name) : nullptr;
        }
        return vkGetInstanceProcAddr(instanceArgument != VK_NULL_HANDLE ? instanceArgument
                                                                        : instance,
                                     name);
    };

    // The three fields below are what Skia reads to build its caps, and Skia
    // refuses to create the context without a memory allocator (this Skia
    // revision has no internal default). Sloppy-ui does the same dance.
    //
    // Extensions first: Skia queries them during GrVkGpu init, before the
    // allocator is used, so the enabled device extension list from vk-bootstrap
    // has to be handed over (instance extensions are not checked by Skia, so
    // sloppy-ui passes none and neither do we).
    const std::vector<std::string> deviceExtensions = selectedPhysicalDevice->get_extensions();
    std::vector<const char*> deviceExtensionNames;
    deviceExtensionNames.reserve(deviceExtensions.size());
    for (const std::string& extension : deviceExtensions) {
        deviceExtensionNames.push_back(extension.c_str());
    }
    fExtensions.init(fBackendContext->fGetProc, fInstance, fPhysicalDevice,
                     0, nullptr,
                     static_cast<uint32_t>(deviceExtensionNames.size()),
                     deviceExtensionNames.data());
    fBackendContext->fVkExtensions = &fExtensions;

    // Report the exact features vk-bootstrap enabled on the device.
    fDeviceFeatures2 = {};
    fDeviceFeatures2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    fDeviceFeatures2.features = selectedPhysicalDevice->features;
    fBackendContext->fDeviceFeatures2 = &fDeviceFeatures2;

    fBackendContext->fMemoryAllocator =
        skgpu::VulkanMemoryAllocators::Make(*fBackendContext, skgpu::ThreadSafe::kNo);
    if (!fBackendContext->fMemoryAllocator) {
        std::fprintf(stderr, "openshow-worker: Skia Vulkan memory allocator failed\n");
        destroy();
        return false;
    }

    std::fprintf(stderr, "openshow-worker: Vulkan device \"%s\" (queue %u presents)\n",
                 fDeviceName.c_str(), fQueueIndex);
    return true;
}

void VulkanContext::destroy() {
    if (fDevice != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(fDevice);
    }
    delete fBackendContext;
    fBackendContext = nullptr;
    if (fDevice != VK_NULL_HANDLE) {
        vkDestroyDevice(fDevice, nullptr);
        fDevice = VK_NULL_HANDLE;
    }
    fQueue = VK_NULL_HANDLE;
    if (fSurface != VK_NULL_HANDLE && fInstance != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(fInstance, fSurface, nullptr);
        fSurface = VK_NULL_HANDLE;
    }
    if (fInstance != VK_NULL_HANDLE) {
        vkDestroyInstance(fInstance, nullptr);
        fInstance = VK_NULL_HANDLE;
    }
    fPhysicalDevice = VK_NULL_HANDLE;
}

}  // namespace openshow::worker
