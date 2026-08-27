# Proton 引擎 (ENGINE=proton)

> 状态:构建体系与 104 补丁系列(`patches/wine-ohos/`)已就绪;wine flavor
> 已在 fork 流水线全链路验证,proton flavor 停在补丁系列对 Proton 基线的
> 28 处冲突(§7.1,需人工 rebase)。
> 设计原则:**Proton 是构建 flavor,不是运行时可切换引擎** — 设备端契约与
> wine flavor 完全一致,现有功能(Wayland 合成器 / 音频 / DXVK·VKD3D overlay /
> VirGL·Venus / 输入 / Box64)全部保留。

## 1. 这是什么

WineHua 的 Proton 版本把兼容层的 Wine 部分换成 Valve 的 Proton wine
([ValveSoftware/wine](https://github.com/ValveSoftware/wine),`proton-wine-*`
分支),其余栈(Box64 指令翻译、嵌入式 Wayland compositor、Guest Mesa
VirGL/Venus、DXVK/VKD3D-Proton 受管 overlay、Host 音频 broker)原样复用:

```
Windows PE (x86_64)
    ↓ Box64 (x86_64 → ARM64 指令翻译)
Proton wine (ValveSoftware/wine + WineHua OHOS 补丁系列, x86_64 musl)
    ↓ winewayland.drv (补丁系列携带)
嵌入式 Wayland compositor / GraphicsBroker / AudioBroker (不变)
```

Steam 的 `proton` 启动脚本(python)、Steam runtime、esync/fsync 均不适用
于 OHOS 无头沙箱 — 这里的 "Proton" 指 **Proton 的 wine 树本身**:它携带
Valve 的游戏兼容补丁与上游 wine 的差异集,由 WineHua 的启动编排
(`wineserver → wineboot --init → explorer`,见 `wine_launch.cpp`)直接驱动。

## 2. 为什么是构建 flavor(而不是 app 内切换引擎)

1. **x86_64 的硬约束**:引擎的 Unix `.so`(ntdll.so / win32u.so /
   winewayland.so …)在 x86_64 设备上必须落在 `entry/libs/x86_64` ——
   OHOS 拒绝从 el2 数据区 dlopen(见 `assemble.sh` 的 guest_gfx el1 注释)。
   `libs/` 是 HAP 打包期固化的,两个引擎的同名 `.so` 无法并存。
2. **设备端契约零改动**:wine flavor 与 proton flavor 产出同名的
   `wine-data.zip` / `wine-runtime-manifest.json`,解压到同一 `files/wine`
   布局,prefix 仍是 `.wine`。ArkTS/C++ 运行时(以及升级/重置/烟测全链路)
   不感知引擎差异,升级检测天然工作(sha 变化 → 提示刷新)。
3. **包体**:两套引擎运行时会使 HAP 增加约一倍 rawfile 体积。

后续若要在 arm64 单 HAP 内双引擎并存,需要把 D3D overlay/图形运行时从
引擎树剥离到共享目录、并为 `WINE_RUNTIME_ROOT` 引入引擎维度 — 详见 §7。

## 3. 使用

```bash
# 完整 Proton HAP (与 wine flavor 同参数)
make ENGINE=proton NATIVE_ARCH=arm64-v8a hap

# 只构建引擎 (host 工具 + OHOS Unix .so + wineserver)
make ENGINE=proton wine

# 换上游基线 (ValveSoftware/wine 的分支/tag)
PROTON_UPSTREAM_REF=proton_11.0 make ENGINE=proton wine
```

产物按 flavor 隔离,与 wine 构建互不干扰:

| 内容 | wine | proton |
|---|---|---|
| host 工具 | `build/wine-native` | `build/proton-native` |
| OHOS 交叉编译 | `build/wine-ohos` | `build/proton-ohos` |
| wineserver | `build/wine_server` | `build/proton_server` |
| 打包暂存 | `build/staging/wine-data` | `build/staging/proton-data` |
| Makefile stamp | `.stamps/wine-<arch>` / `deps` / `assemble` | `.stamps/proton-<arch>` / `deps-proton` / `assemble-proton` |

rawfile 契约不变:`wine-data.zip` + `wine-runtime-manifest.json`(新增
`"engine": "wine"|"proton"` 字段,仅用于诊断与 CI 校验)。

## 4. 源码从哪来(bootstrap 流程)

`scripts/build_proton.sh` 按以下顺序解析引擎源:

1. **fork 路径**:`thirdparty/proton` 已检出(`winehua/proton`,维护者按
   `docs/submodule-patches/wine.md` 的合并策略把 OHOS 系列维护在 Valve
   基线上)→ 直接以 fork 为源构建;要求工作树干净(补丁必须以提交形式
   进入 fork,与 `patches/vkd3d-proton` 约定一致)。
2. **bootstrap 路径**:fork 缺位 → 从
   `PROTON_UPSTREAM_URL@PROTON_UPSTREAM_REF` 做 `--filter=blob:none`
   部分克隆到 `build/proton-source`,用 `git am -3` 应用
   `patches/wine-ohos/*.patch` 系列(blob:none 克隆在 3-way 需要基线 blob
   时按需回源拉取);3-way 失败回退 `patch -p1 --forward` 逐个应用,
   冲突直接失败保留现场。

暂存源以 stamp(基线标识 + 补丁系列 sha256)幂等复用;系列更新或
`PROTON_UPSTREAM_REF` 变化会自动重建。bootstrap 产物在 `build/` 下,
不污染 `thirdparty/`,因此**当前不注册 submodule** — 等 fork 仓库建立后
再注册并删除 `.gitignore` 中的 `thirdparty/proton/` 条目。

## 5. 补丁系列的生成与维护

Proton flavor 的全部 OHOS 适配来自 `patches/wine-ohos/NNNN-*.patch`
(fork 相对上游 wine 的全部独有提交,以 `git format-patch` 导出):

```bash
# 前提: thirdparty/wine 完整检出 + upstream remote 已配置
./scripts/setup-upstream-remotes.sh
git -C thirdparty/wine fetch upstream master

# 生成/刷新系列 (默认 merge-base HEAD upstream/master; 也可显式传基线)
bash scripts/gen-wine-ohos-patches.sh
git add patches/wine-ohos && git commit -m "proton: refresh OHOS patch series"
```

- 系列提交进仓库后,CI 的 `build-proton` job 自动启用(以系列存在为门控);
- wine fork 每次合入新的 OHOS 提交后重跑一次生成脚本保持同步;
- 导出约定与 `patches/vkd3d-proton` 一致:文件名字典序 = 应用顺序;
- 与 Valve 基线的冲突概率集中在 `docs/submodule-patches/wine.md` 标注的
  23 个"删除行敏感"文件 — 3-way 失败时按该文档逐条 review。

## 6. 与 wine flavor 的差异清单

| 环节 | 差异 |
|---|---|
| `scripts/env.sh` | `ENGINE` 变量 + `ENGINE_*` 路由(源/产物目录),`PROTON_*` bootstrap 配置 |
| `scripts/build_engine.sh` | 共享构建管线(wine 原实现参数化;`build_wine.sh` 变为薄入口) |
| `scripts/build_proton.sh` | Proton 入口:源解析 + bootstrap + 补丁应用 |
| `scripts/assemble.sh` | 暂存目录/取源按 `ENGINE` 切换;manifest 增加 `engine` 字段 |
| `scripts/build_deps.sh` | Wine Mono 下载落点随引擎(`build/<engine>-ohos/share/wine/mono`) |
| `Makefile` | `ENGINE ?= wine`;stamp/sentinel 按 flavor 隔离;`proton` 无别名目标(必须 `ENGINE=proton`) |
| CI | `build-proton` job(门控 + payload 契约校验,断言 `engine=proton` 与关键 .so 存在) |
| 运行时 (ArkTS/C++) | **零改动**(契约不变) |

Proton 语义注意事项:

- **esync/fsync 默认关闭**:Valve wine 在非 Steam 环境默认不启用,
  `WINEESYNC`/`WINEFSYNC` 未注入基线环境;OHOS musl 的 futex 语义未验证,
  开启前需单独评估(必要时经设置页 compat 档位注入)。
- **wine.inf**:Proton 树的 `wine.inf` 含 Valve 注册表调整,`assemble.sh`
  的 OHOS 字体替换 sed 照常追加。
- **Wine Mono**:沿用 wine-mono 11.1.0 MSI(`mscoree` 侧版本匹配约束,
  见 `build_deps.sh` 注释),两 flavor 共用。

## 7. 已知限制与后续方向

1. **补丁系列尚未对 Proton 基线 rebase**(2026-08-27 fork 流水线实测):
   - 系列共 104 个补丁,导出自 wine fork 相对上游 master(基线 13289668,
     2026-06-07)的独有提交;
   - 对 `proton_11.0` 与 `experimental_11.0`(两者 wine 基座相同)累积应用,
     **76/104 干净应用,28 个冲突** — 根因是 Valve 基线分叉于 2026 年初,
     落后 fork 基线约半年的上游 wine 改动(首个冲突即 0001 的
     `dlls/ntdll/unix/virtual.c`,目标函数偏移 2400+ 行);
   - `git am -3` 无法挽救: 补丁前镜像 blob 晚于 Valve 分叉点,不在
     ValveSoftware/wine 仓库中("sha1 information is lacking or useless"),
     自动回退 `patch --forward` 后在真实冲突处 fail-fast;
   - **打通 Proton 构建的剩余工作 = 把这 28 个补丁人工 rebase 到
     Proton 基线**(约 23 个"删除行敏感"文件,按 docs/submodule-patches/wine.md
     逐条 review),或在 Valve 仓库存在 winehua/proton fork 时直接维护 fork。
     bootstrap/构建/打包链路本身已由流水线验证就绪。
2. **运行时双引擎**(arm64 理论可行):需要
   - DXVK/VKD3D overlay 与 guest/host 图形运行时移到引擎无关目录
     (现在挂在 `files/wine/{dxvk,vkd3d}` 与 `bin/{guest_vulkan,host_vulkan}`,
     C++ 侧 `WINE_RUNTIME_ROOT` 硬编码,见 `wine_env.cpp` 的
     `AppendD3dBackendEnv`);
   - `wine_constants.h` 的 `WINE_PREFIX`/`WINE_RUNTIME_ROOT` 参数化为
     per-engine;
   - ArkTS `WineEnvService` 增加引擎偏好与第二 zip 的解压/升级流程;
   - x86_64 因 §2 约束仍只能单引擎。
3. **Proton 版本钉子**:`PROTON_UPSTREAM_REF` 默认 `proton_11.0`,
   升级 = 改默认值或构建时覆盖;系列冲突率随版本距离增长。
4. **fork 流水线**(`fork-pipeline.yml`):不依赖私有 buildenv 镜像,
   用 OpenHarmony 公开 SDK(repo.huaweicloud.com 6.1-Release 的
   native-linux-x64 组件,clang-15)+ apt 依赖构建引擎链路;
   wine flavor 已全链路跑通,proton flavor 停在 §7.1 的补丁冲突。
   上游 CI 用私有镜像内的 HarmonyOS 6.1.0(23) SDK(clang 更新),
   fork 路径仅作无凭据环境的冒烟验证。
