#include "virgl_surface_presenter.h"
#include "venus_surface_presenter.h"
#include "native_window_lease.h"
#include "native_window_direct.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <hilog/log.h>
#include <native_buffer/native_buffer.h>
#include <native_window/external_window.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <unistd.h>
#include <unordered_map>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "virgl-presenter"

namespace {

using SteadyClock = std::chrono::steady_clock;

constexpr uint64_t kDefaultFramePeriodNs = 16666667;
constexpr uint64_t kMinFramePeriodNs = 4000000;
constexpr uint64_t kMaxFramePeriodNs = 33333333;
constexpr uint64_t kProducerDispatchLeadNs = 500000;
constexpr auto kVenusTargetAttachTimeout = std::chrono::milliseconds(2500);

uint64_t NormalizeFramePeriodNs(uint64_t framePeriodNs)
{
    if (!framePeriodNs) return kDefaultFramePeriodNs;
    return std::clamp(framePeriodNs, kMinFramePeriodNs, kMaxFramePeriodNs);
}

uint64_t PacingPeriodNs(uint64_t displayPeriodNs)
{
    return displayPeriodNs > kMinFramePeriodNs + kProducerDispatchLeadNs
        ? displayPeriodNs - kProducerDispatchLeadNs : kMinFramePeriodNs;
}

uint64_t NowUs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        SteadyClock::now().time_since_epoch()).count());
}

uint64_t NowNs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        SteadyClock::now().time_since_epoch()).count());
}

bool PresentPerfSummaryEnabled()
{
    const char* summary = std::getenv("WINEHUA_VTEST_PRESENT_PERF_SUMMARY");
    return summary && summary[0] == '1' && !summary[1];
}

/* First presents keep the original deadline so MAIN can attach NativeImage.
 * After that, deadline is 0: the producer waits on RequestBuffer when the
 * queue is full instead of sleeping until the next vsync timestamp. */
constexpr uint64_t kPresentUncapWarmupFrames = 24;

bool PresentUncapArmed(uint64_t frames)
{
    return winehua::PresentUncapRequested() && frames >= kPresentUncapWarmupFrames;
}

uint64_t NextPresentDeadlineNs(uint64_t lastPresentNs, uint64_t framePeriodNs,
                               uint64_t frames)
{
    return PresentUncapArmed(frames) ? 0 : lastPresentNs + framePeriodNs;
}

void (*gColorRemapFn)(uint32_t, uint32_t) = nullptr;
int (*gSetScanoutBacking)(uint32_t, uint32_t, void*) = nullptr;
int (*gClearScanoutBacking)(uint32_t) = nullptr;
int (*gScanoutLastWrite)(uint32_t, uint32_t*, uint32_t*, const char**) = nullptr;
int (*gScanoutGeneration)(uint32_t, uint64_t*, uint64_t*, uint32_t*) = nullptr;

GLuint CompilePresentShader(GLenum type, const char* source)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE) return shader;

    char log[512] = {};
    glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
    OH_LOG_ERROR(LOG_APP, "[VIRGL-ZC][NCP] shader compile failed: %{public}s", log);
    glDeleteShader(shader);
    return 0;
}

class SurfaceQueueTarget {
public:
    int Attach(uint64_t surfaceKey, uint64_t framePeriodNs,
               OHNativeWindow* window, bool releaseWindowWithUnreference)
    {
        if (!surfaceKey || !window) return -1;
        std::lock_guard<std::mutex> lock(mutex_);
        ResetGlLocked();
        windowLease_.Adopt(
            window, releaseWindowWithUnreference
                ? winehua::NativeWindowReleaseMode::UnreferenceNativeObject
                : winehua::NativeWindowReleaseMode::DestroyParcelWindow);
        surfaceKey_ = surfaceKey;
        width_ = 0;
        height_ = 0;
        frames_ = 0;
        failures_ = 0;
        timestampFailures_ = 0;
        throttled_ = 0;
        lastPresentNs_ = 0;
        displayPeriodNs_ = NormalizeFramePeriodNs(framePeriodNs);
        framePeriodNs_ = PacingPeriodNs(displayPeriodNs_);
        OH_LOG_INFO(LOG_APP,
                    "[VIRGL-ZC][NCP] target attached surface_key=%{public}llu "
                    "window=%{public}p display_period_us=%{public}llu "
                    "pace_period_us=%{public}llu",
                    static_cast<unsigned long long>(surfaceKey_), windowLease_.Get(),
                    static_cast<unsigned long long>(displayPeriodNs_ / 1000),
                    static_cast<unsigned long long>(framePeriodNs_ / 1000));
        if (winehua::PresentUncapRequested())
            OH_LOG_INFO(LOG_APP,
                        "[VIRGL-ZC][NCP] present uncap enabled surface_key=%{public}llu "
                        "(queue backpressure, no vsync sleep)",
                        static_cast<unsigned long long>(surfaceKey_));
        return 0;
    }

    int SetFramePeriod(uint64_t framePeriodNs)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const uint64_t displayPeriodNs = NormalizeFramePeriodNs(framePeriodNs);
        if (displayPeriodNs_ == displayPeriodNs) return 0;
        displayPeriodNs_ = displayPeriodNs;
        framePeriodNs_ = PacingPeriodNs(displayPeriodNs_);
        OH_LOG_INFO(LOG_APP,
                    "[VIRGL-ZC][NCP] frame period surface_key=%{public}llu "
                    "display_period_us=%{public}llu pace_period_us=%{public}llu",
                    static_cast<unsigned long long>(surfaceKey_),
                    static_cast<unsigned long long>(displayPeriodNs_ / 1000),
                    static_cast<unsigned long long>(framePeriodNs_ / 1000));
        return 0;
    }

    int Detach(uint64_t surfaceKey)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (surfaceKey_ && surfaceKey && surfaceKey_ != surfaceKey) return -1;
        ResetLocked();
        OH_LOG_INFO(LOG_APP, "[VIRGL-ZC][NCP] target detached surface_key=%{public}llu",
                    static_cast<unsigned long long>(surfaceKey));
        return 0;
    }

    int Present(uint32_t resHandle, GLuint texture, uint32_t width, uint32_t height,
                uint64_t drawable, uint32_t serial,
                uint64_t* nextPresentDeadlineNs)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (nextPresentDeadlineNs) *nextPresentDeadlineNs = 0;
        const EGLDisplay sourceDisplay = eglGetCurrentDisplay();
        const EGLContext sourceContext = eglGetCurrentContext();
        const EGLSurface sourceDraw = eglGetCurrentSurface(EGL_DRAW);
        const EGLSurface sourceRead = eglGetCurrentSurface(EGL_READ);
        const bool sourceVisible = sourceDisplay != EGL_NO_DISPLAY &&
            sourceContext != EGL_NO_CONTEXT && texture != 0 &&
            glIsTexture(texture) == GL_TRUE;
        GLsync sourceReady = nullptr;

        if (!windowLease_) return -2;
        if (!sourceVisible) return -3;
        const uint64_t nowNs = NowNs();
        if (!PresentUncapArmed(frames_) &&
            width_ == width && height_ == height && lastPresentNs_ &&
            nowNs - lastPresentNs_ < framePeriodNs_)
        {
            if (nextPresentDeadlineNs)
                *nextPresentDeadlineNs = lastPresentNs_ + framePeriodNs_;
            ++throttled_;
            return 1;
        }
        if (winehua::DirectNativeWindowEnabled())
            return PresentDirectLocked(resHandle, texture, width, height, serial,
                                       sourceDisplay, sourceContext,
                                       sourceDraw, sourceRead,
                                       nextPresentDeadlineNs);
        lastPresentNs_ = nowNs;
        sourceReady = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (!sourceReady) return -7;
        glFlush();
        if (!EnsureGlLocked(sourceDisplay, sourceContext, width, height))
        {
            eglMakeCurrent(sourceDisplay, sourceDraw, sourceRead, sourceContext);
            glDeleteSync(sourceReady);
            ++failures_;
            return -4;
        }
        if (eglMakeCurrent(display_, surface_, surface_, context_) != EGL_TRUE)
        {
            eglMakeCurrent(sourceDisplay, sourceDraw, sourceRead, sourceContext);
            glDeleteSync(sourceReady);
            ++failures_;
            return -5;
        }
        glWaitSync(sourceReady, 0, GL_TIMEOUT_IGNORED);
        glDeleteSync(sourceReady);

        glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_STENCIL_TEST);
        glUseProgram(program_);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
        glBindSampler(0, sampler_);
        glUniform1i(textureLocation_, 0);
        const uint64_t frameTimestamp = NowNs();
        const int32_t timestampResult = OH_NativeWindow_NativeWindowHandleOpt(
            windowLease_.Get(), SET_UI_TIMESTAMP, frameTimestamp);
        if (timestampResult != 0)
        {
            ++timestampFailures_;
            if (timestampFailures_ == 1 || timestampFailures_ % 120 == 0)
                OH_LOG_WARN(LOG_APP,
                            "[VIRGL-ZC][NCP] timestamp set failed serial=%{public}u "
                            "result=%{public}d failures=%{public}llu",
                            serial, timestampResult,
                            static_cast<unsigned long long>(timestampFailures_));
        }
        glDrawArrays(GL_TRIANGLES, 0, 3);
        const GLenum glError = glGetError();
        const EGLBoolean swapped = glError == GL_NO_ERROR
            ? eglSwapBuffers(display_, surface_) : EGL_FALSE;
        const EGLint eglError = swapped == EGL_TRUE ? EGL_SUCCESS : eglGetError();
        const EGLBoolean restored = eglMakeCurrent(
            sourceDisplay, sourceDraw, sourceRead, sourceContext);

        if (swapped != EGL_TRUE || restored != EGL_TRUE)
        {
            ++failures_;
            if (failures_ == 1 || failures_ % 120 == 0)
                OH_LOG_WARN(LOG_APP,
                            "[VIRGL-ZC][NCP] blit dropped serial=%{public}u gl=0x%{public}x "
                            "egl=0x%{public}x restore=%{public}d drops=%{public}llu",
                            serial, glError, eglError, restored,
                            static_cast<unsigned long long>(failures_));
            return -6;
        }

        ++frames_;
        if (nextPresentDeadlineNs)
            *nextPresentDeadlineNs = NextPresentDeadlineNs(
                lastPresentNs_, framePeriodNs_, frames_);
        if (PresentPerfSummaryEnabled() &&
            (frames_ == 1 || frames_ % 120 == 0))
        {
            OH_LOG_INFO(LOG_APP,
                        "[VIRGL-ZC][NCP] blit frames=%{public}llu surface_key=%{public}llu "
                        "serial=%{public}u drawable=0x%{public}llx tex=%{public}u "
                        "size=%{public}ux%{public}u display_period_us=%{public}llu "
                        "pace_period_us=%{public}llu "
                        "drops=%{public}llu throttled=%{public}llu",
                        static_cast<unsigned long long>(frames_),
                        static_cast<unsigned long long>(surfaceKey_), serial,
                        static_cast<unsigned long long>(drawable), texture, width, height,
                        static_cast<unsigned long long>(displayPeriodNs_ / 1000),
                        static_cast<unsigned long long>(framePeriodNs_ / 1000),
                        static_cast<unsigned long long>(failures_),
                        static_cast<unsigned long long>(throttled_));
        }
        return 0;
    }

    void Reset()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ResetLocked();
    }

private:
    bool EnsureGlLocked(EGLDisplay sourceDisplay, EGLContext sourceContext,
                        uint32_t width, uint32_t height)
    {
        if (display_ != EGL_NO_DISPLAY &&
            (display_ != sourceDisplay || width_ != width || height_ != height))
            ResetGlLocked();
        if (context_ != EGL_NO_CONTEXT && surface_ != EGL_NO_SURFACE) return true;

        if (OH_NativeWindow_NativeWindowHandleOpt(
                windowLease_.Get(), SET_BUFFER_GEOMETRY,
                static_cast<int32_t>(width), static_cast<int32_t>(height)) != 0)
            return false;
        OH_NativeWindow_NativeWindowHandleOpt(
            windowLease_.Get(), SET_FORMAT, NATIVEBUFFER_PIXEL_FMT_RGBA_8888);
        OH_NativeWindow_NativeWindowHandleOpt(
            windowLease_.Get(), SET_USAGE,
            static_cast<uint64_t>(NATIVEBUFFER_USAGE_HW_RENDER | NATIVEBUFFER_USAGE_HW_TEXTURE));
        const int32_t timeoutResult = OH_NativeWindow_NativeWindowHandleOpt(
            windowLease_.Get(), SET_TIMEOUT, static_cast<int32_t>(0));
        int32_t queueSize = 0;
        OH_NativeWindow_NativeWindowHandleOpt(
            windowLease_.Get(), GET_BUFFERQUEUE_SIZE, &queueSize);
        OH_LOG_INFO(LOG_APP,
                    "[VIRGL-ZC][NCP] window configured size=%{public}ux%{public}u "
                    "queue=%{public}d timeout_ms=0 timeout_ret=%{public}d",
                    width, height, queueSize, timeoutResult);

        const EGLint configAttributes[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_NONE,
        };
        EGLint configCount = 0;
        EGLConfig config = nullptr;
        if (!eglChooseConfig(sourceDisplay, configAttributes, &config, 1, &configCount) ||
            configCount == 0)
            return false;

        const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        EGLContext context = eglCreateContext(
            sourceDisplay, config, sourceContext, contextAttributes);
        if (context == EGL_NO_CONTEXT) return false;
        EGLSurface surface = eglCreateWindowSurface(
            sourceDisplay, config,
            reinterpret_cast<EGLNativeWindowType>(windowLease_.Get()), nullptr);
        if (surface == EGL_NO_SURFACE)
        {
            eglDestroyContext(sourceDisplay, context);
            return false;
        }
        if (eglMakeCurrent(sourceDisplay, surface, surface, context) != EGL_TRUE)
        {
            eglDestroySurface(sourceDisplay, surface);
            eglDestroyContext(sourceDisplay, context);
            return false;
        }

        static constexpr const char* vertexSource = R"(#version 300 es
out vec2 vTexCoord;
void main() {
    vec2 positions[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    vec2 texcoords[3] = vec2[3](vec2(0.0, 0.0), vec2(2.0, 0.0), vec2(0.0, 2.0));
    gl_Position = vec4(positions[gl_VertexID], 0.0, 1.0);
    vTexCoord = texcoords[gl_VertexID];
})";
        static constexpr const char* fragmentSource = R"(#version 300 es
precision mediump float;
uniform sampler2D uTexture;
in vec2 vTexCoord;
out vec4 outColor;
void main() { outColor = texture(uTexture, vTexCoord); }
)";
        const GLuint vertex = CompilePresentShader(GL_VERTEX_SHADER, vertexSource);
        const GLuint fragment = CompilePresentShader(GL_FRAGMENT_SHADER, fragmentSource);
        GLuint program = 0;
        if (vertex && fragment)
        {
            program = glCreateProgram();
            glAttachShader(program, vertex);
            glAttachShader(program, fragment);
            glLinkProgram(program);
            GLint linked = GL_FALSE;
            glGetProgramiv(program, GL_LINK_STATUS, &linked);
            if (linked != GL_TRUE)
            {
                char log[512] = {};
                glGetProgramInfoLog(program, sizeof(log), nullptr, log);
                OH_LOG_ERROR(LOG_APP, "[VIRGL-ZC][NCP] program link failed: %{public}s", log);
                glDeleteProgram(program);
                program = 0;
            }
        }
        if (vertex) glDeleteShader(vertex);
        if (fragment) glDeleteShader(fragment);
        if (!program)
        {
            eglMakeCurrent(sourceDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglDestroySurface(sourceDisplay, surface);
            eglDestroyContext(sourceDisplay, context);
            return false;
        }

        GLuint sampler = 0;
        glGenSamplers(1, &sampler);
        glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        eglSwapInterval(sourceDisplay, 0);

        display_ = sourceDisplay;
        context_ = context;
        surface_ = surface;
        program_ = program;
        sampler_ = sampler;
        textureLocation_ = glGetUniformLocation(program_, "uTexture");
        width_ = width;
        height_ = height;
        return true;
    }

    bool InstallScanoutBackingLocked(uint32_t resHandle,
                                     EGLDisplay sourceDisplay, EGLContext sourceContext,
                                     EGLSurface sourceDraw, EGLSurface sourceRead)
    {
        if (!gSetScanoutBacking || !resHandle || !direct_.Current())
            return false;
        eglMakeCurrent(sourceDisplay, sourceDraw, sourceRead, sourceContext);
        const GLuint gl = direct_.ScanoutTextureOnCurrent();
        void* image = direct_.CurrentEglImage();
        if (!gl || !image)
            return false;
        const int ret = gSetScanoutBacking(resHandle, gl, image);
        /* FBOs are not shared across the Direct/vrend share group. Detach the
         * Direct COLOR0 only while the Direct context is current; doing it on
         * vrend unbinds guest COLOR0 when both FBOs happen to be named 1. */
        const EGLDisplay directDisplay = static_cast<EGLDisplay>(direct_.Display());
        const EGLSurface directSurface = static_cast<EGLSurface>(direct_.Pbuffer());
        const EGLContext directContext = static_cast<EGLContext>(direct_.Context());
        if (directDisplay && directContext && directSurface) {
            eglMakeCurrent(directDisplay, directSurface, directSurface, directContext);
            direct_.DetachColorForScanout();
        }
        eglMakeCurrent(sourceDisplay, sourceDraw, sourceRead, sourceContext);
        scanoutRes_ = resHandle;
        scanoutGl_ = gl;
        holdingScanout_ = ret == 0;
        return ret == 0;
    }

    void FinishPresentLocked(uint32_t width, uint32_t height, uint32_t serial,
                             uint64_t* nextPresentDeadlineNs)
    {
        ++frames_;
        width_ = width;
        height_ = height;
        lastPresentNs_ = NowNs();
        if (nextPresentDeadlineNs)
            *nextPresentDeadlineNs = NextPresentDeadlineNs(
                lastPresentNs_, framePeriodNs_, frames_);
        if (frames_ == kPresentUncapWarmupFrames && winehua::PresentUncapRequested())
            OH_LOG_INFO(LOG_APP,
                        "[VIRGL-ZC][NCP] present uncap armed surface_key=%{public}llu "
                        "frames=%{public}llu",
                        static_cast<unsigned long long>(surfaceKey_),
                        static_cast<unsigned long long>(frames_));
        if (frames_ == 1 || frames_ % 120 == 0)
            OH_LOG_INFO(LOG_APP,
                        "%{public}s scanout_backing=%{public}d force_blit=%{public}d",
                        direct_.Timeline().Format("VIRGL-DIRECT", serial).c_str(),
                        winehua::ScanoutBackingEnabled() ? 1 : 0,
                        winehua::ScanoutForceBlitRequested() ? 1 : 0);
    }

    int PresentDirectLocked(uint32_t resHandle, GLuint texture, uint32_t width,
                            uint32_t height, uint32_t serial,
                            EGLDisplay sourceDisplay, EGLContext sourceContext,
                            EGLSurface sourceDraw, EGLSurface sourceRead,
                            uint64_t* nextPresentDeadlineNs)
    {
        if (!direct_.Configure(windowLease_.Get(), width, height)) {
            ++failures_;
            if (failures_ == 1 || failures_ % 120 == 0)
                OH_LOG_ERROR(LOG_APP,
                             "[VIRGL-DIRECT] configure failed serial=%{public}u "
                             "size=%{public}ux%{public}u drops=%{public}llu",
                             serial, width, height,
                             static_cast<unsigned long long>(failures_));
            return -4;
        }
        eglMakeCurrent(sourceDisplay, sourceDraw, sourceRead, sourceContext);
        const bool scanoutOk = winehua::ScanoutBackingEnabled() &&
            direct_.SharedWithVrend() && resHandle && gSetScanoutBacking;
        if (!scanoutOk && gColorRemapFn) gColorRemapFn(0, 0);

        uint32_t writeGl = 0;
        uint32_t fullCover = 0;
        const char* writer = "NONE";
        if (gScanoutLastWrite && resHandle)
            gScanoutLastWrite(resHandle, &writeGl, &fullCover, &writer);
        GLint liveFb = 0;
        GLint liveColor0 = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &liveFb);
        if (liveFb) {
            glGetFramebufferAttachmentParameteriv(
                GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &liveColor0);
        }
        const bool writerOk = writer &&
            (std::strcmp(writer, "DRAW") == 0 ||
             std::strcmp(writer, "CLEAR") == 0 ||
             std::strcmp(writer, "BLIT") == 0 ||
             std::strcmp(writer, "COPY") == 0);
        const bool guestWroteScanout = scanoutOk && holdingScanout_ &&
            direct_.Current() && scanoutGl_ != 0 && writerOk &&
            writeGl == scanoutGl_ &&
            (liveColor0 <= 0 || liveColor0 == static_cast<GLint>(scanoutGl_));

        uint32_t requestCount = 0;
        uint64_t destRequestUs = 0;
        uint64_t nextRequestUs = 0;
        const auto beginDestIfNeeded = [&]() -> bool {
            if (direct_.Current())
                return true;
            if (!direct_.BeginFrame())
                return false;
            ++requestCount;
            destRequestUs = direct_.LastRequestUs();
            eglMakeCurrent(sourceDisplay, sourceDraw, sourceRead, sourceContext);
            direct_.WaitAcquireOnCurrent();
            return true;
        };
        const auto flushAndAcquireNext = [&](const uint32_t flushSeq,
                                             bool* installedOut) -> int {
            if (!direct_.EndFrameWithRenderFence()) {
                holdingScanout_ = false;
                scanoutGl_ = 0;
                ++failures_;
                eglMakeCurrent(sourceDisplay, sourceDraw, sourceRead, sourceContext);
                return -6;
            }
            if (!scanoutOk) {
                holdingScanout_ = false;
                scanoutGl_ = 0;
                if (installedOut) *installedOut = false;
                return 0;
            }
            if (!direct_.BeginFrame()) {
                holdingScanout_ = false;
                scanoutGl_ = 0;
                ++failures_;
                eglMakeCurrent(sourceDisplay, sourceDraw, sourceRead, sourceContext);
                if (installedOut) *installedOut = false;
                return -4;
            }
            ++requestCount;
            nextRequestUs = direct_.LastRequestUs();
            eglMakeCurrent(sourceDisplay, sourceDraw, sourceRead, sourceContext);
            direct_.WaitAcquireOnCurrent();
            const bool installed = InstallScanoutBackingLocked(
                resHandle, sourceDisplay, sourceContext, sourceDraw, sourceRead);
            if (installedOut) *installedOut = installed;
            (void)flushSeq;
            return 0;
        };

        if (guestWroteScanout) {
            eglMakeCurrent(sourceDisplay, sourceDraw, sourceRead, sourceContext);
            const uint32_t flushSeq = direct_.SeqNum();
            bool installed = false;
            const int rotate = flushAndAcquireNext(flushSeq, &installed);
            if (rotate < 0) return rotate;
            if (installed)
                direct_.Timeline().AddSkippedCopy();
            eglMakeCurrent(sourceDisplay, sourceDraw, sourceRead, sourceContext);
            uint64_t genReq = 0, genApp = 0;
            uint32_t drawGl = 0;
            if (gScanoutGeneration && resHandle)
                gScanoutGeneration(resHandle, &genReq, &genApp, &drawGl);
            if (frames_ < 8 || frames_ % 120 == 0)
                OH_LOG_INFO(LOG_APP,
                            "[SCANOUT] frame=%{public}llu event=FLUSH+REQUEST "
                            "res=%{public}u writer=%{public}s full_cover=%{public}u "
                            "write_gl=%{public}u live_fb=%{public}d live_color0=%{public}d "
                            "flush_seq=%{public}u next_seq=%{public}u gl=%{public}u "
                            "installed=%{public}d requests=%{public}u next_req_us=%{public}llu "
                            "gen_req=%{public}llu gen_app=%{public}llu draw_gl=%{public}u",
                            static_cast<unsigned long long>(frames_ + 1), resHandle,
                            writer, fullCover, writeGl, liveFb, liveColor0,
                            flushSeq, direct_.SeqNum(), scanoutGl_,
                            installed ? 1 : 0, requestCount,
                            static_cast<unsigned long long>(nextRequestUs),
                            static_cast<unsigned long long>(genReq),
                            static_cast<unsigned long long>(genApp), drawGl);
            FinishPresentLocked(width, height, serial, nextPresentDeadlineNs);
            return 0;
        }

        /* Fallback / bootstrap: blit writeGl into the already-acquired dest,
         * then the same Flush + Request next as skip. One extra RequestBuffer
         * only when Current() is null (frame 0). */
        if (!beginDestIfNeeded()) {
            ++failures_;
            eglMakeCurrent(sourceDisplay, sourceDraw, sourceRead, sourceContext);
            OH_LOG_ERROR(LOG_APP,
                         "[VIRGL-DIRECT] BeginFrame failed serial=%{public}u "
                         "reason=%{public}s request_ret=%{public}d "
                         "wait_us=%{public}llu drops=%{public}llu",
                         serial, direct_.LastBeginReason(),
                         direct_.LastRequestResult(),
                         static_cast<unsigned long long>(direct_.LastRequestUs()),
                         static_cast<unsigned long long>(failures_));
            return -4;
        }

        const GLuint destGl = scanoutGl_ ? scanoutGl_ : direct_.ScanoutTextureOnCurrent();
        GLsync sourceReady = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (!sourceReady) return -7;
        glFlush();
        if (!direct_.MakeCurrent()) {
            glDeleteSync(sourceReady);
            ++failures_;
            eglMakeCurrent(sourceDisplay, sourceDraw, sourceRead, sourceContext);
            return -4;
        }
        direct_.WaitAcquireOnCurrent();
        glWaitSync(sourceReady, 0, GL_TIMEOUT_IGNORED);
        glDeleteSync(sourceReady);
        if (!EnsureBlitProgramLocked(width, height)) {
            direct_.AbortFrame();
            holdingScanout_ = false;
            ++failures_;
            eglMakeCurrent(sourceDisplay, sourceDraw, sourceRead, sourceContext);
            OH_LOG_ERROR(LOG_APP,
                         "[VIRGL-DIRECT] blit program failed serial=%{public}u",
                         serial);
            return -4;
        }
        direct_.PrepareBlitFramebuffer();
        glBindFramebuffer(GL_FRAMEBUFFER, direct_.Framebuffer());
        glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        if (!scanoutPrivateGl_ && texture)
            scanoutPrivateGl_ = texture;
        GLuint blitSrc = scanoutPrivateGl_ ? scanoutPrivateGl_ : texture;
        if (writeGl && scanoutPrivateGl_ && writeGl != scanoutPrivateGl_ &&
            writeGl != scanoutGl_)
            blitSrc = writeGl;
        /* Canonical NativeBuffer is skip-path compositor-upright. Flip only
         * when the blit source is still logical GL Y-up (private tex). */
        const bool flipY = blitSrc && !direct_.IsScanoutTexture(blitSrc);
        glUseProgram(program_);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, blitSrc);
        glBindSampler(0, sampler_);
        glUniform1i(textureLocation_, 0);
        if (flipYLocation_ >= 0)
            glUniform1f(flipYLocation_, flipY ? 1.f : 0.f);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        direct_.Timeline().AddGpuCopy();
        const uint32_t flushSeq = direct_.SeqNum();
        bool installed = false;
        const int rotate = flushAndAcquireNext(flushSeq, &installed);
        if (rotate < 0) return rotate;
        eglMakeCurrent(sourceDisplay, sourceDraw, sourceRead, sourceContext);
        uint64_t genReq = 0, genApp = 0;
        uint32_t drawGl = 0;
        if (gScanoutGeneration && resHandle)
            gScanoutGeneration(resHandle, &genReq, &genApp, &drawGl);
        /* Intentional force-blit A/B logs like skip (first 8 + every 120).
         * Unexpected fallback while skip is on still logs every frame. */
        if (scanoutOk || frames_ < 8 || frames_ % 120 == 0)
            OH_LOG_INFO(LOG_APP,
                        "[SCANOUT] frame=%{public}llu event=BLIT-FALLBACK "
                        "res=%{public}u writer=%{public}s write_gl=%{public}u "
                        "dest_gl=%{public}u blit_src=%{public}u flip_y=%{public}d "
                        "live_fb=%{public}d live_color0=%{public}d current=%{public}d "
                        "requests=%{public}u dest_req_us=%{public}llu next_req_us=%{public}llu "
                        "next_seq=%{public}u next_gl=%{public}u installed=%{public}d "
                        "gen_req=%{public}llu gen_app=%{public}llu draw_gl=%{public}u "
                        "single_dest=1 scanout_ok=%{public}d",
                        static_cast<unsigned long long>(frames_ + 1), resHandle,
                        writer, writeGl, destGl, blitSrc, flipY ? 1 : 0,
                        liveFb, liveColor0, direct_.Current() ? 1 : 0, requestCount,
                        static_cast<unsigned long long>(destRequestUs),
                        static_cast<unsigned long long>(nextRequestUs),
                        direct_.SeqNum(), scanoutGl_, installed ? 1 : 0,
                        static_cast<unsigned long long>(genReq),
                        static_cast<unsigned long long>(genApp), drawGl,
                        scanoutOk ? 1 : 0);
        if (!scanoutOk && gColorRemapFn) gColorRemapFn(0, 0);
        FinishPresentLocked(width, height, serial, nextPresentDeadlineNs);
        return 0;
    }

    bool EnsureBlitProgramLocked(uint32_t width, uint32_t height)
    {
        if (program_ && sampler_ && width_ == width && height_ == height &&
            flipYLocation_ >= 0)
            return true;
        static constexpr const char* vertexSource = R"(#version 300 es
uniform float uFlipY;
out vec2 vTexCoord;
void main() {
    vec2 positions[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    vec2 texcoords[3] = vec2[3](vec2(0.0, 0.0), vec2(2.0, 0.0), vec2(0.0, 2.0));
    gl_Position = vec4(positions[gl_VertexID], 0.0, 1.0);
    float y = texcoords[gl_VertexID].y;
    vTexCoord = vec2(texcoords[gl_VertexID].x, mix(y, 1.0 - y, uFlipY));
})";
        static constexpr const char* fragmentSource = R"(#version 300 es
precision mediump float;
uniform sampler2D uTexture;
in vec2 vTexCoord;
out vec4 outColor;
void main() { outColor = texture(uTexture, vTexCoord); }
)";
        const GLuint vertex = CompilePresentShader(GL_VERTEX_SHADER, vertexSource);
        const GLuint fragment = CompilePresentShader(GL_FRAGMENT_SHADER, fragmentSource);
        GLuint program = 0;
        if (vertex && fragment) {
            program = glCreateProgram();
            glAttachShader(program, vertex);
            glAttachShader(program, fragment);
            glLinkProgram(program);
            GLint linked = GL_FALSE;
            glGetProgramiv(program, GL_LINK_STATUS, &linked);
            if (linked != GL_TRUE) {
                glDeleteProgram(program);
                program = 0;
            }
        }
        if (vertex) glDeleteShader(vertex);
        if (fragment) glDeleteShader(fragment);
        if (!program) return false;
        if (program_) glDeleteProgram(program_);
        if (sampler_) glDeleteSamplers(1, &sampler_);
        program_ = program;
        glGenSamplers(1, &sampler_);
        glSamplerParameteri(sampler_, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glSamplerParameteri(sampler_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glSamplerParameteri(sampler_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(sampler_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        textureLocation_ = glGetUniformLocation(program_, "uTexture");
        flipYLocation_ = glGetUniformLocation(program_, "uFlipY");
        width_ = width;
        height_ = height;
        return true;
    }

    void ResetGlLocked()
    {
        if (gColorRemapFn) gColorRemapFn(0, 0);
        if (gClearScanoutBacking && scanoutRes_)
            gClearScanoutBacking(scanoutRes_);
        holdingScanout_ = false;
        scanoutSrcTex_ = 0;
        scanoutGl_ = 0;
        scanoutRes_ = 0;
        scanoutPrivateGl_ = 0;
        if (program_ || sampler_) {
            if (sampler_) glDeleteSamplers(1, &sampler_);
            if (program_) glDeleteProgram(program_);
            program_ = 0;
            sampler_ = 0;
            textureLocation_ = -1;
            flipYLocation_ = -1;
        }
        direct_.Reset();
        if (display_ != EGL_NO_DISPLAY)
        {
            const EGLDisplay previousDisplay = eglGetCurrentDisplay();
            const EGLContext previousContext = eglGetCurrentContext();
            const EGLSurface previousDraw = eglGetCurrentSurface(EGL_DRAW);
            const EGLSurface previousRead = eglGetCurrentSurface(EGL_READ);
            const bool cleanupCurrent = context_ != EGL_NO_CONTEXT &&
                surface_ != EGL_NO_SURFACE &&
                eglMakeCurrent(display_, surface_, surface_, context_) == EGL_TRUE;
            if (cleanupCurrent)
            {
                if (sampler_) glDeleteSamplers(1, &sampler_);
                if (program_) glDeleteProgram(program_);
                if (previousDisplay != EGL_NO_DISPLAY)
                    eglMakeCurrent(previousDisplay, previousDraw, previousRead, previousContext);
                else
                    eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            }
            if (surface_ != EGL_NO_SURFACE) eglDestroySurface(display_, surface_);
            if (context_ != EGL_NO_CONTEXT) eglDestroyContext(display_, context_);
        }
        display_ = EGL_NO_DISPLAY;
        context_ = EGL_NO_CONTEXT;
        surface_ = EGL_NO_SURFACE;
        program_ = 0;
        sampler_ = 0;
        textureLocation_ = -1;
        flipYLocation_ = -1;
        width_ = 0;
        height_ = 0;
    }

    void ResetLocked()
    {
        ResetGlLocked();
        ReleaseWindowLocked();
        surfaceKey_ = 0;
    }

    void ReleaseWindowLocked()
    {
        windowLease_.Reset();
    }

    std::mutex mutex_;
    winehua::NativeWindowLease windowLease_;
    winehua::NativeWindowGlesTarget direct_;
    bool holdingScanout_ = false;
    GLuint scanoutSrcTex_ = 0;
    GLuint scanoutGl_ = 0;
    GLuint scanoutPrivateGl_ = 0;
    uint32_t scanoutRes_ = 0;
    uint64_t surfaceKey_ = 0;
    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLContext context_ = EGL_NO_CONTEXT;
    EGLSurface surface_ = EGL_NO_SURFACE;
    GLuint program_ = 0;
    GLuint sampler_ = 0;
    GLint textureLocation_ = -1;
    GLint flipYLocation_ = -1;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint64_t frames_ = 0;
    uint64_t failures_ = 0;
    uint64_t timestampFailures_ = 0;
    uint64_t throttled_ = 0;
    uint64_t lastPresentNs_ = 0;
    uint64_t displayPeriodNs_ = kDefaultFramePeriodNs;
    uint64_t framePeriodNs_ = kDefaultFramePeriodNs;
};

class SurfaceQueuePresenterManager {
public:
    int Attach(uint64_t surfaceKey, uint64_t framePeriodNs, uint32_t flags,
               OHNativeWindow* window)
    {
        if (!surfaceKey || !window) return -1;
        std::lock_guard<std::mutex> lock(mutex_);
        auto& entry = surfaces_[surfaceKey];
        entry.missingTargetLogged = false;
        const bool releaseWindowWithUnreference =
            (flags & winehua::virgl_ipc::kSurfaceNativeObjectReference) != 0;
        entry.info.flags =
            (entry.info.flags & ~(winehua::virgl_ipc::kSurfaceVulkan |
                                  winehua::virgl_ipc::kSurfaceAttached)) |
            (flags & winehua::virgl_ipc::kSurfaceVulkan);
        int result;
        if (entry.info.flags & winehua::virgl_ipc::kSurfaceVulkan)
        {
            if (entry.virglTarget) {
                entry.virglTarget->Detach(surfaceKey);
                entry.virglTarget.reset();
            }
            RetireVenusTargetLocked(surfaceKey, entry.venusTarget);
            entry.venusTarget = std::make_unique<winehua::VenusSurfaceQueueTarget>();
            result = entry.venusTarget->Attach(surfaceKey, framePeriodNs, window,
                                                releaseWindowWithUnreference);
        }
        else
        {
            RetireVenusTargetLocked(surfaceKey, entry.venusTarget);
            if (!entry.virglTarget)
                entry.virglTarget = std::make_unique<SurfaceQueueTarget>();
            result = entry.virglTarget->Attach(surfaceKey, framePeriodNs, window,
                                               releaseWindowWithUnreference);
        }
        if (result == 0) {
            entry.info.flags |= winehua::virgl_ipc::kSurfaceAttached;
            targetCondition_.notify_all();
        }
        return result;
    }

    int Detach(uint64_t surfaceKey)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = surfaces_.find(surfaceKey);
        if (it == surfaces_.end()) return 0;
        if (it->second.virglTarget) it->second.virglTarget->Detach(surfaceKey);
        RetireVenusTargetLocked(surfaceKey, it->second.venusTarget);
        ++surfaceGenerations_[surfaceKey];
        surfaces_.erase(it);
        targetCondition_.notify_all();
        return 0;
    }

    int PrepareVenusDeviceRelease(uint32_t contextId, uintptr_t device)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        int matches = 0;
        for (auto& [surfaceKey, entry] : surfaces_)
        {
            static_cast<void>(surfaceKey);
            if (entry.venusTarget &&
                entry.venusTarget->PrepareDeviceRelease(contextId, device))
                ++matches;
        }
        for (auto& target : retiredVenusTargets_)
        {
            if (target->PrepareDeviceRelease(contextId, device)) ++matches;
        }
        OH_LOG_INFO(LOG_APP,
                    "[VENUS-PRESENT][NCP] device release prepare complete "
                    "ctx=%{public}u device=0x%{public}llx targets=%{public}d",
                    contextId, static_cast<unsigned long long>(device), matches);
        return matches;
    }

    int FinishVenusDeviceRelease(uint32_t contextId, uintptr_t device,
                                 int32_t waitResult)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        int matches = 0;
        for (auto& [surfaceKey, entry] : surfaces_)
        {
            static_cast<void>(surfaceKey);
            if (entry.venusTarget && entry.venusTarget->FinishDeviceRelease(
                    contextId, device, waitResult))
                ++matches;
        }
        for (auto& target : retiredVenusTargets_)
        {
            if (target->FinishDeviceRelease(contextId, device, waitResult)) ++matches;
        }
        retiredVenusTargets_.erase(
            std::remove_if(retiredVenusTargets_.begin(), retiredVenusTargets_.end(),
                           [](const auto& target) {
                               return !target->HasVulkanDevice();
                           }),
            retiredVenusTargets_.end());
        OH_LOG_INFO(LOG_APP,
                    "[VENUS-PRESENT][NCP] device release after-wait complete "
                    "ctx=%{public}u device=0x%{public}llx wait_result=%{public}d "
                    "targets=%{public}d",
                    contextId, static_cast<unsigned long long>(device),
                    waitResult, matches);
        return matches;
    }

    int SetFramePeriod(uint64_t surfaceKey, uint64_t framePeriodNs)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = surfaces_.find(surfaceKey);
        if (it == surfaces_.end()) return -2;
        if (it->second.info.flags & winehua::virgl_ipc::kSurfaceVulkan)
            return it->second.venusTarget
                ? it->second.venusTarget->SetFramePeriod(framePeriodNs) : -2;
        return it->second.virglTarget
            ? it->second.virglTarget->SetFramePeriod(framePeriodNs) : -2;
    }

    int Present(uint32_t clientPid, uint32_t surfaceId, uint32_t resHandle,
                GLuint texture, uint32_t width, uint32_t height,
                uint64_t drawable, uint32_t serial,
                uint64_t* nextPresentDeadlineNs)
    {
        if (!clientPid || !surfaceId) return -2;
        const uint64_t surfaceKey =
            (static_cast<uint64_t>(clientPid) << 32) | surfaceId;
        std::lock_guard<std::mutex> lock(mutex_);
        auto& entry = surfaces_[surfaceKey];
        if (entry.info.flags & winehua::virgl_ipc::kSurfaceVulkan) return -EINVAL;
        entry.info.surfaceKey = surfaceKey;
        entry.info.clientPid = clientPid;
        entry.info.surfaceId = surfaceId;
        entry.info.width = width;
        entry.info.height = height;
        entry.info.serial = serial;
        entry.lastPresentUs = NowUs();
        if (!entry.virglTarget) return -2;
        return entry.virglTarget->Present(
            resHandle, texture, width, height, drawable, serial, nextPresentDeadlineNs);
    }

    int PresentVenus(uint32_t contextId,
                     uintptr_t instance,
                     uintptr_t physicalDevice,
                     uintptr_t device,
                     uintptr_t queue,
                     uint64_t image,
                     uint32_t queueFamily,
                     uint32_t width,
                     uint32_t height,
                     uint32_t format,
                     uint32_t layout,
                     uint32_t clientPid,
                     uint32_t surfaceId,
                     uint32_t serial,
                     uint64_t* nextPresentDeadlineNs,
                     void (*releaseQueue)(void*),
                     void* queueSyncData)
    {
        if (!clientPid || !surfaceId) return -EINVAL;
        const uint64_t surfaceKey =
            (static_cast<uint64_t>(clientPid) << 32) | surfaceId;
        std::unique_lock<std::mutex> lock(mutex_);
        auto& entry = surfaces_[surfaceKey];
        if (entry.virglTarget) return -EINVAL;
        entry.info.surfaceKey = surfaceKey;
        entry.info.clientPid = clientPid;
        entry.info.surfaceId = surfaceId;
        entry.info.width = width;
        entry.info.height = height;
        entry.info.serial = serial;
        entry.info.flags |= winehua::virgl_ipc::kSurfaceVulkan;
        entry.lastPresentUs = NowUs();
        const auto targetReady = [this, surfaceKey]() {
            const auto it = surfaces_.find(surfaceKey);
            return it != surfaces_.end() && it->second.venusTarget &&
                   (it->second.info.flags &
                    winehua::virgl_ipc::kSurfaceAttached);
        };
        if (!targetReady()) {
            if (!entry.missingTargetLogged) {
                entry.missingTargetLogged = true;
                OH_LOG_WARN(LOG_APP,
                            "[VENUS-PRESENT][NCP] target missing key=%{public}llu "
                            "ctx=%{public}u pid=%{public}u surface=%{public}u",
                            static_cast<unsigned long long>(surfaceKey),
                            contextId, clientPid, surfaceId);
            }
            const uint64_t generation = surfaceGenerations_[surfaceKey];
            const auto waitStart = SteadyClock::now();
            targetCondition_.wait_for(
                lock, kVenusTargetAttachTimeout,
                [this, surfaceKey, generation, &targetReady]() {
                    return targetReady() ||
                           surfaceGenerations_[surfaceKey] != generation;
                });
            const uint64_t waitedUs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    SteadyClock::now() - waitStart).count());
            if (!targetReady()) {
                OH_LOG_WARN(LOG_APP,
                            "[VENUS-PRESENT][NCP] target wait ended key=%{public}llu "
                            "ctx=%{public}u waited_us=%{public}llu reason=%{public}s",
                            static_cast<unsigned long long>(surfaceKey), contextId,
                            static_cast<unsigned long long>(waitedUs),
                            surfaceGenerations_[surfaceKey] != generation
                                ? "detached" : "timeout");
                return -EAGAIN;
            }
            OH_LOG_INFO(LOG_APP,
                        "[VENUS-PRESENT][NCP] target ready key=%{public}llu "
                        "ctx=%{public}u waited_us=%{public}llu",
                        static_cast<unsigned long long>(surfaceKey), contextId,
                        static_cast<unsigned long long>(waitedUs));
        }
        auto readyIt = surfaces_.find(surfaceKey);
        if (readyIt == surfaces_.end() || !readyIt->second.venusTarget)
            return -EAGAIN;
        return readyIt->second.venusTarget->Present(
            contextId, instance, physicalDevice, device, queue, image,
            queueFamily, width, height, format, layout, serial,
            nextPresentDeadlineNs, releaseQueue, queueSyncData);
    }

    winehua::virgl_ipc::SurfaceQueryReply Query() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        winehua::virgl_ipc::SurfaceQueryReply reply;
        const uint64_t nowUs = NowUs();
        std::vector<const Entry*> candidates;
        candidates.reserve(surfaces_.size());
        for (const auto& [surfaceKey, entry] : surfaces_)
        {
            static_cast<void>(surfaceKey);
            if (!entry.info.surfaceId ||
                (!(entry.info.flags & winehua::virgl_ipc::kSurfaceAttached) &&
                 nowUs - entry.lastPresentUs > 2000000))
                continue;
            candidates.push_back(&entry);
        }

        // unordered_map iteration is deliberately unspecified. Returning that
        // order made the main compositor bind a different live surface after a
        // restart when multiple Wine/Explorer clients were present. Prefer the
        // surface that most recently submitted a frame, with deterministic
        // serial/key tie breakers for startup races.
        std::sort(candidates.begin(), candidates.end(),
                  [](const Entry* a, const Entry* b) {
                      if (a->lastPresentUs != b->lastPresentUs)
                          return a->lastPresentUs > b->lastPresentUs;
                      if (a->info.serial != b->info.serial)
                          return a->info.serial > b->info.serial;
                      return a->info.surfaceKey > b->info.surfaceKey;
                  });
        for (const Entry* entry : candidates)
        {
            if (reply.count == winehua::virgl_ipc::kMaxSurfaces) break;
            reply.surfaces[reply.count++] = entry->info;
        }
        return reply;
    }

    void Reset()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [surfaceKey, entry] : surfaces_)
        {
            if (entry.virglTarget) entry.virglTarget->Detach(surfaceKey);
            RetireVenusTargetLocked(surfaceKey, entry.venusTarget);
            ++surfaceGenerations_[surfaceKey];
        }
        surfaces_.clear();
        targetCondition_.notify_all();
    }

private:
    void RetireVenusTargetLocked(
        uint64_t surfaceKey,
        std::unique_ptr<winehua::VenusSurfaceQueueTarget>& target)
    {
        if (!target) return;
        target->Detach(surfaceKey);
        if (target->HasVulkanDevice())
            retiredVenusTargets_.push_back(std::move(target));
        else
            target.reset();
    }

    struct Entry {
        winehua::virgl_ipc::SurfaceInfo info;
        std::unique_ptr<SurfaceQueueTarget> virglTarget;
        std::unique_ptr<winehua::VenusSurfaceQueueTarget> venusTarget;
        uint64_t lastPresentUs = 0;
        bool missingTargetLogged = false;
    };

    mutable std::mutex mutex_;
    std::condition_variable targetCondition_;
    std::unordered_map<uint64_t, Entry> surfaces_;
    std::unordered_map<uint64_t, uint64_t> surfaceGenerations_;
    std::vector<std::unique_ptr<winehua::VenusSurfaceQueueTarget>>
        retiredVenusTargets_;
};

SurfaceQueuePresenterManager g_presenters;

} // namespace

namespace winehua {

int AttachVirglSurfaceTarget(uint64_t surfaceKey, uint64_t framePeriodNs,
                             uint32_t flags, OHNativeWindow* window)
{
    return g_presenters.Attach(surfaceKey, framePeriodNs, flags, window);
}

int DetachVirglSurfaceTarget(uint64_t surfaceKey)
{
    return g_presenters.Detach(surfaceKey);
}

int SetVirglSurfaceFramePeriod(uint64_t surfaceKey, uint64_t framePeriodNs)
{
    return g_presenters.SetFramePeriod(surfaceKey, framePeriodNs);
}

int PresentVirglSurface(uint32_t clientPid, uint32_t surfaceId,
                        uint32_t resHandle, uint32_t texture, uint32_t width, uint32_t height,
                        uint64_t drawable, uint32_t serial,
                        uint64_t* nextPresentDeadlineNs)
{
    return g_presenters.Present(
        clientPid, surfaceId, resHandle, texture, width, height, drawable, serial,
        nextPresentDeadlineNs);
}

int PresentVenusSurface(uint32_t contextId,
                        uintptr_t instance,
                        uintptr_t physicalDevice,
                        uintptr_t device,
                        uintptr_t queue,
                        uint64_t image,
                        uint32_t queueFamily,
                        uint32_t width,
                        uint32_t height,
                        uint32_t format,
                        uint32_t layout,
                        uint32_t clientPid,
                        uint32_t surfaceId,
                        uint32_t serial,
                        uint64_t* nextPresentDeadlineNs,
                        void (*releaseQueue)(void*),
                        void* queueSyncData)
{
    return g_presenters.PresentVenus(
        contextId, instance, physicalDevice, device, queue, image,
        queueFamily, width, height, format, layout, clientPid, surfaceId,
        serial, nextPresentDeadlineNs, releaseQueue, queueSyncData);
}

int PrepareVenusDeviceRelease(uint32_t contextId, uintptr_t device)
{
    return g_presenters.PrepareVenusDeviceRelease(contextId, device);
}

int FinishVenusDeviceRelease(uint32_t contextId, uintptr_t device,
                             int32_t waitResult)
{
    return g_presenters.FinishVenusDeviceRelease(
        contextId, device, waitResult);
}

virgl_ipc::SurfaceQueryReply QueryVirglSurfaces()
{
    return g_presenters.Query();
}

void ResetVirglSurfaces()
{
    g_presenters.Reset();
}

void SetVirglColorRemapFn(void (*fn)(uint32_t, uint32_t))
{
    gColorRemapFn = fn;
}

void SetVirglScanoutBackingFn(int (*setBacking)(uint32_t, uint32_t, void*),
                              int (*clearBacking)(uint32_t),
                              int (*lastWrite)(uint32_t, uint32_t*, uint32_t*, const char**),
                              int (*generation)(uint32_t, uint64_t*, uint64_t*, uint32_t*))
{
    gSetScanoutBacking = setBacking;
    gClearScanoutBacking = clearBacking;
    gScanoutLastWrite = lastWrite;
    gScanoutGeneration = generation;
}

} // namespace winehua
