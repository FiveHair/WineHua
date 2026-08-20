# OpenHarmony 迁移兼容方案

> 目标：把当前 HarmonyOS 项目迁移为**针对 OpenHarmony** 的项目。
> 结论先行：**编译层几乎全兼容**，需要改的是工程配置（签名/SDK 版本/编译器声明）+ 2 处代码硬编码；真正的未知在运行时（OpenHarmony 设备行为），需真机实测。
> 本文所有验证均基于本机 **OpenHarmony SDK `6.1.1.125 / API 24`**（`/apps/harmony/sdk/default/openharmony/`）与 **OpenHarmony 源码树**（`/src/ohos/`）逐项对照，非推测。

## 0. TL;DR

| 层 | 结论 |
|---|---|
| ArkTS API（kit/import symbol） | ✅ 全部在 OpenHarmony SDK |
| ArkUI 组件（SegmentButton/ChipGroup/XComponent/装饰器） | ✅ 全部在 |
| C++ native（库/头文件/XComponent 桥接） | ✅ 全部在 |
| 权限声明（含 ALLOW_WRITABLE_CODE_MEMORY） | ✅ 全部在 |
| 工程配置（签名/SDK 版本/编译器） | 🔧 需改（配置，非代码） |
| 代码硬编码（华为文件管理器 bundleName） | 🔧 需改（2 处） |
| 运行时行为（多窗口/图形栈/子进程/IME） | ⚠️ 需 OpenHarmony 真机实测 |

关键背景：**CI 构建环境（`ci/Dockerfile.buildenv`）实际只挂载 OpenHarmony SDK，native 层早就在用 OpenHarmony 的 llvm 编译**。底层工具链已就绪，缺的只是把工程声明对齐。

---

## 1. 验证方法

| 资源 | 路径 | 用途 |
|---|---|---|
| OpenHarmony SDK（open 层） | `/apps/harmony/sdk/default/openharmony/` | kit 导出清单、component 声明、native sysroot |
| HarmonyOS SDK（hms 层） | `/apps/harmony/sdk/default/hms/` | 与 open 层对比，找出华为私有 kit |
| OpenHarmony 源码树 | `/src/ohos/` | 权限定义、API 声明（`interface/sdk-js`）、官方工具链 |

> **SDK 结构认知**：HarmonyOS SDK = `openharmony/`（47 个开源 kit）+ `hms/`（51 个华为私有 kit）。这正是"HarmonyOS 是 OpenHarmony 额外增加 API"的直接体现。

---

## 2. 逐层验证结果

### 2.1 Kit 命名空间层（✅）

项目 import 的全部 8 个 kit、24 个 symbol，**逐个在 OpenHarmony kit 导出清单中命中**（`build-tools/ets-loader/kit_configs/*.json`）：

```
✅ @kit.AbilityKit      bundleManager, common, AbilityConstant, ConfigurationConstant, UIAbility, Want
✅ @kit.ArkData         preferences
✅ @kit.ArkUI           window, display, promptAction, ChipGroup, ChipGroupItemOptions, SegmentButton, SegmentButtonOptions
✅ @kit.BasicServicesKit deviceInfo, zlib, BusinessError
✅ @kit.CoreFileKit     fileIo, fileUri, picker, statfs
✅ @kit.IMEKit          inputMethod
✅ @kit.ImageKit        image
✅ @kit.PerformanceAnalysisKit  hilog
```

无任何 `@hms.*` / `@hw.*` / `@ohos.*`（旧式）/ distributed import。唯一非系统 import 是 `libentry.so`（应用自身 NAPI）。

### 2.2 ArkUI 组件层（✅）

- `SegmentButton` / `ChipGroup`：在 `@ohos.arkui.advanced.*`（OpenHarmony SDK 中有 `SegmentButton.d.ets` / `ChipGroup.d.ets`）。
- `XComponent`：4 处（`HostVulkanSmoke.ets:46` / `WineWindow.ets:100` / `WinePopup.ets:90` / `DesktopLayer.ets:345`）全为 `type: XComponentType.SURFACE` + 自定义 `XComponentController` 覆写 `onSurface*` 回调；**未用 `onLoad`/`onDestroy`、未设 `libraryname`**。此模式 OpenHarmony 标准兼容。
- 装饰器：@Entry/@Component/@Builder/@CustomDialog/@State/@Prop/@Link/@Watch + V1 状态管理（AppStorage/PersistentStorage/@StorageLink/@StorageProp）——全为标准 V1，OpenHarmony 支持。

### 2.3 C++ native 层（✅）

**链接库**（`entry/src/main/cpp/CMakeLists.txt` 全部 12 个在 arm64 sysroot 命中）：

```
libace_napi.z.so  libace_ndk.z.so  libnative_window.so  libnative_image.so
libnative_vsync.so  libEGL.so  libGLESv3.so  libvulkan.so  libhilog_ndk.z.so
libchild_process.so  libipc_capi.so  libohaudio.so
```

**头文件**（16/16 命中）：`native_interface_xcomponent.h`、`native_api.h`、`external_window.h`、`native_buffer.h`、`native_image.h`、`native_vsync.h`、`EGL/egl.h`、`EGL/eglext.h`、`GLES3/gl3.h`、`GLES2/gl2ext.h`、`vulkan/vulkan.h`、`hilog/log.h`、`native_child_process.h`、`ipc_kit.h`、`ohaudio/*.h`。

**关键架构事实**：native 层**完全没有调用 `OH_NativeXComponent_*`**（仅 `egl_renderer.h:2` 有遗留 include）。窗口桥接走：
- ArkTS `XComponentController.onSurfaceCreated` → surfaceId 转 BigInt → NAPI
- C++ `OH_NativeWindow_CreateNativeWindowFromSurfaceId`（`plugin_manager.cpp:38` / `host_vulkan_probe.cpp:800`）

这是 OpenHarmony 标准路线，迁移无需改桥接层。

### 2.4 权限层（✅，且是好消息）

`module.json5` 三个权限全在 OpenHarmony：

| 权限 | OpenHarmony 定义 |
|---|---|
| `ohos.permission.kernel.ALLOW_WRITABLE_CODE_MEMORY` | `permission_definitions.json:6387`，`system_basic` + `provisionEnable: true` + `since: 14`。**比 HarmonyOS 的 system_core/ACL 门槛低**，box64 的 W^X JIT 内存可用 |
| `ohos.permission.INTERNET` | 标准权限 |
| `ohos.permission.MICROPHONE` | 标准权限 |

无 `requiredSystemCapabilities` 依赖。

### 2.5 敏感 API 逐项核对（✅）

| API | 用途 | OpenHarmony d.ts |
|---|---|---|
| `window.setWindowLayoutFullScreen` | 沉浸布局 | ✅ |
| `window.setSpecificSystemBarEnabled` | 系统栏显示/隐藏 | ✅ |
| `window.MaximizePresentation.ENTER_IMMERSIVE_DISABLE_TITLE_AND_DOCK_HOVER` / `EXIT_IMMERSIVE` | 沉浸全屏 | ✅ |
| `window.WindowStatusType.MAXIMIZE` / `FULL_SCREEN` | 全屏状态事件 | ✅ |
| `setWindowDecorVisible` / `setWindowDecorHeight` / `setWindowTitleMoveEnabled` / `setWindowTitleButtonVisible` / `getWindowDecorHeight` / `setWindowShadowEnabled` / `setWindowMask` / `setWindowCornerRadius` / `startMoving` | 窗口装饰/异型/拖拽 | ✅ 9/9 |
| `display.getDefaultDisplaySync` | 屏幕尺寸 | ✅ |
| `image.createPixelMap` / `createImagePacker` / `packToFile` | 图标提取 | ✅ |
| `picker.DocumentPickerMode.DOWNLOAD` / `DocumentSelectMode.FOLDER` | 选择器 | ✅ |
| `deviceInfo.brand/productModel/deviceType/osFullName/sdkApiVersion` | 设备信息 | ✅（语义差异见 §3.2） |
| `inputMethod.getController/attach/on/off/detach/hideTextInput` | IME 链路 | ✅ |

---

## 3. 需要改造的 HarmonyOS 依赖点

### 3.1 工程配置（必改，改配置不改代码）

| 文件 | 字段 | 现状 | 迁移 |
|---|---|---|---|
| `build-profile.json5` + `.ohos/build-profile.release.json5` | `runtimeOS` | `"HarmonyOS"` | 删除该字段 |
| 同上 | `signingConfig.type` | `"HarmonyOS"` | 换 OpenHarmony 签名（见 §3.3） |
| 同上 | `targetSdkVersion` / `compatibleSdkVersion` | `"6.1.0(23)"` | OpenHarmony 对应版本（本机 SDK 6.1.1.125，建议先试 `"6.1.1(24)"`，以 hvigor 实测匹配为准） |
| 同上 | `nativeCompiler` | `"BiSheng"` | 删除（OpenHarmony SDK 只有 clang/llvm） |
| `Makefile:20-21` | `TARGET_SDK_VERSION` / `COMPATIBLE_SDK_VERSION` | `6.1.0(23)` | 同上改版本号 |
| `.ohos/` + `.ohos/release/` | 华为签名材料（`default_*.cer/.p7b/.p12`、`WineHua_Release.p7b`） | 华为证书链 | 换 OpenHarmony 证书链 + profile |

### 3.2 代码硬编码（真正的代码改动，仅 2 处）

1. **`components/FileBrowserView.ets:524`** — `bundleName: 'com.huawei.hmos.filemanager'`（拉起华为文件管理器）。
   **OpenHarmony 系统没有此应用**，startAbility 会失败。需改为 OpenHarmony 的文件管理应用包名，或去掉"用系统文件管理器打开"入口（兜底）。
2. **`components/AboutView.ets:62`** — `deviceInfo.osFullName` 在 OpenHarmony 返回 `OpenHarmony xx` 而非 `HarmonyOS xx`。仅关于页文案差异，不影响功能。

### 3.3 需注意、不阻塞编译的点

- `deviceInfo.deviceType` 判断（`EntryAbility.ets:222`、`WineWindowManager.ets:145/153`、`SettingsView.ets:135`）：OpenHarmony 设备的 deviceType 枚举值可能与华为设备不同，fork-shim（phone）/ 桌面模式（tablet/2in1）的分支可能要调整。
- `picker.DocumentPickerMode.DOWNLOAD`、`DocumentSelectMode.FOLDER`：枚举存在，但选择器的系统实现（可选目录集合）需在 OpenHarmony 设备实测。

### 3.4 签名体系替换

- OpenHarmony 官方工具链在本机源码树：`/src/ohos/developtools/hapsigner`（签名）+ `packing_tool`（打包）。
- 需要为 OpenHarmony 生成新的 `.p7b` profile 与证书（`system_basic` 级才能放行 `ALLOW_WRITABLE_CODE_MEMORY`），替换 `.ohos/` 下全部华为材料。

---

## 4. 运行时风险清单（真机验证，非代码改造）

编译层全过不代表运行层一致。以下每项依赖 OpenHarmony 设备的系统实现：

| # | 风险点 | 依赖 | 挂了会怎样 |
|---|---|---|---|
| 1 | **多窗口模型**：multiton UIAbility + `createSubWindowWithOptions` 子窗口 + `startMoving` 原生拖拽 | OpenHarmony WMS | 子窗口（popup/异型 ARGB）建不出来、窗口拖不动 |
| 2 | **图形栈**：`VK_OHOS_surface` + EGL（`EGLNativeWindowType=OHNativeWindow*`）+ `OH_NativeImage` zero-copy + `OH_NativeVSync` | OpenHarmony 设备 Vulkan/EGL 驱动 | **DXVK/Venus 不出画面，游戏窗口黑屏**（最致命） |
| 3 | **子进程**：`libchild_process` + appspawn 沙箱 + NCP | OpenHarmony 进程框架 | wineboot/wineserver 拉不起，直接卡死 |
| 4 | **输入法**：`inputMethod.attach/on('insertText')` 链路 | OpenHarmony IMS | 文本输入失效 |
| 5 | **system_basic 签名**：`ALLOW_WRITABLE_CODE_MEMORY` | OpenHarmony 签名体系 | box64 dynarec 无法分配 W^X 内存 |
| 6 | **设备形态**：`module.json5` 声明 `2in1`/`tablet` | 目标设备 | 包安装/启动行为差异 |

**建议验证顺序**：先验 #2（游戏窗口能出画面 = VK/EGL 通）和 #3（wineboot 能跑起来 = 子进程通），这两个不通直接卡死；再验 #1/#4/#5。

---

## 5. 迁移步骤

### 阶段 1：换工程壳（纯配置，风险最低）
1. 备份 `build-profile.json5`，写 OpenHarmony 版（删 `runtimeOS`/`nativeCompiler`，改 `signingConfig.type` 与 SDK 版本号）
2. 生成 OpenHarmony 签名（hapsigner + 新 profile），替换 `.ohos/` 材料
3. 改 `Makefile` 的 `TARGET_SDK_VERSION` / `COMPATIBLE_SDK_VERSION`

### 阶段 2：编译验证（本机即可，无需设备）
4. `make NATIVE_ARCH=arm64-v8a hap` — 暴露所有配置层问题（版本字符串匹配、签名校验、编译器路径）

### 阶段 3：代码改动（2 处硬编码）
5. 处理 `com.huawei.hmos.filemanager` 与 `osFullName` 文案

### 阶段 4：真机验证（OpenHarmony 设备）
6. 部署跑「启动 → 桌面 → 游戏窗口 → 全屏」全链路，按 §4 清单逐项验证
7. 重点先验图形栈（#2）与子进程（#3）

---

## 6. 参考

- 构建环境：`ci/Dockerfile.buildenv`、`Dockerfile`（CI 已用 OpenHarmony SDK）
- 相关文档：[ARCHITECTURE.md](ARCHITECTURE.md)、[BUILD_ENV.md](BUILD_ENV.md)
