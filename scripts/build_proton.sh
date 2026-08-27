#!/bin/bash
# build_proton.sh — Proton 引擎构建入口 (ValveSoftware/wine + WineHua OHOS 补丁系列)
#
# Proton flavor 与 wine flavor 共享全部构建管线 (build_engine.sh) 与设备端
# 契约 (files/wine 布局 / .wine prefix / wine-data.zip), 因此 ArkTS/C++ 运行时
# 无需感知引擎差异 — Box64/wineserver/winewayland.drv/wineohos.drv/DXVK overlay
# 全部经由同一套 OHOS 补丁系列获得。
#
# 源码来源 (二选一, 自动选择):
#   1. thirdparty/proton fork 已初始化 (winehua/proton, 维护者按
#      docs/submodule-patches/wine.md 的合并策略维护) → 直接使用, 要求工作树干净
#      (补丁必须留在 patches/wine-ohos, 与 vkd3d-proton 约定一致);
#   2. fork 缺位 → bootstrap: 从 PROTON_UPSTREAM_URL@PROTON_UPSTREAM_REF 克隆
#      (blob:none 部分克隆, 供 git am -3 按需取基线 blob), 在 build/proton-source
#      应用 patches/wine-ohos/*.patch 系列后构建。
# 暂存源以 stamp (基线 + 补丁系列 sha) 幂等复用; 系列更新自动重建。
#
# 用法:
#   make ENGINE=proton hap                          # 完整 Proton HAP
#   bash scripts/build_proton.sh                    # 仅引擎
#   PROTON_UPSTREAM_REF=proton-wine-10.0 bash ...   # 换基线
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

# 本脚本是 Proton 入口: 无论调用方环境如何, 强制 proton 路由
# (env.sh 已按原 ENGINE 初始化过的变量由 build_engine.sh 重新 source 覆盖)
export ENGINE=proton

STAGED_SRC="$BUILD_DIR/proton-source"   # = env.sh 的 ENGINE_SRC (proton)
STAMP_FILE="$STAGED_SRC/.winehua-source-id"

mapfile -t patch_files < <(find "$PROTON_PATCH_ROOT" -maxdepth 1 -type f -name '*.patch' -print | sort 2>/dev/null || true)
if [ "${#patch_files[@]}" -gt 0 ]; then
    series_sha="$(sha256sum "${patch_files[@]}" | sha256sum | awk '{print $1}')"
else
    series_sha="none"
fi

# fork 路径: 直接暂存 (无补丁 — fork 本身携带 OHOS 提交)
stage_from_fork() {
    local fork_commit
    fork_commit="$(git -c safe.directory="$PROTON_SRC" -C "$PROTON_SRC" rev-parse HEAD)"
    local dirty
    dirty="$(git -c safe.directory="$PROTON_SRC" -C "$PROTON_SRC" \
        status --porcelain --untracked-files=no)"
    if [ -n "$dirty" ]; then
        echo "$dirty" >&2
        err "thirdparty/proton 工作树不干净; OHOS 适配必须以提交形式进入 fork (或走 bootstrap 路径)"
    fi
    local source_id="fork-${fork_commit}-series-${series_sha}"
    if [ -f "$STAMP_FILE" ] && [ "$(cat "$STAMP_FILE")" = "$source_id" ]; then
        log "Proton 暂存源已是最新 (fork ${fork_commit:0:12})"
        return
    fi
    log "暂存 Proton fork 源 → $STAGED_SRC"
    rm -rf "$STAGED_SRC"
    mkdir -p "$STAGED_SRC"
    tar -C "$PROTON_SRC" --exclude='.git' -cf - . | tar -C "$STAGED_SRC" -xf -
    printf '%s\n' "$source_id" > "$STAMP_FILE"
}

# bootstrap 路径: 上游 clone + 应用补丁系列
stage_from_upstream() {
    [ "${#patch_files[@]}" -gt 0 ] || \
        err "patches/wine-ohos 补丁系列为空 — 先运行 scripts/gen-wine-ohos-patches.sh 生成 (见 docs/PROTON_ENGINE.md)"
    local source_id="upstream-${PROTON_UPSTREAM_REF}-series-${series_sha}"
    if [ -f "$STAMP_FILE" ] && [ "$(cat "$STAMP_FILE")" = "$source_id" ]; then
        log "Proton 暂存源已是最新 ($PROTON_UPSTREAM_REF + 系列 ${series_sha:0:12})"
        return
    fi
    log "Bootstrap Proton 源: $PROTON_UPSTREAM_URL @ $PROTON_UPSTREAM_REF"
    rm -rf "$STAGED_SRC"
    # --filter=blob:none: 只取树/提交元数据, git am -3 需要补丁 index 行引用的
    # 基线 blob 时按需回源拉取 (GitHub 支持部分克隆协议)
    git clone --filter=blob:none --branch "$PROTON_UPSTREAM_REF" \
        "$PROTON_UPSTREAM_URL" "$STAGED_SRC"
    local applied=0
    if (cd "$STAGED_SRC" && git am -3 --quiet "${patch_files[@]}"); then
        applied=1
        log "补丁系列已按 3-way 应用 (${#patch_files[@]} 个)"
    else
        # 3-way 失败 (基线 blob 不可得 / 上下文冲突): 回退 plain apply,
        # 冲突直接失败并保留现场供人工排查
        warn "git am -3 失败, 回退 patch --forward 逐个应用"
        (cd "$STAGED_SRC" && git am --abort >/dev/null 2>&1 || true)
        for pf in "${patch_files[@]}"; do
            log "  应用 $(basename "$pf")"
            patch -d "$STAGED_SRC" -p1 --forward --batch < "$pf"
        done
        applied=1
    fi
    [ "$applied" = 1 ] || err "补丁系列应用失败"
    printf '%s\n' "$source_id" > "$STAMP_FILE"
}

log "=== Proton 引擎源准备 ==="
if [ -f "$PROTON_SRC/configure.ac" ]; then
    log "检测到 thirdparty/proton fork, 使用 fork 源"
    stage_from_fork
else
    log "thirdparty/proton fork 未初始化, 走上游 bootstrap"
    stage_from_upstream
fi

# 共享引擎构建管线 (ENGINE=proton → ENGINE_SRC=build/proton-source)
ENGINE=proton bash "$SCRIPT_DIR/build_engine.sh"
