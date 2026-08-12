#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/probes"
CC="${CC:-x86_64-w64-mingw32-gcc}"
OHOS_SDK="${OHOS_SDK:-/apps/harmony/sdk/default/openharmony}"
VULKAN_INCLUDE="${VULKAN_INCLUDE:-$OHOS_SDK/native/sysroot/usr/include}"
VULKAN_IMPORT="${VULKAN_IMPORT:-$ROOT/build/wine-ohos/dlls/vulkan-1/x86_64-windows/libvulkan-1.a}"
VERT_SPV="$ROOT/smoke/pso_storm.vert.spv"
FRAG_SPV="$ROOT/smoke/pso_storm.frag.spv"
VERT_SHA256="d3ba9114bdbc621fb8eed259accc2a8bb7142697d366888d044ad6fc20343276"
FRAG_SHA256="dc8f7b00840a341380ebb3aa875f2f1a1b57dd9b04ed85d0349899b91b67dac2"

mkdir -p "$OUT"

"$CC" -std=c11 -O2 -Wall -Wextra -Werror \
    -o "$OUT/winehua_d3d12_persistent_upload.exe" \
    "$ROOT/smoke/winehua_d3d12_persistent_upload.c" \
    -ld3d12 -ldxgi -ldxguid -luuid -lkernel32

printf '%s  %s\n' "$VERT_SHA256" "$VERT_SPV" | sha256sum -c -
printf '%s  %s\n' "$FRAG_SHA256" "$FRAG_SPV" | sha256sum -c -
cp "$VERT_SPV" "$OUT/pso_storm.vert.spv"
cp "$FRAG_SPV" "$OUT/pso_storm.frag.spv"

test -s "$VULKAN_IMPORT" || {
    echo "Missing Wine Vulkan import library: $VULKAN_IMPORT" >&2
    exit 1
}
test -s "$VULKAN_INCLUDE/vulkan/vulkan.h" || {
    echo "Missing Vulkan headers: $VULKAN_INCLUDE/vulkan/vulkan.h" >&2
    exit 1
}
"$CC" -std=c11 -O2 -Wall -Wextra -Werror \
    -idirafter "$VULKAN_INCLUDE" \
    -o "$OUT/winehua_vulkan_pso_storm.exe" \
    "$ROOT/smoke/winehua_vulkan_pso_storm.c" \
    "$VULKAN_IMPORT" -lkernel32

(
    cd "$OUT"
    sha256sum winehua_d3d12_persistent_upload.exe \
        winehua_vulkan_pso_storm.exe \
        pso_storm.vert.spv pso_storm.frag.spv > manifest.sha256
)
