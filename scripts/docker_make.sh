#!/bin/bash
# docker_make.sh — Host 端构建封装脚本
#
# 用途: AI 助手或开发者在 Host(WSL2) 端直接调用，代理到 Docker 容器内执行 make。
# 自动处理容器状态检测、路径转换、HAP 产物复制。
#
# 用法:
#   bash scripts/docker_make.sh [make 参数...]
#
# 示例:
#   bash scripts/docker_make.sh hap NATIVE_ARCH=arm64-v8a DEVICE_TYPE=pad
#   bash scripts/docker_make.sh wine NATIVE_ARCH=arm64-v8a DEVICE_TYPE=pad
#   bash scripts/docker_make.sh clean
#   bash scripts/docker_make.sh help
set -euo pipefail

CONTAINER_NAME="${WINEHUA_CONTAINER:-winehua-dev}"
CONTAINER_SRC="/data/src/winehua"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# 颜色
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

die() {
    echo -e "${RED}[ERROR]${NC} $*" >&2
    exit 1
}

info() {
    echo -e "${GREEN}[docker_make]${NC} $*"
}

warn() {
    echo -e "${YELLOW}[WARN]${NC} $*" >&2
}

# ── 容器状态检测 ──
check_container() {
    if ! docker ps --format '{{.Names}}' | grep -qx "$CONTAINER_NAME"; then
        cat >&2 <<EOF
${RED}[ERROR]${NC} 容器 '$CONTAINER_NAME' 未运行。

启动开发容器:
  bash scripts/docker_wsl_build.sh dev-start

或手动启动:
  docker run -d --name $CONTAINER_NAME \\
    -v /mnt/f/WineHua:$CONTAINER_SRC \\
    -v /mnt/f/command-line-tools:/apps/harmony:ro \\
    -w $CONTAINER_SRC \\
    winehua-dev bash -c 'sudo service ssh start && sleep infinity'
EOF
        exit 1
    fi
}

# ── 路径转换: 容器路径 → Host 路径 ──
# 将编译输出中的容器路径替换为 WSL2 可访问路径，方便 VS Code 点击跳转
translate_paths() {
    sed -e "s|$CONTAINER_SRC/|$ROOT/|g" \
        -e 's|/apps/harmony/|/mnt/f/command-line-tools/|g'
}

# ── 解析 make 参数中的架构和设备类型 ──
parse_make_args() {
    local arch="${NATIVE_ARCH:-arm64-v8a}"
    local device="${DEVICE_TYPE:-pad}"

    for arg in "$@"; do
        case "$arg" in
            NATIVE_ARCH=*) arch="${arg#NATIVE_ARCH=}" ;;
            DEVICE_TYPE=*) device="${arg#DEVICE_TYPE=}" ;;
        esac
    done

    # 也读取环境变量
    arch="${NATIVE_ARCH:-$arch}"
    device="${DEVICE_TYPE:-$device}"

    echo "$arch" "$device"
}

# ── 复制 HAP 到 Host ──
copy_hap() {
    local arch device
    read -r arch device <<< "$(parse_make_args "$@")"

    local hap_src="$CONTAINER_SRC/entry/build/default/outputs/default/entry-default-signed.hap"
    local hap_dst="$ROOT/dist/entry-default-signed-${arch}-${device}.hap"

    # 检查容器内 HAP 是否存在
    if docker exec "$CONTAINER_NAME" test -f "$hap_src" 2>/dev/null; then
        mkdir -p "$ROOT/dist"
        docker cp "$CONTAINER_NAME:$hap_src" "$hap_dst"
        info "HAP 已复制: dist/entry-default-signed-${arch}-${device}.hap"
        ls -lh "$hap_dst" 2>/dev/null || true
    fi
}

# ── 主入口 ──
main() {
    check_container

    if [ "$#" -eq 0 ]; then
        set -- "help"
    fi

    info "容器: $CONTAINER_NAME | 参数: $*"

    # 执行 make，输出经路径转换
    # 使用 pipefail + 临时文件方案: 先收集全部输出，再转换，保持退出码
    local tmpfile
    tmpfile="$(mktemp)"
    local rc=0

    docker exec \
        -e NATIVE_ARCH="${NATIVE_ARCH:-arm64-v8a}" \
        -e DEVICE_TYPE="${DEVICE_TYPE:-pad}" \
        -e BUILD_GUEST_GFX="${BUILD_GUEST_GFX:-0}" \
        "$CONTAINER_NAME" \
        make "$@" > "$tmpfile" 2>&1 || rc=$?

    translate_paths < "$tmpfile"
    rm -f "$tmpfile"

    if [ "$rc" -ne 0 ]; then
        echo -e "${RED}[docker_make]${NC} make 失败 (exit=$rc)" >&2
        exit "$rc"
    fi

    # 构建成功后复制 HAP (hap 目标才会生成 .hap)
    if echo "$*" | grep -qE '\bhap\b'; then
        copy_hap "$@"
    fi
}

main "$@"
