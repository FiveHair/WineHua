#!/bin/bash
# Build cnc-ddraw (DirectDraw compatibility layer) PE overlay for WineHua.
#
# The upstream submodule (thirdparty/cnc-ddraw) stays pinned and clean; the
# build runs on an isolated copy because cnc-ddraw's Makefile generates
# inc/git.h and object files inside the source tree.
#
# Primary artifact is the 32-bit ddraw.dll. cnc-ddraw is a Win32 wrapper
# (GWL_WNDPROC); an x64 MinGW build fails, and classic DDraw games are x86.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

CNC_SRC="$ROOT/thirdparty/cnc-ddraw"
CNC_BUILD_ROOT="$BUILD_DIR/cnc-ddraw"
SOURCE_COPY="$CNC_BUILD_ROOT/source"
OUTPUT_ROOT="$CNC_BUILD_ROOT/dist"
EXPECTED_BASE_COMMIT="541b5de"
CNC_TAG="v7.1.0.0"

[ -f "$CNC_SRC/Makefile" ] || err "cnc-ddraw submodule missing: $CNC_SRC (run: git submodule update --init thirdparty/cnc-ddraw)"

if git -c safe.directory="$CNC_SRC" -C "$CNC_SRC" rev-parse HEAD >/dev/null 2>&1; then
    base_commit="$(git -c safe.directory="$CNC_SRC" -C "$CNC_SRC" rev-parse --short=7 HEAD)"
    [ -z "$(git -c safe.directory="$CNC_SRC" -C "$CNC_SRC" status --porcelain)" ] || \
        err "cnc-ddraw submodule is dirty; keep modifications out of the submodule"
else
    base_commit="unknown"
fi
case "$base_commit" in
    "$EXPECTED_BASE_COMMIT"*) ;;
    *) err "cnc-ddraw base drifted: expected $EXPECTED_BASE_COMMIT ($CNC_TAG), got $base_commit" ;;
esac

command -v i686-w64-mingw32-gcc >/dev/null 2>&1 || \
    err "i686-w64-mingw32-gcc missing (install gcc-mingw-w64-i686; the Docker buildenv already provides it)"
command -v i686-w64-mingw32-windres >/dev/null 2>&1 || \
    err "i686-w64-mingw32-windres missing (binutils-mingw-w64-i686)"

rm -rf "$SOURCE_COPY"
mkdir -p "$SOURCE_COPY" "$OUTPUT_ROOT/x86"
tar -C "$CNC_SRC" --exclude='.git' -cf - . | tar -C "$SOURCE_COPY" -xf -

log "=== Build cnc-ddraw $CNC_TAG (x86) ==="
make -C "$SOURCE_COPY" clean >/dev/null 2>&1 || true
make -C "$SOURCE_COPY" -j"$(nproc)" TARGET=ddraw.dll
[ -s "$SOURCE_COPY/ddraw.dll" ] || err "cnc-ddraw x86 build produced no ddraw.dll"
cp "$SOURCE_COPY/ddraw.dll" "$OUTPUT_ROOT/x86/ddraw.dll"
x86_sha="$(sha256sum "$OUTPUT_ROOT/x86/ddraw.dll" | awk '{print $1}')"
log "  x86 ddraw.dll sha256=$x86_sha"

x64_sha=""

# WineHua overlay defaults: force the OpenGL renderer (GDI is the Wine
# fallback and would go through wl_shm), keep windowed so smoke does not
# fight winewayland exclusive fullscreen, and skip libretro shaders that
# are not packaged with this overlay.
write_winehua_ini() {
    cat > "$1" <<'INI'
; WineHua cnc-ddraw overlay defaults. Games may override via
; CNC_DDRAW_CONFIG_FILE or a ddraw.ini next to the game executable.
[ddraw]
renderer=opengl
windowed=true
fullscreen=false
vsync=false
maxfps=0
shader=Bilinear
hook=4
no_compat_warning=true
savesettings=0
INI
}
write_winehua_ini "$OUTPUT_ROOT/x86/ddraw.ini"
[ -d "$OUTPUT_ROOT/x64" ] && write_winehua_ini "$OUTPUT_ROOT/x64/ddraw.ini"
write_winehua_ini "$OUTPUT_ROOT/ddraw.ini"
ini_sha="$(sha256sum "$OUTPUT_ROOT/ddraw.ini" | awk '{print $1}')"

x64_manifest=""
[ -n "$x64_sha" ] && x64_manifest=",
    \"x64\": {\"ddraw.dll\": \"$x64_sha\"}"
cat > "$OUTPUT_ROOT/manifest.json" <<EOF
{
  "schemaVersion": 1,
  "backend": "cnc-ddraw",
  "upstream": "https://github.com/FunkyFr3sh/cnc-ddraw",
  "tag": "$CNC_TAG",
  "commit": "$base_commit",
  "runtimeRoot": "cnc-ddraw",
  "files": {
    "ddraw.ini": "$ini_sha",
    "x86": {"ddraw.dll": "$x86_sha"}$x64_manifest
  }
}
EOF

log "cnc-ddraw dist → $OUTPUT_ROOT"
