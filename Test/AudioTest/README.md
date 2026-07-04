# WineHua Audio Test

独立 PCM 音频播放器，验证 WineHua 完整音频链路。

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
# 需要 mingw-w64 交叉编译工具链
chmod +x build.sh
./build.sh
```

产物: `winehua_audio_test.exe` (~200KB static, 零依赖)

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
