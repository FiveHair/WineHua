#include "host_nativebuffer_probe.h"

#include <dlfcn.h>
#include <hilog/log.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

#undef LOG_TAG
#define LOG_TAG "WL_NB_PROBE"

namespace {

using WindowProbeFn = int (*)(uint64_t, const char*);

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

std::string JsonEscape(const char* value)
{
    std::string out;
    for (const unsigned char c : std::string(value ? value : "")) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c >= 0x20) out += static_cast<char>(c);
            break;
        }
    }
    return out;
}

bool WriteAtomic(const std::string& path, const std::string& text)
{
    const std::string temporary = path + ".tmp." + std::to_string(getpid());
    const int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return false;
    const char* data = text.data();
    size_t remaining = text.size();
    bool ok = true;
    while (remaining) {
        const ssize_t written = write(fd, data, remaining);
        if (written <= 0) {
            ok = false;
            break;
        }
        data += written;
        remaining -= static_cast<size_t>(written);
    }
    if (ok) ok = fsync(fd) == 0;
    close(fd);
    if (ok) ok = rename(temporary.c_str(), path.c_str()) == 0;
    if (!ok) unlink(temporary.c_str());
    return ok;
}

std::string ReadText(const std::string& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

std::string ExtractStatus(const std::string& json)
{
    const char* patterns[] = {"\"status\":\"", "\"status\": \""};
    for (const char* pattern : patterns) {
        const size_t start = json.find(pattern);
        if (start == std::string::npos) continue;
        const size_t valueStart = start + strlen(pattern);
        const size_t end = json.find('"', valueStart);
        if (end != std::string::npos) return json.substr(valueStart, end - valueStart);
    }
    return {};
}

std::string FailureResult(const std::string& message, int probeResult)
{
    std::ostringstream out;
    out << "{\n"
        << "  \"schemaVersion\":1,\n"
        << "  \"testId\":\"host-nativebuffer\",\n"
        << "  \"status\":\"FAIL\",\n"
        << "  \"fatal\":\"" << JsonEscape(message.c_str()) << "\",\n"
        << "  \"probeResult\":" << probeResult << "\n"
        << "}";
    return out.str();
}

void RunProbe(uint64_t surfaceId, const std::string& runId)
{
    const std::string base = "/data/storage/el2/base/files/automation";
    const std::string root = base + "/results";
    const std::string directory = root + "/" + runId;
    EnsureDir(base);
    EnsureDir(root);
    EnsureDir(directory);

    const std::string resultPath = directory + "/host-nativebuffer.json";
    int probeResult = 4;
    std::string failure;
    void* module = nullptr;
    if (gCancel.load()) {
        failure = "surface destroyed before Host NativeBuffer probe started";
    } else {
        module = dlopen("libwinehua_ohos_nativebuffer_vulkan_probe.so", RTLD_NOW | RTLD_LOCAL);
        if (!module) {
            const char* loadError = dlerror();
            failure = std::string("dlopen Host NativeBuffer module failed: ") +
                (loadError ? loadError : "unknown dlopen error");
        } else {
            dlerror();
            auto probe = reinterpret_cast<WindowProbeFn>(
                dlsym(module, "winehua_host_nativebuffer_window_probe"));
            const char* symbolError = dlerror();
            if (symbolError || !probe) {
                failure = std::string("Host NativeBuffer entry point missing: ") +
                    (symbolError ? symbolError : "unknown dlsym error");
            } else {
                probeResult = probe(surfaceId, resultPath.c_str());
            }
        }
    }

    std::string result = ReadText(resultPath);
    if (result.empty()) {
        if (failure.empty()) failure = "Host NativeBuffer probe produced no result";
        result = FailureResult(failure, probeResult);
        WriteAtomic(resultPath, result + "\n");
    }
    if (module) dlclose(module);

    const std::string testStatus = ExtractStatus(result);
    const char* suiteStatus = testStatus == "PASS" ? "PASS" : "FAIL";
    std::ostringstream summary;
    summary << "{\n"
            << "  \"schemaVersion\":1,\n"
            << "  \"runId\":\"" << JsonEscape(runId.c_str()) << "\",\n"
            << "  \"suite\":\"host-nativebuffer\",\n"
            << "  \"prefixMode\":\"reuse\",\n"
            << "  \"status\":\"" << suiteStatus << "\",\n"
            << "  \"tests\":[" << result << "]\n"
            << "}\n";
    WriteAtomic(directory + "/suite-summary.json", summary.str());
    OH_LOG_INFO(LOG_APP,
                "[HostNativeBuffer] complete run=%{public}s probeResult=%{public}d "
                "testStatus=%{public}s suiteStatus=%{public}s",
                runId.c_str(), probeResult, testStatus.c_str(), suiteStatus);
    gRunning.store(false);
}

} // namespace

bool StartHostNativeBufferProbe(uint64_t surfaceId, const std::string& runId)
{
    if (!surfaceId || !SafeId(runId) || gRunning.exchange(true)) return false;
    gCancel.store(false);
    std::thread([surfaceId, runId]() { RunProbe(surfaceId, runId); }).detach();
    return true;
}

void StopHostNativeBufferProbe()
{
    gCancel.store(true);
}
