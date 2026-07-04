# AI 开发上下文 — Wine for HarmonyOS

> 本文档供 AI 助手（GitHub Copilot 等）理解本项目的 Docker 化构建环境和开发流程。

---

## 构建环境

本项目**无法在 Windows 本机构建**。构建环境是 Docker 容器（Ubuntu 26.04），通过 WSL2 操作。

```
Host (Windows)                WSL2                    Docker
───────────────     ─────────────────────    ──────────────────────
f:\WineHua\    ←→   /mnt/f/WineHua/    ←→   /data/src/winehua/   (bind mount rw)
f:\command-    ←→   /mnt/f/command-    ←→   /apps/harmony/       (bind mount ro)
line-tools\          line-tools/
```

## 路径映射

| Host (Windows) | WSL2 | Docker 容器 |
|----------------|------|-------------|
| `f:\WineHua\` | `/mnt/f/WineHua/` | `/data/src/winehua/` |
| `f:\command-line-tools\` | `/mnt/f/command-line-tools/` | `/apps/harmony/` |
| `f:\command-line-tools\sdk\default\openharmony\` | `/mnt/f/command-line-tools/sdk/default/openharmony/` | `/apps/harmony/sdk/default/openharmony/` |

## 触发构建

AI 通过 Host 端脚本触发 Docker 容器内的 make：

```bash
# 完整 HAP 构建
bash scripts/docker_make.sh hap NATIVE_ARCH=arm64-v8a DEVICE_TYPE=pad

# 单阶段构建
bash scripts/docker_make.sh deps          # 交叉编译依赖 → build/sysroot-ext/
bash scripts/docker_make.sh wine          # Wine + wineserver (x86_64 OHOS 交叉编译)
bash scripts/docker_make.sh box64         # Box64 ARM64 翻译器
bash scripts/docker_make.sh native        # Native compositor 依赖 → entry/libs/
bash scripts/docker_make.sh assemble      # 组装 Pad/PC 布局
bash scripts/docker_make.sh hap           # HAP 打包 + 签名
bash scripts/docker_make.sh clean         # 清理所有中间产物
```

`docker_make.sh` 会自动：
1. 检测 `winehua-dev` 容器是否运行
2. 执行 `docker exec winehua-dev make ...`
3. 将容器路径 `/data/src/winehua/...` 转换为 `f:\WineHua\...`（方便 VS Code 点击错误跳转）
4. 构建成功后复制 HAP 到 `dist/`

## 增量构建规则

| 修改了哪些文件 | 只需执行 |
|---------------|----------|
| `entry/src/main/ets/**` (ArkTS) | `bash scripts/docker_make.sh hap` |
| `entry/src/main/cpp/**` (C++ napi) | `bash scripts/docker_make.sh hap` |
| `thirdparty/wine/**` (Wine C 源码) | `bash scripts/docker_make.sh wine` + `hap` |
| `thirdparty/libffi/**`, `wayland/**`, `virglrenderer/**` 等 | `bash scripts/docker_make.sh native` + `hap` |
| `thirdparty/freetype/**`, `mesa/**` 等 sysroot 依赖 | `bash scripts/docker_make.sh deps` → `wine` → `hap` |
| `build-profile.json5`, `module.json5` | `bash scripts/docker_make.sh hap` |

> hvigorw (HarmonyOS 构建工具) **自带增量检测**，改 `entry/src/main/cpp/` 也只需 `make hap`。

## 构建产物

```
entry/build/default/outputs/default/entry-default-signed.hap   ← 标准产物
dist/entry-default-signed-{arch}-{device}.hap                   ← docker_make.sh 额外复制
```

## 容器管理

```bash
# 构建开发镜像
bash scripts/docker_wsl_build.sh dev-build

# 启动持久化容器
bash scripts/docker_wsl_build.sh dev-start

# 进入容器交互
bash scripts/docker_wsl_build.sh dev-exec

# 停止容器
bash scripts/docker_wsl_build.sh dev-stop

# 查看容器状态
bash scripts/docker_wsl_build.sh dev-status
```

## 关键默认值

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `NATIVE_ARCH` | `arm64-v8a` | 鸿蒙设备 CPU 架构 |
| `DEVICE_TYPE` | `pad` | `pad`(fork only) / `pc`(execve) |
| `OHOS_SDK` | `/apps/harmony/sdk/default/openharmony` | 容器内路径 |
| `TOOL_HOME` | `/apps/harmony` | hvigorw 所在目录 |

## 构建阶段依赖链

```
deps (freetype, wayland, xkbcommon... → sysroot-ext)
  └─ wine (Wine x86_64 交叉编译 .so + PE DLL)
       └─ box64 (ARM64 翻译器, 仅 arm64)
            └─ native (libffi, wayland, libepoxy, virglrenderer)
                 └─ assemble (组装布局)
                      └─ hap (HAP 打包 + 签名)
```

Makefile 使用 stamp 文件跟踪增量状态，存放在 `build/.stamps/`。

## AI 协作注意事项

- **不要尝试在 Host 直接编译**，必须通过 `docker exec winehua-dev make ...` 或 `scripts/docker_make.sh`
- **路径转换**：容器内错误路径 `/data/src/winehua/foo.c:42` 对应 Host `f:\WineHua\foo.c:42`
- **Dockerfile** 在 `f:\WineHua\Dockerfile`，镜像名 `winehua-dev`
- **第三方源码** 在 `thirdparty/`，编译产物在 `build/`（Docker volume，Host 不可见）
- **不要 `rm -rf`** 任何带变量展开的路径，优先用 `make clean` 清理
- dev-start 之前确保 `F:\command-line-tools` 路径存在且有完整 OHOS SDK
