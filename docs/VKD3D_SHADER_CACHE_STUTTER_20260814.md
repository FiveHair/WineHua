# VKD3D shader 缓存与 ROTTR DX12 卡顿定位（2026-08-14）

> 本 memo 记录本轮针对 ROTTR DX12 卡顿的定位与修复进展。上屏（present）路径
> 的进一步调查另开后续工作，本文只收口已经验证到的结论。

## 一、结论概览

ROTTR DX12 卡顿有三个独立来源，按影响排序：

1. EOS 联网 SSL 失败（启动/菜单阶段主导，已修）
2. 上屏/present 路径瓶颈（当前剩余主导，未修，后续调查）
3. Vulkan shader 缓存 miss（已修，但实测不是当前主导瓶颈）

## 二、shader 缓存（已修并验证）

### Venus 空缓存 bug

`thirdparty/mesa/src/virtio/vulkan/vn_pipeline.c` 的 `vn_GetPipelineCacheData`：

```c
// 修复前
if (*pDataSize <= sizeof(*header)) { *pDataSize = 0; return VK_INCOMPLETE; }
// 修复后
if (*pDataSize <  sizeof(*header)) { *pDataSize = 0; return VK_INCOMPLETE; }
```

空缓存两阶段查询时，第一阶段返回 `header_size`，第二阶段 `*pDataSize ==
sizeof(header)` 被旧代码误判为 VK_INCOMPLETE。

### 小样例闭环（vulkan-cache-reuse-x64）

新增 `smoke/winehua_vulkan_cache_reuse.c`，补上了 offscreen 渲染（Maleoon 惰性
编译，必须先 draw 才会填充缓存），并修掉了 `physical_count = 0`（会令
vkEnumeratePhysicalDevices 直接返回 VK_INCOMPLETE）。

实测结果（设备 Virtio-GPU Venus / Maleoon 920）：

```text
status=PASS iterations=32 coldMs=3.555 warmMs=0.185 cacheBytes=3240
warmOk=true speedup=19.2x vkResult=0
```

证明 VkPipelineCache 序列化/复用链路可用。相关改动：

- `smoke/winehua_vulkan_cache_reuse.c`（新增）
- `Makefile`：把该样例加入 `GUEST_VULKAN_PROBE_INPUTS`
- `scripts/build_ohos_guest_vulkan.sh`：编译 + manifest 条目
- `entry/src/main/ets/service/SmokeRunner.ets`：注册 `vulkan-cache-reuse-x64`，
  并给它设置 `VN_PERF=no_fence_feedback,no_query_feedback`（OHOS shadow-memory
  桥需要 Host 侧 fence 轮询，否则 `vkWaitForFences` 卡死）。

## 三、VKD3D-Proton 缓存校验/写回（确认无 bug）

`thirdparty/vkd3d-proton/libs/vkd3d/cache.c`：

- 加载时校验 vendorID/deviceID/vkd3d_build/shader_interface_key/
  pipelineCacheUUID，不匹配则拒绝（cache.c:1592-1613），符合预期。
- `Serialize` 用当前设备 UUID 写回（cache.c:1414-1421），符合预期。

结论：VKD3D 层缓存逻辑正确，无额外 bug。

## 四、EOS 联网卡顿（已定位，靠 stub DLL 解决）

游戏日志（ROTTR.log）：

```text
[Epic] LogHttp: request failed, libcurl error: 35 (SSL connect error)
[Epic] LogHttp: Lockout of 58.026978s on https://api.epicgames.dev
[Epic] LogEOS: Large tick time detected 8.19 / 10.91 / 45.80
```

根因：`entry/src/main/ets/pages/Index.ets` 第 1051 行游戏启动时把
`WINEDLLOVERRIDES` 覆盖为 `d3d12=n`，冲掉了 C++ 侧
`AppendD3dBackendEnv`（wine_env.cpp:201）里的 `EOSSDK-Win64-Shipping=`。

注意：`EOSSDK-Win64-Shipping=`（空值 = 禁用）不可行，游戏对 EOSSDK 是静态导入，
禁用后直接起不来（Box64 SIGTRAP）。正确做法是替换 stub DLL：

```text
D:\MyProject\EOSSDK-Win64-Shipping.dll  （@0x17EE0: 33 C0 C3 = xor eax,eax; ret）
原文件备份为 .orig
```

替换后游戏日志变为：

```text
[Epic] Creating platform.
[Epic] Failed to create platform!            // stub 生效，不再联网
ZGameInterface::PlatformInit - ERROR: OSuite game URL is invalid.  // 转离线
```

`Lockout / Large tick` 消失，进菜单从约 4 分钟缩短到约 1.5 分钟。

Index.ets 当前值（已回退掉会崩溃的禁用）：

```ts
environment['WINEDLLOVERRIDES'] = requiresVkd3dRuntime
  ? 'd3d12=n;d3d11=n;dxgi=n'
  : 'd3d12=n,b';
```

## 五、上屏/present 路径（新暴露，未修，后续调查）

EOS 修掉后，剩余瓶颈移到上屏路径：

```text
guest WineHuaGuestFramePerf: presents=120 present_us 平均 14.4ms（约 70 fps）
                             renderer_us 平均 1.4ms
实际显示 winehua_display_fps.txt: 0.055 ~ 0.577 fps（约 1.7 秒/帧）
```

即渲染/提交侧约 70 fps，但帧送到 OHOS surface 上屏这一步（swapOk）只有约 0.5
fps，大量帧被丢。涉及 VirGL/Venus present -> surface_queue -> external_oes 传输，
与 shader 缓存无关，后续单独调查。

## 六、构建/部署速记

```bash
# 容器内构建（Mesa dirty 时需带 verified hash）
cd /data/src/winehua && export \
  WINEHUA_GUEST_MESA_VERIFIED_COMMIT=0a239288f90ba0afe074aa90832d8fd71bd0ffe8 \
  WINEHUA_GUEST_MESA_VERIFIED_PATCH_SHA256=ff2ca022284a9663df61392739d22e6a947afd8e9a41a0349737a86c333e35f2 \
  WINEHUA_VKD3D_VERIFIED_COMMIT=3e5aab6fb3e18f81a71b339be4cb5cdf55140980 \
  WINEHUA_VKD3D_VERIFIED_CLEAN=1 \
  && make hap NATIVE_ARCH=arm64-v8a
```

启动 ROTTR（DX12/VKD3D）：

```text
aa start -a EntryAbility -b app.hackeris.winehua --ps winehua.mode game \
  --ps winehua.game_path 'z:\games\Raider\ROTTR-500K.exe' \
  --ps winehua.d3d_backend vkd3d_limited_500k
```

观察路径：

```text
游戏日志: files/.wine/drive_c/users/100/Documents/Rise of the Tomb Raider/Rise of the Tomb Raider.log
帧率:     files/.wine/drive_c/windows/temp/winehua_display_fps.txt
wine 日志: files/../temp/wine_stderr_20260814.log
```
