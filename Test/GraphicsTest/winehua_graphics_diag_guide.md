# WineHua / Wine + VirGL 图形诊断测试推进文档

> 目标：提供一个可独立运行的 Win32 图形诊断程序，用于验证 Wine 下 VirGL/OpenGL/WGL/D3D 相关接口的实现完整度，方便后续由 Codex 继续补测试、接入 CI、定位游戏兼容问题。

---

## 1. 背景与定位

当前已有测试程序可以验证：

- Win32 窗口创建；
- `ChoosePixelFormat` / `SetPixelFormat`；
- `wglCreateContext` / `wglMakeCurrent`；
- OpenGL 固定管线绘制；
- 深度测试；
- `SwapBuffers`；
- FPS 输出；
- WineHua 自定义图形运行时环境变量检查。

这些内容适合作为 **VirGL 能否出图的冒烟测试**，但还不足以证明游戏常用图形路径已经完整可用。

游戏/引擎更容易触发的问题包括：

- SDL2 通过 WGL 扩展创建 OpenGL context；
- Unity / Godot / 现代 OpenGL 使用 shader、VBO、FBO、纹理格式；
- D3D9 游戏依赖 device reset、render target、DXT 纹理、depth-stencil；
- D3D11 / Unity 常用 DXGI swapchain、RTV/SRV、BC 压缩纹理；
- 全屏、窗口缩放、最小化恢复、present/vsync 行为异常；
- VirGL host/guest 资源生命周期不一致导致泄漏或崩溃。

因此本次新增 `winehua_graphics_diag.c`，用于覆盖更多接口并输出可读诊断报告。

---

## 2. 新增文件

### 2.1 `winehua_graphics_diag.c`

单文件 Win32 C 程序，不依赖 WineHua 项目私有头文件。

主要能力：

| 模块 | 覆盖内容 |
|---|---|
| ENV | 检测是否运行在 Wine 下 |
| WIN32 | 注册窗口类、创建窗口、获取 HDC |
| WGL 基础 | `ChoosePixelFormat`、`SetPixelFormat`、`DescribePixelFormat`、`wglCreateContext`、`wglMakeCurrent` |
| WGL 扩展 | `wglGetExtensionsStringARB/EXT`、`wglChoosePixelFormatARB`、`wglCreateContextAttribsARB`、`wglSwapIntervalEXT` |
| Context | legacy context、share context、OpenGL 3.3 core / 3.0 compatibility 尝试创建 |
| GL 能力 | vendor、renderer、version、GLSL、extension、max texture、max samples |
| 纹理 | RGBA8、BGRA、NPOT、sRGB、mipmap、DXT1/DXT5 |
| Shader/VBO | GLSL 110、vertex shader、fragment shader、program link、VBO、VAO、draw call |
| FBO | RGBA8 color attachment、D24S8 depth-stencil、clear/readback、MSAA FBO、blit/resolve |
| Present | `SwapBuffers` 循环、resize、FPS 粗测 |
| Stress | 纹理/Buffer 创建销毁压力测试 |
| D3D9 | 可选：`Direct3DCreate9`、device、present、A8R8G8B8 texture、DXT1 texture |
| D3D11 | 可选：`D3D11CreateDeviceAndSwapChain`、RTV、present、RGBA8 Texture2D、BC1 SRV texture |

---

## 3. 编译方式

### 3.1 MinGW-w64

```bash
x86_64-w64-mingw32-gcc -O2 -Wall -DCOBJMACROS \
  -o winehua_graphics_diag.exe winehua_graphics_diag.c \
  -lopengl32 -lgdi32 -luser32
```

说明：

- 程序对 D3D9/D3D11 采用动态加载；
- 如果编译环境有 `d3d9.h`、`d3d11.h`、`dxgi.h`，会自动启用 D3D 测试；
- 如果没有这些头文件，会自动跳过 D3D 测试并输出 WARN；
- 通常不需要显式链接 `d3d9.lib` / `d3d11.lib`，因为程序通过 `LoadLibrary/GetProcAddress` 获取入口。

### 3.2 MSVC Developer Prompt

```bat
cl /O2 /W3 /DCOBJMACROS /DWH_ENABLE_D3D=1 winehua_graphics_diag.c user32.lib gdi32.lib opengl32.lib
```

如遇 D3D 头文件或 SDK 问题，可关闭 D3D 部分：

```bat
cl /O2 /W3 /DCOBJMACROS /DWH_ENABLE_D3D=0 winehua_graphics_diag.c user32.lib gdi32.lib opengl32.lib
```

---

## 4. 运行方式

### 4.1 Wine 下运行

```bash
wine winehua_graphics_diag.exe --visible --seconds 5 --log diag.log
```

### 4.2 快速诊断

```bash
wine winehua_graphics_diag.exe --quick --log diag_quick.log
```

`--quick` 会跳过 D3D9/D3D11 测试。

### 4.3 压力测试

```bash
wine winehua_graphics_diag.exe --visible --stress --seconds 30 --log diag_stress.log
```

### 4.4 参数说明

| 参数 | 说明 |
|---|---|
| `--visible` | 显示测试窗口 |
| `--quick` | 跳过 D3D 测试，快速检查 WGL/GL |
| `--stress` | 启用资源创建销毁压力测试 |
| `--seconds N` | present/stress 测试时长提示，默认 3 秒 |
| `--log PATH` | 输出日志到指定文件 |

---

## 5. 日志判定方式

日志格式示例：

```text
[PASS] WGL          wglCreateContext                    rc=0000000000012345
[WARN] WGL          wglCreateContextAttribsARB           missing; GL 3.x/4.x core profile creation may fail
[FAIL] GLFBO        RGBA8 + D24S8 FBO                    status=0x8CD6

SUMMARY: PASS=35 WARN=4 FAIL=1
RESULT: FAILED
```

### 5.1 PASS

说明该接口或功能路径基本可用。

### 5.2 WARN

说明该能力缺失或不完整，但不一定导致所有游戏失败。

常见 WARN：

- `wglCreateContextAttribsARB` 缺失：现代 OpenGL core profile 创建可能失败；
- `wglSwapIntervalEXT` 缺失：vsync 控制不可用；
- DXT/S3TC 缺失：很多 PC 游戏贴图可能异常；
- D3D 头文件不可用：当前编译版本跳过 D3D 测试。

### 5.3 FAIL

说明基础接口行为异常，建议优先修复。

常见 FAIL 优先级：

| FAIL 项 | 优先级 | 影响 |
|---|---:|---|
| `SetPixelFormat` 失败 | P0 | GL 窗口无法建立 |
| `wglCreateContext` 失败 | P0 | OpenGL 基础链路不可用 |
| `SwapBuffers` 失败 | P0 | present 链路不可用 |
| FBO incomplete | P1 | 后处理、render target、Unity/Godot 画面异常 |
| Shader compile/link 失败 | P1 | 现代 GL 游戏无法运行 |
| BGRA / sRGB / DXT 失败 | P1 | 贴图、颜色、UI、字体可能异常 |
| D3D9 CreateDevice 失败 | P1 | DX9 游戏不可用 |
| D3D11 SwapChain 失败 | P1 | Unity/DX11 游戏不可用 |

---

## 6. 推荐测试顺序

### 阶段 1：VirGL/OpenGL 基础可用性

```bash
wine winehua_graphics_diag.exe --quick --visible --log gl_quick.log
```

重点看：

- `GL_VENDOR` / `GL_RENDERER` 是否指向 VirGL/Mesa/宿主渲染路径；
- `wglCreateContext` 是否 PASS；
- `SwapBuffers loop` 是否 PASS；
- `PixelFormat acceleration` 是否不是明显软件路径。

### 阶段 2：现代 GL 完整性

```bash
wine winehua_graphics_diag.exe --visible --seconds 5 --log gl_full.log
```

重点看：

- `wglCreateContextAttribsARB`；
- `VBO functions`；
- `Shader functions`；
- `RGBA8 + D24S8 FBO`；
- `MSAA 4x FBO`；
- `DXT1/BC1 compressed texture`；
- `sRGB texture`。

### 阶段 3：稳定性

```bash
wine winehua_graphics_diag.exe --visible --stress --seconds 60 --log gl_stress.log
```

重点看：

- 是否中途 GL error；
- 是否崩溃；
- 是否 host renderer / virglrenderer 资源泄漏；
- 是否出现长时间卡顿。

### 阶段 4：D3D9/D3D11 游戏路径

```bash
wine winehua_graphics_diag.exe --visible --seconds 5 --log d3d_diag.log
```

重点看：

- `D3D9 CreateDevice`；
- `D3D9 DXT1 texture`；
- `D3D11 CreateDeviceAndSwapChain`；
- `D3D11 Backbuffer RTV`；
- `D3D11 BC1 SRV texture`。

---

## 7. Codex 后续推进建议

### 7.1 优先补充项

建议 Codex 后续按以下顺序扩展：

1. 增加 JSON 输出模式：`--json diag.json`；
2. 增加 SDL2 测试版本：`SDL_CreateWindow`、`SDL_GL_SetAttribute`、`SDL_GL_CreateContext`、`SDL_GL_SwapWindow`；
3. 增加 D3D9 device reset 测试；
4. 增加 D3D11 `ResizeBuffers` 测试；
5. 增加 D3D11 shader compile/draw 测试；
6. 增加 typeless/sRGB/depth-stencil 格式映射测试；
7. 增加多窗口、多 context、多线程 make-current 测试；
8. 增加截图对比或 readback checksum；
9. 增加 virgl host/broker 资源计数输出；
10. 接入 CI，自动运行并保存日志。

### 7.2 建议新增命令行参数

```text
--json PATH             输出 JSON 结果
--no-d3d                跳过 D3D9/D3D11
--no-fbo                跳过 FBO
--no-stress             跳过压力测试
--sdl2                  启用 SDL2 路径测试
--threads N             多线程 context 测试
--frames N              固定帧数测试
--dump-caps PATH        仅输出能力报告
```

### 7.3 建议 JSON 结构

```json
{
  "env": {
    "wine": true,
    "backend": "virgl"
  },
  "gl": {
    "vendor": "...",
    "renderer": "...",
    "version": "...",
    "glsl": "..."
  },
  "wgl": {
    "arb_pixel_format": true,
    "arb_create_context": true,
    "swap_control": true
  },
  "tests": [
    {
      "category": "GLFBO",
      "name": "RGBA8 + D24S8 FBO",
      "result": "PASS",
      "detail": "complete"
    }
  ],
  "summary": {
    "pass": 35,
    "warn": 4,
    "fail": 0
  }
}
```

---

## 8. 与游戏兼容性的对应关系

| 游戏/引擎路径 | 关键测试项 |
|---|---|
| SDL2 + OpenGL | WGL extension、context、SwapBuffers、resize |
| 老 OpenGL 游戏 | compatibility pipeline、fixed function、texture、present |
| 现代 OpenGL 游戏 | shader、VBO、VAO、FBO、sRGB、compressed texture |
| Unity OpenGLCore | OpenGL 3.x context、shader、FBO、MSAA、texture format |
| Unity D3D11 | D3D11 device、swapchain、RTV/SRV、BC texture、ResizeBuffers |
| DX9 游戏 | D3D9 device、device reset、render target、DXT texture、D24S8 |
| WineD3D 路径 | GL capability、FBO、format mapping、shader、present |
| DXVK 路径 | 需要 Vulkan/Venus，不是当前 VirGL/OpenGL 测试能完整覆盖 |

---

## 9. 当前程序的边界

当前 `winehua_graphics_diag.c` 仍然是诊断工具，不是完整游戏兼容测试套件。

未覆盖或只粗略覆盖：

- SDL2 自身窗口/context 路径；
- D3D9 lost device / reset 完整流程；
- D3D11 shader 编译和 draw call；
- DXGI `ResizeBuffers`；
- 多线程 GL context；
- 多进程图形资源共享；
- Vulkan/DXVK/Venus；
- 实际游戏资源格式大规模加载；
- 图像正确性自动比对。

这些应作为后续 Codex 继续扩展的任务。

---

## 10. 建议工程放置位置

建议放到：

```text
tools/graphics_diag/winehua_graphics_diag.c
tools/graphics_diag/winehua_graphics_diag_guide.md
```

或：

```text
tests/graphics/winehua_graphics_diag.c
tests/graphics/README.md
```

建议日志输出目录：

```text
artifacts/graphics_diag/YYYYMMDD_HHMMSS/diag.log
```

---

## 11. 后续判断标准

### 最低可用目标：2D + 轻 3D

应至少满足：

- WGL legacy context PASS；
- `SwapBuffers loop` PASS；
- RGBA8/BGRA texture PASS；
- shader/VBO PASS；
- basic FBO PASS；
- 不出现明显资源泄漏或长时间卡死。

### SDL2/OpenGL 游戏目标

应进一步满足：

- `wglCreateContextAttribsARB` 可用；
- `wglChoosePixelFormatARB` 可用；
- fullscreen/resize 稳定；
- sRGB、NPOT、mipmap 正常；
- SDL2 专用测试通过。

### Unity/DX9/DX11 游戏目标

应进一步满足：

- DXT/BC 压缩纹理可用；
- D24S8 / depth-stencil 可用；
- MSAA FBO / blit resolve 可用；
- D3D9 CreateDevice/Present/Texture 可用；
- D3D11 CreateDeviceAndSwapChain/RTV/SRV 可用；
- 后续补充 D3D9 reset 和 D3D11 ResizeBuffers 测试。

---

## 12. Codex 任务提示词建议

可以把下面提示词直接交给 Codex：

```text
请基于 tools/graphics_diag/winehua_graphics_diag.c 继续扩展 WineHua 图形诊断测试工具。
目标是验证 Wine + VirGL/OpenGL/WGL/D3D9/D3D11 在游戏兼容场景下的实现完整度。

优先任务：
1. 增加 --json 输出，结构参考 winehua_graphics_diag_guide.md。
2. 增加 D3D9 device reset 测试，覆盖窗口 resize、Reset、资源重建。
3. 增加 D3D11 ResizeBuffers 测试，覆盖 RTV 释放、ResizeBuffers、RTV 重建、Present。
4. 增加 SDL2 OpenGL 路径测试文件，覆盖 SDL_GL_SetAttribute、SDL_GL_CreateContext、SDL_GL_SwapWindow。
5. 保持单文件可编译，尽量动态加载外部 API，避免强依赖链接库。
6. 每个测试项输出 PASS/WARN/FAIL，失败时输出 HRESULT/GetLastError/glGetError。

不要把测试写成 benchmark，重点是接口完整性、能力误报、资源生命周期和窗口/present 行为。
```

---

# GUI 点击测试版本补充说明

## 1. 当前程序形态

本包现在包含两种入口：

| 文件 | 用途 |
|---|---|
| `winehua_graphics_diag.c` | 诊断核心，命令行版本，适合自动化、CI、Codex 批量运行 |
| `winehua_graphics_diag_gui.c` | Win32 GUI 前端，适合双击运行、按钮测试、现场诊断 |

GUI 版本并不是重新写一套测试逻辑，而是通过 `#include "winehua_graphics_diag.c"` 复用同一套诊断核心。这样后续 Codex 增加新的 GL/WGL/D3D 测试项时，优先改 `winehua_graphics_diag.c`，GUI 端可以继续复用，避免 CLI 与 GUI 两套结果不一致。

## 2. GUI 功能

GUI 窗口包含以下按钮：

| 按钮 | 说明 |
|---|---|
| `Quick Test` | 快速测试，覆盖基础 WGL、GL、纹理、FBO、Shader、Present，跳过 D3D9/D3D11 |
| `Full Test` | 完整测试，执行 OpenGL/WGL 诊断，并尝试 D3D9/D3D11 诊断 |
| `Stress Test` | 压力测试，增加资源创建/销毁、纹理/Buffer 循环等稳定性验证 |
| `Clear Log` | 清空 GUI 显示和临时日志 |
| `Open Log` | 用记事本打开日志文件 |
| `Exit` | 退出程序 |

运行测试时会出现一个小的渲染窗口，用于验证 `SwapBuffers`、窗口 resize、OpenGL present 路径是否正常。

## 3. GUI 日志位置

GUI 版本默认把日志写到：

```text
%TEMP%\winehua_graphics_diag_gui.log
```

GUI 右侧文本框会周期刷新这个日志，测试完成后会显示最终结果：

```text
SUMMARY: PASS=xx WARN=xx FAIL=xx
RESULT: OK_WITH_WARNINGS_ALLOWED
```

或：

```text
SUMMARY: PASS=xx WARN=xx FAIL=xx
RESULT: FAILED
```

## 4. GUI 编译方式

### MinGW-w64，启用 D3D9/D3D11

```bash
x86_64-w64-mingw32-gcc -O2 -Wall -DCOBJMACROS -DWH_ENABLE_D3D=1 \
  -mwindows -o winehua_graphics_diag_gui.exe winehua_graphics_diag_gui.c \
  -lopengl32 -lgdi32 -luser32 -lshell32 -ld3d9 -ld3d11 -ldxgi
```

### MinGW-w64，不启用 D3D

如果当前交叉编译环境没有 D3D SDK 头文件或库，可以先关闭 D3D：

```bash
x86_64-w64-mingw32-gcc -O2 -Wall -DCOBJMACROS -DWH_ENABLE_D3D=0 \
  -mwindows -o winehua_graphics_diag_gui.exe winehua_graphics_diag_gui.c \
  -lopengl32 -lgdi32 -luser32 -lshell32
```

### MSVC

```bat
cl /O2 /W3 /DCOBJMACROS /DWH_ENABLE_D3D=1 /Fe:winehua_graphics_diag_gui.exe ^
  winehua_graphics_diag_gui.c user32.lib gdi32.lib opengl32.lib shell32.lib d3d9.lib d3d11.lib dxgi.lib /link /SUBSYSTEM:WINDOWS
```

## 5. Codex 后续推进建议

### 5.1 保持测试核心和 GUI 解耦

后续新增测试时，建议优先在 `winehua_graphics_diag.c` 内补充：

```text
static void test_xxx(void)
```

然后在 CLI 主流程中挂接。GUI 不需要重复实现测试逻辑。

### 5.2 建议新增的按钮/模式

后续 GUI 可以继续增加：

| 模式 | 目标 |
|---|---|
| `WGL Only` | 只测 PixelFormat、WGL extension、context 创建 |
| `Texture Only` | 只测 RGBA/BGRA/sRGB/DXT/NPOT/mipmap |
| `FBO Only` | 只测 render target、depth-stencil、MSAA resolve |
| `D3D9 Only` | 只测 Direct3D9 device/reset/texture/present |
| `D3D11 Only` | 只测 DXGI swapchain、RTV、shader、texture |
| `Export JSON` | 输出机器可读结果，方便自动化分析 |

### 5.3 推荐新增 JSON 输出

目前日志是人类可读的 PASS/WARN/FAIL。后续建议 Codex 增加：

```text
--json diag.json
```

输出示例：

```json
{
  "env": {
    "wine": true,
    "gl_vendor": "...",
    "gl_renderer": "...",
    "gl_version": "..."
  },
  "summary": {
    "pass": 35,
    "warn": 4,
    "fail": 1
  },
  "tests": [
    {"level":"PASS", "category":"WGL", "name":"wglCreateContext", "detail":"ok"},
    {"level":"WARN", "category":"GLTEX", "name":"DXT5", "detail":"extension not advertised"}
  ]
}
```

这样可以在 WineHua 启动器中展示图形能力，也可以在 Codex/CI 中对不同设备进行对比。

## 6. 使用建议

现场手工验证时：

1. 双击 `winehua_graphics_diag_gui.exe`；
2. 先点 `Quick Test`；
3. 如果没有 FAIL，再点 `Full Test`；
4. 如果要验证长时间稳定性，再点 `Stress Test`；
5. 把 `Open Log` 打开的日志提交给 Codex 或开发者继续分析。

工程自动化验证时，仍建议使用 CLI 版本：

```bash
wine winehua_graphics_diag.exe --quick --log diag_quick.log
wine winehua_graphics_diag.exe --visible --seconds 8 --log diag_full.log
wine winehua_graphics_diag.exe --visible --stress --seconds 30 --log diag_stress.log
```
