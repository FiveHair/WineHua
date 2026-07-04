# HarmonyOS Pad 音频与 GL/VirGL 迁移要点

> 更新日期: 2026-07-04
>
> 目的: 把 WineHua 在 HarmonyOS Pad 上调通音频、GL/VirGL、外部交换目录时沉淀下来的关键知识整理成迁移笔记，方便在其他工程中按清单对应。

## 适用范围

这份文档关注 HarmonyOS Pad / arm64 设备上的 Wine 运行时适配，尤其是:

- Windows 程序通过 Wine 播放声音。
- Windows OpenGL/WGL 程序通过 EGL/GLES、guest Mesa、VirGL 走到设备屏幕。
- Wine 子进程、wineserver、wineboot 在 Pad 上的启动和环境传递。
- Download 外部目录与 `Z:` 盘交换目录映射。

不建议把 PC 模拟器上能跑的启动方式、OpenGL 方式直接搬到 Pad。Pad 上的关键差异是 appspawn / native child process、沙盒文件权限、EGL/GLES 图形栈，以及子进程环境和 fd 不会自然完整继承。

## 一句话结论

- Pad 上 Wine 子进程应通过 `OH_Ability_StartNativeChildProcess` 启动，复杂子进程链路需要一个主进程里的 process broker 代为启动。
- 音频应由宿主主进程持有 `OH_AudioRenderer`，Wine 子进程只通过 IPC + 共享内存 ring buffer 写 PCM。
- GL/VirGL 应拆成两段看: guest 渲染命令链路和最终上屏链路。`virgl ready` 只代表 socket / library / guest bundle 等基础条件就绪，不代表 WGL 的 `ChoosePixelFormat` 或 `wglCreateContext` 一定成功。
- PC 上常见的桌面 OpenGL 路径在 Pad 上不一定存在。Pad 通常是 EGL/GLES 中心的环境，WGL 需要明确走 GLES fallback 或 guest Mesa/VirGL。
- 初始化链路要保持干净。普通 `wineboot --init` 不要默认塞入完整 GL/VirGL 环境，避免图形环境失败反过来破坏 Wine prefix 初始化。

## Pad 与 PC 的核心差异

| 项目 | PC / x86_64 常见方式 | Pad / arm64 推荐方式 | 迁移注意点 |
| --- | --- | --- | --- |
| 进程创建 | `fork` / `execve` 可直接使用 | `OH_Ability_StartNativeChildProcess` / appspawn | Wine 内部 spawn 出来的子进程要通过 broker 回到主进程启动 |
| 环境变量 | 子进程自然继承较完整 | appspawn 子进程不保证继承完整环境 | 用 `entryParams` 追加 `|__env=KEY=VALUE`，在 native child 入口恢复 |
| fd 传递 | fork 后 fd 天然可继承 | 需要 `NativeChildProcess_Fd` 显式传入 | `WINESERVERSOCKET`、音频 bootstrap fd 都要命名传递 |
| Wine 初始化 | 可直接执行 `wine wineboot --init` | 顶层 wineboot 用 `wineboot --init` 进入 Wine | prefix 未创建前不要强行跑 `C:\windows\system32\wineboot.exe --init` |
| 图形 API | 可能有桌面 OpenGL / `EGL_OPENGL_BIT` | 多数情况下以 EGL/GLES 为中心 | `ChoosePixelFormat` 失败通常要查 EGL config、guest Mesa、GLES fallback |
| 文件映射 | `dosdevices/z:` symlink 通常可用 | 沙盒可能拒绝 symlink | 用 `WINEHUA_EXCHANGE_DIR` 做 env-backed `Z:` fallback |

## 进程启动链路

Pad 上不要只把 Wine 当成一个普通 native 可执行文件启动。推荐拆成三层:

1. 宿主 Ability 主进程准备 runtime:
   - Wayland socket / `XDG_RUNTIME_DIR`
   - Wine prefix / runtime bin 路径
   - process broker socket
   - audio broker
   - graphics broker
   - Download 交换目录

2. 顶层 Wine 进程通过 `OH_Ability_StartNativeChildProcess` 启动:
   - `wineboot --init` 用来初始化 prefix。
   - 普通 exe 启动时用 `wine <exe>` 或项目自己的入口。
   - 顶层 `wineboot` 不要设置 `WINESERVERSOCKET`，让它走 `server_connect()`，完成 `setup_config_dir()`、`drive_c`、`dosdevices` 等初始化。

3. Wine 内部继续 spawn 的子进程走 broker:
   - Wine 侧通过 `PROCESSBROKER` 找到主进程 broker socket。
   - broker 在主进程上下文调用 `OH_Ability_StartNativeChildProcess`。
   - broker 把必要 env 和 fd 传给 `libwine_child.so:Main`。

关键实现参考:

- `entry/src/main/cpp/napi_init.cpp`
- `entry/src/main/cpp/broker.cpp`
- `entry/src/main/cpp/wine_child.cpp`
- `thirdparty/wine/dlls/ntdll/unix/process.c`
- `thirdparty/wine/dlls/ntdll/unix/server.c`

必须稳定传递的基础 env:

```text
XDG_RUNTIME_DIR=<runtime socket dir>
WAYLAND_DISPLAY=<wayland socket name>
HOME=<app wine home>
WINEPREFIX=/data/storage/el2/base/files/.wine
WINEBINDIR=<runtime bin>
WINEUNIXDIR=<runtime bin>
WINEDLLDIR=<runtime bin>/x86_64-unix
WINEDLLPATH=<runtime bin>/x86_64-windows:<runtime bin>
WINEDLLOVERRIDES=mscoree,mshtml=
PROCESSBROKER=/data/storage/el2/base/files/.wine/../.wine_broker
WINEHUA_EXCHANGE_DIR=<Download/bundle/Exchange 或 sandbox fallback>
```

Pad / arm64 + Box64 还要注意:

```text
LD_LIBRARY_PATH=<native arm64 libs>
BOX64_LD_LIBRARY_PATH=<x86_64 guest/runtime libs>
BOX64_LOG=0
BOX64_NOBANNER=1
BOX64_SHOWSEGV=1
```

## 音频链路

音频的稳定模型是宿主主进程集中渲染:

```text
Windows app
  -> Wine mmdevapi / winmm / dsound / xaudio2
  -> wineohos.drv
  -> audio bootstrap fd
  -> private control socket
  -> shared-memory ring buffer
  -> host AudioBroker mixer
  -> OH_AudioRenderer
  -> speaker
```

核心原则:

- `OH_AudioRenderer` 只在宿主主进程或稳定的 broker 上创建，不要让每个 Wine 子进程直接创建系统音频设备。
- 控制面走 socket IPC，数据面走共享内存 ring buffer。
- `OH_AudioRenderer` callback 里只做读 ring、混音、补零，不做阻塞 IPC、文件 IO、复杂锁等待。
- 多个 Wine stream 在 host 侧混成一个 `OH_AudioRenderer`。

当前固定 mix 格式:

```text
sample rate: 48000
channels: 2
format: s16le
```

相关代码:

- Wine 侧: `thirdparty/wine/dlls/wineohos.drv/ohos.c`
- Wine 音频 IPC 客户端: `thirdparty/wine/dlls/wineohos.drv/ohos_audio_client.c`
- Host broker: `entry/src/main/cpp/audio_broker.cpp`
- IPC server: `entry/src/main/cpp/audio_ipc_server.cpp`
- Ring buffer: `entry/src/main/cpp/audio_stream.cpp`
- 共享协议: `shared/audio/audio_ipc_protocol.h`

音频 env / fd:

```text
WINE_OHOS_AUDIO_ENABLE=1
WINE_OHOS_AUDIO_BOOTSTRAP_FD=<fd>
WINE_OHOS_AUDIO_PROTOCOL_VERSION=1
```

Pad 上最重要的是 fd 传递。顶层 Wine 进程和 broker-spawn 的 Wine 子进程都要拿到一个有效的 `audio_bootstrap` fd:

- 主进程调用 `AudioBroker::EnsureStarted(runtimeDir)`。
- 主进程或 broker 调用 `AudioBroker::CreateBootstrapHandle()`。
- 通过 `NativeChildProcess_Fd` 以 `audio_bootstrap` 名字传给 child。
- `wine_child.cpp` 把 fd 恢复成 `WINE_OHOS_AUDIO_BOOTSTRAP_FD=<fd>`。

音频迁移清单:

- [ ] 宿主侧能创建并启动 `OH_AudioRenderer`。
- [ ] `AudioBroker` 日志出现 `renderer ready rate=48000 ch=2 callbackFrames=...`。
- [ ] Wine 子进程日志出现 `WINE_OHOS_AUDIO_BOOTSTRAP_FD=...`。
- [ ] Wine 侧 `ohos_audio_client_connect()` 能完成 bootstrap + HELLO。
- [ ] `OPEN_STREAM` 后 host 侧能创建 memfd/shared memory ring。
- [ ] `START` 后 callback 中能读到 queued frames。
- [ ] callback 无阻塞 IPC，无长时间 mutex 持有。
- [ ] underrun / overflow 日志可观测，方便判断是数据没写进来还是 host 没取走。

常见问题判断:

| 现象 | 优先检查 |
| --- | --- |
| Wine 侧提示 audio broker unreachable | `WINE_OHOS_AUDIO_BOOTSTRAP_FD` 是否存在且 fd 未被提前 close |
| host 没有 `renderer ready` | `OH_AudioStreamBuilder_*` 参数、权限、设备状态 |
| 有 `open stream` 但无声音 | ring buffer 是否写入、stream 是否 STARTED、callback 是否 underrun |
| 多进程只有第一个有声音 | broker-spawn 子进程是否也传了新的 `audio_bootstrap` fd |

## GL/VirGL 链路

GL 链路要拆开看:

```text
Windows EXE
  -> Wine opengl32 / win32u / winewayland.drv
  -> guest_gfx: Mesa libEGL/libGLES/libgallium
  -> GALLIUM_DRIVER=virpipe
  -> VTEST_SOCKET_NAME=<virgl.sock>
  -> virgl_test_server
  -> libvirglrenderer
  -> host EGL/GLES
  -> Wayland surface
  -> embedded compositor
  -> XComponent
```

宿主侧组件:

- `entry/src/main/cpp/graphics_broker.cpp`
- `entry/src/main/cpp/virgl_child.cpp`
- `entry/src/main/cpp/backend_detector.cpp`
- `entry/src/main/cpp/egl_renderer.cpp`
- `entry/src/main/cpp/cpu_shm_presenter.cpp`
- `entry/src/main/cpp/wayland_server.cpp`

guest 侧关键产物:

- `guest_gfx/lib/libEGL.so`
- `guest_gfx/lib/libGLESv2.so`
- `guest_gfx/lib/dri` 或 gallium 相关库
- `guest_gfx/winehua-guest-gfx.env`
- `virgl_test_server`
- `libvirglrenderer.so.1`

关键 env:

```text
WINEHUA_GRAPHICS_BACKEND=virgl
WINEHUA_GRAPHICS_ACTIVE=virgl
WINEHUA_GUEST_GFX_READY=1
WINEHUA_GUEST_GFX_MODE=mesa-virpipe
WINEHUA_GUEST_GFX_DIR=<runtime>/guest_gfx
WINEHUA_VIRGL_SOCKET_READY=1
WINEHUA_VIRGL_LIBRARY_READY=1
WINEHUA_VIRGL_SOCKET=<prefix>/graphics/virgl.sock
WINEHUA_VIRGL_READY=1
VTEST_SOCKET_NAME=<prefix>/graphics/virgl.sock
GALLIUM_DRIVER=virpipe
LIBGL_DRIVERS_PATH=<guest_gfx paths>
EGL_DRIVERS_PATH=<guest_gfx paths>
__EGL_VENDOR_LIBRARY_DIRS=<guest_gfx paths>
```

Pad / Box64 上还要确保 guest Mesa 的 EGL/GLES 作为 x86_64 emulated library 被加载，而不是误走 native linker namespace。可通过类似下面的环境强制:

```text
BOX64_EMULATED_LIBS=libEGL.so:libEGL.so.1:libGLESv2.so:libGLESv2.so.2:libwayland-client.so:libwayland-server.so:libwayland-egl.so:libdrm.so
```

## OpenGL 与 GLES 的关键区别

PC 上可以运行，不代表 Pad 上能用同一路径。原因通常是:

- PC 或 x86_64 模拟器可能能拿到 desktop OpenGL 兼容路径。
- Pad 系统图形栈更偏 EGL/GLES，`EGL_OPENGL_BIT` 可能缺失或不可用。
- Wine 的 WGL API 需要从 EGL configs 中枚举 pixel format。如果只存在 `EGL_OPENGL_ES2_BIT` / `EGL_OPENGL_ES3_BIT_KHR`，普通 desktop OpenGL WGL 路径可能枚举不到格式。
- `ChoosePixelFormat` 失败通常意味着 WGL pixel format 列表为空或和窗口 surface 不匹配。
- `wglCreateContext` 失败通常意味着 context API、EGL config、no-config context、GLES fallback 之间不匹配。

这类问题不要只看 `force_gl=0/1`。更应该看:

```text
WINEHUA_OPENGL_DIAG=1
WINEHUA_WGL_FORCE_GLES=1
EGL_LOG_LEVEL=debug
```

以及 Wine `win32u/opengl.c` 中类似诊断:

```text
egl configs total=... filtered=... force_gles=...
renderable_counts gl=... es1=... es2=... es3=...
using GLES renderable EGL configs as WGL forced path/fallback
desktop OpenGL context creation failed ... trying GLES2 WGL fallback
created GLES2 context through WGL fallback
no EGL configs advertise EGL_OPENGL_BIT or usable GLES2/3 bits
```

迁移判断顺序:

1. 先确认 `guest_gfx` 是否真的被 Wine 子进程加载。
2. 再确认 `VTEST_SOCKET_NAME` 是否可连接。
3. 再确认 EGL config 枚举是否有可用 GL/GLES renderable type。
4. 再确认 `ChoosePixelFormat` 是否得到非 0 format。
5. 最后看 `wglCreateContext`、`eglCreateContext`、`eglMakeCurrent`。

如果 `virgl socket/library/guest ready` 都是 1，但仍然 `ChoosePixelFormat` 失败，优先怀疑:

- Wine 子进程实际加载的是系统/stock EGL，不是 guest Mesa EGL。
- `BOX64_EMULATED_LIBS` 没覆盖 EGL/GLES/Wayland/DRM 相关库。
- `LIBGL_DRIVERS_PATH`、`EGL_DRIVERS_PATH`、`__EGL_VENDOR_LIBRARY_DIRS` 未传到真正的 Wine 子进程。
- `wineboot` / 普通 exe / broker-spawn 子进程之间 env 不一致。
- EGL configs 只有 GLES，但 WGL 代码仍按 desktop GL 过滤。
- 选到的 pixel format 与当前 Wayland/EGL surface 不兼容。

## VirGL server 在 Pad 上的启动方式

Pad 上 `virgl_test_server` 也应作为 native child 启动，不要假设可以在任意进程里 fork/exec:

- `GraphicsBroker` 准备 `<prefix>/graphics/virgl.sock`。
- Pad 模式调用 `OH_Ability_StartNativeChildProcess("libvirgl_child.so:VirglServerMain", ...)`。
- `virgl_child.cpp` 设置:

```text
VTEST_USE_GLES=1
VTEST_USE_EGL_SURFACELESS=1
EGL_PLATFORM=surfaceless
```

注意 host 侧和 guest 侧 env 不要混淆:

- guest 侧 Wine/Mesa 用 `GALLIUM_DRIVER=virpipe`，通过 `VTEST_SOCKET_NAME` 把命令发给 vtest server。
- host 侧 `virgl_test_server` 不应再设置 `GALLIUM_DRIVER=virpipe`，否则可能递归回 guest transport。
- host 侧需要能加载 `libvirglrenderer` 和 EGL/GLES runtime。

当前方案里 host `virgl_test_server` 可能设置了:

```text
LIBGL_ALWAYS_SOFTWARE=1
MESA_LOADER_DRIVER_OVERRIDE=swrast
```

这能提高兼容性，但也意味着性能判断要谨慎。看到 `virgl ready` 不等于已经获得硬件 3D 加速。

## 最终显示与刷新问题

当前显示链路可能是:

```text
Wayland wl_surface
  -> wl_shm commit
  -> CPU surface cache
  -> GL texture upload
  -> XComponent EGLSurface
```

即使 GL/VirGL 命令链路可用，最终上屏也可能仍有 CPU copy 和 GL upload。刷新不完整、画面更新慢、窗口变成小条或尺寸异常时，不要只查 WGL，也要查 compositor / presenter:

- Wayland surface 是否拿到正确 width / height。
- `wl_shm` commit 是否有 damage。
- damage rect 是否被错误合并、裁剪或丢失。
- `TakeFrame` / snapshot 路径是否还能取到完整 frame。
- `gl_compositor_direct` 是否正确绑定 XComponent surface。
- XComponent resize 后 EGLSurface 是否重建。
- desktop root toplevel 和普通 toplevel 的 frame source 是否取错。

区分两个层次:

- `ChoosePixelFormat` / `wglCreateContext` 是 Windows OpenGL 初始化问题。
- 画面只刷新一部分、窗口尺寸不对、系统弹窗异常，是 Wayland compositor / presenter / XComponent 上屏问题。

## 图形迁移清单

- [ ] `GraphicsBroker` 能创建 `<prefix>/graphics/virgl.sock`。
- [ ] `virgl_test_server` 在 Pad 上通过 native child 启动，并监听 socket。
- [ ] `libvirglrenderer.so.1` 可加载，关键 symbol 可解析。
- [ ] `guest_gfx/winehua-guest-gfx.env` 存在并随包安装。
- [ ] Wine 子进程日志显示 `WINEHUA_GUEST_GFX_READY=1`。
- [ ] Wine 子进程日志显示 `VTEST_SOCKET_NAME=<prefix>/graphics/virgl.sock`。
- [ ] `GALLIUM_DRIVER=virpipe` 只出现在 guest Wine/Mesa 侧。
- [ ] Pad / Box64 下 guest EGL/GLES/Wayland/DRM 库进入 `BOX64_EMULATED_LIBS`。
- [ ] `WINEHUA_WGL_FORCE_GLES=1` 时 EGL config 枚举能看到 GLES2/3 configs。
- [ ] `ChoosePixelFormat` 返回非 0。
- [ ] `wglCreateContext` 能创建 context，失败时能打印 EGL error。
- [ ] `SwapBuffers` 后 Wayland surface 有 commit。
- [ ] presenter 统计能区分 full upload / partial upload / snapshot copy / GL upload。

## 初始化链路不要被 GL 污染

之前最值得保留的教训是: 修 GL 时不要把启动链改坏。

推荐策略:

- `BuildWineEnv(..., includeGraphicsEnv=false)` 作为默认初始化环境。
- `wineboot --init`、普通 prefix 检查、非图形命令默认不注入完整 VirGL/Mesa env。
- 只有明确运行图形测试程序或用户 exe 需要 GL 时，才 `includeGraphicsEnv=true`。
- broker 只在请求参数明确需要 graphics 时追加 guest gfx env。
- `WINESERVERSOCKET` 只用于 broker-spawn 且已经收到 socket fd 的 Wine child，不用于顶层 wineboot。

如果为了修 WGL，把 `guest_gfx`、`VTEST_SOCKET_NAME`、`WINEHUA_WGL_FORCE_GLES` 默认塞进所有 Wine 初始化过程，容易出现:

- wineboot 初始化变慢或失败。
- prefix 没创建完整但 UI 提示已就绪。
- 后续启动没有 Wine 窗口。
- appspawn 子进程环境不一致，导致第一次能起、后续起不来。

## Download / `Z:` 盘交换目录

Pad 上外部资源交换建议使用:

```text
/storage/Users/currentUser/Download/<bundleName>/Exchange
```

并设置:

```text
WINEHUA_EXCHANGE_DIR=<上面的 Exchange 目录>
```

理想情况下:

```text
$WINEPREFIX/dosdevices/z: -> $WINEHUA_EXCHANGE_DIR
```

但 HarmonyOS 沙盒下 `symlink()` 可能失败。不要因为 `dosdevices/z:` symlink 失败就退回完全不可见的沙盒目录。更稳的做法是:

- 启动时创建 Download 下的 bundle 专属目录和 `Exchange` 子目录。
- 始终把最终可用路径写入 `WINEHUA_EXCHANGE_DIR`。
- `dosdevices/z:` symlink 成功就用 symlink。
- symlink 失败时保留 env-backed fallback，在 Wine path 解析层把 `Z:` 指向 `WINEHUA_EXCHANGE_DIR`。

相关代码参考:

- `entry/src/main/cpp/napi_init.cpp` 的 `EnsureDriveZMapping()`
- `thirdparty/wine/dlls/ntdll/unix/file.c` 中 `WINEHUA_EXCHANGE_DIR` fallback

## 日志诊断清单

启动链路:

```text
[Launch-Async] wineserver + wineboot starting in background
[Launch-Async] running wineboot --init via appspawn
[Launch-Async] top-level wineboot starts without WINESERVERSOCKET
[Broker] request entryParams=...
[Broker] StartNativeChildProcess ret=0 childPid=...
[WineChild] Main() ENTER
[WineChild] WINESERVERSOCKET=...
[WineChild] audio bootstrap fd not provided / WINE_OHOS_AUDIO_BOOTSTRAP_FD=...
```

音频:

```text
[AudioBroker] started runtimeDir=...
[AudioBroker] renderer ready rate=48000 ch=2 callbackFrames=...
[AudioBroker] bootstrap accepted conn=...
[AudioBroker] open stream id=...
[AudioBroker] start stream id=...
trace+ohosaudio
ohos_audio_client_connect
ohos_audio_client_open_stream
```

图形 / VirGL:

```text
[GraphicsBroker] virgl_test_server runtime: reuseGuestMesa=...
[GraphicsBroker] StartNativeChildProcess virgl ret=0 childPid=...
[GraphicsBroker] virgl native child pid=... listening at ...
[WineChild] guest gfx env active=virgl guestLib=... socket=...
[WineChild] guest gfx mesa env vtest=... gallium=virpipe ...
winehua_virgl_guest_probe: connected to ...
egl configs total=... filtered=...
using GLES renderable EGL configs as WGL ...
desktop OpenGL context creation failed ... trying GLES2 WGL fallback
created GLES2 context through WGL fallback
ChoosePixelFormat
wglCreateContext
```

显示 / 刷新:

```text
WINEHUA_FRAME_PRESENTER=...
WINEHUA_FRAME_TRANSPORT=...
full_upload=...
partial_upload=...
commit_copy_mb=...
snapshot_copy_mb=...
gl_upload_mb=...
```

## 推荐落地顺序

1. 先跑通 Wine prefix 初始化，不带 GL/VirGL 环境。
2. 跑通 process broker，确认 Wine 内部 spawn 的子进程都能创建。
3. 跑通 Download 交换目录和 `Z:` fallback。
4. 跑通音频 broker，先用简单 wav / audio smoke 验证。
5. 启动 Wayland / XComponent 基础显示，先验证普通 explorer 窗口。
6. 启动 VirGL server，只验证 socket、library、guest bundle ready。
7. 让 Wine 图形子进程加载 guest Mesa，确认 `VTEST_SOCKET_NAME` 和 `GALLIUM_DRIVER=virpipe`。
8. 单独处理 WGL: `ChoosePixelFormat`、`wglCreateContext`、`SwapBuffers`。
9. 最后优化刷新和性能: direct compositor、damage rect、partial upload、NativeBuffer/EGLImage 能力验证。

这个顺序的重点是把启动、音频、GL 初始化、最终上屏拆开。Pad 上任何一层失败都可能表现成“初始化失败”或“窗口异常”，但根因完全不同。
