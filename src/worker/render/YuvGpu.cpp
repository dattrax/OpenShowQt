#include "render/YuvGpu.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <utility>

#include "include/core/SkColorSpace.h"
#include "include/core/SkImage.h"
#include "include/core/SkYUVAInfo.h"
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/GrYUVABackendTextures.h"
#include "include/gpu/ganesh/SkImageGanesh.h"
#include "include/gpu/ganesh/vk/GrVkBackendSurface.h"
#include "include/gpu/ganesh/vk/GrVkTypes.h"

namespace openshow::worker {

namespace {

int64_t steadyNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

uint64_t alignUp(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

bool findMemoryType(VkPhysicalDevice physical, uint32_t typeBits, VkMemoryPropertyFlags wanted,
                    uint32_t* out) {
    VkPhysicalDeviceMemoryProperties properties = {};
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (properties.memoryTypes[i].propertyFlags & wanted) == wanted) {
            *out = i;
            return true;
        }
    }
    return false;
}

VkResult createBuffer(VkDevice device, VkPhysicalDevice physical, VkDeviceSize size,
                      VkBufferUsageFlags usage, VkMemoryPropertyFlags memoryProperties,
                      VkBuffer* buffer, VkDeviceMemory* memory) {
    VkBufferCreateInfo bufferInfo = {};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;  // host writes + one queue read
    if (vkCreateBuffer(device, &bufferInfo, nullptr, buffer) != VK_SUCCESS) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkMemoryRequirements requirements = {};
    vkGetBufferMemoryRequirements(device, *buffer, &requirements);
    uint32_t memoryType = 0;
    if (!findMemoryType(physical, requirements.memoryTypeBits, memoryProperties, &memoryType)) {
        vkDestroyBuffer(device, *buffer, nullptr);
        *buffer = VK_NULL_HANDLE;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkMemoryAllocateInfo allocateInfo = {};
    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = memoryType;
    if (vkAllocateMemory(device, &allocateInfo, nullptr, memory) != VK_SUCCESS) {
        vkDestroyBuffer(device, *buffer, nullptr);
        *buffer = VK_NULL_HANDLE;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (vkBindBufferMemory(device, *buffer, *memory, 0) != VK_SUCCESS) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    return VK_SUCCESS;
}

}  // namespace

struct GpuYuvUploader::Slot {
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    VkDeviceSize stagingSize = 0;
    VkDeviceSize uvOffset = 0;

    VkImage yImage = VK_NULL_HANDLE;
    VkDeviceMemory yMemory = VK_NULL_HANDLE;
    VkImage uvImage = VK_NULL_HANDLE;
    VkDeviceMemory uvMemory = VK_NULL_HANDLE;
    bool onDevice = false;        // images exist
    bool firstTransfer = true;    // next transfer starts from UNDEFINED layout
    sk_sp<SkImage> wrapped;       // one wrap per slot, reused forever after

    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    bool inUse = false;           // consumer contract
    uint64_t sequence = 0;
};

struct GpuYuvUploader::Sync {
    std::mutex mutex;
    std::condition_variable available;
};

bool GpuYuvUploader::create(VkDevice device, VkPhysicalDevice physicalDevice,
                            uint32_t graphicsQueueIndex, uint32_t uploadQueueIndex,
                            VkQueue uploadQueue, GrDirectContext* context, int width, int height,
                            SkYUVColorSpace yuvColorSpace, std::string* error) {
    if (fSlots) {
        if (error) {
            *error = "uploader already created";
        }
        return false;
    }
    if (width <= 0 || height <= 0 || (width & 1) || (height & 1) || context == nullptr ||
        device == VK_NULL_HANDLE || uploadQueue == VK_NULL_HANDLE) {
        if (error) {
            *error = "invalid uploader parameters";
        }
        return false;
    }
    fDevice = device;
    fPhysicalDevice = physicalDevice;
    fGraphicsQueueIndex = graphicsQueueIndex;
    fUploadQueueIndex = uploadQueueIndex;
    fUploadQueue = uploadQueue;
    fContext = context;
    fWidth = width;
    fHeight = height;
    fYuvColorSpace = yuvColorSpace;
    fSlots = std::make_unique<Slot[]>(kSlots);
    fNextSlot = 0;
    fStats = Stats{};
    fSync = std::make_unique<Sync>();
    return true;
}

bool GpuYuvUploader::createSlotResources(Slot& slot, std::string* error) {
    const uint64_t yBytes = static_cast<uint64_t>(fWidth) * static_cast<uint64_t>(fHeight);
    const uint64_t uvBytes = yBytes / 2;  // R8G8 over (w/2)×(h/2)
    slot.stagingSize = alignUp(yBytes, 256) + alignUp(uvBytes, 256);
    slot.uvOffset = alignUp(yBytes, 256);

    if (createBuffer(fDevice, fPhysicalDevice, slot.stagingSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     &slot.staging, &slot.stagingMemory) != VK_SUCCESS) {
        if (error) {
            *error = "could not create staging buffer";
        }
        return false;
    }

    // Both planes: device-local, optimal tiling, copy destination now and
    // sampled later. CONCURRENT over {graphics, upload} so the two queues may
    // touch the same image without ownership transfers (VkQueue submissions
    // are ordered by the host through the fence below).
    uint32_t familyIndices[2] = {fGraphicsQueueIndex, fUploadQueueIndex};
    const uint32_t familyCount = fGraphicsQueueIndex == fUploadQueueIndex ? 1u : 2u;

    auto createPlane = [&](VkFormat format, int planeWidth, int planeHeight, VkImage* image,
                           VkDeviceMemory* memory) {
        VkImageCreateInfo imageInfo = {};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format;
        imageInfo.extent = {static_cast<uint32_t>(planeWidth), static_cast<uint32_t>(planeHeight),
                            1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        // TRANSFER_SRC as well as DST: Skia's check_image_info demands both
        // bits on every wrapped texture (GrVkGpu.cpp), even though the
        // uploader only ever copies *into* these images.
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                          VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_CONCURRENT;
        imageInfo.queueFamilyIndexCount = familyCount;
        imageInfo.pQueueFamilyIndices = familyIndices;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(fDevice, &imageInfo, nullptr, image) != VK_SUCCESS) {
            return false;
        }
        VkMemoryRequirements requirements = {};
        vkGetImageMemoryRequirements(fDevice, *image, &requirements);
        uint32_t memoryType = 0;
        if (!findMemoryType(fPhysicalDevice, requirements.memoryTypeBits,
                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memoryType)) {
            vkDestroyImage(fDevice, *image, nullptr);
            *image = VK_NULL_HANDLE;
            return false;
        }
        VkMemoryAllocateInfo allocateInfo = {};
        allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocateInfo.allocationSize = requirements.size;
        allocateInfo.memoryTypeIndex = memoryType;
        if (vkAllocateMemory(fDevice, &allocateInfo, nullptr, memory) != VK_SUCCESS) {
            vkDestroyImage(fDevice, *image, nullptr);
            *image = VK_NULL_HANDLE;
            return false;
        }
        if (vkBindImageMemory(fDevice, *image, *memory, 0) != VK_SUCCESS) {
            return false;
        }
        return true;
    };

    if (!createPlane(VK_FORMAT_R8_UNORM, fWidth, fHeight, &slot.yImage, &slot.yMemory) ||
        !createPlane(VK_FORMAT_R8G8_UNORM, fWidth / 2, fHeight / 2, &slot.uvImage,
                     &slot.uvMemory)) {
        if (error) {
            *error = "could not create device-local YUV images";
        }
        return false;
    }

    VkCommandPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = fUploadQueueIndex;
    if (vkCreateCommandPool(fDevice, &poolInfo, nullptr, &slot.commandPool) != VK_SUCCESS) {
        if (error) {
            *error = "could not create upload command pool";
        }
        return false;
    }
    VkFenceCreateInfo fenceInfo = {};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(fDevice, &fenceInfo, nullptr, &slot.fence) != VK_SUCCESS) {
        if (error) {
            *error = "could not create upload fence";
        }
        return false;
    }
    slot.onDevice = true;
    ++fStats.slotsCreated;
    return true;
}

bool GpuYuvUploader::waitForFreeSlot(Slot& slot) {
    std::unique_lock lock(fSync->mutex);
    while (slot.inUse) {
        ++fStats.waitsForFreeSlot;
        // 1.5e replaces the polling wait with the release carrying the flush
        // semaphore (the GPU waits, not this thread).
        fSync->available.wait_for(lock, std::chrono::milliseconds(1));
    }
    return true;
}

bool GpuYuvUploader::upload(const uint8_t* yPlane, int yRowBytes, const uint8_t* uvPlane,
                            int uvRowBytes, uint64_t sequence, double ptsSeconds, Frame* out) {
    if (!fSlots || yPlane == nullptr || uvPlane == nullptr || yRowBytes < fWidth ||
        uvRowBytes < fWidth /* w/2 pairs of bytes */) {
        return false;
    }

    // Round-robin; block only when every slot is still held downstream.
    Slot* chosen = nullptr;
    {
        std::unique_lock lock(fSync->mutex);
        for (int attempt = 0; attempt < kSlots; ++attempt) {
            Slot& candidate = fSlots[fNextSlot];
            if (!candidate.inUse) {
                chosen = &candidate;
                break;
            }
            ++fStats.waitsForFreeSlot;
            fSync->available.wait_for(lock, std::chrono::milliseconds(1));
            fNextSlot = (fNextSlot + 1) % kSlots;
        }
    }
    if (chosen == nullptr) {
        return false;  // all slots stuck in use; caller decides
    }

    std::string error;
    if (!chosen->onDevice && !createSlotResources(*chosen, &error)) {
        std::fprintf(stderr, "openshow-worker: YUV slot creation failed: %s\n", error.c_str());
        return false;
    }

    // Stage: rows copied verbatim so the copy command can reuse the source
    // strides (Vulkan bufferRowLength accepts any >= extent width).
    const int64_t stageStart = steadyNowNs();
    {
        void* mapped = nullptr;
        if (vkMapMemory(fDevice, chosen->stagingMemory, 0, chosen->stagingSize, 0, &mapped) !=
            VK_SUCCESS) {
            return false;
        }
        auto* destination = static_cast<uint8_t*>(mapped);
        for (int row = 0; row < fHeight; ++row) {
            std::memcpy(destination + static_cast<size_t>(row) * yRowBytes,
                        yPlane + static_cast<size_t>(row) * yRowBytes, static_cast<size_t>(yRowBytes));
        }
        uint8_t* uvDestination = destination + chosen->uvOffset;
        for (int row = 0; row < fHeight / 2; ++row) {
            std::memcpy(uvDestination + static_cast<size_t>(row) * uvRowBytes,
                        uvPlane + static_cast<size_t>(row) * uvRowBytes,
                        static_cast<size_t>(uvRowBytes));
        }
        vkUnmapMemory(fDevice, chosen->stagingMemory);
    }
    fStats.totalStageMs += static_cast<double>(steadyNowNs() - stageStart) / 1e6;

    // Record: layout in, both plane copies, transition to shader-read.
    if (vkResetCommandPool(fDevice, chosen->commandPool, 0) != VK_SUCCESS) {
        return false;
    }
    VkCommandBuffer command = VK_NULL_HANDLE;
    {
        VkCommandBufferAllocateInfo allocateInfo = {};
        allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocateInfo.commandPool = chosen->commandPool;
        allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(fDevice, &allocateInfo, &command) != VK_SUCCESS) {
            return false;
        }
    }
    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(command, &beginInfo) != VK_SUCCESS) {
        return false;
    }

    // CONCURRENT images never transfer ownership: qfi fields stay IGNORED.
    auto barrier = [&](VkImage image, VkImageLayout from, VkImageLayout to,
                       VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
                       VkPipelineStageFlags dstStage, VkAccessFlags dstAccess) {
        VkImageMemoryBarrier memoryBarrier = {};
        memoryBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        memoryBarrier.srcAccessMask = srcAccess;
        memoryBarrier.dstAccessMask = dstAccess;
        memoryBarrier.oldLayout = from;
        memoryBarrier.newLayout = to;
        memoryBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        memoryBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        memoryBarrier.image = image;
        memoryBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(command, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1,
                             &memoryBarrier);
    };

    // First use starts from UNDEFINED (contents discarded anyway).
    const VkImageLayout before = chosen->firstTransfer ? VK_IMAGE_LAYOUT_UNDEFINED
                                                       : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    for (VkImage image : {chosen->yImage, chosen->uvImage}) {
        barrier(image, before, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                chosen->firstTransfer ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                      : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                chosen->firstTransfer ? 0 : VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    }

    // Vulkan measures buffer rows in texels, not bytes: the R8 plane's row
    // stride in texels equals its stride in bytes, the R8G8 UV plane's is
    // half of it. (Getting this wrong copies the UV plane at double row
    // stride and the chroma bands land one band off — seen as green garbage.)
    auto copyPlane = [&](VkImage image, VkDeviceSize bufferOffset, int planeWidth,
                         int planeHeight, int rowLengthInTexels) {
        VkBufferImageCopy region = {};
        region.bufferOffset = bufferOffset;
        region.bufferRowLength = static_cast<uint32_t>(rowLengthInTexels);
        region.bufferImageHeight = 0;  // contiguous rows of planes
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {static_cast<uint32_t>(planeWidth),
                              static_cast<uint32_t>(planeHeight), 1};
        vkCmdCopyBufferToImage(command, chosen->staging, image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    };
    copyPlane(chosen->yImage, 0, fWidth, fHeight, yRowBytes);
    copyPlane(chosen->uvImage, chosen->uvOffset, fWidth / 2, fHeight / 2, uvRowBytes / 2);

    for (VkImage image : {chosen->yImage, chosen->uvImage}) {
        barrier(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT);
    }
    if (vkEndCommandBuffer(command) != VK_SUCCESS) {
        return false;
    }

    const int64_t gpuStart = steadyNowNs();
    if (vkResetFences(fDevice, 1, &chosen->fence) != VK_SUCCESS) {
        return false;
    }
    {
        VkSubmitInfo submitInfo = {};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &command;
        if (vkQueueSubmit(fUploadQueue, 1, &submitInfo, chosen->fence) != VK_SUCCESS) {
            return false;
        }
    }
    if (vkWaitForFences(fDevice, 1, &chosen->fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        return false;
    }
    fStats.totalGpuMs += static_cast<double>(steadyNowNs() - gpuStart) / 1e6;
    chosen->firstTransfer = false;

    // One wrap per slot: the layout never leaves shader-read afterwards, so
    // the same SkImage serves every later frame of this slot.
    if (!chosen->wrapped) {
        auto makeTexture = [&](VkImage image, VkFormat format, int planeWidth, int planeHeight) {
            GrVkImageInfo imageInfo;
            imageInfo.fImage = image;
            imageInfo.fImageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imageInfo.fFormat = format;
            imageInfo.fImageUsageFlags = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                         VK_IMAGE_USAGE_SAMPLED_BIT;
            imageInfo.fSampleCount = VK_SAMPLE_COUNT_1_BIT;
            imageInfo.fLevelCount = 1;
            imageInfo.fCurrentQueueFamily = VK_QUEUE_FAMILY_IGNORED;
            imageInfo.fProtected = skgpu::Protected::kNo;
            imageInfo.fSharingMode = VK_SHARING_MODE_CONCURRENT;
            return GrBackendTextures::MakeVk(planeWidth, planeHeight, imageInfo);
        };
        GrBackendTexture planeTextures[SkYUVAInfo::kMaxPlanes] = {
            makeTexture(chosen->yImage, VK_FORMAT_R8_UNORM, fWidth, fHeight),
            makeTexture(chosen->uvImage, VK_FORMAT_R8G8_UNORM, fWidth / 2, fHeight / 2),
            GrBackendTexture(),
            GrBackendTexture(),
        };
        const SkYUVAInfo yuvaInfo(SkISize{fWidth, fHeight}, SkYUVAInfo::PlaneConfig::kY_UV,
                                  SkYUVAInfo::Subsampling::k420, fYuvColorSpace);
        const GrYUVABackendTextures yuvaTextures(yuvaInfo, planeTextures, kTopLeft_GrSurfaceOrigin);
        chosen->wrapped = SkImages::TextureFromYUVATextures(fContext, yuvaTextures);
        if (!chosen->wrapped) {
            std::fprintf(stderr, "openshow-worker: TextureFromYUVATextures failed\n");
            return false;
        }
    }

    {
        std::lock_guard lock(fSync->mutex);
        chosen->inUse = true;
        chosen->sequence = sequence;
        fNextSlot = (static_cast<int>(chosen - fSlots.get()) + 1) % kSlots;
    }

    if (out != nullptr) {
        out->image = chosen->wrapped;
        out->sequence = sequence;
        out->ptsSeconds = ptsSeconds;
        out->slot = static_cast<unsigned>(chosen - fSlots.get());
    }
    ++fStats.uploads;
    return true;
}

void GpuYuvUploader::release(unsigned slot) {
    if (!fSlots || slot >= kSlots) {
        return;
    }
    {
        std::lock_guard lock(fSync->mutex);
        fSlots[slot].inUse = false;
    }
    fSync->available.notify_all();
}

void GpuYuvUploader::releaseAll() {
    if (!fSlots) {
        return;
    }
    {
        std::lock_guard lock(fSync->mutex);
        for (int i = 0; i < kSlots; ++i) {
            fSlots[i].inUse = false;
        }
    }
    fSync->available.notify_all();
}

void GpuYuvUploader::destroy() {
    if (!fSlots) {
        return;
    }
    // Drop the Skia wrappers first (the GrDirectContext is still alive by
    // contract), then let the GPU finish before releasing the VkImages.
    for (int i = 0; i < kSlots; ++i) {
        fSlots[i].wrapped.reset();
    }
    if (fDevice != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(fDevice);
    }
    for (int i = 0; i < kSlots; ++i) {
        Slot& slot = fSlots[i];
        if (!slot.onDevice) {
            continue;
        }
        if (slot.fence != VK_NULL_HANDLE) {
            vkDestroyFence(fDevice, slot.fence, nullptr);
        }
        if (slot.commandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(fDevice, slot.commandPool, nullptr);
        }
        if (slot.yImage != VK_NULL_HANDLE) {
            vkDestroyImage(fDevice, slot.yImage, nullptr);
            vkFreeMemory(fDevice, slot.yMemory, nullptr);
        }
        if (slot.uvImage != VK_NULL_HANDLE) {
            vkDestroyImage(fDevice, slot.uvImage, nullptr);
            vkFreeMemory(fDevice, slot.uvMemory, nullptr);
        }
        if (slot.staging != VK_NULL_HANDLE) {
            vkDestroyBuffer(fDevice, slot.staging, nullptr);
            vkFreeMemory(fDevice, slot.stagingMemory, nullptr);
        }
        slot = Slot{};
    }
    fSlots.reset();
    fSync.reset();
    fContext = nullptr;
    fDevice = VK_NULL_HANDLE;
    fPhysicalDevice = VK_NULL_HANDLE;
    fUploadQueue = VK_NULL_HANDLE;
}

GpuYuvUploader::GpuYuvUploader() = default;
GpuYuvUploader::~GpuYuvUploader() {
    destroy();
}

}  // namespace openshow::worker
