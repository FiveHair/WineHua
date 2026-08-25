#include "spawner.h"
#include "env_spec.h"
#include "wine_constants.h"
#include "wine_exe.h"  // SpawnViaBroker

#include <sys/types.h>
#include <AbilityKit/native_child_process.h>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_SPAWN"
#include <hilog/log.h>

namespace winehua {
namespace {

// 会话上下文 (见 spawner.h); 仅 LaunchThreadFunc 写, NCP 路线 spawn 读
std::string gHomeDir, gBinDir, gPrefixDir;

constexpr const char* kNcpEntryMain = "libwine_child.so:Main";
constexpr const char* kNcpEntryWineserver = "libwine_child.so:WineserverMain";

// x86_64 原生需要 wine 加载器 token ("wine" 作为 argv[0]); aarch64 由
// wine_child Main 直接 dlopen box64 跑 binDir/wine ELF, argv 不含该前缀
#ifdef __aarch64__
constexpr bool kNeedsWineLoaderToken = false;
#else
constexpr bool kNeedsWineLoaderToken = true;
#endif

void AppendToken(std::string& out, const std::string& tok) {
    out += "|";
    out += tok;
}

pid_t SpawnViaNcp(const char* entryPoint, const std::string& entryParams) {
    OH_LOG_INFO(LOG_APP, "[Spawner] NCP %{public}s params=%{public}s", entryPoint, entryParams.c_str());
    NativeChildProcess_Args args = {};
    args.entryParams = const_cast<char*>(entryParams.c_str());
    NativeChildProcess_Options opts = {};
    opts.isolationMode = NCP_ISOLATION_MODE_NORMAL;
    int32_t pid = -1;
    const auto ret = OH_Ability_StartNativeChildProcess(entryPoint, args, opts, &pid);
    if (ret != NCP_NO_ERROR || pid <= 0) {
        OH_LOG_ERROR(LOG_APP, "[Spawner] NCP %{public}s FAILED ret=%{public}d pid=%{public}d",
                     entryPoint, (int)ret, (int)pid);
        return -1;
    }
    return pid;
}

} // namespace

void Spawner::ConfigureSession(std::string homeDir, std::string binDir, std::string prefixDir) {
    gHomeDir = std::move(homeDir);
    gBinDir = std::move(binDir);
    gPrefixDir = std::move(prefixDir);
}

pid_t Spawner::Spawn(const SpawnRequest& req) {
    const std::string& binDir = req.binDir.empty() ? gBinDir : req.binDir;

    // ---- NCP 直启路线 (wineserver / wineboot: 先于/独立于 broker) ----
    if (req.kind == SpawnKind::Wineserver || req.kind == SpawnKind::Wineboot) {
        // token 布局: homeDir|binDir|[desktop]|[wine]|argv...|__env=...
        // (wine_child Main/WineserverMain 按此解析; __env 由 EnvSpec 序列化,
        // fd 变量与不可编码条目自动过滤)
        std::string params = gHomeDir + "|" + binDir;
        if (req.desktopSurface) params += "|__winehua_desktop__";
        const char* entryPoint = kNcpEntryMain;
        if (req.kind == SpawnKind::Wineserver) {
            entryPoint = kNcpEntryWineserver;
            params += "|wineserver|-f|-p";
        } else {
            if (kNeedsWineLoaderToken) params += "|wine";
            params += "|wineboot|--init";
        }
        EnvSpec env = EnvSpec::fromLines(req.env);
        // 会话权威: WINEPREFIX 最后写入 (后写胜出), 与 broker 服务端尾部追加同级
        env.set("WINEPREFIX", gPrefixDir);
        if (req.kind == SpawnKind::Wineserver && gPrefixDir == WINE_SMOKE_PREFIX)
            env.set("WINEHUA_PROCESS_EXIT_TELEMETRY", "1");
        params += env.serializeEntryParams();
        return SpawnViaNcp(entryPoint, params);
    }

    // ---- broker 路线 (broker 服务端补 homeDir 前缀 / WINEPREFIX 权威 / audio fd) ----
    std::string params = binDir;
    switch (req.kind) {
    case SpawnKind::DesktopShell:
        params += "|__winehua_desktop__";
        if (kNeedsWineLoaderToken) params += "|wine";
        params += "|explorer";
        break;
    case SpawnKind::WineExe:
        if (kNeedsWineLoaderToken) params += "|wine";
        break;
    case SpawnKind::GuestElf:
        params += "|__winehua_guest_elf__";
        break;
    case SpawnKind::HostElf:
        params += "|__winehua_host_elf__";
        break;
    default:
        break;
    }
    for (const std::string& arg : req.argv) AppendToken(params, arg);
    const pid_t pid = SpawnViaBroker(params, req.env);
    if (pid <= 0)
        OH_LOG_ERROR(LOG_APP, "[Spawner] broker spawn FAILED kind=%{public}d params=%{public}s",
                     (int)req.kind, params.c_str());
    return pid;
}

} // namespace winehua
