/*
 * GpuYuvUploader: I420 frames to GPU textures Skia can sample (task 1.5d).
 *
 * The shape follows the decode side: a fixed ring of slots (never grows).
 * Each slot owns host-visible staging plus two device-local optimal-tiling
 * images - R8 for Y, R8G8 for the interleaved UV plane of I420 - created
 * with CONCURRENT sharing over the graphics and upload queue families. The
 * copy commands (plane copies plus the layout transitions to
 * shader-read-optimal) are recorded and submitted on the upload queue and
 * fenced there, so on GPUs with a separate transfer family nothing of the
 * upload touches the render thread. Each slot is wrapped into a persistent
 * SkImage exactly once via SkImages::TextureFromYUVATextures (the YUV→RGB
 * conversion runs inside the compositing draw - no render-to-texture), and
 * that wrapper is reused for every subsequent frame that lands in the slot.
 *
 * Slot lifetime is a consumer contract, not a queue trick: upload() blocks
 * while the next slot in ring order is still in use, and the consumer calls
 * release(slot) once it has finished sampling it. Task 1.5e upgrades that
 * release to carry the flush's GPU semaphore so the wait happens on the GPU
 * rather than in a thread.
 *
 * Not thread-safe by itself: one caller uploads, one caller consumes, and
 * the ring state is guarded internally by a mutex + condition variable so
 * the 1.5e producer thread and render thread can sit on either side.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <vulkan/vulkan.h>

#include "include/core/SkImageInfo.h"  // SkYUVColorSpace
#include "include/core/SkRefCnt.h"

class GrDirectContext;
class SkImage;

namespace openshow::worker {

class GpuYuvUploader {
public:
    static constexpr int kSlots = 4;

    struct Frame {
        sk_sp<SkImage> image;
        uint64_t sequence = 0;
        double ptsSeconds = 0.0;
        unsigned slot = 0;  // for release()
    };

    struct Stats {
        uint64_t uploads = 0;
        uint64_t slotsCreated = 0;   // GPU slot allocations (≤ kSlots)
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
    // 1.5e; tests pick one explicitly).
    bool create(VkDevice device, VkPhysicalDevice physicalDevice,
                uint32_t graphicsQueueIndex, uint32_t uploadQueueIndex,
                VkQueue uploadQueue, GrDirectContext* context,
                int width, int height, SkYUVColorSpace yuvColorSpace,
                std::string* error);
    void destroy();

    // Blocks while the next slot is in use, then stages the planes, copies,
    // transitions to shader-read and hands back the slot's wrapped image.
    // Returns false on creation failure (after reporting once).
    bool upload(const uint8_t* yPlane, int yRowBytes,
                const uint8_t* uvPlane, int uvRowBytes,
                uint64_t sequence, double ptsSeconds, Frame* out);

    // The consumer is done sampling this slot: safe to overwrite. By the time
    // this is called the consumer's GPU work must be finished or fenced; a
    // plain release after vkDeviceWaitIdle/readPixels satisfies that today,
    // and 1.5e will pass the flush semaphore instead.
    void release(unsigned slot);

    // After a device-wide idle: clear all in-use marks.
    void releaseAll();

    const Stats& stats() const { return fStats; }
    int width() const { return fWidth; }
    int height() const { return fHeight; }

private:
    struct Slot;

    bool createSlotResources(Slot& slot, std::string* error);
    bool waitForFreeSlot(Slot& slot);

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
