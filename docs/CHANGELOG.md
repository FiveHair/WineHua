# Changelog

## 2026-07-05

### Audio

- 完成 HarmonyOS Pad 音频全链路，`WASAPI`、`DirectSound`、`waveOut`、`MCI`、`MIDI` 全部通过实机验证。
- 宿主侧 Audio Broker 补齐渲染 / 采集生命周期、流状态同步与共享内存 ring buffer 协议细节。
- Pad 运行时新增 `wine-data.zip` stamp 校验，覆盖安装后可自动刷新解压出来的 Wine 运行时。
- 应用随包携带 `winehua-gm.sf2`，首次启动自动部署到沙箱供 MIDI soft synth 使用。

### Tests

- 补齐 `Test/AudioTest` 全套验证程序，覆盖 `WASAPI`、`DirectSound`、`waveOut`、`MCI`、`MIDI`。
- 恢复 `Test/GraphicsTest` 与 `winehua_graphics_diag` 相关工具，保留完整回归测试入口。

### Third-Party

- `thirdparty/wine`:
  - `wineohos.drv` 新增 OHOS MIDI soft synth 与 soundfont 加载路径。
  - 修复 MIDI 驱动初始化成功码，解决 `midiOutGetNumDevs() == 0`。
  - 补强 `ohos.c` 音频通知 / 同步路径，稳定 `WASAPI event` 与 `DirectSound`。
  - 扩展 `mciqtz32`，补齐 Pad 侧 MP3 / WAV 的 MCI 播放支持。
  - 更新 `winehua_graphics_smoke` 运行时环境头文件，保持图形验证工具可用。
