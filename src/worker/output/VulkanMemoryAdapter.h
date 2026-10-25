/*
 * VulkanMemoryAdapter: the worker's own GPU memory allocator (task 6.2a,
 * review R9), written against the PUBLIC Skia contract
 * include/gpu/vk/VulkanMemoryAllocator.h.
 *
 * Until now the worker reached through Skia's private
 * VulkanMemoryAllocators::Make() for the allocator its backend context
 * demands (this Skia revision has no internal default). That contract —
 * "bring your own implementation of VulkanMemoryAllocator" — is upstream's
 * documented answer, and it is the only VulkanMemoryAllocator Skia ever
 * talks to: this class is therefore the single place the process allocates
 * Skia's device memory, for every Vulkan path (the windowed output, the
 * headless GPU-test context and anything the preview adds), so nobody has
 * to. The upstream VulkanMemoryAllocator library suballocates underneath,
 * so "owning" the allocator does not cost the sharing that made Skia's own
 * default viable.
 *
 * The VMA linkage is deliberately non-obvious: libskia.a carries its own
 * compiled-in copy of VMA behind the private factory, and the VMA API is
 * C, so a second VMA_IMPLEMENTATION would collide with (or silently
 * cross-version-bind to) that copy. The adapter's implementation TU
 * therefore compiles VMA with VMA_CALL_PRE static: every vma* symbol is
 * internal to that one translation unit, the two copies can never meet,
 * and a stale libskia VMA object getting pulled in by a fallback branch
 * can never run against structs built from our headers. The adapter
 * header includes no VMA at all.
 *
 * Semantics are the contract's (include/gpu/vk/VulkanMemoryAllocator.h),
 * which the adapter implements 1:1 on VMA: buffer usage maps to memory
 * property requirements, the allocation property flags map to VMA flags,
 * getAllocInfo fills VulkanAlloc (mappable/non-coherent/lazy) from the
 * memory type, map/flush/invalidate/free delegate, and
 * totalAllocatedAndUsedMemory reports VMA's block/allocation statistics.
 * Thread safety matches what the worker always passed to the private
 * factory (ThreadSafe::kNo): Ganesh calls the allocator under its own
 * context lock, so VMA runs externally synchronized — one less mutex on
 * the allocation path.
 */

#pragma once

#include "include/core/SkRefCnt.h"
#include "include/gpu/vk/VulkanMemoryAllocator.h"  // the public contract

namespace skgpu {
struct VulkanBackendContext;
}

// VMA's handle type is the global-scope `struct VmaAllocator_T*`;
// forward-declared so this header stays free of the allocator
// implementation (which lives entirely in VulkanMemoryAdapter.cpp).
struct VmaAllocator_T;

namespace openshow::worker {

class VulkanMemoryAdapter final : public skgpu::VulkanMemoryAllocator {
public:
    // Builds VMA over the instance/physical device/device of a (partly
    // filled) Skia backend context: fGetProc must already be set, because
    // VMA's function pointers are resolved through it — the same entry
    // point view Skia gets. Returns null on failure.
    static sk_sp<VulkanMemoryAdapter> Create(const skgpu::VulkanBackendContext& backendContext);

    ~VulkanMemoryAdapter() override;

    // skgpu::VulkanMemoryAllocator — see the public header for the
    // contract each member must honour.
    VkResult allocateImageMemory(VkImage image, uint32_t allocationPropertyFlags,
                                 skgpu::VulkanBackendMemory* memory) override;
    VkResult allocateBufferMemory(VkBuffer buffer, BufferUsage usage,
                                  uint32_t allocationPropertyFlags,
                                  skgpu::VulkanBackendMemory* memory) override;
    void getAllocInfo(const skgpu::VulkanBackendMemory&, skgpu::VulkanAlloc*) const override;
    VkResult mapMemory(const skgpu::VulkanBackendMemory& memory, void** data) override;
    void unmapMemory(const skgpu::VulkanBackendMemory&) override;
    VkResult flushMemory(const skgpu::VulkanBackendMemory&, VkDeviceSize, VkDeviceSize) override;
    VkResult invalidateMemory(const skgpu::VulkanBackendMemory&, VkDeviceSize,
                              VkDeviceSize) override;
    void freeMemory(const skgpu::VulkanBackendMemory&) override;
    std::pair<uint64_t, uint64_t> totalAllocatedAndUsedMemory() const override;

private:
    explicit VulkanMemoryAdapter(VmaAllocator_T* vma) : fVma(vma) {}

    VmaAllocator_T* fVma = nullptr;
};

}  // namespace openshow::worker
