#pragma once

#include <native_window/external_window.h>
#define VK_USE_PLATFORM_OHOS 1
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_ohos.h>

#include <array>
#include <cstdint>
#include <string>

struct OH_NativeBuffer;

namespace winehua {

/* Kill switch: WINEHUA_DIRECT_NATIVEWINDOW=0 restores eglSwapBuffers / WSI.
 * Unset or any other value enables RequestBuffer + fence FlushBuffer. */
bool DirectNativeWindowEnabled();

struct FramePercentiles {
    uint64_t p50 = 0;
    uint64_t p90 = 0;
    uint64_t p99 = 0;
    uint64_t max = 0;
    uint64_t count = 0;
};

class FrameTimeline {
public:
    void Add(uint64_t totalUs, uint64_t requestUs, uint64_t flushUs,
             uint64_t copyUs, uint64_t recycleWaitUs);
    FramePercentiles Total() const;
    uint64_t Count() const { return count_; }
    uint64_t CacheHits() const { return cacheHits_; }
    uint64_t CacheMisses() const { return cacheMisses_; }
    uint64_t GpuCopies() const { return gpuCopies_; }
    uint64_t SkippedCopies() const { return skippedCopies_; }
    uint64_t CpuFenceWaits() const { return cpuFenceWaits_; }
    uint64_t GlFinishCount() const { return glFinishCount_; }
    void AddCacheHit() { ++cacheHits_; }
    void AddCacheMiss() { ++cacheMisses_; }
    void AddGpuCopy() { ++gpuCopies_; }
    void AddSkippedCopy() { ++skippedCopies_; }
    void AddCpuFenceWait() { ++cpuFenceWaits_; }
    void AddGlFinish() { ++glFinishCount_; }
    std::string Format(const char* tag, uint32_t serial) const;

private:
    static constexpr size_t kWindow = 120;
    std::array<uint64_t, kWindow> totals_{};
    size_t index_ = 0;
    uint64_t count_ = 0;
    uint64_t cacheHits_ = 0;
    uint64_t cacheMisses_ = 0;
    uint64_t gpuCopies_ = 0;
    uint64_t skippedCopies_ = 0;
    uint64_t cpuFenceWaits_ = 0;
    uint64_t glFinishCount_ = 0;
};

struct GlesDirectSlot {
    uint32_t seq = 0;
    OHNativeWindowBuffer* windowBuffer = nullptr;
    OH_NativeBuffer* nativeBuffer = nullptr;
    void* eglImage = nullptr;
    unsigned texture = 0;
    unsigned framebuffer = 0;
};

class NativeWindowGlesTarget {
public:
    NativeWindowGlesTarget() = default;
    ~NativeWindowGlesTarget() { Reset(); }
    NativeWindowGlesTarget(const NativeWindowGlesTarget&) = delete;
    NativeWindowGlesTarget& operator=(const NativeWindowGlesTarget&) = delete;

    bool Configure(OHNativeWindow* window, uint32_t width, uint32_t height);
    bool BeginFrame();
    bool EndFrame(int renderFenceFd);
    void AbortFrame();
    void Reset();

    unsigned ColorTexture() const { return current_ ? current_->texture : 0; }
    unsigned Framebuffer() const { return current_ ? current_->framebuffer : 0; }
    uint32_t SeqNum() const { return current_ ? current_->seq : 0; }
    GlesDirectSlot* Current() { return current_; }
    FrameTimeline& Timeline() { return timeline_; }
    const FrameTimeline& Timeline() const { return timeline_; }
    uint32_t Width() const { return width_; }
    uint32_t Height() const { return height_; }
    bool Ready() const { return window_ && display_ != nullptr; }

    int CreateNativeFenceFd();
    bool GpuWaitAcquireFd(int fenceFd);

private:
    GlesDirectSlot* ImportLocked(OHNativeWindowBuffer* windowBuffer);
    void DestroySlot(GlesDirectSlot& slot);
    bool EnsureContext();

    OHNativeWindow* window_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    void* display_ = nullptr;
    void* context_ = nullptr;
    void* pbuffer_ = nullptr;
    bool borrowedContext_ = false;
    std::array<GlesDirectSlot, 8> cache_{};
    GlesDirectSlot* current_ = nullptr;
    FrameTimeline timeline_;
    uint64_t requestUs_ = 0;
    uint64_t flushUs_ = 0;
};

struct VkDirectSlot {
    uint32_t seq = 0;
    OHNativeWindowBuffer* windowBuffer = nullptr;
    OH_NativeBuffer* nativeBuffer = nullptr;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
};

class NativeWindowVkTarget {
public:
    NativeWindowVkTarget() = default;
    ~NativeWindowVkTarget() { Reset(); }
    NativeWindowVkTarget(const NativeWindowVkTarget&) = delete;
    NativeWindowVkTarget& operator=(const NativeWindowVkTarget&) = delete;

    bool Configure(OHNativeWindow* window, uint32_t width, uint32_t height,
                   VkPhysicalDevice physical, VkDevice device);
    bool BeginFrame();
    bool AcquireGpu(VkSemaphore semaphore, VkFence fence);
    int SignalRelease(VkQueue queue, uint32_t waitSemaphoreCount,
                      const VkSemaphore* waitSemaphores);
    bool EndFrame(int renderFenceFd);
    void AbortFrame();
    void Reset();

    VkImage ColorImage() const { return current_ ? current_->image : VK_NULL_HANDLE; }
    VkFormat ColorFormat() const { return current_ ? current_->format : VK_FORMAT_R8G8B8A8_UNORM; }
    uint32_t SeqNum() const { return current_ ? current_->seq : 0; }
    FrameTimeline& Timeline() { return timeline_; }
    uint32_t Width() const { return width_; }
    uint32_t Height() const { return height_; }

private:
    VkDirectSlot* ImportLocked(OHNativeWindowBuffer* windowBuffer);
    void DestroySlot(VkDirectSlot& slot);

    OHNativeWindow* window_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    PFN_vkGetNativeBufferPropertiesOHOS getProps_ = nullptr;
    PFN_vkAcquireImageOHOS acquireImage_ = nullptr;
    PFN_vkQueueSignalReleaseImageOHOS releaseImage_ = nullptr;
    std::array<VkDirectSlot, 8> cache_{};
    VkDirectSlot* current_ = nullptr;
    int pendingAcquireFd_ = -1;
    FrameTimeline timeline_;
    uint64_t requestUs_ = 0;
    uint64_t flushUs_ = 0;
};

} // namespace winehua
