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
#include <poll.h>
#include <unistd.h>
#include <vector>

#ifndef EGL_NATIVE_BUFFER_OHOS
#define EGL_NATIVE_BUFFER_OHOS 0x34E1
#endif
#ifndef EGL_IMAGE_PRESERVED_KHR
#define EGL_IMAGE_PRESERVED_KHR 0x30D2
#endif
#ifndef EGL_SYNC_NATIVE_FENCE_ANDROID
#define EGL_SYNC_NATIVE_FENCE_ANDROID 0x3144
#endif
#ifndef EGL_SYNC_NATIVE_FENCE_FD_ANDROID
#define EGL_SYNC_NATIVE_FENCE_FD_ANDROID 0x3145
#endif
#ifndef EGL_NO_NATIVE_FENCE_FD_ANDROID
#define EGL_NO_NATIVE_FENCE_FD_ANDROID (-1)
#endif

/* RequestBuffer timeout 0 returns immediately and never waits for a free
 * NativeImage slot. Warmup uses kDirectFirstBufferTimeoutMs so the first
 * GPU allocation can finish. After that, kDirectQueuePaceTimeoutMs is 0:
 * blocking dequeue deadlocks with NativeImage consume (same SET_TIMEOUT).
 * Do not use the OHOS 3000 ms default. */
constexpr int32_t kRequestTimeoutMs = winehua::kDirectFirstBufferTimeoutMs;

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "virgl-presenter"

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
PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC gImageTargetRbo;
PFNEGLCREATESYNCKHRPROC gCreateSync;
PFNEGLDESTROYSYNCKHRPROC gDestroySync;
PFNEGLWAITSYNCKHRPROC gWaitSync;
PFNEGLDUPNATIVEFENCEFDANDROIDPROC gDupFenceFd;
bool gEglProcsLoaded;

template <typename T>
T LoadProc(const char* name)
{
    return reinterpret_cast<T>(eglGetProcAddress(name));
}

void LoadEglProcs()
{
    if (!gCreateImage) gCreateImage = LoadProc<PFNEGLCREATEIMAGEKHRPROC>("eglCreateImageKHR");
    if (!gCreateImage) gCreateImage = LoadProc<PFNEGLCREATEIMAGEKHRPROC>("eglCreateImage");
    if (!gDestroyImage) gDestroyImage = LoadProc<PFNEGLDESTROYIMAGEKHRPROC>("eglDestroyImageKHR");
    if (!gDestroyImage) gDestroyImage = LoadProc<PFNEGLDESTROYIMAGEKHRPROC>("eglDestroyImage");
    if (!gImageTargetTexture)
        gImageTargetTexture = LoadProc<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
            "glEGLImageTargetTexture2DOES");
    if (!gImageTargetRbo)
        gImageTargetRbo = LoadProc<PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC>(
            "glEGLImageTargetRenderbufferStorageOES");
    if (!gCreateSync) gCreateSync = LoadProc<PFNEGLCREATESYNCKHRPROC>("eglCreateSyncKHR");
    if (!gCreateSync) gCreateSync = LoadProc<PFNEGLCREATESYNCKHRPROC>("eglCreateSync");
    if (!gDestroySync) gDestroySync = LoadProc<PFNEGLDESTROYSYNCKHRPROC>("eglDestroySyncKHR");
    if (!gDestroySync) gDestroySync = LoadProc<PFNEGLDESTROYSYNCKHRPROC>("eglDestroySync");
    if (!gWaitSync) gWaitSync = LoadProc<PFNEGLWAITSYNCKHRPROC>("eglWaitSyncKHR");
    if (!gWaitSync) gWaitSync = LoadProc<PFNEGLWAITSYNCKHRPROC>("eglWaitSync");
    if (!gDupFenceFd)
        gDupFenceFd = LoadProc<PFNEGLDUPNATIVEFENCEFDANDROIDPROC>("eglDupNativeFenceFDANDROID");
    gEglProcsLoaded = gCreateImage && gImageTargetTexture;
}

void StampFlushTimestamp(OHNativeWindow* window)
{
    if (!window) return;
    const uint64_t timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch()).count());
    OH_NativeWindow_NativeWindowHandleOpt(window, SET_UI_TIMESTAMP, timestamp);
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

int32_t NativePixelFormat(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
        return NATIVEBUFFER_PIXEL_FMT_BGRA_8888;
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    default:
        return NATIVEBUFFER_PIXEL_FMT_RGBA_8888;
    }
}

EGLImageKHR CreateOhosImage(EGLDisplay display, EGLContext context,
                            EGLClientBuffer client, const EGLint* attribs)
{
    eglGetError();
    if (!gCreateImage || !client) return EGL_NO_IMAGE_KHR;
    return gCreateImage(display, context, EGL_NATIVE_BUFFER_OHOS, client, attribs);
}

bool AttachImageToFramebuffer(EGLImageKHR image, GLuint* texture, GLuint* renderbuffer,
                              GLuint* framebuffer, GLenum* statusOut)
{
    *texture = 0;
    *renderbuffer = 0;
    *framebuffer = 0;
    *statusOut = 0;
    glGenFramebuffers(1, framebuffer);

    if (gImageTargetTexture) {
        glGenTextures(1, texture);
        glBindTexture(GL_TEXTURE_2D, *texture);
        gImageTargetTexture(GL_TEXTURE_2D, image);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindFramebuffer(GL_FRAMEBUFFER, *framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               *texture, 0);
        *statusOut = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindTexture(GL_TEXTURE_2D, 0);
        if (*statusOut == GL_FRAMEBUFFER_COMPLETE) {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            return true;
        }
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        glDeleteTextures(1, texture);
        *texture = 0;
    }

    if (gImageTargetRbo) {
        glGenRenderbuffers(1, renderbuffer);
        glBindRenderbuffer(GL_RENDERBUFFER, *renderbuffer);
        gImageTargetRbo(GL_RENDERBUFFER, image);
        glBindFramebuffer(GL_FRAMEBUFFER, *framebuffer);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                  GL_RENDERBUFFER, *renderbuffer);
        *statusOut = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
        if (*statusOut == GL_FRAMEBUFFER_COMPLETE) {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            return true;
        }
        glDeleteRenderbuffers(1, renderbuffer);
        *renderbuffer = 0;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, framebuffer);
    *framebuffer = 0;
    return false;
}

} // namespace

bool DirectNativeWindowEnabled()
{
    const char* value = std::getenv("WINEHUA_DIRECT_NATIVEWINDOW");
    if (!value || !value[0]) return true;
    return value[0] != '0';
}

bool ScanoutForceBlitRequested()
{
    return access("/data/storage/el2/base/temp/winehua_scanout_force_blit", F_OK) == 0;
}

bool ScanoutBackingEnabled()
{
    if (!DirectNativeWindowEnabled()) return false;
    if (ScanoutForceBlitRequested()) return false;
    const char* value = std::getenv("WINEHUA_SCANOUT_BACKING");
    if (value && value[0] == '0') return false;
    return true;
}

bool VenusScanoutBackingEnabled()
{
    if (!DirectNativeWindowEnabled()) return false;
    const char* value = std::getenv("WINEHUA_VENUS_SCANOUT_BACKING");
    return value && value[0] == '1' && !value[1];
}

bool ScanoutGlFinishEnabled()
{
    const char* value = std::getenv("WINEHUA_SCANOUT_GLFINISH");
    return value && value[0] == '1';
}

bool NativeImageDropUnreadRequested()
{
    const char* value = std::getenv("WINEHUA_NATIVEIMAGE_DROP");
    return value && value[0] == '1' && !value[1];
}

bool PresentUncapRequested()
{
    const char* value = std::getenv("WINEHUA_PRESENT_UNCAP");
    if (value && value[0] == '1' && !value[1])
        return true;
    return access("/data/storage/el2/base/temp/winehua_present_uncap", F_OK) == 0;
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
        "render_fence=%llu p50=%llu p90=%llu p99=%llu max=%llu us",
        tag ? tag : "DIRECT", serial,
        static_cast<unsigned long long>(p.count),
        static_cast<unsigned long long>(cacheHits_),
        static_cast<unsigned long long>(cacheMisses_),
        static_cast<unsigned long long>(gpuCopies_),
        static_cast<unsigned long long>(skippedCopies_),
        static_cast<unsigned long long>(cpuFenceWaits_),
        static_cast<unsigned long long>(glFinishCount_),
        static_cast<unsigned long long>(renderFenceCount_),
        static_cast<unsigned long long>(p.p50),
        static_cast<unsigned long long>(p.p90),
        static_cast<unsigned long long>(p.p99),
        static_cast<unsigned long long>(p.max));
    return line;
}

bool NativeWindowGlesTarget::EnsureContext()
{
    LoadEglProcs();
    if (!gEglProcsLoaded) {
        static bool logged;
        if (!logged) {
            logged = true;
            OH_LOG_ERROR(LOG_APP,
                         "[NW-DIRECT] missing EGLImage procs image=%{public}d target=%{public}d "
                         "rbo=%{public}d sync=%{public}d wait=%{public}d dup=%{public}d",
                         gCreateImage ? 1 : 0, gImageTargetTexture ? 1 : 0,
                         gImageTargetRbo ? 1 : 0,
                         gCreateSync ? 1 : 0, gWaitSync ? 1 : 0, gDupFenceFd ? 1 : 0);
        }
        return false;
    }
    if (display_ && context_) return true;

    /* NativeBuffer EGLImage is a Harmony EGL extension. Do not borrow the
     * virgl context: that display may not implement EGL_NATIVE_BUFFER_OHOS.
     * Share with the current virgl context so its textures stay visible. */
    const EGLDisplay currentDisplay = eglGetCurrentDisplay();
    const EGLContext share = eglGetCurrentContext();
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) display = currentDisplay;
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
    const EGLContext shareContext =
        (share != EGL_NO_CONTEXT && currentDisplay == display) ? share : EGL_NO_CONTEXT;
    EGLContext context = eglCreateContext(display, config, shareContext, ctxAttrs);
    if (context == EGL_NO_CONTEXT) {
        eglDestroySurface(display, pbuffer);
        return false;
    }
    if (eglMakeCurrent(display, pbuffer, pbuffer, context) != EGL_TRUE) {
        eglDestroyContext(display, context);
        eglDestroySurface(display, pbuffer);
        return false;
    }
    display_ = display;
    context_ = context;
    pbuffer_ = pbuffer;
    borrowedContext_ = false;
    sharedWithVrend_ = shareContext != EGL_NO_CONTEXT;
    LoadEglProcs();
    OH_LOG_INFO(LOG_APP,
                "[NW-DIRECT] GLES context ready shared=%{public}d "
                "direct_dpy=%{public}p vrend_dpy=%{public}p same_dpy=%{public}d",
                sharedWithVrend_ ? 1 : 0, display, currentDisplay,
                display == currentDisplay ? 1 : 0);
    return gEglProcsLoaded;
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
    OH_NativeWindow_NativeWindowHandleOpt(window_, SET_TIMEOUT, kRequestTimeoutMs);
    int32_t timeoutMs = -1;
    OH_NativeWindow_NativeWindowHandleOpt(window_, GET_TIMEOUT, &timeoutMs);
    static bool loggedConfig;
    if (!loggedConfig) {
        loggedConfig = true;
        int32_t queueSize = 0;
        OH_NativeWindow_NativeWindowHandleOpt(window_, GET_BUFFERQUEUE_SIZE, &queueSize);
        OH_LOG_INFO(LOG_APP,
                    "[NW-DIRECT] GLES configure size=%{public}ux%{public}u "
                    "timeout_ms=%{public}d queue=%{public}d",
                    width, height, timeoutMs, queueSize);
    }
    return EnsureContext();
}

void NativeWindowGlesTarget::DestroySlot(GlesDirectSlot& slot)
{
    EGLDisplay display = static_cast<EGLDisplay>(display_);
    if (slot.framebuffer) glDeleteFramebuffers(1, &slot.framebuffer);
    if (slot.renderbuffer) glDeleteRenderbuffers(1, &slot.renderbuffer);
    if (slot.scanoutTexture) glDeleteTextures(1, &slot.scanoutTexture);
    if (slot.texture) glDeleteTextures(1, &slot.texture);
    if (slot.eglImage && gDestroyImage && display)
        gDestroyImage(display, static_cast<EGLImageKHR>(slot.eglImage));
    slot = GlesDirectSlot{};
}

void NativeWindowGlesTarget::Reset()
{
    AbortFrame();
    CloseFd(&pendingAcquireFd_);
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
    sharedWithVrend_ = false;
    window_ = nullptr;
    current_ = nullptr;
    width_ = 0;
    height_ = 0;
}

GlesDirectSlot* NativeWindowGlesTarget::ImportLocked(OHNativeWindowBuffer* windowBuffer)
{
    OH_NativeBuffer* native = nullptr;
    const int32_t fromRet =
        OH_NativeBuffer_FromNativeWindowBuffer(windowBuffer, &native);
    const uint32_t seq = (fromRet == 0 && native)
        ? OH_NativeBuffer_GetSeqNum(native)
        : static_cast<uint32_t>(reinterpret_cast<uintptr_t>(windowBuffer));
    for (GlesDirectSlot& slot : cache_) {
        if (slot.seq == seq && slot.framebuffer) {
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
    const EGLContext current = eglGetCurrentContext();
    const EGLint preserved[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    EGLClientBuffer clients[2] = {
        static_cast<EGLClientBuffer>(windowBuffer),
        static_cast<EGLClientBuffer>(native),
    };
    const EGLContext contexts[2] = {EGL_NO_CONTEXT, current};
    const EGLint* attribSets[2] = {nullptr, preserved};

    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    EGLint imageError = EGL_SUCCESS;
    for (EGLClientBuffer client : clients) {
        if (!client) continue;
        for (EGLContext context : contexts) {
            for (const EGLint* attribs : attribSets) {
                image = CreateOhosImage(display, context, client, attribs);
                if (image != EGL_NO_IMAGE_KHR) break;
                imageError = eglGetError();
            }
            if (image != EGL_NO_IMAGE_KHR) break;
        }
        if (image != EGL_NO_IMAGE_KHR) break;
    }
    if (image == EGL_NO_IMAGE_KHR) {
        lastRequestResult_ = static_cast<int32_t>(imageError);
        lastBeginReason_ = "egl-image";
        static int logged;
        if (logged < 3 || (logged % 120) == 0)
            OH_LOG_ERROR(LOG_APP,
                         "[NW-DIRECT] eglCreateImage failed from_nb=%{public}d "
                         "egl=0x%{public}x native=%{public}d",
                         fromRet, imageError, native ? 1 : 0);
        ++logged;
        return nullptr;
    }

    GLuint texture = 0;
    GLuint renderbuffer = 0;
    GLuint framebuffer = 0;
    GLenum status = 0;
    if (!AttachImageToFramebuffer(image, &texture, &renderbuffer, &framebuffer, &status)) {
        lastRequestResult_ = static_cast<int32_t>(status);
        lastBeginReason_ = "fbo";
        static int logged;
        if (logged < 3 || (logged % 120) == 0)
            OH_LOG_ERROR(LOG_APP,
                         "[NW-DIRECT] FBO incomplete status=0x%{public}x rbo_proc=%{public}d",
                         status, gImageTargetRbo ? 1 : 0);
        ++logged;
        if (gDestroyImage) gDestroyImage(display, image);
        return nullptr;
    }
    empty->seq = seq;
    empty->windowBuffer = windowBuffer;
    empty->nativeBuffer = native;
    empty->eglImage = image;
    empty->texture = texture;
    empty->renderbuffer = renderbuffer;
    empty->framebuffer = framebuffer;
    OH_LOG_INFO(LOG_APP,
                "[NW-DIRECT] GLES import seq=%{public}u tex=%{public}u rbo=%{public}u "
                "fbo=%{public}u from_nb=%{public}d",
                seq, texture, renderbuffer, framebuffer, fromRet);
    return empty;
}

bool NativeWindowGlesTarget::GpuWaitAcquireFd(int fenceFd)
{
    if (fenceFd < 0) return true;
    EGLDisplay display = eglGetCurrentDisplay();
    if (display == EGL_NO_DISPLAY)
        display = static_cast<EGLDisplay>(display_);
    if (display != EGL_NO_DISPLAY && gCreateSync && gWaitSync && gDestroySync) {
        const EGLint attribs[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fenceFd, EGL_NONE};
        EGLSyncKHR sync = gCreateSync(display, EGL_SYNC_NATIVE_FENCE_ANDROID, attribs);
        if (sync != EGL_NO_SYNC_KHR) {
            gWaitSync(display, sync, 0);
            gDestroySync(display, sync);
            return true;
        }
    }
    pollfd wait{fenceFd, POLLIN, 0};
    poll(&wait, 1, kRequestTimeoutMs);
    timeline_.AddCpuFenceWait();
    CloseFd(&fenceFd);
    return true;
}

bool NativeWindowGlesTarget::MakeCurrent()
{
    if (!EnsureContext()) return false;
    const EGLDisplay display = static_cast<EGLDisplay>(display_);
    if (eglMakeCurrent(display, static_cast<EGLSurface>(pbuffer_),
                       static_cast<EGLSurface>(pbuffer_),
                       static_cast<EGLContext>(context_)) != EGL_TRUE) {
        lastBeginReason_ = "make-current";
        lastRequestResult_ = static_cast<int32_t>(eglGetError());
        return false;
    }
    return true;
}

bool NativeWindowGlesTarget::BeginFrame()
{
    lastBeginReason_ = "ok";
    lastRequestResult_ = 0;
    if (!EnsureContext() || !window_) {
        lastBeginReason_ = !window_ ? "no-window" : "no-context";
        return false;
    }
    if (!borrowedContext_) {
        const EGLDisplay display = static_cast<EGLDisplay>(display_);
        if (eglMakeCurrent(display, static_cast<EGLSurface>(pbuffer_),
                           static_cast<EGLSurface>(pbuffer_),
                           static_cast<EGLContext>(context_)) != EGL_TRUE) {
            lastBeginReason_ = "make-current";
            lastRequestResult_ = static_cast<int32_t>(eglGetError());
            return false;
        }
    }
    OHNativeWindowBuffer* buffer = nullptr;
    int fenceFd = -1;
    const uint64_t started = NowUs();
    const int32_t result =
        OH_NativeWindow_NativeWindowRequestBuffer(window_, &buffer, &fenceFd);
    requestUs_ = NowUs() - started;
    lastRequestResult_ = result;
    if (result != 0 || !buffer) {
        CloseFd(&fenceFd);
        lastBeginReason_ = "request-buffer";
        OH_LOG_ERROR(LOG_APP,
                     "[NW-DIRECT] GLES RequestBuffer failed ret=%{public}d "
                     "wait_us=%{public}llu",
                     result, static_cast<unsigned long long>(requestUs_));
        return false;
    }
    current_ = ImportLocked(buffer);
    if (!current_) {
        OH_NativeWindow_NativeWindowAbortBuffer(window_, buffer);
        CloseFd(&fenceFd);
        if (!lastBeginReason_ || lastBeginReason_ == "ok" || lastBeginReason_[0] == '\0')
            lastBeginReason_ = "import";
        return false;
    }
    CloseFd(&pendingAcquireFd_);
    pendingAcquireFd_ = fenceFd;
    glBindFramebuffer(GL_FRAMEBUFFER, current_->framebuffer);
    glViewport(0, 0, static_cast<GLsizei>(width_), static_cast<GLsizei>(height_));
    return true;
}

bool NativeWindowGlesTarget::WaitAcquireOnCurrent()
{
    const int fd = pendingAcquireFd_;
    pendingAcquireFd_ = -1;
    return GpuWaitAcquireFd(fd);
}

void NativeWindowGlesTarget::PrepareBlitFramebuffer()
{
    if (!current_ || !current_->framebuffer || !current_->texture) return;
    glBindFramebuffer(GL_FRAMEBUFFER, current_->framebuffer);
    if (current_->colorDetached) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               current_->texture, 0);
        current_->colorDetached = false;
    }
    glViewport(0, 0, static_cast<GLsizei>(width_), static_cast<GLsizei>(height_));
}

void NativeWindowGlesTarget::DetachColorForScanout()
{
    if (!current_ || !current_->framebuffer) return;
    glBindFramebuffer(GL_FRAMEBUFFER, current_->framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    current_->colorDetached = true;
}

unsigned NativeWindowGlesTarget::ScanoutTextureOnCurrent()
{
    if (!current_ || !current_->eglImage || !gImageTargetTexture) return 0;
    if (current_->scanoutTexture) return current_->scanoutTexture;
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    gImageTargetTexture(GL_TEXTURE_2D, static_cast<EGLImageKHR>(current_->eglImage));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    current_->scanoutTexture = tex;
    return tex;
}

bool NativeWindowGlesTarget::IsScanoutTexture(unsigned gl) const
{
    if (!gl) return false;
    for (const auto& slot : cache_) {
        if (slot.scanoutTexture == gl) return true;
    }
    return false;
}

int NativeWindowGlesTarget::CreateNativeFenceFd()
{
    if (!gCreateSync || !gDupFenceFd || !gDestroySync) return -1;
    EGLDisplay display = eglGetCurrentDisplay();
    if (display == EGL_NO_DISPLAY)
        display = static_cast<EGLDisplay>(display_);
    if (!display || display == EGL_NO_DISPLAY) return -1;
    glFlush();
    EGLSyncKHR sync = gCreateSync(display, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr);
    if (sync == EGL_NO_SYNC_KHR) return -1;
    const int fd = gDupFenceFd(display, sync);
    gDestroySync(display, sync);
    if (fd == EGL_NO_NATIVE_FENCE_FD_ANDROID) return -1;
    return fd;
}

bool NativeWindowGlesTarget::EndFrameWithRenderFence()
{
    int fenceFd = -1;
    if (!ScanoutGlFinishEnabled())
        fenceFd = CreateNativeFenceFd();
    if (fenceFd >= 0) {
        timeline_.AddRenderFence();
    } else {
        static int logged;
        if (logged < 4) {
            OH_LOG_WARN(LOG_APP,
                        "[NW-DIRECT] render fence unavailable, glFinish fallback "
                        "force=%{public}d",
                        ScanoutGlFinishEnabled() ? 1 : 0);
            ++logged;
        }
        glFinish();
        timeline_.AddGlFinish();
        fenceFd = -1;
    }
    return EndFrame(fenceFd);
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
    StampFlushTimestamp(window_);
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
    CloseFd(&pendingAcquireFd_);
    if (window_ && current_ && current_->windowBuffer)
        OH_NativeWindow_NativeWindowAbortBuffer(window_, current_->windowBuffer);
    current_ = nullptr;
}

bool NativeWindowVkTarget::Configure(OHNativeWindow* window, uint32_t width,
                                     uint32_t height, VkPhysicalDevice physical,
                                     VkDevice device, VkFormat sourceFormat)
{
    if (!window || !width || !height || !physical || !device) return false;
    const int32_t nativeFormat = NativePixelFormat(sourceFormat);
    if (window_ == window && width_ == width && height_ == height &&
        device_ == device && nativeFormat_ == nativeFormat)
        return true;
    Reset();
    window_ = window;
    width_ = width;
    height_ = height;
    nativeFormat_ = nativeFormat;
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
    OH_NativeWindow_NativeWindowHandleOpt(window_, SET_FORMAT, nativeFormat_);
    appliedTimeoutMs_ = 0x7fffffff;
    SetRequestTimeoutMs(kDirectFirstBufferTimeoutMs);
    OH_LOG_INFO(LOG_APP,
                "[NW-DIRECT] VK configure size=%{public}ux%{public}u "
                "vk_format=%{public}u native_format=%{public}d",
                width, height, static_cast<uint32_t>(sourceFormat), nativeFormat_);
    return true;
}

void NativeWindowVkTarget::SetRequestTimeoutMs(int32_t timeoutMs)
{
    if (!window_ || appliedTimeoutMs_ == timeoutMs)
        return;
    const int32_t setRet =
        OH_NativeWindow_NativeWindowHandleOpt(window_, SET_TIMEOUT, timeoutMs);
    int32_t got = timeoutMs;
    OH_NativeWindow_NativeWindowHandleOpt(window_, GET_TIMEOUT, &got);
    appliedTimeoutMs_ = timeoutMs;
    OH_LOG_INFO(LOG_APP,
                "[NW-DIRECT] VK SET_TIMEOUT want=%{public}d got=%{public}d "
                "ret=%{public}d",
                timeoutMs, got, setRet);
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
    nativeFormat_ = 0;
    appliedTimeoutMs_ = 0x7fffffff;
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
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                      VK_IMAGE_USAGE_SAMPLED_BIT;
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
    OH_LOG_INFO(LOG_APP,
                "[NW-DIRECT] VK import seq=%{public}u vk_format=%{public}u "
                "native_format=%{public}d",
                seq, static_cast<uint32_t>(imageFormat), nativeFormat_);
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
    lastRequestResult_ = result;
    if (result != 0 || !buffer) {
        lastBeginReason_ = "request";
        CloseFd(&fenceFd);
        static bool logged;
        if (!logged) {
            logged = true;
            OH_LOG_ERROR(LOG_APP,
                         "[NW-DIRECT] VK RequestBuffer failed ret=%{public}d",
                         result);
        }
        return false;
    }
    current_ = ImportLocked(buffer);
    if (!current_) {
        lastBeginReason_ = "import";
        lastRequestResult_ = -1;
        OH_NativeWindow_NativeWindowAbortBuffer(window_, buffer);
        CloseFd(&fenceFd);
        static bool logged;
        if (!logged) {
            logged = true;
            OH_LOG_ERROR(LOG_APP, "[NW-DIRECT] VK import NativeBuffer failed");
        }
        return false;
    }
    lastBeginReason_ = "ok";
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
    StampFlushTimestamp(window_);
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
