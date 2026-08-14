#!/bin/bash
set -euo pipefail

source "$(dirname "$0")/env.sh"

SYSROOT="$OHOS_SDK/native/sysroot"
CXX="$OHOS_SDK/native/llvm/bin/clang++"
READELF="$OHOS_SDK/native/llvm/bin/llvm-readelf"
OUT="$BUILD_DIR/probes/$NATIVE_ARCH"
HOST_OUT="$BUILD_DIR/host_vulkan/$NATIVE_ARCH"
OUTPUT_LIB="$HOST_OUT/lib/libwinehua_ohos_nativebuffer_vulkan_probe.so"
OUTPUT_MARKER="$HOST_OUT/bin/winehua_ohos_nativebuffer_vulkan_probe"
BUNDLE_LIB="$ROOT/entry/libs/$NATIVE_ARCH/libwinehua_ohos_nativebuffer_vulkan_probe.so"

if [ ! -x "$CXX" ] || [ ! -d /apps/harmony ]; then
    echo "ERROR: run this build inside the WineHua Docker environment" >&2
    exit 1
fi

case "$NATIVE_TARGET" in
    aarch64-linux-ohos) SDK_LIBDIR="aarch64-linux-ohos" ;;
    x86_64-linux-ohos) SDK_LIBDIR="x86_64-linux-ohos" ;;
    *) echo "ERROR: unsupported NATIVE_TARGET=$NATIVE_TARGET" >&2; exit 1 ;;
esac

mkdir -p "$OUT" "$HOST_OUT/lib" "$HOST_OUT/bin" "$(dirname "$BUNDLE_LIB")"
"$CXX" \
    --target="$NATIVE_TARGET" \
    --sysroot="$SYSROOT" \
    -std=c++17 -O2 -g -fPIE -pie \
    -Wall -Wextra -Werror -Wno-missing-field-initializers \
    "$ROOT/smoke/winehua_ohos_nativebuffer_vulkan_probe.cpp" \
    -L"$SYSROOT/usr/lib/$SDK_LIBDIR" \
    -lvulkan -lnative_buffer -lnative_image -lnative_window -lEGL -lGLESv3 \
    -o "$OUT/winehua_ohos_nativebuffer_vulkan_probe"

"$CXX" \
    --target="$NATIVE_TARGET" \
    --sysroot="$SYSROOT" \
    -std=c++17 -O2 -g -fPIC -shared \
    -DWINEHUA_NATIVEBUFFER_PROBE_LIBRARY=1 \
    -Wall -Wextra -Werror -Wno-missing-field-initializers \
    "$ROOT/smoke/winehua_ohos_nativebuffer_vulkan_probe.cpp" \
    -L"$SYSROOT/usr/lib/$SDK_LIBDIR" \
    -lvulkan -lnative_buffer -lnative_image -lnative_window -lEGL -lGLESv3 \
    -Wl,-soname,libwinehua_ohos_nativebuffer_vulkan_probe.so \
    -o "$OUTPUT_LIB"
cp "$OUTPUT_LIB" "$BUNDLE_LIB"
printf '%s\n' 'winehua-host-module-v1' > "$OUTPUT_MARKER"

"$READELF" -h "$OUT/winehua_ohos_nativebuffer_vulkan_probe" | grep -E 'Class:|Machine:'
"$READELF" -h "$OUTPUT_LIB" | grep -E 'Class:|Machine:'
sha256sum "$OUT/winehua_ohos_nativebuffer_vulkan_probe" "$OUTPUT_LIB" "$OUTPUT_MARKER"
