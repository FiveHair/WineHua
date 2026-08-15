# WineHua VKD3D 上屏路径调研交接（2026-08-15）

> 本文档用于在其他设备/新环境继续 ROTTR DX12 上屏卡顿的调研。
> 继承前序交接：`VKD3D_ENVIRONMENT_HANDOFF_20260813.md`（对象/内存探针方向）、
> `ROTTR_STUTTER_DIAGNOSIS_NOTE.md`（本轮三层卡顿定位）、
> `CACHE_REUSE_VERIFICATION_NOTE.md`（shader 缓存验证闭环）。

## 1. 项目与目标

WineHua（HarmonyOS 上的 Wine/Box64 容器）跑 ROTTR（Rise of the Tomb Raider）DX12
（VKD3D-Proton 2.6 → Vulkan → Venus → virglrenderer → Maleoon 920）。

当前目标：**定位并解决上屏（present）路径的稳态卡顿**（guest 渲染 ~67fps，但
compositor 显示 ~3fps，静态菜单更低至 ~0.06fps）。

## 2. 环境与设备

- WSL distro: Ubuntu；源码: `/home/maple/Work/WineHua-vkd3d-probes`
- Docker container: `winehua-vkd3d-probes-ext4`（mount `/home/maple/Work/WineHua-vkd3d-probes` → `/data/src/winehua`，`/apps/harmony` ro）
- 设备: `62T0225B10005882`（Maleoon 920, Virtio-GPU Venus）
- 游戏: `z:\games\Raider\ROTTR-500K.exe`（需最低配置 + DX12/VKD3D 500K 才能启动）
- 注意：设备 developer mode 下屏幕锁无法自动解锁，`aa start` 前需手动解锁

## 3. 已完成并已提交

### 3.1 shader 缓存（主目标，已闭环）

- Venus 空缓存 bug 修复：`vn_GetPipelineCacheData` 的 `*pDataSize <= sizeof(header)`
  改为 `<`（Mesa submodule，commit `8386e66`）
- 小样例 `vulkan-cache-reuse-x64` PASS：`coldMs=3.555 → warmMs=0.185`（19.2x），
  `cacheBytes=3240`，证明 VkPipelineCache 序列化/复用链路可用
- 主仓 commit `62ed3a7`：cache-reuse 样例 + Makefile + SmokeRunner + Index.ets
- 两个仓库均已 push（本地 + GitHub）：
  - `winehua/WineHua.git` → `feature/vkd3d-object-memory-probes` @ `62ed3a7`
  - `winehua/mesa-ohos.git` → `feature/vkd3d-object-memory-probes` @ `8386e66`
- memo 已提交：`docs/VKD3D_SHADER_CACHE_STUTTER_20260814.md`

### 3.2 EOS 联网卡顿（已修，靠 stub DLL）

- 根因：`Index.ets` 第 1051 行把 `WINEDLLOVERRIDES` 覆盖成 `d3d12=n`，冲掉 C++ 侧
  `AppendD3dBackendEnv` 的 `EOSSDK-Win64-Shipping=`
- 禁用 DLL 会因静态导入崩溃，正确做法是替换 stub DLL：
  `D:\MyProject\EOSSDK-Win64-Shipping.dll`（@0x17EE0: `33 C0 C3`），原文件备份 `.orig`
- 已在设备上由用户手动替换，验证通过（`[Epic] Failed to create platform!`，转离线）
- 当前 `Index.ets` WINEDLLOVERRIDES 值（已回退禁用）：
  `d3d12=n;d3d11=n;dxgi=n`（vkd3d）/ `d3d12=n,b`（dxvk）

## 4. ROTTR 卡顿三层根因

| 层 | 现象 | 状态 |
|---|---|---|
| 1. EOS 联网 | 启动/菜单期 `Lockout 58s / Large tick 8-45s` | ✅ 已修（stub DLL） |
| 2. shader 缓存 miss | 启动期重编译 shader，~14min 才进菜单 | 🔍 已定位，缓存未持久化 |
| 3. 上屏/present 路径 | 稳态 guest 67fps → 显示 ~3fps | 🔍 已定位，未修 |

## 5. 上屏路径（当前调研重点）

### 5.1 数据证据

guest 侧 `WineHuaGuestFramePerf`：
```
presents=120 present_us=15ms(avg) drain_us=13.5ms renderer_us=1.49ms
```

compositor 消费 `[VIRGL-ZC][MAIN]`：
```
frame=1   (22:49:37)  signals=1
frame=120 (22:50:17)  signals=120   → 120 帧/40s ≈ 3fps
```

`[GL-PERF]`（compositor 帧处理很快）：
```
第一窗: upload_bytes=260MB（CPU fallback/回读）
第二窗: upload_bytes=0（zero-copy 生效）
take_us max=34~76ms  swap_us max=4~6ms  total_us max=40~100ms
```

swapchain：
```
present_mode=fifo requested_mode=fifo release_mode=wait images=3 blit_supported=1
```

compositor 显示 fps（`[render]` 与 `winehua_display_fps.txt`）：
```
菜单动画期 2~4fps → 静态菜单 0.06fps（约 17s/帧）
```

### 5.2 结论

卡点在 **present 链路的 FIFO 同步 present + SurfaceQueue(深度 3) + compositor 慢消费
三者耦合**：

```
guest 67fps → venus_broker_present（wait fence → acquire → blit → vkQueuePresentKHR → release wait）
→ SurfaceQueue(3) → EGL compositor（OH_NativeImage 消费 + eglSwapBuffers）→ 显示
```

producer 拿不到 SurfaceQueue buffer 就 early-return（`framesPresented_` 不涨），
帧在 present 层被丢。`mailbox`（非阻塞、只取最新帧）是理论解耦方案，**但未验证**。

### 5.3 mailbox 测试失败原因（重要）

尝试 `aa start --ps winehua.perf_profile shadow-precise-strong-ring-mailbox`：

- hilog 确认参数被读到：`perf=shadow-precise-strong-ring-mailbox`
- 但实际 `WINEHUA_VENUS_PRESENT_MODE=fifo`（还是 fifo）
- 根因：`Index.ets` 第 491 行 `finishManualVkd3dSession()` 把 `perfProfileText`
  重置为 `'shadow-precise'`，覆盖了 mailbox profile

所以 mailbox 测试实际没跑。要验证需：

1. **临时强制**：`napi_init.cpp` 里 present mode 固定成 `"mailbox"`（或加
   `WINEHUA_VENUS_PRESENT_MODE` 环境变量兜底优先），重编 HAP 再跑；
2. **修重置逻辑**：查 `finishManualVkd3dSession()` 为何在启动路径被调用，把
   perf profile 覆盖时机修对。

## 6. 未完成事项与下一步

1. **验证 mailbox present 模式是否解耦**（改 `napi_init.cpp` 强制 mailbox → 重编 → 对比 FPS）
2. **抓 `[VENUS-PRESENT][NCP] frames=` summary** 精确定位 present 各阶段耗时
   （`wait_fence / acquire / submit / queue_present / release_wait`）：
   - 每 120 帧才打一次，hilog 会滚掉，要在游戏持续 present 时清 hilog 抓取
   - 若 `acquire_avg` 爆表 → SurfaceQueue 满，mailbox 应有效
   - 若 `queue_present_avg` 爆表 → FIFO present 阻塞
3. **compositor 消费慢的根因**：`[MW-RNDR] NativeVSync unavailable`（failures=1）+
   `[render]` 从 4.45fps 掉到 0.06fps，疑似 NativeVSync 失活/阻塞（自适应刷新率
   或 `OH_NativeVSync_RequestFrame` 阻塞）。需要抓 `[MW-RNDR]` VSync 周期/failures 时序
4. **shader 缓存持久化**：游戏退出前被 force-stop，VKD3D 编译缓存没写回，每次启动
   重编译 ~14min。需让游戏正常退出（或缓存写回路径）后再启动验证二次复用
5. 若 mailbox 有效，考虑产品默认切 mailbox 或修 profile 重置逻辑

## 7. 构建/部署/启动命令

### 构建（必须 WSL + Docker）

```bash
cd /data/src/winehua && export \
  WINEHUA_GUEST_MESA_VERIFIED_COMMIT=0a239288f90ba0afe074aa90832d8fd71bd0ffe8 \
  WINEHUA_GUEST_MESA_VERIFIED_PATCH_SHA256=ff2ca022284a9663df61392739d22e6a947afd8e9a41a0349737a86c333e35f2 \
  WINEHUA_VKD3D_VERIFIED_COMMIT=3e5aab6fb3e18f81a71b339be4cb5cdf55140980 \
  WINEHUA_VKD3D_VERIFIED_CLEAN=1 \
  && make hap NATIVE_ARCH=arm64-v8a
```

（若只改 ArkTS/entry C++ 用 `make hap`；若改 Mesa 需先 clean guest 构建。）

### 部署（Windows HDC）

```powershell
$hdc='C:\Program Files\Huawei\DevEco Studio\sdk\default\openharmony\toolchains\hdc.exe'
& $hdc -t 62T0225B10005882 shell "aa force-stop app.hackeris.winehua"
& $hdc -t 62T0225B10005882 install -r 'D:\MyProject\artifacts\<new>.hap'
& $hdc -t 62T0225B10005882 shell "power-shell setmode 602"
& $hdc -t 62T0225B10005882 shell "aa start -a EntryAbility -b app.hackeris.winehua --ps winehua.mode game --ps winehua.game_path 'z:\games\Raider\ROTTR-500K.exe' --ps winehua.d3d_backend vkd3d_limited_500k"
```

### 抓日志

```powershell
$hdc -t 62T0225B10005882 shell "hilog -x 2>/dev/null | grep -aE 'VENUS-PRESENT|VIRGL-ZC|MW-RNDR|GL-PERF'"
```

路径：
```text
游戏日志: files/.wine/drive_c/users/100/Documents/Rise of the Tomb Raider/Rise of the Tomb Raider.log
帧率:     files/.wine/drive_c/windows/temp/winehua_display_fps.txt
wine:     temp/wine_stderr_20260814.log
host:     cache/winehua_virgl_host.log
```

## 8. 关键源码位置

- `entry/src/main/cpp/venus_surface_presenter.cpp`
  - `RequestedPresentMode()`（~line 51）：读 `WINEHUA_VENUS_PRESENT_MODE`，默认 fifo
  - `Present()`（~line 297）：present 主流程，`throttled_` / `framesPresented_`
  - perf summary 每 120 帧打一次（`[VENUS-PRESENT][NCP] frames=...`）
- `entry/src/main/cpp/virgl_surface_presenter.cpp`
  - `SurfaceQueueTarget::Present()`（~line 139）：D3D11/VirGL present 也做 GL blit
- `entry/src/main/cpp/egl_renderer.cpp`
  - `waitForFrameTick()`（~line 680）：NativeVSync + 100ms 超时 + deadline fallback
  - `UpdateZeroCopyFrame()`（~line 283）：SurfaceQueue 帧消费
  - `[GL-PERF]` / `[VIRGL-ZC][MAIN] frame=` 每 120 帧打一次
- `entry/src/main/cpp/napi_init.cpp`（~line 219-287）：present mode 由 perf profile 决定
  （`mailbox`/`fifo-async`/`fifo-poll`/`fifo`）
- `entry/src/main/ets/pages/Index.ets`
  - line 491：`finishManualVkd3dSession()` 重置 `perfProfileText='shadow-precise'`（mailbox 测试失败根因）
  - line 1051：游戏启动 `WINEDLLOVERRIDES`（EOS 相关）

## 9. 交接要点（给新设备上的新 agent）

1. 新环境先拉两个仓库的 `feature/vkd3d-object-memory-probes` 分支（都已在 GitHub）
2. 设备需：安装 HAP、解锁屏幕、游戏目录 `z:\games\Raider\ROTTR-500K.exe`、
   EOS stub DLL 替换
3. 先复现稳态卡顿（进菜单后抓 `[VIRGL-ZC][MAIN] frame=` 和 `[GL-PERF]`），
   确认 present 是否仍被限流到 ~3fps
4. 关键实验：临时强制 mailbox present mode → 对比 FPS → 判断是否解耦
5. 若 mailbox 有效，再决定修 profile 重置逻辑还是默认切 mailbox
6. 抓 `[VENUS-PRESENT][NCP] frames=` summary 前先清 hilog，且要在游戏持续
   present（菜单动画/游戏画面）时抓，不要在静态菜单抓（present 太少，summary 出不来）
