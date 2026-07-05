# WineHua Graphics Test

独立 Win32/WGL 图形样例，验证 WineHua 图形链路是否已经切到 VirGL，并观察最终上屏路径。

## 编译

```bash
chmod +x build.sh
./build.sh
```

## 部署到 Pad

```bash
hdc file send winehua_graphics_test.exe /data/storage/el2/base/files/wine/bin/
```

## 验证图形链路

```bash
hdc hilog | grep -E "GraphicsBroker|Virgl|WL_EGL|WL_Server|winehua_graphics"
```

### 验证清单

| 步骤 | 预期日志 | 含义 |
|---|---|---|
| 1 | active=virgl | broker 已选择 VirGL |
| 2 | VTEST socket ready / virgl_test_server | host vtest server 已拉起 |
| 3 | ChoosePixelFormat succeeded | WGL 像素格式可枚举 |
| 4 | wglCreateContext succeeded | GLES fallback / WGL context 正常 |
| 5 | GL vendor/renderer/version | guest OpenGL 路径实际工作 |
| 6 | fps=... | 持续出图，窗口和交换链正常 |

## 文件结构

```
Test/GraphicsTest/
  main.c
  build.sh
  README.md
  winehua_graphics_test.exe
```

这个样例参考了 thirdparty/wine/programs/winehua_graphics_smoke 的做法，但独立放在 Test 目录，便于快速交叉编译和分发。