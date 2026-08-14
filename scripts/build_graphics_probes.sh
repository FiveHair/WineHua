#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/probes"
CC="${CC:-x86_64-w64-mingw32-gcc}"

mkdir -p "$OUT"

"$CC" -std=c11 -O2 -Wall -Wextra -Werror \
    -o "$OUT/winehua_d3d12_persistent_upload.exe" \
    "$ROOT/smoke/winehua_d3d12_persistent_upload.c" \
    -ld3d12 -ldxgi -ldxguid -luuid -lkernel32

(
    cd "$OUT"
    sha256sum winehua_d3d12_persistent_upload.exe > manifest.sha256
)
