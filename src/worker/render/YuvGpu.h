/*
 * Direct video: reference-counted, mapped LINEAR decoder images feed four
 * independent OPTIMAL presentation images via one vkCmdCopyImage per frame.
 * Linear images stay GENERAL after their initial PREINITIALIZED transition.
 * FFmpeg references and the synchronous copy fence govern decoder-image reuse.
 * imageForSlot borrows each optimal texture once with a Vulkan YCbCr sampler;
 * it must run on the Skia/render thread. No RGB intermediate is created.
 *
 * slotPlanes/commit and upload are the optional compatibility path and do
 * perform CPU staging/download copies. release(slot, doneVk) supplies the
 * sampling semaphore waited at TRANSFER before the next optimal-image write.
 * Stop and join producers before destroy; the VkDevice must outlive all
 * decoder allocations and borrowed textures.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/gpu/vk/VulkanTypes.h"

#include "media/FrameRing.h"

class GrDirectContext;

namespace openshow::worker {

class GpuYuvUploader : public openshow::media::FrameBacking {
public:
    // Presentation tokens; decoder reference images have a separate bounded pool.
    static constexpr int kSlots = 4;

    GpuYuvUploader();
    ~GpuYuvUploader() override;

    GpuYuvUploader(const GpuYuvUploader&) = delete;
    GpuYuvUploader& operator=(const GpuYuvUploader&) = delete;

    // width/height are the visible coded frame size; format picks the
    // native YUV image format (and with it the Skia colour type the wrap
    // reports); yuvColorSpace fixes the sampler's YUV→RGB matrix, matched
    // to what the stream's probe hints at. uploadQueue must accept
    // transfer submissions from the family uploadQueueIndex names.
    bool create(VkDevice device, VkPhysicalDevice physicalDevice, uint32_t graphicsQueueIndex,
                uint32_t uploadQueueIndex, VkQueue uploadQueue, GrDirectContext* context,
                int width, int height, openshow::media::FrameFormat format,
                SkYUVColorSpace yuvColorSpace, std::string* error);
    void destroy();

    // --- openshow::media::FrameBacking (task 1.5i) ---

    // Compatibility plane pointers into the slot's mapped LINEAR image;
    // unused planes are null. The linesize is the
    // driver's rowPitch and may pad beyond the visible width. Creates the
    // slot's resources on first call — FrameRing::create() drives this for
    // every slot, so a ring attached to this uploader is fully resident
    // before the first frame is claimed.
    bool slotPlanes(unsigned slot, uint8_t* data[4], int linesize[4]) override;

    // Producer done writing: copies the slot's LINEAR image to its OPTIMAL
    // twin and waits the copy's fence, so a published frame is GPU-ready.
    // Blocks while the consumer still holds the slot (or until
    // requestShutdown()).
    bool commit(unsigned slot) override;

    static constexpr unsigned kMaxDecodeImages = 64;
    std::shared_ptr<openshow::media::FrameStorage> allocateDecodeFrame(
        const openshow::media::FrameAllocation&, std::string* error) override;
    bool commitFrame(unsigned slot,
                     const std::shared_ptr<openshow::media::FrameStorage>&) override;

    // Producer without a FrameRing: stages NV12-shaped caller bytes
    // (yPlane/uvPlane, rows of yRowBytes/uvRowBytes — the interleaved UV
    // row spans width bytes for NV12, twice that for P010) into a free slot and commits it. Picks the
    // free slot round-robin and refuses (false) when every slot is still
    // held downstream, leaving that judgement to the caller.
    bool upload(const uint8_t* yPlane, int yRowBytes, const uint8_t* uvPlane, int uvRowBytes,
                uint64_t sequence, double ptsSeconds, unsigned* slotOut);

    // The wrapped SkImage of a committed slot, made on first call and
    // reused after. Must run on the thread that draws.
    sk_sp<SkImage> imageForSlot(unsigned slot, std::string* error);

    // Consumer done holding a slot; doneVk, when not VK_NULL_HANDLE, is a
    // semaphore the next commit to the slot will wait before copying.
    void release(unsigned slot, VkSemaphore doneVk = VK_NULL_HANDLE);
    // Everything at once, valid only after whole-device idle.
    void releaseAll();

    // Unblocks producers parked in commit()/upload() waits; afterwards
    // they fail fast so a decode thread can unwind during teardown.
    void requestShutdown() override;

    struct Stats {
        uint64_t uploads = 0;          // committed slots
        uint64_t slotsCreated = 0;     // slot resource pairs created
        uint64_t wraps = 0;            // SkImages borrowed from slots
        uint64_t waitsForFreeSlot = 0; // producer waits on held slots
        uint64_t decodeImagesCreated = 0;
        uint64_t directCopies = 0;
        double totalStageMs = 0.0;     // upload() memcpy only
        double totalGpuMs = 0.0;       // copy submit + fence per commit
    };
    const Stats& stats() const { return fStats; }

    int width() const { return fWidth; }
    int height() const { return fHeight; }

private:
    struct Slot;
    struct DecodeImage;

    bool createSlotResources(Slot& slot, std::string* error, bool optimalOnly = false);
    bool createImage(VkImageTiling, VkImageUsageFlags, VkMemoryPropertyFlags, int, int,
                     VkImage*, VkDeviceMemory*, VkDeviceSize*, std::string*);
    bool waitForFree(Slot& slot);
    bool commitSlot(Slot& slot, uint64_t sequence, DecodeImage* source = nullptr);

    VkDevice fDevice = VK_NULL_HANDLE;
    VkPhysicalDevice fPhysicalDevice = VK_NULL_HANDLE;
    uint32_t fGraphicsQueueIndex = 0;
    uint32_t fUploadQueueIndex = 0;
    VkQueue fUploadQueue = VK_NULL_HANDLE;
    GrDirectContext* fContext = nullptr;
    int fWidth = 0;
    int fHeight = 0;

    openshow::media::FrameFormat fFormat = openshow::media::FrameFormat::kNV12;
    VkFormat fVulkanFormat = VK_FORMAT_UNDEFINED;
    SkColorType fSkColorType = kUnknown_SkColorType;
    // Built once at create() from the device's format features; carried
    // into every wrap's GrVkImageInfo.
    skgpu::VulkanYcbcrConversionInfo fYcbcr;

    struct Sync;
    std::unique_ptr<Sync> fSync;  // mutex + condvar + shutdown flag
    std::unique_ptr<Slot[]> fSlots;
    int fNextSlot = 0;  // upload() round-robin cursor
    Stats fStats;
    std::mutex fDecodeMutex;
    std::vector<std::shared_ptr<DecodeImage>> fDecodeImages;
};

}  // namespace openshow::worker
