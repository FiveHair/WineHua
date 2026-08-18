#define VK_USE_PLATFORM_OHOS 1

#include "native_window_direct.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <hilog/log.h>
#include <native_buffer/native_buffer.h>
#include <vulkan/vulkan_ohos.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <vector>

#ifndef EGL_NATIVE_BUFFER_OHOS
#define EGL_NATIVE_BUFFER_OHOS 0x34E1
#endif

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "nw-direct"

namespace winehua {
namespace {

using Clock = std::chrono::steady_clock;

uint64_t NowUs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now().time_since_epoch()).count());
}

void CloseFd(int* fd)
{
    if (fd && *fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

uint64_t PercentileOf(std::array<uint64_t, 120> values, size_t count,
                      unsigned percentile)
{
    if (!count) return 0;
    std::sort(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(count));
    const size_t index = std::min(count - 1, (count * percentile + 99) / 100 - 1);
    return values[index];
}

PFNEGLCREATEIMAGEKHRPROC gCreateImage;
PFNEGLDESTROYIMAGEKHRPROC gDestroyImage;
PFNGLEGLIMAGETARGETTEXTURE2DOESPROC gImageTargetTexture;
PFNEGLCREATESYNCKHRPROC gCreateSync;
PFNEGLDESTROYSYNCKHRPROC gDestroySync;
PFNEGLWAITSYNCKHRPROC gWaitSync;
PFNEGLDUPNATIVEFENCEFDANDROIDPROC gDupFenceFd;
bool gEglProcsLoaded;

void LoadEglProcs()
{
    if (gEglProcsLoaded) return;
    gCreateImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(
        eglGetProcAddress("eglCreateImageKHR"));
    gDestroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
        eglGetProcAddress("eglDestroyImageKHR"));
    gImageTargetTexture = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    gCreateSync = reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(
        eglGetProcAddress("eglCreateSyncKHR"));
    gDestroySync = reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(
        eglGetProcAddress("eglDestroySyncKHR"));
    gWaitSync = reinterpret_cast<PFNEGLWAITSYNCKHRPROC>(
        eglGetProcAddress("eglWaitSyncKHR"));
    gDupFenceFd = reinterpret_cast<PFNEGLDUPNATIVEFENCEFDANDROIDPROC>(
        eglGetProcAddress("eglDupNativeFenceFDANDROID"));
    gEglProcsLoaded = true;
}

uint32_t PickMemoryType(VkPhysicalDevice physical, uint32_t bits)
{
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(physical, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            return i;
    }
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if (bits & (1u << i)) return i;
    }
    return UINT32_MAX;
}

} // namespace

bool DirectNativeWindowEnabled()
{
    const char* value = std::getenv("WINEHUA_DIRECT_NATIVEWINDOW");
    if (!value || !value[0]) return true;
    return value[0] != '0';
}

void FrameTimeline::Add(uint64_t totalUs, uint64_t, uint64_t, uint64_t, uint64_t)
{
    totals_[index_ % kWindow] = totalUs;
    ++index_;
    ++count_;
}

FramePercentiles FrameTimeline::Total() const
{
    FramePercentiles out;
    const size_t n = std::min(count_, static_cast<uint64_t>(kWindow));
    out.count = count_;
    if (!n) return out;
    std::array<uint64_t, kWindow> copy = totals_;
    out.p50 = PercentileOf(copy, n, 50);
    out.p90 = PercentileOf(copy, n, 90);
    out.p99 = PercentileOf(copy, n, 99);
    out.max = 0;
    for (size_t i = 0; i < n; ++i) out.max = std::max(out.max, totals_[i]);
    return out;
}

std::string FrameTimeline::Format(const char* tag, uint32_t serial) const
{
    const FramePercentiles p = Total();
    char line[512];
    std::snprintf(
        line, sizeof(line),
        "[%s] serial=%u frames=%llu cache_hit=%llu cache_miss=%llu "
        "gpu_copy=%llu skipped_copy=%llu cpu_fence_wait=%llu glFinish=%llu "
        "p50=%llu p90=%llu p99=%llu max=%llu us",
        tag ? tag : "DIRECT", serial,
        static_cast<unsigned long long>(p.count),
        static_cast<unsigned long long>(cacheHits_),
        static_cast<unsigned long long>(cacheMisses_),
        static_cast<unsigned long long>(gpuCopies_),
        static_cast<unsigned long long>(skippedCopies_),
        static_cast<unsigned long long>(cpuFenceWaits_),
        static_cast<unsigned long long>(glFinishCount_),
        static_cast<unsigned long long>(p.p50),
        static_cast<unsigned long long>(p.p90),
        static_cast<unsigned long long>(p.p99),
        static_cast<unsigned long long>(p.max));
    return line;
}

bool NativeWindowGlesTarget::EnsureContext()
{
    LoadEglProcs();
    if (!gCreateImage || !gImageTargetTexture || !gCreateSync || !gDupFenceFd)
        return false;
    if (display_ && context_) return true;

    EGLDisplay currentDisplay = eglGetCurrentDisplay();
    EGLContext currentContext = eglGetCurrentContext();
    EGLSurface currentDraw = eglGetCurrentSurface(EGL_DRAW);
    if (currentDisplay != EGL_NO_DISPLAY && currentContext != EGL_NO_CONTEXT) {
        display_ = currentDisplay;
        context_ = currentContext;
        pbuffer_ = currentDraw;
        borrowedContext_ = true;
        return true;
    }

    EGLDisplay display = eglGetCurrentDisplay();
    if (display == EGL_NO_DISPLAY) display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) return false;
    EGLint major = 0, minor = 0;
    if (!eglInitialize(display, &major, &minor)) return false;

    const EGLint configAttrs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };
    EGLConfig config = nullptr;
    EGLint count = 0;
    if (!eglChooseConfig(display, configAttrs, &config, 1, &count) || count == 0)
        return false;
    const EGLint pbufferAttrs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    EGLSurface pbuffer = eglCreatePbufferSurface(display, config, pbufferAttrs);
    if (pbuffer == EGL_NO_SURFACE) return false;
    const EGLint ctxAttrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext share = eglGetCurrentContext();
    EGLContext context = eglCreateContext(
        display, config, share != EGL_NO_CONTEXT ? share : EGL_NO_CONTEXT, ctxAttrs);
    if (context == EGL_NO_CONTEXT) {
        eglDestroySurface(display, pbuffer);
        return false;
    }
    display_ = display;
    context_ = context;
    pbuffer_ = pbuffer;
    return true;
}

bool NativeWindowGlesTarget::Configure(OHNativeWindow* window, uint32_t width,
                                       uint32_t height)
{
    if (!window || !width || !height) return false;
    if (window_ == window && width_ == width && height_ == height && Ready())
        return true;
    Reset();
    window_ = window;
    width_ = width;
    height_ = height;
    OH_NativeWindow_NativeWindowHandleOpt(
        window_, SET_BUFFER_GEOMETRY, static_cast<int32_t>(width),
        static_cast<int32_t>(height));
    OH_NativeWindow_NativeWindowHandleOpt(
        window_, SET_FORMAT, NATIVEBUFFER_PIXEL_FMT_RGBA_8888);
    OH_NativeWindow_NativeWindowHandleOpt(
        window_, SET_USAGE,
        static_cast<uint64_t>(NATIVEBUFFER_USAGE_HW_RENDER |
                              NATIVEBUFFER_USAGE_HW_TEXTURE));
    OH_NativeWindow_NativeWindowHandleOpt(window_, SET_TIMEOUT, 0);
    return EnsureContext();
}

void NativeWindowGlesTarget::DestroySlot(GlesDirectSlot& slot)
{
    EGLDisplay display = static_cast<EGLDisplay>(display_);
    if (slot.framebuffer) glDeleteFramebuffers(1, &slot.framebuffer);
    if (slot.texture) glDeleteTextures(1, &slot.texture);
    if (slot.eglImage && gDestroyImage && display)
        gDestroyImage(display, static_cast<EGLImageKHR>(slot.eglImage));
    slot = GlesDirectSlot{};
}

void NativeWindowGlesTarget::Reset()
{
    if (display_ && context_) {
        const EGLDisplay display = static_cast<EGLDisplay>(display_);
        const EGLContext previous = eglGetCurrentContext();
        const EGLSurface draw = eglGetCurrentSurface(EGL_DRAW);
        const EGLSurface read = eglGetCurrentSurface(EGL_READ);
        if (pbuffer_)
            eglMakeCurrent(display, static_cast<EGLSurface>(pbuffer_),
                           static_cast<EGLSurface>(pbuffer_),
                           static_cast<EGLContext>(context_));
        for (GlesDirectSlot& slot : cache_) DestroySlot(slot);
        eglMakeCurrent(display, draw, read, previous);
        if (!borrowedContext_) {
            eglDestroyContext(display, static_cast<EGLContext>(context_));
            if (pbuffer_) eglDestroySurface(display, static_cast<EGLSurface>(pbuffer_));
        }
    }
    display_ = nullptr;
    context_ = nullptr;
    pbuffer_ = nullptr;
    borrowedContext_ = false;
    window_ = nullptr;
    current_ = nullptr;
    width_ = 0;
    height_ = 0;
}

GlesDirectSlot* NativeWindowGlesTarget::ImportLocked(OHNativeWindowBuffer* windowBuffer)
{
    OH_NativeBuffer* native = nullptr;
    if (OH_NativeBuffer_FromNativeWindowBuffer(windowBuffer, &native) != 0 || !native)
        return nullptr;
    const uint32_t seq = OH_NativeBuffer_GetSeqNum(native);
    for (GlesDirectSlot& slot : cache_) {
        if (slot.seq == seq && slot.texture) {
            slot.windowBuffer = windowBuffer;
            slot.nativeBuffer = native;
            timeline_.AddCacheHit();
            return &slot;
        }
    }
    timeline_.AddCacheMiss();
    GlesDirectSlot* empty = nullptr;
    for (GlesDirectSlot& slot : cache_) {
        if (!slot.seq) {
            empty = &slot;
            break;
        }
    }
    if (!empty) empty = &cache_[0];
    if (empty->seq) DestroySlot(*empty);

    const EGLDisplay display = static_cast<EGLDisplay>(display_);
    EGLImageKHR image = gCreateImage(
        display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_OHOS,
        static_cast<EGLClientBuffer>(native), nullptr);
    if (image == EGL_NO_IMAGE_KHR) return nullptr;
    GLuint texture = 0;
    GLuint framebuffer = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    gImageTargetTexture(GL_TEXTURE_2D, image);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           texture, 0);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        glDeleteFramebuffers(1, &framebuffer);
        glDeleteTextures(1, &texture);
        gDestroyImage(display, image);
        return nullptr;
    }
    empty->seq = seq;
    empty->windowBuffer = windowBuffer;
    empty->nativeBuffer = native;
    empty->eglImage = image;
    empty->texture = texture;
    empty->framebuffer = framebuffer;
    OH_LOG_INFO(LOG_APP,
                "[NW-DIRECT] GLES import seq=%{public}u tex=%{public}u fbo=%{public}u",
                seq, texture, framebuffer);
    return empty;
}

bool NativeWindowGlesTarget::GpuWaitAcquireFd(int fenceFd)
{
    if (fenceFd < 0) return true;
    if (!gCreateSync || !gWaitSync) {
        CloseFd(&fenceFd);
        return false;
    }
    const EGLDisplay display = static_cast<EGLDisplay>(display_);
    const EGLint attribs[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fenceFd, EGL_NONE};
    EGLSyncKHR sync = gCreateSync(display, EGL_SYNC_NATIVE_FENCE_ANDROID, attribs);
    if (sync == EGL_NO_SYNC_KHR) {
        CloseFd(&fenceFd);
        return false;
    }
    gWaitSync(display, sync, 0);
    gDestroySync(display, sync);
    return true;
}

bool NativeWindowGlesTarget::BeginFrame()
{
    if (!EnsureContext() || !window_) return false;
    if (!borrowedContext_) {
        const EGLDisplay display = static_cast<EGLDisplay>(display_);
        if (eglMakeCurrent(display, static_cast<EGLSurface>(pbuffer_),
                           static_cast<EGLSurface>(pbuffer_),
                           static_cast<EGLContext>(context_)) != EGL_TRUE)
            return false;
    }
    OHNativeWindowBuffer* buffer = nullptr;
    int fenceFd = -1;
    const uint64_t started = NowUs();
    const int32_t result =
        OH_NativeWindow_NativeWindowRequestBuffer(window_, &buffer, &fenceFd);
    requestUs_ = NowUs() - started;
    if (result != 0 || !buffer) {
        CloseFd(&fenceFd);
        return false;
    }
    current_ = ImportLocked(buffer);
    if (!current_) {
        OH_NativeWindow_NativeWindowAbortBuffer(window_, buffer);
        CloseFd(&fenceFd);
        return false;
    }
    GpuWaitAcquireFd(fenceFd);
    glBindFramebuffer(GL_FRAMEBUFFER, current_->framebuffer);
    glViewport(0, 0, static_cast<GLsizei>(width_), static_cast<GLsizei>(height_));
    return true;
}

int NativeWindowGlesTarget::CreateNativeFenceFd()
{
    if (!gCreateSync || !gDupFenceFd) return -1;
    const EGLDisplay display = static_cast<EGLDisplay>(display_);
    glFlush();
    EGLSyncKHR sync = gCreateSync(display, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr);
    if (sync == EGL_NO_SYNC_KHR) return -1;
    const int fd = gDupFenceFd(display, sync);
    gDestroySync(display, sync);
    return fd;
}

bool NativeWindowGlesTarget::EndFrame(int renderFenceFd)
{
    if (!window_ || !current_ || !current_->windowBuffer) {
        CloseFd(&renderFenceFd);
        return false;
    }
    Region region{};
    region.rects = nullptr;
    region.rectNumber = 0;
    const uint64_t started = NowUs();
    const int32_t result = OH_NativeWindow_NativeWindowFlushBuffer(
        window_, current_->windowBuffer, renderFenceFd, region);
    flushUs_ = NowUs() - started;
    if (result != 0) {
        CloseFd(&renderFenceFd);
        OH_NativeWindow_NativeWindowAbortBuffer(window_, current_->windowBuffer);
        current_ = nullptr;
        return false;
    }
    timeline_.Add(requestUs_ + flushUs_, requestUs_, flushUs_, 0, 0);
    current_ = nullptr;
    return true;
}

void NativeWindowGlesTarget::AbortFrame()
{
    if (window_ && current_ && current_->windowBuffer)
        OH_NativeWindow_NativeWindowAbortBuffer(window_, current_->windowBuffer);
    current_ = nullptr;
}

bool NativeWindowVkTarget::Configure(OHNativeWindow* window, uint32_t width,
                                     uint32_t height, VkPhysicalDevice physical,
                                     VkDevice device)
{
    if (!window || !width || !height || !physical || !device) return false;
    if (window_ == window && width_ == width && height_ == height &&
        device_ == device)
        return true;
    Reset();
    window_ = window;
    width_ = width;
    height_ = height;
    physical_ = physical;
    device_ = device;
    getProps_ = reinterpret_cast<PFN_vkGetNativeBufferPropertiesOHOS>(
        vkGetDeviceProcAddr(device_, "vkGetNativeBufferPropertiesOHOS"));
    acquireImage_ = reinterpret_cast<PFN_vkAcquireImageOHOS>(
        vkGetDeviceProcAddr(device_, "vkAcquireImageOHOS"));
    releaseImage_ = reinterpret_cast<PFN_vkQueueSignalReleaseImageOHOS>(
        vkGetDeviceProcAddr(device_, "vkQueueSignalReleaseImageOHOS"));
    if (!getProps_ || !acquireImage_ || !releaseImage_) return false;
    OH_NativeWindow_NativeWindowHandleOpt(
        window_, SET_BUFFER_GEOMETRY, static_cast<int32_t>(width),
        static_cast<int32_t>(height));
    OH_NativeWindow_NativeWindowHandleOpt(
        window_, SET_USAGE,
        static_cast<uint64_t>(NATIVEBUFFER_USAGE_HW_RENDER |
                              NATIVEBUFFER_USAGE_HW_TEXTURE));
    OH_NativeWindow_NativeWindowHandleOpt(window_, SET_TIMEOUT, 0);
    return true;
}

void NativeWindowVkTarget::DestroySlot(VkDirectSlot& slot)
{
    if (device_ && slot.image) vkDestroyImage(device_, slot.image, nullptr);
    if (device_ && slot.memory) vkFreeMemory(device_, slot.memory, nullptr);
    slot = VkDirectSlot{};
}

void NativeWindowVkTarget::Reset()
{
    CloseFd(&pendingAcquireFd_);
    AbortFrame();
    for (VkDirectSlot& slot : cache_) DestroySlot(slot);
    window_ = nullptr;
    current_ = nullptr;
    device_ = VK_NULL_HANDLE;
    physical_ = VK_NULL_HANDLE;
}

VkDirectSlot* NativeWindowVkTarget::ImportLocked(OHNativeWindowBuffer* windowBuffer)
{
    OH_NativeBuffer* native = nullptr;
    if (OH_NativeBuffer_FromNativeWindowBuffer(windowBuffer, &native) != 0 || !native)
        return nullptr;
    const uint32_t seq = OH_NativeBuffer_GetSeqNum(native);
    for (VkDirectSlot& slot : cache_) {
        if (slot.seq == seq && slot.image) {
            slot.windowBuffer = windowBuffer;
            slot.nativeBuffer = native;
            timeline_.AddCacheHit();
            return &slot;
        }
    }
    timeline_.AddCacheMiss();
    VkDirectSlot* empty = nullptr;
    for (VkDirectSlot& slot : cache_) {
        if (!slot.seq) {
            empty = &slot;
            break;
        }
    }
    if (!empty) {
        DestroySlot(cache_[0]);
        empty = &cache_[0];
    }

    VkNativeBufferFormatPropertiesOHOS formatProps{
        VK_STRUCTURE_TYPE_NATIVE_BUFFER_FORMAT_PROPERTIES_OHOS};
    VkNativeBufferPropertiesOHOS props{VK_STRUCTURE_TYPE_NATIVE_BUFFER_PROPERTIES_OHOS};
    props.pNext = &formatProps;
    if (getProps_(device_, native, &props) != VK_SUCCESS) return nullptr;
    const VkFormat imageFormat = formatProps.format != VK_FORMAT_UNDEFINED
        ? formatProps.format : VK_FORMAT_R8G8B8A8_UNORM;

    VkExternalMemoryImageCreateInfo external{
        VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OHOS_NATIVE_BUFFER_BIT_OHOS;
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.pNext = &external;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = imageFormat;
    imageInfo.extent = {width_, height_, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage image = VK_NULL_HANDLE;
    if (vkCreateImage(device_, &imageInfo, nullptr, &image) != VK_SUCCESS)
        return nullptr;

    VkImportNativeBufferInfoOHOS import{VK_STRUCTURE_TYPE_IMPORT_NATIVE_BUFFER_INFO_OHOS};
    import.buffer = native;
    VkMemoryDedicatedAllocateInfo dedicated{
        VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.pNext = &import;
    dedicated.image = image;
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.pNext = &dedicated;
    alloc.allocationSize = props.allocationSize;
    alloc.memoryTypeIndex = PickMemoryType(physical_, props.memoryTypeBits);
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (alloc.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(device_, &alloc, nullptr, &memory) != VK_SUCCESS) {
        vkDestroyImage(device_, image, nullptr);
        return nullptr;
    }
    if (vkBindImageMemory(device_, image, memory, 0) != VK_SUCCESS) {
        vkFreeMemory(device_, memory, nullptr);
        vkDestroyImage(device_, image, nullptr);
        return nullptr;
    }
    empty->seq = seq;
    empty->windowBuffer = windowBuffer;
    empty->nativeBuffer = native;
    empty->image = image;
    empty->memory = memory;
    empty->format = imageFormat;
    OH_LOG_INFO(LOG_APP, "[NW-DIRECT] VK import seq=%{public}u", seq);
    return empty;
}

bool NativeWindowVkTarget::BeginFrame()
{
    if (!window_ || !device_) return false;
    OHNativeWindowBuffer* buffer = nullptr;
    int fenceFd = -1;
    const uint64_t started = NowUs();
    const int32_t result =
        OH_NativeWindow_NativeWindowRequestBuffer(window_, &buffer, &fenceFd);
    requestUs_ = NowUs() - started;
    if (result != 0 || !buffer) {
        CloseFd(&fenceFd);
        return false;
    }
    current_ = ImportLocked(buffer);
    if (!current_) {
        OH_NativeWindow_NativeWindowAbortBuffer(window_, buffer);
        CloseFd(&fenceFd);
        return false;
    }
    pendingAcquireFd_ = fenceFd;
    return true;
}

bool NativeWindowVkTarget::AcquireGpu(VkSemaphore semaphore, VkFence fence)
{
    if (!current_ || !acquireImage_) return false;
    const VkResult result = acquireImage_(
        device_, current_->image, pendingAcquireFd_, semaphore, fence);
    pendingAcquireFd_ = -1;
    if (result != VK_SUCCESS) {
        AbortFrame();
        return false;
    }
    return true;
}

int NativeWindowVkTarget::SignalRelease(VkQueue queue, uint32_t waitSemaphoreCount,
                                        const VkSemaphore* waitSemaphores)
{
    if (!current_ || !releaseImage_) return -1;
    int fenceFd = -1;
    if (releaseImage_(queue, waitSemaphoreCount, waitSemaphores, current_->image,
                      &fenceFd) != VK_SUCCESS)
        return -1;
    return fenceFd;
}

bool NativeWindowVkTarget::EndFrame(int renderFenceFd)
{
    if (!window_ || !current_ || !current_->windowBuffer) {
        CloseFd(&renderFenceFd);
        return false;
    }
    Region region{};
    region.rects = nullptr;
    region.rectNumber = 0;
    const uint64_t started = NowUs();
    const int32_t result = OH_NativeWindow_NativeWindowFlushBuffer(
        window_, current_->windowBuffer, renderFenceFd, region);
    flushUs_ = NowUs() - started;
    if (result != 0) {
        CloseFd(&renderFenceFd);
        OH_NativeWindow_NativeWindowAbortBuffer(window_, current_->windowBuffer);
        current_ = nullptr;
        return false;
    }
    timeline_.Add(requestUs_ + flushUs_, requestUs_, flushUs_, 0, 0);
    current_ = nullptr;
    return true;
}

void NativeWindowVkTarget::AbortFrame()
{
    CloseFd(&pendingAcquireFd_);
    if (window_ && current_ && current_->windowBuffer)
        OH_NativeWindow_NativeWindowAbortBuffer(window_, current_->windowBuffer);
    current_ = nullptr;
}

} // namespace winehua
