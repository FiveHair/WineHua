# WineHua Audio Test

独立 PCM 音频播放器 + 音频接口验证工具，用来检查 WineHua 完整音频链路。

```
MP3 → minimp3(解码) → s16le PCM → Resample(→48000Hz/stereo) → waveOutWrite
WAV → 手动解析 RIFF  → s16le PCM → Resample(→48000Hz/stereo) → waveOutWrite
                                        ↓
                              winmm → mmdevapi → wineohos.drv
                                        ↓
                        AudioBootstrapFd → AudioBroker → OH_AudioRenderer → Speaker
```

## 编译

```bash
# 按工程规范，默认通过 winehua-dev 容器构建
bash Test/AudioTest/build.sh
```

产物: `winehua_audio_test.exe` (~200KB static, 零依赖)

如果容器未启动，先执行：

```bash
bash scripts/docker_wsl_build.sh dev-start
```

## 运行说明

- 顶部按钮会把每一项音频测试放到独立子进程里运行。
- 这样即使 Wine 中某个 `callback` 路径卡住，主界面也不会一起冻结。
- 如果某项测试超时，日志会明确标出该测试疑似卡在 Wine callback/driver 路径。

## 部署到 Pad

```bash
# 传 EXE 到 Wine runtime 目录
hdc file send winehua_audio_test.exe /data/storage/el2/base/files/wine/bin/

# 创建 music 目录并放入测试文件
hdc shell mkdir -p /data/storage/el2/base/files/wine/bin/music
hdc file send test.mp3 /data/storage/el2/base/files/wine/bin/music/
hdc file send test.wav /data/storage/el2/base/files/wine/bin/music/
```

## 验证音频链路

```bash
# 监控 hilog, 关注以下关键字
hdc hilog | grep -E "AudioBroker|ohos_audio|WINE_OHOS|renderer ready|OPEN_STREAM|queued"
```

### 验证清单

| 步骤 | 预期日志 | 含义 |
|---|---|---|
| 1 | `renderer ready rate=48000 ch=2 callbackFrames=...` | OH_AudioRenderer 创建成功 |
| 2 | `WINE_OHOS_AUDIO_BOOTSTRAP_FD=<n>` | bootstrap fd 传递到 Wine |
| 3 | `ohos_audio_client_connect` + `HELLO` | IPC 控制通道握手 |
| 4 | `OPEN_STREAM` | 数据流创建 + ring buffer |
| 5 | `START` | 开始播放 |
| 6 | callback `queued_frames` > 0 | 音频数据在流动 |
| 7 | 无 `underrun` / `overflow` 持续增长 | buffer 稳定 |

## 文件结构

```
Test/AudioTest/
  main.c                    ← 播放器源码 (Win32 GUI + PCM)
  minimp3.h                 ← MP3 解码器 (build.sh 自动下载)
  build.sh                  ← 交叉编译脚本
  README.md                 ← 本文件
  winehua_audio_test.exe    ← 编译产物
```
