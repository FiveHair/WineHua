#!/bin/bash
# gen-wine-ohos-patches.sh — 从 thirdparty/wine fork 导出 WineHua OHOS 补丁系列
#
# 用途: Proton flavor (build_proton.sh) 需要把 winehua/wine fork 相对上游的
# 全部 OHOS 适配提交重放到 ValveSoftware/wine 上。本脚本把 fork 独有提交
# 导出为 patches/wine-ohos/NNNN-*.patch 系列 (与 patches/vkd3d-proton 同一套
# 约定), 由维护者在本地运行后提交到仓库 — CI 不需要 wine fork 的上游历史。
#
# 前提:
#   1. thirdparty/wine 已完整检出 (非 shallow)
#   2. 上游 remote 已配置: ./scripts/setup-upstream-remotes.sh
#      (并 git -C thirdparty/wine fetch upstream master)
#   3. fork 历史线性于 merge-base 之后 (rebase 过; format-patch 对 merge
#      提交会跳过并告警)
#
# 用法: bash scripts/gen-wine-ohos-patches.sh [base_commit]
#   base_commit 缺省 = git merge-base HEAD upstream/master
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

WINE_GIT=(git -c safe.directory="$WINE_SRC" -C "$WINE_SRC")
PATCH_OUT="$PROTON_PATCH_ROOT"

[ -f "$WINE_SRC/configure.ac" ] || err "thirdparty/wine 未检出: $WINE_SRC"
if ! "${WINE_GIT[@]}" remote get-url upstream >/dev/null 2>&1; then
    err "wine fork 缺少 upstream remote — 先运行 ./scripts/setup-upstream-remotes.sh 并 fetch"
fi

if [ $# -ge 1 ]; then
    BASE_COMMIT="$1"
else
    BASE_COMMIT="$("${WINE_GIT[@]}" merge-base HEAD upstream/master)"
fi
[ -n "$BASE_COMMIT" ] || err "无法确定 merge-base (上游历史未 fetch?)"

SERIES_RANGE="${BASE_COMMIT}..HEAD"
COMMIT_COUNT="$("${WINE_GIT[@]}" rev-list --count "$SERIES_RANGE")"
[ "$COMMIT_COUNT" -gt 0 ] || err "fork 相对 $BASE_COMMIT 无独有提交, 无需生成"
log "导出 $COMMIT_COUNT 个 fork 提交 ($SERIES_RANGE) → $PATCH_OUT"

mkdir -p "$PATCH_OUT"
find "$PATCH_OUT" -maxdepth 1 -type f -name '*.patch' -delete
"${WINE_GIT[@]}" format-patch --no-signature -o "$PATCH_OUT" "$SERIES_RANGE"

# format-patch 的默认命名 0001-*.patch 按提交顺序编号, 排序即应用顺序
# (build_proton.sh / CI 校验均按字典序消费, 与 vkd3d-proton 约定一致)
log "已生成 $(find "$PATCH_OUT" -maxdepth 1 -name '*.patch' | wc -l) 个补丁"
log "下一步: 提交 patches/wine-ohos/ 到仓库; CI 的 proton job 检测到系列后自动启用"
log "验证: make ENGINE=proton wine"
