# Wine 三大定制分支（Proton / CrossOver / Winlator）分析

> 目的：分析 proton、crossover、winlator 在 Wine 基础上各自定制了什么，
> 提炼对 wineohos 有参考价值的机制。源码参考目录：`.temp/Proton`、`.temp/crossover`、`.temp/winlator`。
> 整理日期：2026-08-20

## 一、三者共通的定制骨架

三者的平台不同（Proton=Linux/Steam、CrossOver=macOS/Linux、Winlator=Android），
但定制的方向几乎一致，核心是五件事：

| 定制方向 | 做法 | 为什么 |
|---|---|---|
| 图形栈替换 | DXVK（DX9/10/11→Vulkan）+ VKD3D（DX12→Vulkan）替换 wined3d | wined3d 是保底方案，性能差一截 |
| 音频 | FAudio 重实现 XAudio2 | 游戏音效/语音依赖 XAudio2 |
| 同步优化 | esync/fsync 补丁 | 用内核 fd/event 替换 Wine 进程内锁，多线程游戏锁开销大降 |
| per-app 兼容配置 | 按具体游戏打补丁（三者的核心差异） | 每个游戏的坑不同，无法统一修 |
| prefix 管理 | 容器/bottle 体系 + 版本迁移 | Wine 升级后旧 prefix 不兼容 |

## 二、Proton（Valve，Steam 上的 Wine 兼容层）

### 2.1 Steam 深度集成（Proton 独有）

- **lsteamclient**：把 Windows 版 steamclient.dll 桥接到 Linux 版 ——
  游戏内 Steam 成就、联机、Overlay 都靠它
- **steam_helper.c**（`.temp/Proton/steam_helper/`）：Steam 手柄桥接
- **vrclient_x64 + OpenXR-SDK + wineopenxr**：VR 支持
- **媒体管线全套自备**：ffmpeg + gstreamer + dav1d（过场视频解码），
  kaldi / piper / vosk-api（Steam 语音聊天转录/合成）
- **前缀版本迁移**：`CURRENT_PREFIX_VERSION = "11.0-100"`（`.temp/Proton/proton` 主脚本头部），
  每次 Proton 大版本更新自动迁移 prefix

### 2.2 per-app 兼容配置（重点，机制分两层）

**第一层：内建修复表** —— `default_compat_config()`（`.temp/Proton/proton:1232`），
按 SteamAppId（环境变量 `SteamAppId`）返回修复项集合，硬编码了上百个游戏的修复：

```python
# .temp/Proton/proton（节选）
if appid in ["1017900", #Age of Empires: DE
             "1331440", #FUSER
             ...]:
    ret.add("nomfdxgiman")     # 禁 MF DXGI MAN

if appid in ["1172620", #Sea of Thieves
             "962130",  #Grounded
             "1551360", #Forza Horizon 5
             ...]:
    ret.add("noopwr")          # 禁 Overlay Present Wait（修文字输入延迟）

if appid in ["257420",  #Serious Sam 4
             "2021880", #Ara: History Untold
             ...]:
    ret.add("hidevggpu")       # 隐藏 AMD VG GPU

if appid in ["1977170"]:       # Jusant
    ret.add("hideintelgpu")    # 隐藏 Intel GPU
```

修复项全集（proton 主脚本 1810 行起逐一映射）：
`noopwr`、`nomfdxgiman`、`noforcelgadd`、`hidevggpu`、`hideintelgpu`、`hideapu`、
`hidenvgpu`、`usenativexinput13`、`disablelibglesv2`、`heapdelayfree`、
`heapzeromemory`、`heaptopdown`、`vkd3dbindlesstb`、`vkd3dfl12`、`wined3d11`、
`dxvkd3d8`、`disablenvapi`、`forcenvapi`、`forcelgadd`、`oldglstr`、`nofsync` …

**第二层：集合 → 具体动作的映射**（`.temp/Proton/proton:1810` 起）：

```python
if "forcelgadd" in self.compat_config:
    self.env["WINE_FORCE_LARGE_GPU_ADDRESS"] = "1"
if "hidevggpu" in self.compat_config:
    self.env["WINE_HIDE_AMD_GPU"] = "1"
if "noopwr" in self.compat_config:
    self.env["WINE_DISABLE_OWPR"] = "1"
if "nomfdxgiman" in self.compat_config:
    self.env["WINE_DISABLE_MF_DXGI_MAN"] = "1"
```

**按 appid 的环境变量表**（`.temp/Proton/proton:1482` 起）：

```python
appid = os.environ.get("SteamGameId", 0)
if "PROTON_CPU_TOPOLOGY" in self.env:
    self.env["WINE_CPU_TOPOLOGY"] = self.env["PROTON_CPU_TOPOLOGY"]
elif appid in default_cpu_limit:
    self.env["WINE_CPU_TOPOLOGY"] = str(default_cpu_limit[appid])  # 按游戏限核

if "WINE_HIDE_AMD_GPU" not in self.env and appid in ["1282690"]:
    self.env["WINE_HIDE_AMD_GPU"] = "1"
```

**两层覆盖机制**：
- 用户启动参数 `STEAM_COMPAT_FLAGS` 可增删修复项（`.temp/Proton/proton:1564` 解析）
- `user_settings.sample.py` → 用户改名成 `user_settings.py` 启用全局设置
  （PROTON_LOG、WINEDEBUG、DXVK_LOG_LEVEL 等调试开关）

**其他细节**：
- `HOST_LC_ALL` 处理：Steam 会给游戏设 `LC_ALL=C`，这会破坏 Wine 的 win32↔host
  路径转换，Proton 从 `HOST_LC_ALL` 取真实值回填（proton 主脚本 init_wine()）
- WOW64 支持：`PROTON_USE_WOW64=1` 时设 `WINEARCH=wow64`

## 三、CrossOver（CodeWeavers 商业版）

本地 `.temp/crossover/` 主要是构建依赖集合（dxvk、moltenvk、gnutls、gstreamer、
ghostscript、freetype、pyxdg…）+ 定制 wine 树；完整源码在
`.temp/crossover-sources-26.3.0.tar.gz`（sources/ 下有 moltenvk 等）。

- **CrossTie 脚本（cxinstaller）**：一键安装复杂应用的自动化脚本系统 ——
  Office、游戏点一下装完（装组件、改注册表、配 env 全套），比 winetricks 更系统化。
  这是 CrossOver 最大的差异化。
- **Bottle 管理**：容器体系，每个 bottle 独立配置
- **定制 Wine 补丁**：CodeWeavers 是 Wine 最大补丁来源之一，定制大量最终上游化进主线
- **macOS 特有**：MoltenVK（Vulkan→Metal）、D3D→Metal 路径、
  wine32on64（64 位进程里跑 32 位应用）
- **兼容性数据库**：CodeWeavers App Database，社区报告每应用兼容性等级

## 四、Winlator（Android：box64 + Wine + 图形模拟）

源码 `.temp/winlator/`，关键定制：

- **box64 集成**：`app/app/src/main/java/com/winlator/box64/Box64PresetManager.java` ——
  4 档位（STABILITY/CONSERVATIVE/INTERMEDIATE/PERFORMANCE），每档完整 11 个
  `BOX64_DYNAREC_*` 参数。→ **本项目已计划对齐（见下 5.1）**
- **容器系统**：`ContainersFragment` / `ContainerDetailFragment` / `container/` ——
  每容器独立 prefix + 配置（图形驱动/分辨率/env/启动参数），每游戏一个快捷方式
- **图形栈自备**：`mesa3d-custom/`（turnip-25.2.0 = Adreno 开源 Vulkan 驱动、
  zink-22.2.5 = OpenGL→Vulkan）+ virgl + DXVK，按设备能力选择
- **自研 X11 服务器**：`xserver/`、`xconnector/`、`xenvironment/` —— Android 无 X，
  自建 X 供 Wine 输出（对应本项目的 Wayland compositor）
- **输入**：`inputcontrols/` 的 .icp 布局文件，50+ 个按游戏分配 + 布局编辑器
  （`ControlsEditorActivity`）
- **杂项**：`glibc_patches`（Android bionic 与 glibc 差异）、`sysvshm`
  （Android 无 SysV 共享内存的模拟）、`android_alsa`（音频）、
  `installable_components`（vcrun/mono/dxvk 等可安装组件）、`wine_addons`

## 五、对 wineohos 的借鉴建议（按优先级）

### 5.1 ✅ 已计划：box64 dynarec 4 档位对齐 Winlator

自定义启动的「兼容模式」从 3 档扩到 5 按钮（跟随全局 + 稳定/保守/均衡/性能），
完整 11 参数注入。详见此前计划（Box64PresetManager 参数表已摘录）。

### 5.2 🏆 最值得做：per-app 兼容配置数据库（gamefixes）

现状：自定义启动只有手动参数、不记忆；红警2/PAL2 等逐个修的坑散落在记忆里。

参考 Proton `default_compat_config` 的「修复表 + 映射层」设计：

```
gamefixes 表（按 exe 名/路径匹配）:
  { "redalert2.exe": { env: {...}, cpu_topology: 2, hide_gpu: true }, ... }

映射层: 修复项 → 具体动作
  hide_gpu    → BOX64_DYNAREC 或 wine env 注入
  cpu_topology→ WINE_CPU_TOPOLOGY
  dll_override→ 注入 DLL override
  env         → 环境变量注入
```

内建一张修复表 + UI 上「为此游戏保存配置」持久化覆盖（对齐 Proton
`STEAM_COMPAT_FLAGS` 的 add/discard 语义）。

### 5.3 按游戏限核（WINE_CPU_TOPOLOGY）

老游戏（红警2 这类）多核行为异常，Proton `default_cpu_limit` 一行 env 的事，
低成本高收益，直接并入 5.2 的表。

### 5.4 隐藏 GPU 类修复

arm64 上 Mali/Adreno 的名字被游戏检测到可能拒绝运行或走错渲染路径；
`hidevggpu`/`WINE_HIDE_AMD_GPU` 思路可直接搬。

### 5.5 前缀版本迁移机制（Proton CURRENT_PREFIX_VERSION）

Wine 版本升级后 prefix 兼容问题反复出现（wineboot 崩溃、locale 两步修复都涉及），
加 prefix 版本标记 + 升级迁移脚本，避免用户旧数据被新 Wine 弄坏。

### 5.6 esync/fsync 补丁

box64 下同步是性能瓶颈之一，Wine 侧补丁 + 环境开关。

### 5.7 DXVK-NVAPI + dxvk-gplasync

DXVK 后端可选的增强：NV 专属 API 模拟（DLSS/RTX）+ 异步着色器编译防卡顿。

### 5.8 手柄布局多样化

Winlator 50+ 布局 vs 本项目 1 种，已有讨论，属后续 UI 工作量。

### 5.9 CrossTie 一键安装（远期）

适合「应用商店」式体验，工程量大，优先级最低。

## 附：源码参考位置速查

| 机制 | 位置 |
|---|---|
| Proton 内建修复表 | `.temp/Proton/proton:1232`（default_compat_config） |
| Proton 修复项→env 映射 | `.temp/Proton/proton:1810` 起 |
| Proton 按 appid env/CPU 拓扑 | `.temp/Proton/proton:1482` 起 |
| Proton 用户覆盖 | `.temp/Proton/proton:1564`（STEAM_COMPAT_FLAGS）、`user_settings.sample.py` |
| Proton prefix 版本 | `.temp/Proton/proton` 头部 CURRENT_PREFIX_VERSION |
| Proton Steam 集成 | `.temp/Proton/lsteamclient/`、`steam_helper/` |
| Winlator box64 档位 | `.temp/winlator/app/app/src/main/java/com/winlator/box64/Box64PresetManager.java` |
| Winlator 容器 | `.temp/winlator/app/app/src/main/java/com/winlator/`（ContainersFragment 等） |
| Winlator 手柄布局 | `.temp/winlator/input_controls/` |
| Winlator 定制 wine | `.temp/winlator/wine-10.10-custom/` |
| CrossOver 依赖+定制 wine | `.temp/crossover/`；完整源码 `.temp/crossover-sources-26.3.0.tar.gz` |
