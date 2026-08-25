#ifndef WINE_SPAWNER_H
#define WINE_SPAWNER_H

/**
 * spawner.h — SpawnRequest/Spawner: 进程启动的意图与机制分离 (重构第 4 步)
 *
 * 调用方只声明 "启动什么 + 带什么 env 增量"; 机制细节由 kind 推导:
 *   - 路由: Wineserver/Wineboot → NCP 直启; 其余 → broker SPAWN (SpawnViaBroker)
 *   - 入口符号: NCP 路线 WineserverMain/Main
 *   - token 布局: homeDir 前缀 (NCP) / __winehua_desktop__ / guest|host elf 标记 /
 *     x86_64 的 wine 加载器前缀
 *   - 会话权威: NCP 路线尾部追加 WINEPREFIX=<session> (与 broker 服务端尾部
 *     追加同级语义); Wineserver 在 smoke prefix 下自动带退出遥测
 *
 * 刻意不在请求里的: homeDir/prefixDir (会话单例, ConfigureSession 设置;
 * broker 路线由 broker 服务端权威), fd (broker 自动挂 audio bootstrap)。
 */

#include <string>
#include <vector>

namespace winehua {

enum class SpawnKind {
    Wineserver,    // NCP WineserverMain: argv 固定 "wineserver -f -p", 极简基线
    Wineboot,      // NCP Main: argv 固定 "wineboot --init", 极简 env (省 entryParams 长度)
    DesktopShell,  // broker Main: explorer + argv (桌面 shell)
    WineExe,       // broker Main: argv = [exePath, args...]
    GuestElf,      // broker Main: __winehua_guest_elf__ + argv (guest vulkan smoke)
    HostElf,       // broker Main: __winehua_host_elf__ + argv (host vulkan probe)
};

struct SpawnRequest {
    SpawnKind kind;
    // DesktopShell: explorer 的参数; WineExe/GuestElf/HostElf: [exePath, args...];
    // Wineserver/Wineboot: 忽略 (argv 由 kind 固定)
    std::vector<std::string> argv;
    // K=V 增量行 (BuildSessionEnv 成品或极简集); NCP 路线经 EnvSpec 序列化
    // (fd 变量/不可编码条目自动过滤), broker 路线经 SpawnViaBroker 序列化
    std::vector<std::string> env;
    // __winehua_desktop__ token (explorer 桌面 / wineboot 首启的桌面 surface 路由)
    bool desktopSurface = false;
    // 空 = 会话默认 (ConfigureSession 的 binDir); RunWineExe 等 ArkTS 显式
    // 传 binDir 的路径在此透传
    std::string binDir;
};

class Spawner {
public:
    // 会话上下文。NCP 路线 (Wineserver/Wineboot) 先于 broker 启动, 不能依赖
    // broker 的 gBroker* 全局, 必须在每个 launch 会话开头先调一次。
    // 仅 NCP 路线消费; broker 路线的 homeDir/WINEPREFIX 权威在 broker 服务端。
    static void ConfigureSession(std::string homeDir, std::string binDir, std::string prefixDir);

    // 返回子进程 pid, <= 0 表示失败 (失败原因在内部已记日志)
    static pid_t Spawn(const SpawnRequest& req);
};

} // namespace winehua

#endif // WINE_SPAWNER_H
