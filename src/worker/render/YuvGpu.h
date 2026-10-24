/*
 * GpuYuvUploader: NV12/P010 frames to GPU textures Skia can sample
 * (tasks 1.5d/1.5e, rebuilt on the raster-slot scheme for 1.5i).
 *
 * The shape follows the decode side: a fixed ring of slots (never grows).
 * Each slot owns a pair of two-plane images in the stream's format —
 * G8_B8R8_2PLANE_420_UNORM for NV12, the PACK16 variant of
 * G10X6_B10X6R10X6 for P010 — created with CONCURRENT sharing over the
 * graphics and upload queue families:
 *
 *  - a LINEAR image bound to host-visible coherent memory, mapped once for
 *    its life. Implementing FrameBacking, its plane mappings ARE the
 *    FrameRing's slot storage: the decode side's destination pointer is
 *    GPU-visible staging memory, and av_hwframe_transfer_data (or the
 *    software path's interleave) writes into it with no copy at all,
 *  - a device-local OPTIMAL twin that Skia samples.
 *
 * commit() is the pipeline's only GPU work per frame: one vkCmdCopyImage
 * (both plane regions) LINEAR to OPTIMAL, layout transitions included.
 * Recorded and submitted on the upload queue and fenced there, so on GPUs
 * with a separate transfer family nothing of the upload touches the
 * render thread.
 *
 * Skia lives behind one call: imageForSlot() wraps a slot exactly once via
 * SkImages::BorrowTextureFrom with a GrVkImageInfo carrying an
 * fYcbcrConversionInfo — the backend format is then a ycbcr format, so the
 * YUV→RGB conversion runs in the sampled image view and sampler inside the
 * compositing draw (no render-to-texture, no Skia-side plane assembly),
 * and the wrapper is reused for every later frame of that slot. Borrow
 * ownership means Skia never destroys the VkImage or its memory. This
 * revision gives no thread-safety guarantee for GrDirectContext, so
 * slotPlanes()/commit()/upload() are pure Vulkan (call them from any
 * thread) and imageForSlot() must be called from the thread that draws.
 *
 * Slot life is the consumer's contract: commit() (and upload()) wait for
 * the slot to be free, the consumer calls release(slot, doneVk) once the
 * sampling submits are queued, where doneVk is the Vulkan semaphore its
 * flush will signal. The next commit to that slot waits the collected
 * semaphores on the GPU (never a thread) before copying and destroys them
 * when its fence passes. release(slot) without a semaphore means "already
 * safe GPU-side" (test teardown). requestShutdown() releases producers
 * parked in a commit wait so a decode thread can never outlive teardown.
 */

#pragma once

#include <cstdint>
#include <memory>
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
    // Slots in the ring; the FrameRing attached to this backing must use
    // exactly this many slots so ring slot i and GPU slot i coincide.
    static constexpr int kSlots = 4;

    GpuYuvUploader();
    ~GpuYuvUploader() override;

    GpuYuvUploader(const GpuYuvUploader&) = delete;
    GpuYuvUploader& operator=(const GpuYuvUploader&) = delete;

    // width/height are the visible coded frame size; format picks the
    // two-plane image format (and with it the Skia colour type the wrap
    // reports); yuvColorSpace fixes the sampler's YUV→RGB matrix, matched
    // to what the stream's probe hints at. uploadQueue must accept
    // transfer submissions from the family uploadQueueIndex names.
    bool create(VkDevice device, VkPhysicalDevice physicalDevice, uint32_t graphicsQueueIndex,
                uint32_t uploadQueueIndex, VkQueue uploadQueue, GrDirectContext* context,
                int width, int height, openshow::media::FrameFormat format,
                SkYUVColorSpace yuvColorSpace, std::string* error);
    void destroy();

    // --- openshow::media::FrameBacking (task 1.5i) ---

    // Plane pointers into the slot's mapped LINEAR image, NV12/P010
    // semi-planar convention (data[2] = data[1] + 1); the linesize is the
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

    // Producer without a FrameRing: stages NV12-shaped caller bytes
    // (yPlane/uvPlane, rows of yRowBytes/uvRowBytes — the interleaved UV
    // row spans width bytes) into a free slot and commits it. Picks the
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
    void requestShutdown();

    struct Stats {
        uint64_t uploads = 0;          // committed slots
        uint64_t slotsCreated = 0;     // slot resource pairs created
        uint64_t wraps = 0;            // SkImages borrowed from slots
        uint64_t waitsForFreeSlot = 0; // producer waits on held slots
        double totalStageMs = 0.0;     // upload() memcpy only
        double totalGpuMs = 0.0;       // copy submit + fence per commit
    };
    const Stats& stats() const { return fStats; }

    int width() const { return fWidth; }
    int height() const { return fHeight; }

private:
    struct Slot;

    bool createSlotResources(Slot& slot, std::string* error);
    bool waitForFree(Slot& slot);
    bool commitSlot(Slot& slot, uint64_t sequence);

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
};

}  // namespace openshow::worker
