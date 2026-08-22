# cnc-ddraw：DirectDraw 怎么工作、怎么用

WineHua 用上游 [cnc-ddraw](https://github.com/FunkyFr3sh/cnc-ddraw) v7.1.0.0 做 **PE overlay**，不改 Wine 的 `ddraw.dll` 源码。红警 2 / 尤里的复仇这类 32 位 DirectDraw 游戏默认走这条路。

基线实测：[cnc-ddraw-baseline.md](cnc-ddraw-baseline.md)  
游戏矩阵：[opengl-compat-matrix.md](opengl-compat-matrix.md)

## 现在能不能 GPU 加速红警 2？

**能。默认就会走 GPU 合成/上屏，不需要开 Zink，也不需要改游戏目录。**

要分清三件事：

| 说法 | 实际 |
|------|------|
| GPU 加速（相对 GDI / `glReadPixels`） | **是。** 每帧把游戏画好的 primary 用 `glTexSubImage2D` 交给 guest OpenGL，宿主 VirGL GLES 合成，`GPU_ACTIVE` 后走零拷贝 present，不再每帧 CPU 读回整屏。 |
| 游戏内部 3D 光栅化在 GPU 上 | **不是。** 红警 2 仍是 CPU 画 2D primary（调色板/软件 blit），cnc-ddraw 只负责把这块表面送到 GPU 去缩放和显示。 |
| 已经是 Zink / DXVK / 桌面 GL 3.2 | **不是。** 默认仍是 virpipe → host GLES。Zink 是给 Quake3、GL 3.2 准备的，默认不编译、不启用。 |

2026-08-22 在设备上用 **尤里的复仇** `gamemd.exe` 验证过：`selected renderer=opengl`、`consumer attached vk=0`、`GPU_ACTIVE`、`present_ret=0`、guest `winehua_gl_zero_copy` 上涨且不再持续 `glReadPixels`。红警 2 与 YR 同一 overlay / 同一组 ini（`[ra2]` / `[ra2md]` / `[gamemd]`），预期同一条 GPU 路径。

没接零拷贝时是 SHM `glReadPixels` ~242 ms、大约 4 fps，那是 **CPU 读回**，不是这条 GPU 路径。

## 整份移植算完成了吗？

| 部分 | 状态 |
|------|------|
| cnc-ddraw overlay 打包、默认启用、红警 2/YR 出画 | **完成**（YR 已上机） |
| GPU present（VirGL zero-copy，不是 GDI） | **完成**（YR 已上机） |
| 代码/脚本：Zink 构建、`WINEHUA_GL_BACKEND`、opengl32 smoke | **完成**（源码落地） |
| 默认 HAP 里编进 Zink、Quake3 / Unity / `openglcore` 上机 | **未完成**（`make guest-gfx-zink` 是 opt-in，默认不编） |

测红警 2：用当前默认 virpipe + overlay 即可。不要设 `WINEHUA_GL_BACKEND=zink`。

## 它怎么工作

游戏调用的仍是 Windows `ddraw.dll`。WineHua 在 32 位进程里用 **原生 cnc-ddraw** 盖掉 Wine 内置（wined3d）那份——内置那条在西木启动器上会黑屏、调色板错乱或报「找不到图形卡」。

```
ra2.exe / gamemd.exe  (32 位 PE，Box64)
  → cnc-ddraw overlay  ddraw.dll
  → 伪造 HAL / DirectDrawEnumerate（过启动器）
  → renderer=opengl：Lock/Unlock 后 glTexSubImage2D 上传 primary
  → Wine opengl32 / WGL
  → winewayland.drv  pbuffer + present-surface id
  → guest Mesa virpipe
  → vtest WINEHUA_PRESENT
  → 宿主 virglrenderer GLES SURFACE_QUEUE
  → 桌面 compositor EXTERNAL_OES（GPU 上屏）
```

环境由 `AppendDdrawBackendEnv` 注入（游戏目录不用自己放 dll）：

- `WINEDLLOVERRIDES=ddraw=n,b`：32 位优先 native overlay；64 位没有 x64 overlay 时回退 Wine 内置（否则 Explorer 会闪退）
- `WINEHUA_DDRAW_ROOT` + ntdll `search_winehua_ddraw_overlay`：和 DXVK 一样经 `\\??\\unix` 打开 `cnc-ddraw/x86/ddraw.dll`
- `CNC_DDRAW_CONFIG_FILE=C:\windows\syswow64\ddraw.ini`：PE 的 `GetPrivateProfile` 读不了 Unix 路径

打包布局：

```
wine-data/cnc-ddraw/
  ddraw.ini
  manifest.json
  x86/ddraw.dll
```

默认 ini：`renderer=opengl`、窗口模式、`shader=Bilinear`（不用 xBRZ/FSR multipass）、`fake_mode=800x600x16`。`[gamemd]` / `[ra2md]` / `[ra2]` 打开 `tshack`，避免 TS/RA2 黑 primary 和 GDI「slow software rendering」横幅。

64 位进程 **不会** 加载这份 overlay，走 Wine builtin ddraw → wined3d。红警 2 是 32 位，不受影响。

## 应该怎么用（测试红警 2 / YR）

1. 安装带 `wine-data/cnc-ddraw/x86/ddraw.dll` 的 HAP。**不要** 把 cnc-ddraw 拷进游戏目录，也 **不要** 设 DXVK 去跑 DDraw。
2. 不要设 `WINEHUA_DDRAW_BACKEND`（缺省就是 cnc）。不要设 `WINEHUA_GL_BACKEND=zink`。
3. 从 Explorer 打开 `ra2.exe` / `ra2md.exe` / `gamemd.exe`（尤里）。启动器选窗口、800×600 一类经典模式即可。
4. 游戏目录不需要本地 `ddraw.ini`。要改渲染器，改运行时 `C:\windows\syswow64\ddraw.ini`，或设环境变量（见下）。

### 怎样确认走了 GPU overlay

同时看这几条（缺一就还在软件读回或没加载 overlay）：

| 日志 | 含义 |
|------|------|
| `[cnc-ddraw] DllMain attach` / `selected renderer=opengl` | overlay 已加载，不是 Wine wined3d |
| `ogl_create` 成功，**没有** GDI「slow software rendering」 | 没有掉进 CPU StretchBlt |
| `consumer attached` 且 `vk=0` | GL 表面接到 VirGL 队列（不是 Venus） |
| `GPU_ACTIVE`、`present_ret=0` | 宿主零拷贝就绪 |
| guest `winehua_gl_zero_copy: presents=…` 上涨，`glReadPixels` 不再每帧出现 | GPU present，不是 SHM 读回 |

x86 冒烟：`C:\smoke\x86\winehua_ddraw_smoke.exe`。必须能 `DDGetProcAddress`，才证明加载的是 cnc-ddraw。

### 开关（一般不用动）

```
WINEHUA_DDRAW_BACKEND=wine     # 退回 Wine 内置 ddraw（对照黑屏/调色板）
WINEHUA_GL_BACKEND=zink        # 仅当 HAP 里有 guest_gfx_zink；红警 2 不要开
WINEHUA_CNC_RENDERER=opengl    # 即使开了 zink 也保持 legacy GL
```

HAP 里没有 `guest_gfx_zink` 时设 `zink` 会写 `WINEHUA_GL_BACKEND_NOTE=zink-bundle-missing`，继续 virpipe。

## 构建

```
make cnc-ddraw       # MinGW i686，隔离拷贝 + WineHua 补丁
make assemble        # 打进 wine-data/cnc-ddraw/
make hap             # 含 native 环境注入
```

不要改 `thirdparty/cnc-ddraw`；补丁只在 `scripts/build_cnc_ddraw.sh`。Zink：`make guest-gfx-zink`（opt-in，与红警 2 无关）。
