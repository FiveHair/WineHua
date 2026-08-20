#include "host_native_window_probe.h"
#include "native_window_direct.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <hilog/log.h>
#include <native_window/external_window.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <fcntl.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#undef LOG_TAG
#define LOG_TAG "NW_PROBE"

namespace {

using Clock = std::chrono::steady_clock;

std::atomic<bool> gRunning{false};
std::atomic<bool> gCancel{false};

bool SafeId(const std::string& value)
{
    if (value.empty() || value.size() > 120) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '-' || c == '_' || c == '.';
    });
}

bool EnsureDir(const std::string& path)
{
    struct stat st{};
    if (stat(path.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
    return mkdir(path.c_str(), 0755) == 0;
}

bool WriteAtomic(const std::string& path, const std::string& text)
{
    const std::string temporary = path + ".tmp." + std::to_string(getpid());
    const int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return false;
    const ssize_t written = write(fd, text.data(), text.size());
    close(fd);
    if (written != static_cast<ssize_t>(text.size()) || rename(temporary.c_str(), path.c_str()) != 0) {
        unlink(temporary.c_str());
        return false;
    }
    return true;
}

GLuint Compile(GLenum type, const char* source)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok == GL_TRUE) return shader;
    glDeleteShader(shader);
    return 0;
}

class Probe {
public:
    Probe(OHNativeWindow* window, std::string runId)
        : window_(window), runId_(std::move(runId)) {}

    ~Probe()
    {
        if (window_) OH_NativeWindow_DestroyNativeWindow(window_);
    }

    void Run()
    {
        std::string error;
        const bool ok = DrawLoop(&error);
        const winehua::FramePercentiles p = target_.Timeline().Total();
        std::ostringstream json;
        json << "{\"runId\":\"" << runId_ << "\",\"pass\":" << (ok ? "true" : "false")
             << ",\"error\":\"" << error << "\",\"frames\":" << p.count
             << ",\"cacheHits\":" << target_.Timeline().CacheHits()
             << ",\"cacheMisses\":" << target_.Timeline().CacheMisses()
             << ",\"gpuCopies\":" << target_.Timeline().GpuCopies()
             << ",\"cpuFenceWaits\":" << target_.Timeline().CpuFenceWaits()
             << ",\"glFinish\":" << target_.Timeline().GlFinishCount()
             << ",\"renderFence\":" << target_.Timeline().RenderFenceCount()
             << ",\"p50_us\":" << p.p50
             << ",\"p90_us\":" << p.p90
             << ",\"p99_us\":" << p.p99
             << ",\"max_us\":" << p.max
             << ",\"sameBacking\":" << (sameBacking_ ? "true" : "false")
             << "}\n";
        EnsureDir("/data/storage/el2/base/cache");
        WriteAtomic("/data/storage/el2/base/cache/winehua_native_window_probe.json",
                    json.str());
        OH_LOG_INFO(LOG_APP, "[NW-PROBE] %{public}s",
                    target_.Timeline().Format("NW-PROBE", 0).c_str());
        gRunning.store(false);
    }

private:
    bool DrawLoop(std::string* error)
    {
        int32_t width = 0, height = 0;
        OH_NativeWindow_NativeWindowHandleOpt(window_, GET_BUFFER_GEOMETRY, &height, &width);
        if (width <= 0 || height <= 0) {
            width = 1280;
            height = 720;
        }
        if (!target_.Configure(window_, static_cast<uint32_t>(width),
                               static_cast<uint32_t>(height))) {
            *error = "configure failed";
            return false;
        }
        static constexpr const char* kVs =
            "#version 300 es\nvoid main() {\n"
            "  vec2 p[3] = vec2[3](vec2(-1.0,-1.0), vec2(3.0,-1.0), vec2(-1.0,3.0));\n"
            "  gl_Position = vec4(p[gl_VertexID], 0.0, 1.0);\n}\n";
        static constexpr const char* kFs =
            "#version 300 es\nprecision mediump float;\nout vec4 c;\n"
            "uniform float uT;\nvoid main() { c = vec4(0.1, 0.45 + 0.45 * sin(uT), 0.85, 1.0); }\n";
        const GLuint vs = Compile(GL_VERTEX_SHADER, kVs);
        const GLuint fs = Compile(GL_FRAGMENT_SHADER, kFs);
        if (!vs || !fs) {
            *error = "shader compile failed";
            return false;
        }
        GLuint program = glCreateProgram();
        glAttachShader(program, vs);
        glAttachShader(program, fs);
        glLinkProgram(program);
        glDeleteShader(vs);
        glDeleteShader(fs);
        GLint linked = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (linked != GL_TRUE) {
            *error = "program link failed";
            glDeleteProgram(program);
            return false;
        }
        const GLint timeLoc = glGetUniformLocation(program, "uT");
        std::vector<uint32_t> seen;
        seen.reserve(8);
        auto start = Clock::now();
        uint32_t frames = 0;
        while (!gCancel.load() && frames < 180) {
            const auto frameStart = Clock::now();
            if (!target_.BeginFrame()) {
                *error = "RequestBuffer failed";
                break;
            }
            if (!target_.WaitAcquireOnCurrent()) {
                *error = "acquire wait failed";
                break;
            }
            const uint32_t seq = target_.SeqNum();
            if (std::find(seen.begin(), seen.end(), seq) == seen.end())
                seen.push_back(seq);
            else
                sameBacking_ = true;
            glUseProgram(program);
            glUniform1f(timeLoc, static_cast<float>(frames) * 0.08f);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            const int fenceFd = target_.CreateNativeFenceFd();
            if (!target_.EndFrame(fenceFd)) {
                *error = "FlushBuffer failed";
                break;
            }
            ++frames;
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                Clock::now() - frameStart);
            if (elapsed.count() < 16666)
                std::this_thread::sleep_for(std::chrono::microseconds(16666 - elapsed.count()));
            if (Clock::now() - start > std::chrono::seconds(4)) break;
        }
        glDeleteProgram(program);
        if (frames == 0) {
            if (error->empty()) *error = "no frames";
            return false;
        }
        if (target_.Timeline().CpuFenceWaits() || target_.Timeline().GlFinishCount()) {
            *error = "cpu wait used";
            return false;
        }
        return sameBacking_ || seen.size() >= 2;
    }

    OHNativeWindow* window_ = nullptr;
    std::string runId_;
    winehua::NativeWindowGlesTarget target_;
    bool sameBacking_ = false;
};

} // namespace

bool StartHostNativeWindowProbe(uint64_t surfaceId, const std::string& runId)
{
    if (!SafeId(runId) || gRunning.exchange(true)) return false;
    gCancel.store(false);
    OHNativeWindow* window = nullptr;
    if (OH_NativeWindow_CreateNativeWindowFromSurfaceId(surfaceId, &window) != 0 ||
        !window) {
        gRunning.store(false);
        return false;
    }
    std::thread([window, runId]() {
        Probe probe(window, runId);
        probe.Run();
    }).detach();
    return true;
}

void StopHostNativeWindowProbe()
{
    gCancel.store(true);
}
