/*
 * GpuYuvUploader: I420 frames to GPU textures Skia can sample (tasks 1.5d/1.5e).
 *
 * The shape follows the decode side: a fixed ring of slots (never grows).
 * Each slot owns host-visible staging plus two device-local optimal-tiling
 * images - R8 for Y, R8G8 for the interleaved UV plane of I420 - created
 * with CONCURRENT sharing over the graphics and upload queue families. The
 * copy commands (plane copies plus the layout transitions to
 * shader-read-optimal) are recorded and submitted on the upload queue and
 * fenced there, so on GPUs with a separate transfer family nothing of the
 * upload touches the render thread.
 *
 * Skia lives behind one call: imageForSlot() wraps a slot exactly once via
 * SkImages::TextureFromYUVATextures (the YUV→RGB conversion runs inside the
 * compositing draw - no render-to-texture), and the wrapper is reused for
 * every later frame of that slot. This revision gives no thread-safety
 * guarantee for GrDirectContext, so upload() is pure Vulkan (call it from
 * any thread) and imageForSlot() must be called from the thread that draws.
 *
 * Slot lifetime is a consumer contract: upload() blocks briefly for the next
 * slot and refuses when every slot is held; the consumer calls
 * release(slot, doneVk) once its sampling is submitted, where doneVk is a
 * Vulkan semaphore the consumer's flush will signal. The next upload to
 * that slot waits on the collected semaphores on the GPU before copying and
 * destroys them once its fence has passed - no thread sits waiting in between.
 * release(slot) without a semaphore means "already GPU-safe" (e.g. after a
 * device-wide idle in a test).
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "include/core/SkImageInfo.h"  // SkYUVColorSpace
#include "include/core/SkRefCnt.h"

class GrDirectContext;
class SkImage;

namespace openshow::worker {

class GpuYuvUploader {
public:
    static constexpr int kSlots = 4;

    struct Stats {
        uint64_t uploads = 0;
        uint64_t slotsCreated = 0;   // GPU slot allocations (≤ kSlots)
        uint64_t wraps = 0;          // SkImage creations (≤ slotsCreated)
        uint64_t waitsForFreeSlot = 0;
        double totalStageMs = 0.0;   // memcpy into staging
        double totalGpuMs = 0.0;     // submit + fence wait
    };

    // Out of line: Slot/Sync are pimpl types complete only in the .cpp.
    GpuYuvUploader();
    ~GpuYuvUploader();

    GpuYuvUploader(const GpuYuvUploader&) = delete;
    GpuYuvUploader& operator=(const GpuYuvUploader&) = delete;

    // width/height must be even (I420). yuvColorSpace selects the conversion
    // the compositing shader will apply (from the stream's probe info in
    // 1.5e; tests pick one explicitly). context is only used by
    // imageForSlot().
    bool create(VkDevice device, VkPhysicalDevice physicalDevice,
                uint32_t graphicsQueueIndex, uint32_t uploadQueueIndex,
                VkQueue uploadQueue, GrDirectContext* context,
                int width, int height, SkYUVColorSpace yuvColorSpace,
                std::string* error);
    void destroy();

    // Producer side, any thread. Blocks while the next slot is in use, then
    // stages the planes, copies and transitions to shader-read (waiting on
    // the release semaphores of that slot's last consumer on the GPU).
    // *slotOut identifies the slot for imageForSlot()/release().
    bool upload(const uint8_t* yPlane, int yRowBytes,
                const uint8_t* uvPlane, int uvRowBytes,
                uint64_t sequence, double ptsSeconds, unsigned* slotOut);

    // Consumer side, render thread only (first call wraps through Skia).
    // Returns null on wrap failure (reported once per slot).
    sk_sp<SkImage> imageForSlot(unsigned slot, std::string* error);

    // The consumer's sampling of this slot is submitted; doneVk (may be
    // VK_NULL_HANDLE) signals when it is finished. Ownership of doneVk
    // transfers to the uploader, which waits on and destroys it before the
    // slot is written again.
    void release(unsigned slot, VkSemaphore doneVk = VK_NULL_HANDLE);

    // After a device-wide idle: clear all in-use marks and drop any
    // collected semaphores.
    void releaseAll();

    const Stats& stats() const { return fStats; }
    int width() const { return fWidth; }
    int height() const { return fHeight; }

private:
    struct Slot;

    bool createSlotResources(Slot& slot, std::string* error);

    VkDevice fDevice = VK_NULL_HANDLE;
    VkPhysicalDevice fPhysicalDevice = VK_NULL_HANDLE;
    VkQueue fUploadQueue = VK_NULL_HANDLE;
    uint32_t fGraphicsQueueIndex = 0;
    uint32_t fUploadQueueIndex = 0;
    GrDirectContext* fContext = nullptr;
    int fWidth = 0;
    int fHeight = 0;
    SkYUVColorSpace fYuvColorSpace = kRec601_Limited_SkYUVColorSpace;

    std::unique_ptr<Slot[]> fSlots;
    int fNextSlot = 0;
    Stats fStats;

    // Guards the in-use flags and fNextSlot (producer waits, consumer
    // releases).
    struct Sync;
    std::unique_ptr<Sync> fSync;
};

}  // namespace openshow::worker
