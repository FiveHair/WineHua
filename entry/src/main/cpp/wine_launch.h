#pragma once

#include <string>
#include <vector>
#include <sys/types.h>
#include <napi/native_api.h>

struct LaunchParams {
    std::string exePath;
    std::string sockPath;
    std::string libPath;
    std::string homeDir;      // 用户 Download 目录 (Z: 映射)
    std::string sockDir;
    std::string sockName;
    std::string winehuaBin;
};

void LaunchThreadFunc(LaunchParams* p);
bool IsWinePrefixInitialized();

// -- 运行时模式切换: explorer /desktop 进程管理 --
// 用 LaunchThreadFunc 保存的启动参数现算 env (WINEHUA_DESKTOP_MODE 取当前模式)
// 并经 NCP 启动 explorer /desktop=shell,WxH。阻塞调用, NAPI 侧应放后台线程。
bool StartExplorerDesktopProcess();
// SIGKILL 记录的 explorer /desktop 进程, 返回被杀 pid (0=无)。
// 严禁改用 xdg close: 桌面窗口 SC_CLOSE → ExitWindows(0,0) 会广播
// ENDSESSION 杀掉所有 Wine 应用 (programs/explorer/desktop.c)。
pid_t StopExplorerDesktopProcess();
// 启动裸 explorer (无 /desktop 参数, 仅文件管理器 + shell 服务),
// 用于 desktop→multi 切换后恢复 systray/ShellExecute 等基础功能
bool StartBareExplorerProcess();
