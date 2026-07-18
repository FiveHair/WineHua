/*
 * ncp_shim.cpp — fork 版 native_child_process 实现
 *
 * 鸿蒙手机平台限制 OH_Ability_*NativeChildProcess*（仅 2in1 可用），
 * 本文件提供相同 ABI 的 fork 替代实现：
 *   - OH_Ability_StartNativeChildProcess: fork + dlopen + dlsym(entry) + entry(args)
 *   - OH_Ability_CreateNativeChildProcess: 快速失败（virgl ZC 依赖 Binder 跨进程
 *     传递 OHNativeWindow，fork 无法等价，交由 GraphicsBroker 的 shm fallback）
 *
 * 与官方 NCP 的语义差异处理：
 *   1. fork 子进程继承 Ark 主进程低 4GB 映射 → child 里 UnmapLowAnonRegions()
 *   2. NCP 的 fd 所有权转移 → fork 后 parent 显式 close，防泄漏/EOF 语义错乱
 *   3. NCP 子进程由 appspawn 收尸 → 安装 SIGCHLD reaper 防僵尸
 *   4. NCP 同步返回 so 加载结果 → 握手 pipe 模拟同步错误语义
 */
#include "native_child_process.h"   // 同目录 shim 头（官方头副本）

#include <dlfcn.h>
#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>
#include <vector>

#undef LOG_TAG
#define LOG_TAG "NCP_Shim"
#include <hilog/log.h>

namespace {

// ---- 僵尸回收：NCP 由 appspawn 收尸，fork 后主进程必须自己 reap ----
void InstallReaperOnce() {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, [] {
        struct sigaction sa{};
        sa.sa_handler = [](int) {
            int saved = errno;
            while (waitpid(-1, nullptr, WNOHANG) > 0) {}
            errno = saved;
        };
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
        sigaction(SIGCHLD, &sa, nullptr);
    });
}

// ---- 关闭除 keep 外的所有继承 fd（listen/conn/Ark/gfx 全收掉）----
void CloseAllFdsExcept(const std::vector<int>& keep) {
    DIR* d = opendir("/proc/self/fd");
    if (!d) return;
    int dfd = dirfd(d);
    struct dirent* e;
    while ((e = readdir(d))) {
        int fd = atoi(e->d_name);
        if (fd <= 2 || fd == dfd) continue;
        bool k = false;
        for (int f : keep) {
            if (f == fd) { k = true; break; }
        }
        if (!k) close(fd);
    }
    closedir(d);
}

// ---- 释放继承自 Ark 主进程的低 4GB anon/ark 映射 ----
// NCP 子进程由 appspawn fork，低 4GB 天然干净；fork 子进程必须自己让位，
// 否则 Wine WoW64/box32 的 MAP_FIXED 低地址分配会冲突。
void UnmapLowAnonRegions() {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return;

    struct Region { unsigned long start, end; };
    std::vector<Region> targets;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        unsigned long start = 0, end = 0;
        char prot[8] = {0};
        char tag[256] = {0};
        int n = sscanf(line, "%lx-%lx %7s %*s %*s %*s %255[^\n]",
                       &start, &end, prot, tag);
        if (n < 3) continue;
        if (start >= 0x100000000UL) continue;
        bool isArk      = strstr(tag, "[anon:ark") != nullptr;
        bool isPureAnon = (n < 4) || (tag[0] == 0);
        if (isArk || isPureAnon) targets.push_back({start, end});
    }
    fclose(f);

    for (auto& r : targets) {
        munmap((void*)r.start, r.end - r.start);
    }
}

// ---- 握手 pipe：child 解析出入口函数后写 1 字节；parent 同步等待 ----
constexpr int kHandshakeTimeoutMs = 10000;

void ChildHandshakeOk(int wfd) {
    uint8_t b = 1;
    ssize_t unused = write(wfd, &b, 1);
    (void)unused;
    close(wfd);
}

bool ParentWaitHandshake(int rfd) {
    struct pollfd pfd{rfd, POLLIN, 0};
    int pr;
    do { pr = poll(&pfd, 1, kHandshakeTimeoutMs); } while (pr < 0 && errno == EINTR);
    if (pr <= 0) { close(rfd); return false; }
    uint8_t b;
    bool ok = (read(rfd, &b, 1) == 1 && b == 1);
    close(rfd);
    return ok;
}

void* DlopenWithFallback(const std::string& so) {
    void* h = dlopen(so.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
        std::string abs = "/data/storage/el1/bundle/libs/arm64/" + so;
        h = dlopen(abs.c_str(), RTLD_NOW | RTLD_GLOBAL);
    }
    return h;
}

// ---- Start 版 child：复刻官方伪代码 dlopen → dlsym(func) → func(args) ----
[[noreturn]] void StartChildMain(std::string so, std::string func,
                                 NativeChildProcess_Args args, int handshakeWfd) {
    for (int s = 1; s < 32; ++s) signal(s, SIG_DFL);   // 不继承主进程 handler（含 reaper）

    std::vector<int> keep{handshakeWfd};
    for (auto* p = args.fdList.head; p; p = p->next) keep.push_back(p->fd);
    CloseAllFdsExcept(keep);
    UnmapLowAnonRegions();
    prctl(PR_SET_NAME, func.substr(0, 15).c_str(), 0, 0, 0);

    void* h = DlopenWithFallback(so);
    if (!h) {
        fprintf(stderr, "[ncp_shim] dlopen %s failed: %s\n", so.c_str(), dlerror());
        _exit(125);
    }
    using EntryFn = void (*)(NativeChildProcess_Args);   // 官方约定入口签名
    auto fn = (EntryFn)dlsym(h, func.c_str());
    if (!fn) {
        fprintf(stderr, "[ncp_shim] dlsym %s failed\n", func.c_str());
        _exit(126);
    }

    ChildHandshakeOk(handshakeWfd);   // 入口就绪，通知 parent 返回成功
    fn(args);                          // Main 返回 = 子进程退出（官方语义）
    _exit(0);
}

} // namespace

extern "C" {

Ability_NativeChildProcess_ErrCode OH_Ability_StartNativeChildProcess(
    const char* entry, NativeChildProcess_Args args,
    NativeChildProcess_Options /* options 忽略：fork 天然同域 = NORMAL */, int32_t* pid)
{
    if (!entry || !pid) return NCP_ERR_INVALID_PARAM;
    std::string e(entry);
    auto pos = e.find(':');
    if (pos == std::string::npos || pos == 0 || pos + 1 >= e.size()) {
        return NCP_ERR_INVALID_PARAM;
    }

    InstallReaperOnce();
    int hs[2];
    if (pipe(hs) != 0) return NCP_ERR_INTERNAL;

    pid_t child = fork();
    if (child < 0) {
        close(hs[0]);
        close(hs[1]);
        return NCP_ERR_INTERNAL;
    }
    if (child == 0) {
        close(hs[0]);
        StartChildMain(e.substr(0, pos), e.substr(pos + 1), args, hs[1]);  // 不返回
    }

    // ---- parent ----
    close(hs[1]);
    bool ok = ParentWaitHandshake(hs[0]);
    // NCP "fd 所有权转移给子进程"在 fork 下要显式实现：关闭 parent 侧拷贝，
    // 否则 fd 泄漏 + 对端（wineserver/audio）永远收不到 EOF
    for (auto* p = args.fdList.head; p; p = p->next) close(p->fd);
    if (!ok) {
        *pid = -1;
        return NCP_ERR_LIB_LOADING_FAILED;   // 与官方"so 加载失败"语义一致
    }
    *pid = child;
    return NCP_NO_ERROR;
}

int OH_Ability_CreateNativeChildProcess(
    const char* libName, OH_Ability_OnNativeChildProcessStarted onProcessStarted)
{
    (void)libName;
    (void)onProcessStarted;
    // L0: virgl ZC 通道依赖 Binder 跨进程传 OHNativeWindow，fork 无法等价。
    // 快速失败 → GraphicsBroker 走 shm fallback。
    // 已确认调用方在 ret != NCP_NO_ERROR 时不等回调、直接降级，路径安全。
    OH_LOG_WARN(LOG_APP, "[ncp_shim] CreateNativeChildProcess(%{public}s) -> NOT_SUPPORTED",
                libName ? libName : "(null)");
    return NCP_ERR_NOT_SUPPORTED;
}

} // extern "C"
