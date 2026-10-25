/*
 * The adapter implementation (task 6.2a): upstream VulkanMemoryAllocator
 * compiled static into this one translation unit behind the public
 * skgpu::VulkanMemoryAllocator contract. See the header for why VMA lives
 * here and nowhere else in the program.
 */

#include "output/VulkanMemoryAdapter.h"

#include <cstring>

// Vulkan first, then the allocator: vk_mem_alloc.h honours an already
// included vulkan.h rather than pulling in its own, so the whole binary
// shares one set of handle typedefs.
#include <vulkan/vulkan.h>

// No compiled-Vulkan-function linking and no dynamic loading: every entry
// point VMA may call is resolved explicitly through the Skia backend
// context's getProc, the same view of the device Skia itself uses.
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
// Silence the nullability annotations upstream applies to the public API
// (they are documentation here, and upstream uses them inconsistently).
#define VMA_NULLABLE
#define VMA_NOT_NULL
// The implementation, internalised: VMA's C API is normally exported with
// external linkage, but libskia.a carries its own compiled copy of VMA
// behind Skia's private factory. static guarantees the two never bind to
// each other across incompatible headers (see the header comment).
#define VMA_IMPLEMENTATION
#define VMA_CALL_PRE static __attribute__((unused))
#include <vk_mem_alloc.h>

#include "include/gpu/vk/VulkanBackendContext.h"

namespace openshow::worker {

namespace {

// Resolve entry points through the backend context's getProc — with the
// SAME scope discipline Skia's own VulkanInterface used, which this
// platform's loader demands: physical-device functions come from
// vkGetInstanceProcAddr, but device functions must come from
// vkGetDeviceProcAddr on the real device. Instance-resolving a device
// function can hand back a raw ICD entry point that mis-dispatches when
// called with the loader's VkDevice handle (seen 2026-10-25 on RADV: the
// ICD's GetImageMemoryRequirements2KHR jumped through a null dispatch
// entry). KHR-suffixed slots take the Vulkan 1.1 core names, which is
// what the allocator calls for vulkanApiVersion 1.1 anyway.
template <typename T>
void resolveInstance(const skgpu::VulkanBackendContext& ctx, T* slot, const char* name) {
    *slot = reinterpret_cast<T>(ctx.fGetProc(name, ctx.fInstance, VK_NULL_HANDLE));
}

template <typename T>
void resolveDevice(const skgpu::VulkanBackendContext& ctx, T* slot, const char* coreName,
                   const char* khrName = nullptr) {
    *slot = reinterpret_cast<T>(ctx.fGetProc(coreName, VK_NULL_HANDLE, ctx.fDevice));
    if (*slot == nullptr && khrName != nullptr) {
        *slot = reinterpret_cast<T>(ctx.fGetProc(khrName, VK_NULL_HANDLE, ctx.fDevice));
    }
}

}  // namespace

sk_sp<VulkanMemoryAdapter> VulkanMemoryAdapter::Create(
    const skgpu::VulkanBackendContext& backendContext) {
    if (backendContext.fInstance == VK_NULL_HANDLE ||
        backendContext.fPhysicalDevice == VK_NULL_HANDLE ||
        backendContext.fDevice == VK_NULL_HANDLE || !backendContext.fGetProc) {
        return nullptr;
    }

    // The function set VMA needs for Vulkan 1.1. Belt and braces: zero
    // first so nothing uninitialised can be called; the two proc-address
    // getters stay null on purpose — with every other slot filled VMA
    // never needs to fetch anything itself.
    VmaVulkanFunctions functions;
    std::memset(&functions, 0, sizeof(functions));
    resolveInstance(backendContext, &functions.vkGetPhysicalDeviceProperties,
                    "vkGetPhysicalDeviceProperties");
    resolveInstance(backendContext, &functions.vkGetPhysicalDeviceMemoryProperties,
                    "vkGetPhysicalDeviceMemoryProperties");
    resolveInstance(backendContext, &functions.vkGetPhysicalDeviceMemoryProperties2KHR,
                    "vkGetPhysicalDeviceMemoryProperties2");
    resolveDevice(backendContext, &functions.vkAllocateMemory, "vkAllocateMemory");
    resolveDevice(backendContext, &functions.vkFreeMemory, "vkFreeMemory");
    resolveDevice(backendContext, &functions.vkMapMemory, "vkMapMemory");
    resolveDevice(backendContext, &functions.vkUnmapMemory, "vkUnmapMemory");
    resolveDevice(backendContext, &functions.vkFlushMappedMemoryRanges,
                  "vkFlushMappedMemoryRanges");
    resolveDevice(backendContext, &functions.vkInvalidateMappedMemoryRanges,
                  "vkInvalidateMappedMemoryRanges");
    resolveDevice(backendContext, &functions.vkBindBufferMemory, "vkBindBufferMemory");
    resolveDevice(backendContext, &functions.vkBindImageMemory, "vkBindImageMemory");
    resolveDevice(backendContext, &functions.vkGetBufferMemoryRequirements,
                  "vkGetBufferMemoryRequirements");
    resolveDevice(backendContext, &functions.vkGetImageMemoryRequirements,
                  "vkGetImageMemoryRequirements");
    resolveDevice(backendContext, &functions.vkCreateBuffer, "vkCreateBuffer");
    resolveDevice(backendContext, &functions.vkDestroyBuffer, "vkDestroyBuffer");
    resolveDevice(backendContext, &functions.vkCreateImage, "vkCreateImage");
    resolveDevice(backendContext, &functions.vkDestroyImage, "vkDestroyImage");
    resolveDevice(backendContext, &functions.vkCmdCopyBuffer, "vkCmdCopyBuffer");
    // The Vulkan 1.1 core names feed VMA's KHR-suffixed slots — at
    // vulkanApiVersion 1.1 these are the entry points VMA calls, and
    // asking the device for them is exactly what a 1.1 loader answers.
    resolveDevice(backendContext, &functions.vkGetBufferMemoryRequirements2KHR,
                  "vkGetBufferMemoryRequirements2");
    resolveDevice(backendContext, &functions.vkGetImageMemoryRequirements2KHR,
                  "vkGetImageMemoryRequirements2");
    resolveDevice(backendContext, &functions.vkBindBufferMemory2KHR, "vkBindBufferMemory2");
    resolveDevice(backendContext, &functions.vkBindImageMemory2KHR, "vkBindImageMemory2");

    VmaAllocatorCreateInfo info = {};
    // Ganesh calls the allocator under its context lock — this mirrors the
    // ThreadSafe::kNo the worker always passed when it asked Skia to make
    // the allocator for it. Dedicated allocation needs no flag: at
    // vulkanApiVersion 1.1 it is core, and VMA takes the
    // Get*MemoryRequirements2 path on that version alone.
    info.flags = VMA_ALLOCATOR_CREATE_EXTERNALLY_SYNCHRONIZED_BIT;
    info.physicalDevice = backendContext.fPhysicalDevice;
    info.device = backendContext.fDevice;
    info.instance = backendContext.fInstance;
    // 4MB blocks, the value upstream picked from profiling app/GPU
    // workloads and what Skia's own factory used on this path.
    info.preferredLargeHeapBlockSize = 4 * 1024 * 1024;
    info.pAllocationCallbacks = nullptr;
    info.pDeviceMemoryCallbacks = nullptr;
    info.pHeapSizeLimit = nullptr;
    info.pVulkanFunctions = &functions;
    // Clamped to 1.1 exactly like Skia's factory: the function set filled
    // above is everything VMA uses at that level, and it keeps the adapter
    // working against 1.1 devices (llvmpipe) the same tests exercise.
    info.vulkanApiVersion = VK_API_VERSION_1_1;
    info.pTypeExternalMemoryHandleTypes = nullptr;

    VmaAllocator vma = nullptr;
    if (vmaCreateAllocator(&info, &vma) != VK_SUCCESS) {
        return nullptr;
    }
    return sk_sp<VulkanMemoryAdapter>(new VulkanMemoryAdapter(vma));
}

VulkanMemoryAdapter::~VulkanMemoryAdapter() {
    if (fVma != nullptr) {
        vmaDestroyAllocator(fVma);
        fVma = nullptr;
    }
}

VkResult VulkanMemoryAdapter::allocateImageMemory(VkImage image, uint32_t allocationPropertyFlags,
                                                  skgpu::VulkanBackendMemory* memory) {
    VmaAllocationCreateInfo info = {};
    // Images are device-local; the contract's flags refine from there.
    info.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    if (allocationPropertyFlags & kDedicatedAllocation_AllocationPropertyFlag) {
        info.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    }
    if (allocationPropertyFlags & kLazyAllocation_AllocationPropertyFlag) {
        info.requiredFlags |= VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;
    }
    if (allocationPropertyFlags & kProtected_AllocationPropertyFlag) {
        info.requiredFlags |= VK_MEMORY_PROPERTY_PROTECTED_BIT;
    }

    VmaAllocation allocation = nullptr;
    const VkResult result = vmaAllocateMemoryForImage(fVma, image, &info, &allocation, nullptr);
    if (result == VK_SUCCESS) {
        *memory = reinterpret_cast<skgpu::VulkanBackendMemory>(allocation);
    }
    return result;
}

VkResult VulkanMemoryAdapter::allocateBufferMemory(VkBuffer buffer, BufferUsage usage,
                                                   uint32_t allocationPropertyFlags,
                                                   skgpu::VulkanBackendMemory* memory) {
    VmaAllocationCreateInfo info = {};
    // The contract's four usages, mapped to the memory properties they
    // describe (and matching the properties the old Skia factory picked):
    switch (usage) {
        case BufferUsage::kGpuOnly:
            info.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            break;
        case BufferUsage::kCpuWritesGpuReads:
            // Sequential host writes the GPU reads: coherent memory is the
            // point (flushing would serialise the writer), device-local if
            // one exists.
            info.requiredFlags =
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            info.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            break;
        case BufferUsage::kTransfersFromCpuToGpu:
            info.requiredFlags =
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            break;
        case BufferUsage::kTransfersFromGpuToCpu:
            // Host reads of device writes: visible, and cached so the read
            // does not walk to VRAM per byte (invalidation covers staleness).
            info.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
            info.preferredFlags = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
            break;
    }
    if (allocationPropertyFlags & kDedicatedAllocation_AllocationPropertyFlag) {
        info.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    }
    if ((allocationPropertyFlags & kLazyAllocation_AllocationPropertyFlag) &&
        usage == BufferUsage::kGpuOnly) {
        info.preferredFlags |= VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;
    }
    if (allocationPropertyFlags & kPersistentlyMapped_AllocationPropertyFlag) {
        // The contract only allows this for host-visible usages.
        info.flags |= VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }
    if (allocationPropertyFlags & kProtected_AllocationPropertyFlag) {
        info.requiredFlags |= VK_MEMORY_PROPERTY_PROTECTED_BIT;
    }

    VmaAllocation allocation = nullptr;
    const VkResult result = vmaAllocateMemoryForBuffer(fVma, buffer, &info, &allocation, nullptr);
    if (result == VK_SUCCESS) {
        *memory = reinterpret_cast<skgpu::VulkanBackendMemory>(allocation);
    }
    return result;
}

void VulkanMemoryAdapter::getAllocInfo(const skgpu::VulkanBackendMemory& memory,
                                       skgpu::VulkanAlloc* alloc) const {
    const VmaAllocation allocation = reinterpret_cast<VmaAllocation>(memory);
    VmaAllocationInfo vmaInfo = {};
    vmaGetAllocationInfo(fVma, allocation, &vmaInfo);

    VkMemoryPropertyFlags memFlags = 0;
    vmaGetMemoryTypeProperties(fVma, vmaInfo.memoryType, &memFlags);

    uint32_t flags = 0;
    if (memFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        flags |= skgpu::VulkanAlloc::kMappable_Flag;
    }
    if (!(memFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        flags |= skgpu::VulkanAlloc::kNoncoherent_Flag;
    }
    if (memFlags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) {
        flags |= skgpu::VulkanAlloc::kLazilyAllocated_Flag;
    }

    alloc->fMemory = vmaInfo.deviceMemory;
    alloc->fOffset = vmaInfo.offset;
    alloc->fSize = vmaInfo.size;
    alloc->fFlags = flags;
    alloc->fBackendMemory = memory;
}

VkResult VulkanMemoryAdapter::mapMemory(const skgpu::VulkanBackendMemory& memory, void** data) {
    return vmaMapMemory(fVma, reinterpret_cast<VmaAllocation>(memory), data);
}

void VulkanMemoryAdapter::unmapMemory(const skgpu::VulkanBackendMemory& memory) {
    vmaUnmapMemory(fVma, reinterpret_cast<VmaAllocation>(memory));
}

VkResult VulkanMemoryAdapter::flushMemory(const skgpu::VulkanBackendMemory& memory,
                                          VkDeviceSize offset, VkDeviceSize size) {
    return vmaFlushAllocation(fVma, reinterpret_cast<VmaAllocation>(memory), offset, size);
}

VkResult VulkanMemoryAdapter::invalidateMemory(const skgpu::VulkanBackendMemory& memory,
                                               VkDeviceSize offset, VkDeviceSize size) {
    return vmaInvalidateAllocation(fVma, reinterpret_cast<VmaAllocation>(memory), offset, size);
}

void VulkanMemoryAdapter::freeMemory(const skgpu::VulkanBackendMemory& memory) {
    vmaFreeMemory(fVma, reinterpret_cast<VmaAllocation>(memory));
}

std::pair<uint64_t, uint64_t> VulkanMemoryAdapter::totalAllocatedAndUsedMemory() const {
    VmaTotalStatistics stats = {};
    vmaCalculateStatistics(fVma, &stats);
    return {stats.total.statistics.blockBytes, stats.total.statistics.allocationBytes};
}

}  // namespace openshow::worker
