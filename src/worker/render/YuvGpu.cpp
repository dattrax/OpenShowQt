#include "render/YuvGpu.h"
#include "output/VulkanQueueGuard.h"

#include <chrono>
#include <climits>
#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <utility>

#include "include/core/SkColorSpace.h"
#include "include/core/SkColorType.h"
#include "include/core/SkImage.h"
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/SkImageGanesh.h"
#include "include/gpu/ganesh/vk/GrVkBackendSurface.h"
#include "include/gpu/ganesh/vk/GrVkTypes.h"
#include "include/gpu/ganesh/vk/GrVkBackendSemaphore.h"

namespace openshow::worker {

namespace {

int64_t steadyNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool findMemoryType(VkPhysicalDevice physical, uint32_t typeBits, VkMemoryPropertyFlags wanted,
                    uint32_t* out) {
    VkPhysicalDeviceMemoryProperties props = {};
    vkGetPhysicalDeviceMemoryProperties(physical, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & wanted) == wanted) {
            *out = i;
            return true;
        }
    }
    return false;
}

}  // namespace

struct GpuYuvUploader::Sync {
    std::mutex mutex;
    std::condition_variable available;
    bool shutdown = false;
};

struct GpuYuvUploader::Slot {
    // The write side: host-visible LINEAR image, mapped for the slot's
    // life; plane[] carries the driver's offsets and rowPitches.
    VkImage linear = VK_NULL_HANDLE;
    VkDeviceMemory linearMemory = VK_NULL_HANDLE;
    VkDeviceSize linearSize = 0;
    void* linearMap = nullptr;
    VkSubresourceLayout plane[3] = {};
    // The sample side: device-local OPTIMAL twin of the same format.
    VkImage optimal = VK_NULL_HANDLE;
    VkDeviceMemory optimalMemory = VK_NULL_HANDLE;
    VkDeviceSize optimalSize = 0;
    bool onDevice = false;
    bool firstTransfer = true;  // layouts not yet pinned by a barrier

    // Consumer-facing state (guarded by Sync::mutex).
    bool inUse = false;
    uint64_t sequence = 0;
    std::vector<VkSemaphore> pendingDone;

    // Recording side (producer thread only).
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore ready = VK_NULL_HANDLE;
    bool readyPending = false; // producer/consumer handoff, protected by slot ownership

    // Draw-thread only.
    sk_sp<SkImage> wrapped;
    bool wrapFailed = false;
};

struct GpuYuvUploader::DecodeImage final : openshow::media::FrameStorage {
    const GpuYuvUploader* owner = nullptr;
    VkDevice device = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    bool firstTransfer = true;
    ~DecodeImage() override {
        if (base) vkUnmapMemory(device, memory);
        if (image) vkDestroyImage(device, image, nullptr);
        if (memory) vkFreeMemory(device, memory, nullptr);
    }
};

GpuYuvUploader::GpuYuvUploader() = default;

GpuYuvUploader::~GpuYuvUploader() { destroy(); }

bool GpuYuvUploader::create(VkDevice device, VkPhysicalDevice physicalDevice,
                            uint32_t graphicsQueueIndex, uint32_t uploadQueueIndex,
                            VkQueue uploadQueue, GrDirectContext* context, int width, int height,
                            openshow::media::FrameFormat format, SkYUVColorSpace yuvColorSpace,
                            std::string* error) {
    auto fail = [&](const std::string& message) {
        if (error) {
            *error = message;
        }
        return false;
    };
    if (fSlots) {
        return fail("GpuYuvUploader already created");
    }
    if (device == VK_NULL_HANDLE || context == nullptr || uploadQueue == VK_NULL_HANDLE ||
        width <= 0 || height <= 0 || (width & 1) || (height & 1)) {
        return fail("GpuYuvUploader needs a device, context, queue and even positive size");
    }

    fFormat = format;
    fVulkanFormat = format == openshow::media::FrameFormat::kYUV420P
                        ? VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM
                        : format == openshow::media::FrameFormat::kP010
                        ? VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16
                        : VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    fSkColorType = format == openshow::media::FrameFormat::kP010 ? kRGBA_1010102_SkColorType
                                                                 : kRGB_888x_SkColorType;

    // The wrap in imageForSlot() needs the device to sample the native YUV
    // format optimally, and the conversion's chroma siting and filter must
    // be ones this format actually reports — query once and build the
    // persistent ycbcr info from the answer rather than letting a wrap
    // fail per slot later. (Feature bits live on the format properties;
    // the image query below only proves the exact usage is creatable.)
    VkFormatProperties formatFeatures = {};
    vkGetPhysicalDeviceFormatProperties(physicalDevice, fVulkanFormat, &formatFeatures);
    VkImageFormatProperties creatable = {};
    const VkResult optimalQuery = vkGetPhysicalDeviceImageFormatProperties(
        physicalDevice, fVulkanFormat, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT,
        0, &creatable);
    const VkFormatFeatureFlags features =
        optimalQuery == VK_SUCCESS ? formatFeatures.optimalTilingFeatures : 0;
    if (!(features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
        return fail("device cannot sample the native YUV format the stream needs");
    }
    // Midpoint siting matches how the old raw-plane path sampled chroma
    // (same texture coordinates as luma); cosited is the fallback only if
    // the driver reports no midpoint support at all.
    VkChromaLocation siting;
    if (features & VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT) {
        siting = VK_CHROMA_LOCATION_MIDPOINT;
    } else if (features & VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT) {
        siting = VK_CHROMA_LOCATION_COSITED_EVEN;
    } else {
        return fail("native YUV format supports neither midpoint nor cosited chroma siting");
    }
    const VkFilter chromaFilter =
        (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT)
            ? VK_FILTER_LINEAR
            : VK_FILTER_NEAREST;

    // The sampler's YUV→RGB matrix. Skia's colour-space names map onto
    // Vulkan's models directly; kJPEG_Full is the full-range Rec.601
    // mapping (this Skia revision has no Rec.601 full-range entry, see
    // main.cpp's pickYuvColorSpace), and anything else — the source offers
    // no other today — falls back to limited Rec.601.
    VkSamplerYcbcrModelConversion model = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601;
    VkSamplerYcbcrRange range = VK_SAMPLER_YCBCR_RANGE_ITU_NARROW;
    switch (yuvColorSpace) {
        case kRec709_Limited_SkYUVColorSpace:
            model = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
            range = VK_SAMPLER_YCBCR_RANGE_ITU_NARROW;
            break;
        case kRec709_Full_SkYUVColorSpace:
            model = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
            range = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
            break;
        case kJPEG_Full_SkYUVColorSpace:
            model = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601;
            range = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
            break;
        case kRec601_Limited_SkYUVColorSpace:
        default:
            break;
    }
    const VkComponentMapping identitySwizzles = {VK_COMPONENT_SWIZZLE_IDENTITY,
                                                 VK_COMPONENT_SWIZZLE_IDENTITY,
                                                 VK_COMPONENT_SWIZZLE_IDENTITY,
                                                 VK_COMPONENT_SWIZZLE_IDENTITY};
    fYcbcr = skgpu::VulkanYcbcrConversionInfo(fVulkanFormat, model, range, siting, siting,
                                              chromaFilter, VK_FALSE, identitySwizzles, features);

    // The write side must exist as a LINEAR image whose memory the host
    // can map; a device that cannot even transfer from it has no slot
    // story at all. Fail with that name rather than at first frame.
    VkImageFormatProperties linearCreatable = {};
    const VkResult linearQuery = vkGetPhysicalDeviceImageFormatProperties(
        physicalDevice, fVulkanFormat, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_LINEAR,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0, &linearCreatable);
    if (linearQuery != VK_SUCCESS ||
        !(formatFeatures.linearTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT)) {
        return fail("device cannot use the native YUV format with linear tiling");
    }

    fDevice = device;
    fPhysicalDevice = physicalDevice;
    fGraphicsQueueIndex = graphicsQueueIndex;
    fUploadQueueIndex = uploadQueueIndex;
    fUploadQueue = uploadQueue;
    fContext = context;
    fWidth = width;
    fHeight = height;
    fNextSlot = 0;
    fStats = Stats();
    fSync = std::make_unique<Sync>();
    fSlots = std::make_unique<Slot[]>(kSlots);
    return true;
}

bool GpuYuvUploader::createImage(VkImageTiling tiling, VkImageUsageFlags usage,
        VkMemoryPropertyFlags memoryProperties, int width, int height,
        VkImage* image, VkDeviceMemory* memory, VkDeviceSize* size, std::string* error) {
    const uint32_t families[2] = {fGraphicsQueueIndex, fUploadQueueIndex};
    const uint32_t familyCount = fGraphicsQueueIndex == fUploadQueueIndex ? 1u : 2u;
    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = fVulkanFormat;
    imageInfo.extent.width = static_cast<uint32_t>(width);
    imageInfo.extent.height = static_cast<uint32_t>(height);
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = tiling;
    imageInfo.initialLayout = tiling == VK_IMAGE_TILING_LINEAR
        ? VK_IMAGE_LAYOUT_PREINITIALIZED : VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.sharingMode = familyCount > 1 ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.queueFamilyIndexCount = familyCount > 1 ? familyCount : 0;
    imageInfo.pQueueFamilyIndices = families;
    if (vkCreateImage(fDevice, &imageInfo, nullptr, image) != VK_SUCCESS) {
        *error = "vkCreateImage failed";
        return false;
    }
    VkMemoryRequirements requirements = {};
    vkGetImageMemoryRequirements(fDevice, *image, &requirements);
    uint32_t memoryType = 0;
    if (!findMemoryType(fPhysicalDevice, requirements.memoryTypeBits, memoryProperties,
                        &memoryType)) {
        *error = "no compatible Vulkan memory type";
        return false;
    }
    VkMemoryAllocateInfo allocate = {};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = memoryType;
    if (vkAllocateMemory(fDevice, &allocate, nullptr, memory) != VK_SUCCESS) {
        *error = "vkAllocateMemory failed";
        return false;
    }
    if (vkBindImageMemory(fDevice, *image, *memory, 0) != VK_SUCCESS) {
        *error = "vkBindImageMemory failed";
        return false;
    }
    *size = requirements.size;
    return true;
}

bool GpuYuvUploader::createSlotResources(Slot& slot, std::string* error, bool optimalOnly) {
    if (!optimalOnly) {
        // Write side. LINEAR tiling is what makes the planes addressable from
        // the host: the driver hands back a plane offset and rowPitch per
        // aspect, and the decode side writes through that mapping (host-
        // coherent memory: no flushes). TRANSFER_SRC is all the image ever
        // does on the GPU.
        if (!createImage(VK_IMAGE_TILING_LINEAR, VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         fWidth, fHeight, &slot.linear, &slot.linearMemory, &slot.linearSize, error)) {
            return false;
        }
        if (vkMapMemory(fDevice, slot.linearMemory, 0, slot.linearSize, 0, &slot.linearMap) !=
            VK_SUCCESS) {
            slot.linearMap = nullptr;
            *error = "vkMapMemory for the linear write slot failed";
            return false;
        }
        for (int plane = 0; plane < (fFormat == openshow::media::FrameFormat::kYUV420P ? 3 : 2); ++plane) {
            VkImageSubresource sub = {};
            sub.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT << plane;
            sub.mipLevel = 0;
            sub.arrayLayer = 0;
            vkGetImageSubresourceLayout(fDevice, slot.linear, &sub, &slot.plane[plane]);
        }

    }

    // Sample side. TRANSFER_SRC sits alongside the obvious bits because
    // Skia's wrap insists every texture it is handed can take part in a
    // transfer — the same requirement the old plane textures carried.
    if (!createImage(VK_IMAGE_TILING_OPTIMAL,
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                         VK_IMAGE_USAGE_SAMPLED_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, fWidth, fHeight, &slot.optimal, &slot.optimalMemory,
                     &slot.optimalSize, error)) {
        return false;
    }

    VkCommandPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = fUploadQueueIndex;
    if (vkCreateCommandPool(fDevice, &poolInfo, nullptr, &slot.commandPool) != VK_SUCCESS) {
        *error = "vkCreateCommandPool for the upload slot failed";
        return false;
    }
    VkFenceCreateInfo fenceInfo = {};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(fDevice, &fenceInfo, nullptr, &slot.fence) != VK_SUCCESS) {
        *error = "vkCreateFence for the upload slot failed";
        return false;
    }
    VkSemaphoreCreateInfo semaphoreInfo = {};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    if (vkCreateSemaphore(fDevice, &semaphoreInfo, nullptr, &slot.ready) != VK_SUCCESS) {
        *error = "vkCreateSemaphore for the upload handoff failed";
        return false;
    }

    slot.onDevice = true;
    ++fStats.slotsCreated;
    return true;
}

bool GpuYuvUploader::slotPlanes(unsigned slot, uint8_t* data[4], int linesize[4]) {
    if (!fSlots || slot >= static_cast<unsigned>(kSlots)) {
        return false;
    }
    Slot& target = fSlots[slot];
    if (!target.onDevice) {
        std::string error;
        if (!createSlotResources(target, &error)) {
            std::fprintf(stderr, "openshow-worker: gpu slot %u: %s\n", slot, error.c_str());
            return false;
        }
    }
    // Expose the actual planes. Unused semi-planar entries stay null;
    // rowPitch padding is not part of the visible GPU copy.
    auto* base = static_cast<uint8_t*>(target.linearMap);
    data[0] = base + target.plane[0].offset;
    linesize[0] = static_cast<int>(target.plane[0].rowPitch);
    data[1] = base + target.plane[1].offset;
    linesize[1] = static_cast<int>(target.plane[1].rowPitch);
    data[2] = fFormat == openshow::media::FrameFormat::kYUV420P
        ? base + target.plane[2].offset : nullptr;
    linesize[2] = fFormat == openshow::media::FrameFormat::kYUV420P
        ? static_cast<int>(target.plane[2].rowPitch) : 0;
    data[3] = nullptr;
    return true;
}

bool GpuYuvUploader::waitForFree(Slot& slot) {
    std::unique_lock lock(fSync->mutex);
    while (!fSync->shutdown && slot.inUse) {
        ++fStats.waitsForFreeSlot;
        fSync->available.wait_for(lock, std::chrono::milliseconds(1));
    }
    return !fSync->shutdown;
}

bool GpuYuvUploader::commit(unsigned slot) {
    if (!fSlots || slot >= static_cast<unsigned>(kSlots)) {
        return false;
    }
    Slot& target = fSlots[slot];
    if (!waitForFree(target)) {
        return false;  // shutdown; the producer must unwind
    }
    if (!target.onDevice) {
        std::string error;
        if (!createSlotResources(target, &error)) {
            std::fprintf(stderr, "openshow-worker: gpu slot %u: %s\n", slot, error.c_str());
            return false;
        }
    }
    return commitSlot(target, 0);
}

std::shared_ptr<openshow::media::FrameStorage> GpuYuvUploader::allocateDecodeFrame(
        const openshow::media::FrameAllocation& request, std::string* error) {
    std::lock_guard lock(fDecodeMutex);
    auto fail = [&](const char* message) -> std::shared_ptr<openshow::media::FrameStorage> {
        if (error) *error = message;
        return {};
    };
    if (!fDevice || request.format != fFormat || request.width < fWidth ||
        request.height < fHeight || (request.width & 1) || (request.height & 1)) {
        return fail("decoder requested an incompatible image format or extent");
    }
    for (const auto& image : fDecodeImages) {
        if (image.use_count() == 1 && image->width == request.width &&
            image->height == request.height) {
            bool aligned = true;
            const int planes = fFormat == openshow::media::FrameFormat::kYUV420P ? 3 : 2;
            for (int p = 0; p < planes; ++p) {
                const size_t alignment = std::max(request.alignment[p], 1);
                aligned &= image->linesize[p] % alignment == 0 &&
                           reinterpret_cast<uintptr_t>(image->data[p]) % alignment == 0;
            }
            if (aligned) return image;
        }
    }
    if (fDecodeImages.size() == kMaxDecodeImages)
        return fail("direct decoder image pool exhausted (64 retained images)");

    auto image = std::make_shared<DecodeImage>();
    image->owner = this;
    image->device = fDevice;
    image->format = fFormat;
    image->width = request.width;
    image->height = request.height;
    VkDeviceSize size = 0;
    if (!createImage(VK_IMAGE_TILING_LINEAR, VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     request.width, request.height, &image->image, &image->memory, &size, error))
        return {};
    void* mapping = nullptr;
    if (vkMapMemory(fDevice, image->memory, 0, size, 0, &mapping) != VK_SUCCESS)
        return fail("cannot map direct decoder image");
    image->base = static_cast<uint8_t*>(mapping);
    image->size = size;
    const int planes = fFormat == openshow::media::FrameFormat::kYUV420P ? 3 : 2;
    for (int p = 0; p < planes; ++p) {
        VkImageSubresource sub = {};
        sub.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT << p;
        VkSubresourceLayout layout = {};
        vkGetImageSubresourceLayout(fDevice, image->image, &sub, &layout);
        const size_t alignment = std::max(request.alignment[p], 1);
        if (layout.rowPitch > INT_MAX || layout.offset >= size ||
            layout.rowPitch * static_cast<uint64_t>(p ? request.height / 2 : request.height)
                > size - layout.offset || layout.rowPitch % alignment ||
            (reinterpret_cast<uintptr_t>(image->base) + layout.offset) % alignment) {
            return fail("Vulkan plane layout does not satisfy decoder alignment or padding");
        }
        image->data[p] = image->base + layout.offset;
        image->linesize[p] = static_cast<int>(layout.rowPitch);
    }
    if (planes == 3 && image->linesize[1] != image->linesize[2]) {
        return fail("Vulkan chroma planes require matching decoder strides");
    }
    fDecodeImages.push_back(image);
    ++fStats.decodeImagesCreated;
    return image;
}

bool GpuYuvUploader::commitFrame(unsigned index,
        const std::shared_ptr<openshow::media::FrameStorage>& storage) {
    auto image = std::dynamic_pointer_cast<DecodeImage>(storage);
    if (!fSlots || index >= kSlots || !image || image->owner != this) return false;
    Slot& slot = fSlots[index];
    if (!waitForFree(slot)) return false;
    if (!slot.onDevice) {
        std::string error;
        if (!createSlotResources(slot, &error, true)) {
            std::fprintf(stderr, "openshow-worker: gpu direct slot: %s\n", error.c_str());
            return false;
        }
    }
    // The caller retains the AVFrame until this synchronous copy fence passes;
    // the allocation is additionally retained here independently of the slot.
    return commitSlot(slot, 0, image.get());
}

bool GpuYuvUploader::commitSlot(Slot& slot, uint64_t sequence, DecodeImage* source) {
    // Also serialize with redraws of the last presented texture. Such draws
    // can append a sampling semaphore after the token was already released.
    std::lock_guard queueLock(graphicsQueueMutex());
    VkImage linear = source ? source->image : slot.linear;
    bool firstLinear = source ? source->firstTransfer : slot.firstTransfer;
    // Fresh command every commit: reset the pool (which frees the previous
    // one) rather than re-recording over a primary buffer.
    if (vkResetCommandPool(fDevice, slot.commandPool, 0) != VK_SUCCESS) {
        std::fprintf(stderr, "openshow-worker: gpu commit: command pool reset failed\n");
        return false;
    }
    VkCommandBufferAllocateInfo allocate = {};
    allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate.commandPool = slot.commandPool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(fDevice, &allocate, &command) != VK_SUCCESS) {
        std::fprintf(stderr, "openshow-worker: gpu commit: command buffer alloc failed\n");
        return false;
    }
    VkCommandBufferBeginInfo begin = {};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(command, &begin) != VK_SUCCESS) {
        std::fprintf(stderr, "openshow-worker: gpu commit: begin failed\n");
        return false;
    }

    auto barrier = [&](VkImage image, VkImageLayout from, VkImageLayout to,
                       VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
                       VkPipelineStageFlags dstStage, VkAccessFlags dstAccess) {
        VkImageMemoryBarrier memoryBarrier = {};
        memoryBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        memoryBarrier.srcAccessMask = srcAccess;
        memoryBarrier.dstAccessMask = dstAccess;
        memoryBarrier.oldLayout = from;
        memoryBarrier.newLayout = to;
        // CONCURRENT images keep family IGNORED on barriers — no ownership
        // moves, both queues may use them per the sharing mode.
        memoryBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        memoryBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        memoryBarrier.image = image;
        // COLOR names every plane of a multi-planar image, which is what
        // these whole-image transitions mean here.
        memoryBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        memoryBarrier.subresourceRange.levelCount = 1;
        memoryBarrier.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(command, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1,
                             &memoryBarrier);
    };

    barrier(linear, firstLinear ? VK_IMAGE_LAYOUT_PREINITIALIZED : VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    barrier(slot.optimal,
            slot.firstTransfer ? VK_IMAGE_LAYOUT_UNDEFINED
                               : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);

    const int planeCount = fFormat == openshow::media::FrameFormat::kYUV420P ? 3 : 2;
    VkImageCopy regions[3] = {};
    for (int p = 0; p < planeCount; ++p) {
        regions[p].srcSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT << p;
        regions[p].srcSubresource.layerCount = 1;
        regions[p].dstSubresource = regions[p].srcSubresource;
        regions[p].extent = {static_cast<uint32_t>(p ? fWidth / 2 : fWidth),
                             static_cast<uint32_t>(p ? fHeight / 2 : fHeight), 1};
    }
    vkCmdCopyImage(command, linear, VK_IMAGE_LAYOUT_GENERAL, slot.optimal,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, planeCount, regions);
    // Return ownership of host access after the fence, including allocations
    // that FFmpeg keeps as reference frames and may later make writable.
    barrier(linear, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_WRITE_BIT);

    // Drain the writes into shader-visible state before the render queue
    // can sample the image (BOTTOM_OF_PIPE so this queue does the
    // draining, not the producer of the next stage).
    barrier(slot.optimal, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);

    if (vkEndCommandBuffer(command) != VK_SUCCESS) {
        std::fprintf(stderr, "openshow-worker: gpu commit: end failed\n");
        return false;
    }

    const int64_t gpuStart = steadyNowNs();
    if (vkResetFences(fDevice, 1, &slot.fence) != VK_SUCCESS) {
        std::fprintf(stderr, "openshow-worker: gpu commit: fence reset failed\n");
        return false;
    }
    // The consumer's sampling-completion semaphores ride into the submit:
    // the TRANSFER stage cannot touch OPTIMAL until the previous sampling
    // has completed, including when graphics and upload use different queues.
    // A dropped frame was never acquired by Skia: consume its still-signalled
    // ready semaphore before signalling it for the replacement frame.
    std::vector<VkSemaphore> waits = slot.pendingDone;
    if (slot.readyPending) waits.push_back(slot.ready);
    std::vector<VkPipelineStageFlags> waitStages(waits.size(),
                                                 VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkSubmitInfo submit = {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = static_cast<uint32_t>(waits.size());
    submit.pWaitSemaphores = waits.data();
    submit.pWaitDstStageMask = waitStages.data();
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &slot.ready;
    if (vkQueueSubmit(fUploadQueue, 1, &submit, slot.fence) != VK_SUCCESS) {
        std::fprintf(stderr, "openshow-worker: gpu commit: submit failed\n");
        return false;
    }
    if (vkWaitForFences(fDevice, 1, &slot.fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        std::fprintf(stderr, "openshow-worker: gpu commit: fence wait failed\n");
        return false;
    }
    // Past the fence the semaphores have all signalled (their work ran
    // before the copy this fence covers) and nothing waits on them again.
    for (VkSemaphore semaphore : slot.pendingDone) {
        vkDestroySemaphore(fDevice, semaphore, nullptr);
    }
    slot.pendingDone.clear();
    slot.firstTransfer = false;
    slot.readyPending = true;
    if (source) { source->firstTransfer = false; ++fStats.directCopies; }
    fStats.totalGpuMs += static_cast<double>(steadyNowNs() - gpuStart) / 1e6;
    ++fStats.uploads;

    {
        std::lock_guard lock(fSync->mutex);
        slot.inUse = true;
        slot.sequence = sequence;
        fNextSlot = (static_cast<int>(&slot - fSlots.get()) + 1) % kSlots;
    }
    return true;
}

bool GpuYuvUploader::upload(const uint8_t* yPlane, int yRowBytes, const uint8_t* uvPlane,
                            int uvRowBytes, uint64_t sequence, double ptsSeconds,
                            unsigned* slotOut) {
    (void)ptsSeconds;  // carried by the caller's own bookkeeping, as in the ring path
    const int rowBytes = fWidth * (fFormat == openshow::media::FrameFormat::kP010 ? 2 : 1);
    if (fFormat == openshow::media::FrameFormat::kYUV420P) return false;
    if (!fSlots || yPlane == nullptr || uvPlane == nullptr || yRowBytes < rowBytes ||
        uvRowBytes < rowBytes) {
        return false;
    }

    // Round-robin like the ring's claim, but refuse rather than wait out
    // an unbounded hold: kSlots attempts, a millisecond apart, then the
    // caller decides (this is what lets tests pin the full-ring
    // behaviour).
    Slot* chosen = nullptr;
    {
        std::unique_lock lock(fSync->mutex);
        for (int attempt = 0; attempt < kSlots && chosen == nullptr; ++attempt) {
            Slot& candidate = fSlots[fNextSlot];
            if (fSync->shutdown) {
                return false;
            }
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
        return false;  // all slots held downstream
    }
    if (!chosen->onDevice) {
        std::string error;
        if (!createSlotResources(*chosen, &error)) {
            std::fprintf(stderr, "openshow-worker: gpu slot: %s\n", error.c_str());
            return false;
        }
    }

    // Stage into the slot's own write side, visible width per row against
    // the driver's rowPitch (the caller's row padding may be larger or
    // smaller than the driver's; neither is copied).
    const int64_t stageStart = steadyNowNs();
    auto* base = static_cast<uint8_t*>(chosen->linearMap);
    for (int row = 0; row < fHeight; ++row) {
        std::memcpy(base + chosen->plane[0].offset +
                        static_cast<size_t>(row) * chosen->plane[0].rowPitch,
                    yPlane + static_cast<size_t>(row) * yRowBytes, static_cast<size_t>(rowBytes));
    }
    for (int row = 0; row < fHeight / 2; ++row) {
        std::memcpy(base + chosen->plane[1].offset +
                        static_cast<size_t>(row) * chosen->plane[1].rowPitch,
                    uvPlane + static_cast<size_t>(row) * uvRowBytes, static_cast<size_t>(rowBytes));
    }
    fStats.totalStageMs += static_cast<double>(steadyNowNs() - stageStart) / 1e6;

    if (!commitSlot(*chosen, sequence)) {
        return false;
    }
    if (slotOut) {
        *slotOut = static_cast<unsigned>(chosen - fSlots.get());
    }
    return true;
}

sk_sp<SkImage> GpuYuvUploader::imageForSlot(unsigned slot, std::string* error) {
    std::lock_guard queueLock(graphicsQueueMutex());
    if (!fSlots || slot >= static_cast<unsigned>(kSlots) || fContext == nullptr) {
        if (error) {
            *error = "uploader not created";
        }
        return nullptr;
    }
    Slot& target = fSlots[slot];
    if (!target.onDevice) {
        if (error) {
            *error = "slot has no image yet";
        }
        return nullptr;
    }
    if (target.readyPending) {
        const auto semaphore = GrBackendSemaphores::MakeVk(target.ready);
        if (!fContext->wait(1, &semaphore, false)) {
            if (error) *error = "Skia refused the upload completion semaphore";
            return nullptr;
        }
        // Submit the wait even if the caller only acquires the wrapper and
        // drops it without drawing. Otherwise reuse could re-signal the same
        // binary semaphore while its first signal is still unconsumed.
        fContext->flush();
        if (!fContext->submit()) {
            if (error) *error = "Skia failed to submit the upload semaphore wait";
            return nullptr;
        }
        target.readyPending = false;
    }
    // One wrap per slot: the commit leaves the OPTIMAL image in
    // shader-read layout from then on, so the same SkImage carries every
    // later frame of this slot. Borrow ownership means Skia never
    // destroys the image or its memory — this object owns both until
    // destroy().
    if (target.wrapped) {
        return target.wrapped;
    }
    if (target.wrapFailed) {
        if (error) {
            *error = "this slot's wrap failed earlier";
        }
        return nullptr;
    }

    GrVkImageInfo imageInfo;
    imageInfo.fImage = target.optimal;
    // Fill the alloc even though the image is borrowed: Skia's checks read
    // the handle, and a stale VK_NULL_HANDLE would fail the wrap.
    imageInfo.fAlloc.fMemory = target.optimalMemory;
    imageInfo.fAlloc.fSize = target.optimalSize;
    imageInfo.fImageTiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.fImageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo.fFormat = fVulkanFormat;
    imageInfo.fImageUsageFlags = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.fSampleCount = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.fLevelCount = 1;
    imageInfo.fCurrentQueueFamily = fGraphicsQueueIndex == fUploadQueueIndex
        ? fGraphicsQueueIndex : VK_QUEUE_FAMILY_IGNORED;
    imageInfo.fProtected = skgpu::Protected::kNo;
    imageInfo.fSharingMode = fGraphicsQueueIndex == fUploadQueueIndex
        ? VK_SHARING_MODE_EXCLUSIVE : VK_SHARING_MODE_CONCURRENT;
    // The heart of the wrap: a valid ycbcr info turns the backend format
    // into a YUV format, and Skia's image view and sampler then run the
    // YUV→RGB conversion inside the sampling of the compositing draw.
    imageInfo.fYcbcrConversionInfo = fYcbcr;

    const GrBackendTexture texture = GrBackendTextures::MakeVk(fWidth, fHeight, imageInfo);
    // Untagged colour space on purpose: the sampler's matrix is the only
    // conversion, matching what the old plane-texture path produced.
    target.wrapped = SkImages::BorrowTextureFrom(fContext, texture, kTopLeft_GrSurfaceOrigin,
                                                 fSkColorType, kOpaque_SkAlphaType, nullptr);
    if (!target.wrapped) {
        target.wrapFailed = true;
        if (error) {
            *error = "BorrowTextureFrom refused the native YUV slot";
        }
        return nullptr;
    }
    ++fStats.wraps;
    return target.wrapped;
}

void GpuYuvUploader::release(unsigned slot, VkSemaphore doneVk) {
    if (!fSync || !fSlots || slot >= static_cast<unsigned>(kSlots)) {
        return;
    }
    {
        std::lock_guard lock(fSync->mutex);
        Slot& target = fSlots[slot];
        if (doneVk != VK_NULL_HANDLE) {
            target.pendingDone.push_back(doneVk);
        }
        target.inUse = false;
    }
    fSync->available.notify_all();
}

void GpuYuvUploader::releaseAll() {
    for (int i = 0; i < kSlots; ++i) {
        release(static_cast<unsigned>(i), VK_NULL_HANDLE);
    }
}

void GpuYuvUploader::requestShutdown() {
    if (!fSync) {
        return;
    }
    {
        std::lock_guard lock(fSync->mutex);
        fSync->shutdown = true;
    }
    fSync->available.notify_all();
}

void GpuYuvUploader::destroy() {
    if (!fDevice) {
        fSlots.reset();
        fSync.reset();
        return;
    }
    // Wrappers first (Skia must still have its context to forget them),
    // then everything at rest, then the raw objects.
    if (fSlots) {
        for (int i = 0; i < kSlots; ++i) {
            fSlots[i].wrapped.reset();
        }
    }
    vkDeviceWaitIdle(fDevice);
    if (fSlots) {
        for (int i = 0; i < kSlots; ++i) {
            Slot& slot = fSlots[i];
            // Handles are destroyed individually so a half-created slot
            // (createSlotResources failed midway) still unwinds cleanly.
            if (slot.linearMap != nullptr) {
                vkUnmapMemory(fDevice, slot.linearMemory);
                slot.linearMap = nullptr;
            }
            if (slot.fence != VK_NULL_HANDLE) {
                vkDestroyFence(fDevice, slot.fence, nullptr);
                slot.fence = VK_NULL_HANDLE;
            }
            if (slot.ready != VK_NULL_HANDLE) {
                vkDestroySemaphore(fDevice, slot.ready, nullptr);
                slot.ready = VK_NULL_HANDLE;
            }
            if (slot.commandPool != VK_NULL_HANDLE) {
                vkDestroyCommandPool(fDevice, slot.commandPool, nullptr);
                slot.commandPool = VK_NULL_HANDLE;
            }
            if (slot.linear != VK_NULL_HANDLE) {
                vkDestroyImage(fDevice, slot.linear, nullptr);
                slot.linear = VK_NULL_HANDLE;
            }
            if (slot.linearMemory != VK_NULL_HANDLE) {
                vkFreeMemory(fDevice, slot.linearMemory, nullptr);
                slot.linearMemory = VK_NULL_HANDLE;
            }
            if (slot.optimal != VK_NULL_HANDLE) {
                vkDestroyImage(fDevice, slot.optimal, nullptr);
                slot.optimal = VK_NULL_HANDLE;
            }
            if (slot.optimalMemory != VK_NULL_HANDLE) {
                vkFreeMemory(fDevice, slot.optimalMemory, nullptr);
                slot.optimalMemory = VK_NULL_HANDLE;
            }
            for (VkSemaphore semaphore : slot.pendingDone) {
                vkDestroySemaphore(fDevice, semaphore, nullptr);
            }
            slot.pendingDone.clear();
            slot.onDevice = false;
            slot.firstTransfer = true;
            slot.inUse = false;
            slot.wrapFailed = false;
        }
    }
    { std::lock_guard lock(fDecodeMutex); fDecodeImages.clear(); }
    fSlots.reset();
    fSync.reset();
    fContext = nullptr;
    fUploadQueue = VK_NULL_HANDLE;
    fDevice = VK_NULL_HANDLE;
    fPhysicalDevice = VK_NULL_HANDLE;
    fWidth = 0;
    fHeight = 0;
}

}  // namespace openshow::worker
