#include "VulkanContext.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
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

// Task 1.5g: the GPU tests enable validation through the environment because
// they call createHeadless() directly; the worker binary has a CLI flag.
bool envValidationRequested() {
    const char* value = std::getenv("OPENSHOW_VULKAN_VALIDATION");
    return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

// One debug messenger per process would be fine (there is at most one live
// VulkanContext), but validation messages arrive on whichever thread made
// the call, so the dedup table needs a lock.
struct ValidationLog {
    std::mutex mutex;
    std::unordered_map<std::string, uint64_t> seenCounts;
};
ValidationLog gValidationLog;

// VkDebugUtilsMessengerCallbackEXT. Errors and warnings from every layer go
// to stderr. The validation layer reports the same violation once per call
// site per frame, and during a 25 fps presentation loop that floods, so an
// identical message is printed in full three times and then once every 60th
// occurrence with a repeat count.
VKAPI_ATTR VkBool32 VKAPI_CALL validationMessage(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    // Only WARNING and ERROR severities are registered, so the bits arrive
    // singly (VkDebugUtilsMessageSeverityFlagBitsEXT).
    const bool isError = severity == VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    const char* level = isError ? "error" : "warn";
    std::string key = data->pMessageIdName != nullptr ? data->pMessageIdName
                                                      : (data->pMessage != nullptr ? data->pMessage
                                                                                   : "(unnamed)");
    uint64_t count = 0;
    {
        std::lock_guard<std::mutex> lock(gValidationLog.mutex);
        count = ++gValidationLog.seenCounts[key];
    }
    if (count <= 3 || count % 60 == 0) {
        std::fprintf(stderr, "openshow-worker: vulkan validation %s: %s", level,
                     data->pMessage != nullptr ? data->pMessage : "(no message)");
        // Validation messages rarely end with a newline; repeat markers do.
        if (count > 3) {
            std::fprintf(stderr, " [seen %llu times]", static_cast<unsigned long long>(count));
        } else if (data->pMessage == nullptr ||
                   data->pMessage[std::strlen(data->pMessage) - 1] != '\n') {
            std::fprintf(stderr, "\n");
        }
    }
    // Returning VK_FALSE only means "not aborting"; nothing else to do.
    return VK_FALSE;
}
}  // namespace

// The vk-bootstrap instance and selected physical device have to outlive
// their phases so the selector and the Skia feature/extension handover can
// read them.
class VulkanContext::PhysicalDeviceStorage {
public:
    vkb::Instance instance;
    std::optional<vkb::PhysicalDevice> physical;
};

VulkanContext::VulkanContext() = default;
VulkanContext::~VulkanContext() { destroy(); }

bool VulkanContext::createInstance(bool withWindowExtensions, bool validation) {
    if (validation) {
        auto system = vkb::SystemInfo::get_system_info();
        if (!system || !system->validation_layers_available) {
            std::fprintf(stderr,
                         "openshow-worker: vulkan validation requested but "
                         "VK_LAYER_KHRONOS_validation is not installed\n");
            return false;
        }
    }
    vkb::InstanceBuilder instanceBuilder;
    instanceBuilder.set_app_name("openshow-worker").require_api_version(kApiVersion);
    if (validation) {
        // The debug-utils extension and messenger come with it: vk-bootstrap
        // enables VK_EXT_debug_utils whenever a callback is installed and
        // creates the messenger right after the instance. Messages produced
        // during vkCreateInstance itself are too early for the messenger and
        // are lost, as in every vk-bootstrap application.
        instanceBuilder.enable_validation_layers(true);
        instanceBuilder.set_debug_callback(&validationMessage);
        instanceBuilder.set_debug_messenger_severity(
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT);
    }
    if (withWindowExtensions) {
        uint32_t glfwExtensionCount = 0;
        const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
        if (!glfwExtensions || glfwExtensionCount == 0) {
            std::fprintf(stderr, "openshow-worker: GLFW returned no Vulkan instance extensions\n");
            return false;
        }
        for (uint32_t i = 0; i < glfwExtensionCount; ++i) {
            instanceBuilder.enable_extension(glfwExtensions[i]);
        }
    }

    auto builtInstance = instanceBuilder.build();
    if (!builtInstance) {
        std::fprintf(stderr, "openshow-worker: Vulkan instance failed: %s\n",
                     builtInstance.error().message().c_str());
        return false;
    }
    fInstance = builtInstance->instance;
    fMessenger = builtInstance->debug_messenger;
    fValidation = validation;
    fPhysical = std::make_unique<PhysicalDeviceStorage>();
    fPhysical->instance = *builtInstance;
    return true;
}

bool VulkanContext::create(GLFWwindow* window, bool validation) {
    if (!createInstance(true, validation || envValidationRequested())) {
        return false;
    }
    if (glfwCreateWindowSurface(fInstance, window, nullptr, &fSurface) != VK_SUCCESS) {
        std::fprintf(stderr, "openshow-worker: glfwCreateWindowSurface failed\n");
        destroy();
        return false;
    }
    if (!selectPhysicalDevice() || !createDevice()) {
        destroy();
        return false;
    }

    VkBool32 queueCanPresent = VK_FALSE;
    if (vkGetPhysicalDeviceSurfaceSupportKHR(fPhysicalDevice, fQueueIndex, fSurface,
                                             &queueCanPresent) != VK_SUCCESS ||
        !queueCanPresent) {
        std::fprintf(stderr, "openshow-worker: graphics queue cannot present\n");
        destroy();
        return false;
    }

    std::fprintf(stderr, "openshow-worker: Vulkan device \"%s\" (queue %u presents",
                 fDeviceName.c_str(), fQueueIndex);
    if (hasDedicatedUploadQueue()) {
        std::fprintf(stderr, ", uploads on queue %u", fUploadQueueIndex);
    }
    if (fValidation) {
        std::fprintf(stderr, ", validation on");
    }
    std::fprintf(stderr, ")\n");
    return true;
}

bool VulkanContext::createHeadless(bool validation) {
    if (!createInstance(false, validation || envValidationRequested())) {
        return false;
    }
    if (!selectPhysicalDevice() || !createDevice()) {
        destroy();
        return false;
    }
    std::fprintf(stderr, "openshow-worker: Vulkan device \"%s\" (headless", fDeviceName.c_str());
    if (hasDedicatedUploadQueue()) {
        std::fprintf(stderr, ", uploads on queue %u", fUploadQueueIndex);
    }
    if (fValidation) {
        std::fprintf(stderr, ", validation on");
    }
    std::fprintf(stderr, ")\n");
    return true;
}

bool VulkanContext::selectPhysicalDevice() {
    vkb::PhysicalDeviceSelector selector{fPhysical->instance};
    if (fSurface != VK_NULL_HANDLE) {
        selector.set_surface(fSurface);
    } else {
        // Headless: nothing will present, and the selector requires a
        // surface by default (fails with "no_surface_provided").
        selector.require_present(false);
    }
    auto selected = selector.select();
    if (!selected) {
        std::fprintf(stderr, "openshow-worker: no suitable Vulkan device: %s\n",
                     selected.error().message().c_str());
        return false;
    }
    fPhysicalDevice = selected->physical_device;
    fPhysical->physical = *selected;
    return true;
}

int VulkanContext::pickUploadFamily() const {
    const std::vector<VkQueueFamilyProperties> families = fPhysical->physical->get_queue_families();
    int separateTransfer = -1;  // transfer without graphics, compute preferred
    for (size_t family = 0; family < families.size(); ++family) {
        const VkQueueFlags flags = families[family].queueFlags;
        if (flags & VK_QUEUE_GRAPHICS_BIT) {
            continue;
        }
        if (!(flags & VK_QUEUE_TRANSFER_BIT)) {
            continue;
        }
        if (flags & VK_QUEUE_COMPUTE_BIT) {
            return static_cast<int>(family);
        }
        if (separateTransfer < 0) {
            separateTransfer = static_cast<int>(family);
        }
    }
    return separateTransfer;
}

bool VulkanContext::createDevice() {
    // The graphics family is the first one with the graphics bit; the
    // selector guaranteed one exists.
    uint32_t graphicsFamily = 0;
    const std::vector<VkQueueFamilyProperties> families = fPhysical->physical->get_queue_families();
    for (size_t family = 0; family < families.size(); ++family) {
        if (families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            graphicsFamily = static_cast<uint32_t>(family);
            break;
        }
    }
    const int uploadFamily = pickUploadFamily();

    std::vector<vkb::CustomQueueDescription> queues;
    queues.emplace_back(graphicsFamily, 1u, std::vector<float>{1.0f});
    if (uploadFamily >= 0) {
        queues.emplace_back(static_cast<uint32_t>(uploadFamily), 1u, std::vector<float>{1.0f});
    }

    auto builtDevice =
        vkb::DeviceBuilder(*fPhysical->physical).custom_queue_setup(queues).build();
    if (!builtDevice) {
        std::fprintf(stderr, "openshow-worker: Vulkan device failed: %s\n",
                     builtDevice.error().message().c_str());
        return false;
    }
    fDevice = builtDevice->device;
    vkGetDeviceQueue(fDevice, graphicsFamily, 0, &fQueue);
    fQueueIndex = graphicsFamily;
    if (uploadFamily >= 0) {
        vkGetDeviceQueue(fDevice, static_cast<uint32_t>(uploadFamily), 0, &fUploadQueue);
        fUploadQueueIndex = static_cast<uint32_t>(uploadFamily);
    } else {
        fUploadQueue = fQueue;
        fUploadQueueIndex = graphicsFamily;
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
    const std::vector<std::string> deviceExtensions = fPhysical->physical->get_extensions();
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
    fDeviceFeatures2.features = fPhysical->physical->features;
    fBackendContext->fDeviceFeatures2 = &fDeviceFeatures2;

    fBackendContext->fMemoryAllocator =
        skgpu::VulkanMemoryAllocators::Make(*fBackendContext, skgpu::ThreadSafe::kNo);
    if (!fBackendContext->fMemoryAllocator) {
        std::fprintf(stderr, "openshow-worker: Skia Vulkan memory allocator failed\n");
        return false;
    }
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
    fUploadQueue = VK_NULL_HANDLE;
    fPhysical.reset();
    if (fSurface != VK_NULL_HANDLE && fInstance != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(fInstance, fSurface, nullptr);
        fSurface = VK_NULL_HANDLE;
    }
    if (fMessenger != VK_NULL_HANDLE && fInstance != VK_NULL_HANDLE) {
        vkb::destroy_debug_utils_messenger(fInstance, fMessenger);
        fMessenger = VK_NULL_HANDLE;
    }
    if (fInstance != VK_NULL_HANDLE) {
        vkDestroyInstance(fInstance, nullptr);
        fInstance = VK_NULL_HANDLE;
    }
    fPhysicalDevice = VK_NULL_HANDLE;
    fValidation = false;
}

}  // namespace openshow::worker
