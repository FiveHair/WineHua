#!/bin/bash
set -euo pipefail

source "$(dirname "$0")/env.sh"

SYSROOT="$OHOS_SDK/native/sysroot"
CXX="$OHOS_SDK/native/llvm/bin/clang++"
READELF="$OHOS_SDK/native/llvm/bin/llvm-readelf"
OUT="$BUILD_DIR/probes/$NATIVE_ARCH"

if [ ! -x "$CXX" ] || [ ! -d /apps/harmony ]; then
    echo "ERROR: run this build inside the WineHua Docker environment" >&2
    exit 1
fi

case "$NATIVE_TARGET" in
    aarch64-linux-ohos) SDK_LIBDIR="aarch64-linux-ohos" ;;
    x86_64-linux-ohos) SDK_LIBDIR="x86_64-linux-ohos" ;;
    *) echo "ERROR: unsupported NATIVE_TARGET=$NATIVE_TARGET" >&2; exit 1 ;;
esac

mkdir -p "$OUT"
"$CXX" \
    --target="$NATIVE_TARGET" \
    --sysroot="$SYSROOT" \
    -std=c++17 -O2 -g -fPIE -pie \
    -Wall -Wextra -Werror -Wno-missing-field-initializers \
    "$ROOT/smoke/winehua_ohos_nativebuffer_vulkan_probe.cpp" \
    -L"$SYSROOT/usr/lib/$SDK_LIBDIR" \
    -lvulkan -lnative_buffer \
    -o "$OUT/winehua_ohos_nativebuffer_vulkan_probe"

"$READELF" -h "$OUT/winehua_ohos_nativebuffer_vulkan_probe" | grep -E 'Class:|Machine:'
sha256sum "$OUT/winehua_ohos_nativebuffer_vulkan_probe"
