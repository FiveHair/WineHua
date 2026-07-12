# ARM64 Pad VirGL 性能优化执行计划

> 基线：`dev@8fa2f2d`  
> 工作分支：`perf/virgl-frame-pipeline`  
> 设备：ARM64 Pad `5KPBB25818203996`

## 1. 当前稳定约束

性能工作建立在已经验证的 GL 功能路径上，不得破坏以下条件：

- guest WGL 继续提供 desktop compatibility OpenGL，不能强制 GLES。
- host VirGL 继续使用 HarmonyOS surfaceless EGL/GLES。
- Pad 同步模式保持 `WINEHUA_VIRGL_SYNC_MODE=egl-main`、GLsync 和
  `VTEST_SYNC_GL_FINISH=1`。
- 保留 GLES host 上 Z32 深度格式到 Z24/Z24S8 的兼容映射。
- 不恢复 threaded EGL fence wait、native-fd 默认路径、frame callback 等待或
  `wl_buffer.release` 阻塞等待。
- 即使显示缓冲区已满，也必须完成 guest `glFlush` 和 readback；只能丢弃显示副本。
- 不通过扩大 socket buffer、跳过提交或伪造 FPS 掩盖瓶颈。

当前显示链路：

```text
Windows OpenGL/WGL
  -> Wine + guest Mesa virpipe
  -> VirGL host GLES
  -> synchronous glReadPixels
  -> Wayland SHM
  -> embedded compositor CPU copy/composite
  -> glTexSubImage2D
  -> XComponent eglSwapBuffers
```

## 2. 自动化基线与日志

所有性能结论来自真实 ARM64 Pad。每项修改使用相同的 60 秒测试窗口，保存构建提交、
HAP 哈希、截图、Wine stderr、OpenGL diagnostics、VirGL host 日志和 hilog 摘要。

包目录日志：

```text
/data/app/el2/100/base/app.hackeris.winehua/temp/wine_stderr_*.log
/data/app/el2/100/base/app.hackeris.winehua/cache/winehua_opengl_diag.log
/data/app/el2/100/base/app.hackeris.winehua/cache/winehua_virgl_host.log
```

测试程序：

```text
Smoke:  winehua_graphics_smoke.exe --loop
SDLPal: cmd.exe /d /c "cd /d C:\palmod2.0 && start sdlpal.exe"
Mahjong: cmd.exe /d /c "cd /d C:\Publish && call playX86.bat"
```

Mahjong 必须通过 `playX86.bat` 启动；它设置 `lib.win32\lib`，同时要求当前目录为
`C:\Publish`，否则资源或 DLL 查找会失败。

每轮至少检查：首帧、持续画面变化、进程存活、crash/stall、SHM in-flight、内存趋势、
producer/committed/displayed 帧数，以及帧时间 p50/p95/p99。Smoke 是主要性能样本；
SDLPal 和 Mahjong 用于确认复杂程序仍可启动和绘制。

## 3. 实施顺序与提交边界

### 阶段 A：统计，不改变行为

- 增加低开销流水线计数和阶段耗时。
- guest 统计 `glFlush`、`glReadPixels`、CPU conversion/copy、SHM commit/release/drop。
- host 统计 Wayland copy/composite、TakeFrame、texture upload、draw 和 swap。
- 每 120 帧输出一次摘要；禁止逐帧性能日志。
- 区分 producer、committed 和 displayed FPS。

提交：`perf: add virgl display pipeline metrics`

### 阶段 B：readback 与 SHM

- 复用 readback staging 内存，取消逐帧 `malloc/free`。
- 使用固定三槽 SHM pool，release 后复用；resize 时建立新 generation，旧的在途槽释放后销毁。
- 支持时直接读取 BGRA；不支持时保留 RGBA，并使用行级批量转换。
- 队列满时仍完成 flush/readback，只丢显示副本。

提交按可独立验证的变更拆分，不把内存复用、SHM 生命周期和格式探测混成无法 A/B 的提交。

### 阶段 C：显示 pacing

- 删除有新帧路径中固定的 `usleep(16667)` 叠加等待。
- 使用 monotonic deadline 和实际 swap 行为调度。
- renderer 落后时采用 latest-frame-wins，旧显示副本计入 dropped-display。
- 静态桌面仍可休眠，但新帧到达应立即唤醒。

### 阶段 D：条件实验

- 只有 `glReadPixels` 仍主导 p95 时，增加可运行时回退的 2/3 槽 PBO readback。
- 只有 compositor copy/upload 主导时，才增加 damage-aware upload 或 upload PBO。
- NativeBuffer/EGLImage 和 VirGL scanout import 属于后续 R&D，不纳入本轮默认路径。

## 4. 每项修改的门禁

每项修改必须依次完成：

1. 构建 ARM64 Pad HAP，并检查 guest graphics、VirGL helper 和 Wine 内容。
2. 覆盖安装，保留现有 Wine prefix 和 C 盘测试程序。
3. Smoke 运行 60 秒，保存 15/30/60 秒截图和三类包目录日志。
4. SDLPal、Mahjong 各运行约 60 秒，确认启动、出图且无 crash/stall。
5. 确认桌面、任务栏及基本音频没有明显回退。
6. 达到功能门槛并有可重复收益后才提交；否则继续修正或放弃该实验。

性能判断：

- 单项优化的 displayed FPS 或 frame-time p95 改善达到 5% 可接受，前提是重复测试稳定且对应热点阶段同向改善。
- 改善低于 3% 视为噪声并放弃；3% 至 5% 需要增加 A/B 复测，仍不稳定则暂缓。
- 最终累计目标是 960x540 frame-time p95 至少改善 20%。
- 只有 producer FPS 提升而 displayed FPS 不变，不算有效。
- SHM in-flight 必须不超过 3，预热后内存不得线性增长。
- 不允许白屏、旧帧、残影、伪高 FPS、两秒以上关键进度停止或音频回归。

## 5. 分支和交付

- 所有工作保留在 `perf/virgl-frame-pipeline`；需要修改的子模块使用同名分支。
- 子模块真实源码先提交，主仓库随后提交 gitlink 和配套代码。
- 日志、截图、HAP、构建目录和临时分析文件不进入正式提交。
- 无收益的实验不提交；通过的优化逐项提交并记录 A/B 数据。
- 无人值守阶段结束后切回 `dev`，不自动 merge，也不自动 push；以结果报告决定后续整合。
