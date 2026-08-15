#!/bin/bash
# build_fex.sh — 构建 FEX 模拟器 DLL (arm64 原生 wine 转译 x86_64 应用)
#
#   libarm64ecfex.dll : x86_64 模拟 (v1 必需) — arm64ec ABI, 由 Wine 的
#                       load_arm64ec_module() 在 ARM64EC/WoW64 层内加载
#   (libwow64fex.dll : i386 模拟, 后置; v1 仅 x64)
#
# 产物: build/fex-ec/Bin/libarm64ecfex.dll (assemble.sh 负责归位到 aarch64-windows/)
# 前置: LLVM_MINGW (llvm-mingw, 需 LLVM ≥ 18 支持 arm64ec) + thirdparty/fex
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

# 仅 arm64 原生 wine 需要 FEX (x86_64 模拟器同目标, 不需要转译)
[ "$WINE_ARCH" = "aarch64" ] || { log "FEX 仅 arm64 原生需要 (WINE_ARCH=$WINE_ARCH)，跳过"; exit 0; }

FEX_SRC="$ROOT/thirdparty/fex"
OUT_DIR="$BUILD_DIR/fex-ec/Bin"

test -d "$FEX_SRC" || err "FEX 源码缺失: $FEX_SRC (git submodule update --init)"
test -f "$FEX_SRC/Data/CMake/toolchain_mingw.cmake" || err "FEX toolchain_mingw.cmake 缺失"
test -x "$LLVM_MINGW/bin/arm64ec-w64-mingw32-clang" || err "llvm-mingw 缺失 arm64ec 支持: $LLVM_MINGW (需 LLVM ≥ 18)"

export PATH="$LLVM_MINGW/bin:$PATH"

# ---- libarm64ecfex.dll (x86_64 模拟, arm64ec ABI) ----
build_fex_ec() {
    local build="$BUILD_DIR/fex-ec"
    mkdir -p "$build"
    cd "$build"
    if [ ! -f CMakeCache.txt ]; then
        cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo \
            -DCMAKE_TOOLCHAIN_FILE="$FEX_SRC/Data/CMake/toolchain_mingw.cmake" \
            -DENABLE_LTO=False \
            -DMINGW_TRIPLE=arm64ec-w64-mingw32 \
            -DBUILD_TESTS=False \
            "$FEX_SRC"
    fi
    make -j"$JOBS" arm64ecfex

    local dll="$OUT_DIR/libarm64ecfex.dll"
    test -f "$dll" || err "arm64ecfex 构建失败: $dll 不存在"
    # arm64ec 验证: file 显示 "x86-64" 是 magic 误识别, 以 llvm-readobj 为准
    local readobj="$LLVM_MINGW/bin/llvm-readobj"
    if "$readobj" --file-headers "$dll" 2>/dev/null | grep -q "COFF-ARM64EC"; then
        log "OK: libarm64ecfex.dll 为 arm64ec PE"
    else
        warn "架构异常: $("$readobj" --file-headers "$dll" 2>/dev/null | grep -m1 'Format:')"
    fi
    log "产物: $dll (assemble.sh 归位到 aarch64-windows/)"
}

build_fex_ec
log "FEX 构建完成"
