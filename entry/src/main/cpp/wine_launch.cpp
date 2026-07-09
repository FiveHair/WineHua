#include "wine_launch.h"
#include "wine_process.h"
#include "wine_env.h"
#include "wine_constants.h"
#include "wayland_server.h"
#include "audio_ipc_protocol.h"
#include "graphics_broker.h"

#include <unistd.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <string>
#include <thread>
#include <vector>

#undef LOG_TAG
#define LOG_TAG "WL_NAPI"
#include <hilog/log.h>

#include "broker.h"
#include "wait_utils.h"

#ifdef PAD_MODE
#include <AbilityKit/native_child_process.h>

// Pad: 从 envp 重建 environ (fork 后 __wine_main / main 从 environ 读环境变量)
static void rebuild_environ(char* const* envp) {
    extern char** environ;
    environ = (char**)envp;
}
#endif

// 轮询 wineserver socket 是否就绪
static bool IsWineserverSocketReady() {
    const char* prefix = WINE_PREFIX;
    char sockDir[512];
    snprintf(sockDir, sizeof(sockDir), "%s/.wineserver", prefix);
    DIR* d = opendir(sockDir);
    if (!d) return false;
    bool found = false;
    struct dirent* de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char sockPath[1024];
        snprintf(sockPath, sizeof(sockPath), "%s/%s/socket", sockDir, de->d_name);
        struct stat st;
        if (stat(sockPath, &st) == 0 && S_ISSOCK(st.st_mode)) { found = true; break; }
    }
    closedir(d);
    return found;
}

static bool FileHasData(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

static bool DirExists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool IsWinePrefixInitialized() {
    return FileHasData(WINE_PREFIX "/system.reg") &&
           FileHasData(WINE_PREFIX "/user.reg") &&
           DirExists(WINE_PREFIX "/drive_c/windows/system32") &&
           DirExists(WINE_PREFIX "/drive_c/users");
}

static void RemovePathRecursive(const std::string& path)
{
    struct stat st;

    if (lstat(path.c_str(), &st) != 0) return;
    if (!S_ISDIR(st.st_mode))
    {
        unlink(path.c_str());
        return;
    }

    DIR* dir = opendir(path.c_str());
    if (!dir) return;

    while (dirent* entry = readdir(dir))
    {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        RemovePathRecursive(path + "/" + entry->d_name);
    }
    closedir(dir);
    rmdir(path.c_str());
}

static void ClearStaleWineserverState()
{
    const std::string wineserverDir = WINE_PREFIX "/.wineserver";

    if (DirExists(wineserverDir.c_str()))
    {
        OH_LOG_INFO(LOG_APP, "[Launch-Async] clearing stale wineserver state before explorer");
        RemovePathRecursive(wineserverDir);
    }
}

static bool HasRuntimeFileExtension(const char* name)
{
    const char* dot = strrchr(name, '.');
    if (!dot) return false;
    return !strcasecmp(dot, ".dll") ||
           !strcasecmp(dot, ".drv") ||
           !strcasecmp(dot, ".sys") ||
           !strcasecmp(dot, ".exe");
}

static bool EnsureDir(const std::string& path, mode_t mode)
{
    if (DirExists(path.c_str())) return true;
    if (mkdir(path.c_str(), mode) == 0 || errno == EEXIST) return DirExists(path.c_str());
    OH_LOG_ERROR(LOG_APP, "[Launch-Async] mkdir %{public}s failed: %{public}s",
                 path.c_str(), strerror(errno));
    return false;
}

static bool EnsureDirRecursive(const std::string& path, mode_t mode)
{
    if (path.empty() || path == "/") return true;
    if (DirExists(path.c_str())) return true;

    size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && slash > 0)
    {
        if (!EnsureDirRecursive(path.substr(0, slash), mode)) return false;
    }
    return EnsureDir(path, mode);
}

static bool CopyFileIfNeeded(const std::string& src, const std::string& dst)
{
    struct stat srcSt;
    struct stat dstSt;
    if (stat(src.c_str(), &srcSt) != 0 || !S_ISREG(srcSt.st_mode)) return false;
    if (stat(dst.c_str(), &dstSt) == 0 && S_ISREG(dstSt.st_mode) && dstSt.st_size == srcSt.st_size)
        return true;

    int inFd = open(src.c_str(), O_RDONLY);
    if (inFd < 0)
    {
        OH_LOG_ERROR(LOG_APP, "[Launch-Async] open src %{public}s failed: %{public}s",
                     src.c_str(), strerror(errno));
        return false;
    }

    int outFd = open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (outFd < 0)
    {
        OH_LOG_ERROR(LOG_APP, "[Launch-Async] open dst %{public}s failed: %{public}s",
                     dst.c_str(), strerror(errno));
        close(inFd);
        return false;
    }

    char buffer[64 * 1024];
    bool ok = true;
    ssize_t n;
    while ((n = read(inFd, buffer, sizeof(buffer))) > 0)
    {
        char* p = buffer;
        ssize_t remaining = n;
        while (remaining > 0)
        {
            ssize_t w = write(outFd, p, remaining);
            if (w < 0)
            {
                ok = false;
                OH_LOG_ERROR(LOG_APP, "[Launch-Async] write dst %{public}s failed: %{public}s",
                             dst.c_str(), strerror(errno));
                break;
            }
            p += w;
            remaining -= w;
        }
        if (!ok) break;
    }
    if (n < 0)
    {
        ok = false;
        OH_LOG_ERROR(LOG_APP, "[Launch-Async] read src %{public}s failed: %{public}s",
                     src.c_str(), strerror(errno));
    }

    close(outFd);
    close(inFd);
    if (!ok) unlink(dst.c_str());
    return ok;
}

static bool EnsureWow64Files(const std::string& binDir)
{
    const std::string srcDir = binDir + "/i386-windows";
    const std::string dstDir = WINE_PREFIX "/drive_c/windows/syswow64";

    DIR* src = opendir(srcDir.c_str());
    if (!src)
    {
        OH_LOG_ERROR(LOG_APP, "[Launch-Async] wow64 source missing %{public}s: %{public}s",
                     srcDir.c_str(), strerror(errno));
        return false;
    }
    if (!EnsureDirRecursive(dstDir, 0777))
    {
        closedir(src);
        return false;
    }

    int total = 0;
    int copied = 0;
    int failed = 0;
    while (dirent* entry = readdir(src))
    {
        if (entry->d_name[0] == '.' || !HasRuntimeFileExtension(entry->d_name)) continue;
        total++;
        std::string srcPath = srcDir + "/" + entry->d_name;
        std::string dstPath = dstDir + "/" + entry->d_name;
        if (CopyFileIfNeeded(srcPath, dstPath)) copied++;
        else failed++;
    }
    closedir(src);

    OH_LOG_INFO(LOG_APP, "[Launch-Async] wow64 syswow64 total=%{public}d ok=%{public}d failed=%{public}d",
                total, copied, failed);
    return total > 0 && failed == 0;
}

#ifdef PAD_MODE
static bool LaunchPadMode(LaunchParams* p, int audioBootstrapFd) {
    // 通过 fdList 传递 audio bootstrap fd (仅 explorer 需要音频)
    NativeChildProcess_Fd audioFdNode;
    audioFdNode.fdName = const_cast<char*>("wine_audio_bootstrap");
    audioFdNode.fd = audioBootstrapFd;
    audioFdNode.next = nullptr;

    ClearStaleWineserverState();

    // -- wineserver via NCP --
    {
        std::string wsEntryParams = p->homeDir + "|" + p->winehuaBin + "|wineserver|-f|-p|--no-auto-close";
        OH_LOG_INFO(LOG_APP, "[Launch-Async] wineserver args=%{public}s", wsEntryParams.c_str());
        NativeChildProcess_Args wsArgs = {};
        wsArgs.entryParams = const_cast<char*>(wsEntryParams.c_str());
        NativeChildProcess_Options wsOpts = {};
        wsOpts.isolationMode = NCP_ISOLATION_MODE_NORMAL;
        int32_t wsChildPid = -1;
        auto wsRet = OH_Ability_StartNativeChildProcess(
            "libwine_child.so:WineserverMain", wsArgs, wsOpts, &wsChildPid);
        if (wsRet != NCP_NO_ERROR) {
            OH_LOG_ERROR(LOG_APP, "[Launch-Async] wineserver StartNativeChildProcess FAILED ret=%{public}d", (int)wsRet);
            if (gStateTsfn)
                napi_call_threadsafe_function(gStateTsfn, strdup("wineserver-failed"), napi_tsfn_blocking);
            return false;
        }
        OH_LOG_INFO(LOG_APP, "[Launch-Async] wineserver pid=%{public}d (via appspawn)", wsChildPid);
        if (!WaitFor("wineserver socket", IsWineserverSocketReady, 5000, 100)) {
            OH_LOG_WARN(LOG_APP, "[Launch-Async] wineserver socket not detected, "
                        "wineboot will recover via server_connect retry+start_server");
        }
    }

    if (gStateTsfn)
        napi_call_threadsafe_function(gStateTsfn, strdup("wineboot-starting"), napi_tsfn_blocking);

    gBrokerHomeDir = p->homeDir;
    StartBrokerServer();
    setenv("PROCESSBROKER", WINE_BROKER_SOCKET, 1);

    if (!EnsureWow64Files(p->winehuaBin))
    {
        OH_LOG_ERROR(LOG_APP, "[Launch-Async] wow64 syswow64 setup failed before wineboot, abort");
        if (gStateTsfn)
            napi_call_threadsafe_function(gStateTsfn, strdup("wineboot-failed"), napi_tsfn_blocking);
        return false;
    }

    // -- wineboot --init --
    bool prefixReady = IsWinePrefixInitialized();

    if (!prefixReady) {
        OH_LOG_INFO(LOG_APP, "[Launch-Async] prefix not ready, running wineboot --init...");
        auto* ws = WaylandServer::GetInstance();
        ws->SetDesktopRootRecognitionEnabled(false);
#ifdef __aarch64__
        std::string entryParams = p->homeDir + "|" + p->winehuaBin + "|wineboot|--init";
#else
        std::string entryParams = p->homeDir + "|" + p->winehuaBin + "|wine|wineboot|--init";
#endif
        NativeChildProcess_Args childArgs = {};
        childArgs.entryParams = const_cast<char*>(entryParams.c_str());
        NativeChildProcess_Options options = {};
        options.isolationMode = NCP_ISOLATION_MODE_NORMAL;
        int32_t childPid = -1;
        auto ret = OH_Ability_StartNativeChildProcess(
            "libwine_child.so:Main", childArgs, options, &childPid);
        if (ret != NCP_NO_ERROR) {
            ws->SetDesktopRootRecognitionEnabled(true);
            OH_LOG_ERROR(LOG_APP, "[Launch-Async] wineboot FAILED ret=%{public}d", (int)ret);
            if (gStateTsfn)
                napi_call_threadsafe_function(gStateTsfn, strdup("wineboot-failed"), napi_tsfn_blocking);
            return false;
        }
        OH_LOG_INFO(LOG_APP, "[Launch-Async] wineboot started, pid=%{public}d", childPid);
        if (!WaitFor("wine prefix initialization", IsWinePrefixInitialized, 60000, 500)) {
            ws->SetDesktopRootRecognitionEnabled(true);
            OH_LOG_ERROR(LOG_APP, "[Launch-Async] wineboot prefix initialization timeout, abort");
            if (gStateTsfn)
                napi_call_threadsafe_function(gStateTsfn, strdup("wineboot-failed"), napi_tsfn_blocking);
            return false;
        }
        ws->SetDesktopRootRecognitionEnabled(true);
        ws->PromotePendingDesktopRoot();
        OH_LOG_INFO(LOG_APP, "[Launch-Async] wineboot completed");
    } else {
        WaylandServer::GetInstance()->SetDesktopRootRecognitionEnabled(true);
        OH_LOG_INFO(LOG_APP, "[Launch-Async] prefix already initialized, skipping wineboot");
    }

    // -- explorer desktop shell --
    {
        auto* ws = WaylandServer::GetInstance();
        int dw = ws->outputW_ > 0 ? ws->outputW_ : 1280;
        int dh = ws->outputH_ > 0 ? ws->outputH_ : 720;
        char desktopArg[128];
        snprintf(desktopArg, sizeof(desktopArg), "/desktop=shell,%dx%d", dw, dh);
#ifdef __aarch64__
        std::string exEntry = p->homeDir + "|" + p->winehuaBin + "|explorer|" + desktopArg;
#else
        std::string exEntry = p->homeDir + "|" + p->winehuaBin + "|wine|explorer|" + desktopArg;
#endif
        NativeChildProcess_Args exArgs = {};
        exArgs.entryParams = const_cast<char*>(exEntry.c_str());
        exArgs.fdList.head = (audioBootstrapFd >= 0) ? &audioFdNode : nullptr;
        NativeChildProcess_Options exOpts = {};
        exOpts.isolationMode = NCP_ISOLATION_MODE_NORMAL;
        int32_t exPid = -1;
        auto exRet = OH_Ability_StartNativeChildProcess(
            "libwine_child.so:Main", exArgs, exOpts, &exPid);
        OH_LOG_INFO(LOG_APP, "[Launch-Async] explorer desktop pid=%{public}d ret=%{public}d",
                    exPid, (int)exRet);
    }
    return true;
}
#else
static bool LaunchPcMode(LaunchParams* p, int audioBootstrapFd) {
    // -- wineserver via fork + execve --
    {
        std::vector<std::string> wsEnvStrs = p->envStrs;
        std::vector<char*> wsEnvp;
        for (auto& s : wsEnvStrs) wsEnvp.push_back((char*)s.c_str());
        wsEnvp.push_back(nullptr);
        int wsPipe[2];
        bool wsPipeOk = (pipe(wsPipe) == 0);
        if (!wsPipeOk)
            OH_LOG_ERROR(LOG_APP, "[Launch-Async] wineserver pipe failed: %{public}s", strerror(errno));

        pid_t wsPid = fork();
        if (wsPid == 0) {
            if (wsPipeOk) {
                close(wsPipe[0]); dup2(wsPipe[1], STDERR_FILENO);
                if (wsPipe[1] > 2) close(wsPipe[1]);
            }
            CloseInheritedFds({STDOUT_FILENO, STDERR_FILENO, audioBootstrapFd});
            for (int s = 1; s < 32; ++s) signal(s, SIG_DFL);
            prctl(PR_SET_NAME, "wl-wineserver", 0, 0, 0);
            chdir(p->winehuaBin.c_str());
            const char* wsArgv[] = {"./box64", "./wineserver", nullptr};
            execve("./box64", (char* const*)wsArgv, wsEnvp.data());
            _exit(127);
        }
        if (wsPid > 0) {
            OH_LOG_INFO(LOG_APP, "[Launch-Async] wineserver pid=%{public}d", wsPid);
            if (wsPipeOk) { close(wsPipe[1]); StartStderrLogger(wsPipe[0], "wineserver-stderr"); }
            if (!WaitFor("wineserver socket (PC)", IsWineserverSocketReady, 5000, 100)) {
                OH_LOG_WARN(LOG_APP, "[Launch-Async] wineserver socket not detected, "
                            "wineboot will recover via server_connect retry");
            }
        } else {
            OH_LOG_ERROR(LOG_APP, "[Launch-Async] wineserver fork failed");
            if (wsPipeOk) { close(wsPipe[0]); close(wsPipe[1]); }
            if (audioBootstrapFd >= 0) close(audioBootstrapFd);
            if (gStateTsfn)
                napi_call_threadsafe_function(gStateTsfn, strdup("wineserver-failed"), napi_tsfn_blocking);
            return false;
        }
    }

    if (gStateTsfn)
        napi_call_threadsafe_function(gStateTsfn, strdup("wineboot-starting"), napi_tsfn_blocking);

    // -- wineboot --init via fork + execve --
    {
        OH_LOG_INFO(LOG_APP, "[Launch-Async] running wineboot --init...");
        pid_t bootPid = fork();
        if (bootPid == 0) {
            CloseInheritedFds({STDOUT_FILENO, STDERR_FILENO, audioBootstrapFd});
            for (int s = 1; s < 32; ++s) signal(s, SIG_DFL);
            prctl(PR_SET_NAME, "wl-wineboot", 0, 0, 0);
            chdir(p->winehuaBin.c_str());
            const char* bootArgv[] = {"./box64", "./wine", "./wineboot", "--init", nullptr};
            execve("./box64", (char* const*)bootArgv, p->envp.data());
            _exit(127);
        }
        if (audioBootstrapFd >= 0) {
            close(audioBootstrapFd);
            audioBootstrapFd = -1;
        }
        if (bootPid > 0) {
            int bootStatus = 0;
            waitpid(bootPid, &bootStatus, 0);
            LogProcessExit("wineboot", bootPid, bootStatus);
        }
    }
    return true;
}
#endif

void LaunchThreadFunc(LaunchParams* p) {
    OH_LOG_INFO(LOG_APP, "[Launch-Async] wineserver + wineboot + wine starting in background");
    OH_LOG_INFO(LOG_APP, "[Launch-Async] XKB_CONFIG_ROOT=%{public}s",
                (p->winehuaBin + "/../share/X11/xkb").c_str());

    winehua::GraphicsBroker::GetInstance().SetWineRuntimeBinaryDir(p->winehuaBin);
    winehua::GraphicsBroker::GetInstance().EnsureStarted(p->sockDir);

    int audioBootstrapFd = CreateAudioBootstrapFd(p->sockDir);
    p->envStrs = BuildWineEnv(p->sockDir, p->sockName, p->libPath, p->winehuaBin,
                               audioBootstrapFd, p->homeDir);
    for (auto& s : p->envStrs) p->envp.push_back((char*)s.c_str());
    p->envp.push_back(nullptr);

    mkdir(WINE_PREFIX, 0755);

    if (gStateTsfn)
        napi_call_threadsafe_function(gStateTsfn, strdup("wineserver-starting"), napi_tsfn_blocking);

    bool ok = false;
#ifdef PAD_MODE
    ok = LaunchPadMode(p, audioBootstrapFd);
#else
    ok = LaunchPcMode(p, audioBootstrapFd);
#endif

    if (ok && gStateTsfn)
        napi_call_threadsafe_function(gStateTsfn, strdup("wine-ready"), napi_tsfn_blocking);

    delete p;
}
