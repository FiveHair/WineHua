#!/bin/bash
# WineHua Audio Test - Docker/WSL build wrapper
#
# 默认行为:
#   1. 在宿主机执行时，自动复用工程的 `winehua-dev` 容器编译
#   2. 在容器内执行时，直接调用 mingw-w64 交叉编译
#
# 用法:
#   bash Test/AudioTest/build.sh
#   WINEHUA_CONTAINER=winehua-dev bash Test/AudioTest/build.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
CONTAINER_NAME="${WINEHUA_CONTAINER:-winehua-dev}"
CONTAINER_SRC="${WINEHUA_CONTAINER_SRC:-/data/src/winehua}"
TARGET_EXE="winehua_audio_test.exe"

download_minimp3() {
    if [ -f "$SCRIPT_DIR/minimp3.h" ]; then
        return 0
    fi

    echo ">>> Downloading minimp3.h ..."
    if command -v curl >/dev/null 2>&1; then
        curl -sLo "$SCRIPT_DIR/minimp3.h" \
            "https://raw.githubusercontent.com/lieff/minimp3/master/minimp3.h"
    elif command -v wget >/dev/null 2>&1; then
        wget -qO "$SCRIPT_DIR/minimp3.h" \
            "https://raw.githubusercontent.com/lieff/minimp3/master/minimp3.h"
    else
        echo "ERROR: need curl or wget to download minimp3.h" >&2
        exit 1
    fi

    echo ">>> minimp3.h downloaded ($(wc -c < "$SCRIPT_DIR/minimp3.h") bytes)"
}

build_inside_container() {
    cd "$SCRIPT_DIR"
    download_minimp3
    if [ ! -f "$SCRIPT_DIR/test.mp3" ] && [ -f "$ROOT/thirdparty/wine/dlls/winmm/tests/test.mp3" ]; then
        cp "$ROOT/thirdparty/wine/dlls/winmm/tests/test.mp3" "$SCRIPT_DIR/test.mp3"
    fi

    echo ">>> Compiling $TARGET_EXE inside container ..."
    x86_64-w64-mingw32-gcc -O2 \
        -o "$TARGET_EXE" \
        main.c test_wasapi.c test_dsound.c test_waveout.c test_mci.c test_midi.c \
        -lcomctl32 -lwinmm -ldsound -ldxguid -lole32 -lshell32 \
        -mwindows -static \
        -Wall -Wextra

    echo ""
    echo "=== DONE ==="
    ls -lh "$TARGET_EXE"
    echo ""
    echo "Deploy:"
    echo "  hdc file send $TARGET_EXE /data/storage/el2/base/files/wine/bin/"
    echo "  hdc file send test.mp3 /data/storage/el2/base/files/wine/bin/"
    echo "  hdc shell mkdir -p /data/storage/el2/base/files/wine/bin/music"
    echo "  hdc file send <your-audio>.mp3 /data/storage/el2/base/files/wine/bin/music/"
}

if [ "${WINEHUA_AUDIO_TEST_IN_CONTAINER:-0}" = "1" ] || [ -f "/.dockerenv" ]; then
    build_inside_container
    exit 0
fi

if ! command -v docker >/dev/null 2>&1; then
    echo "ERROR: docker is required. Use the project's WSL/Docker build flow." >&2
    exit 1
fi

if ! docker ps --format '{{.Names}}' | grep -qx "$CONTAINER_NAME"; then
    cat >&2 <<EOF
ERROR: container '$CONTAINER_NAME' is not running.

Start the project dev container first:
  bash scripts/docker_wsl_build.sh dev-start
EOF
    exit 1
fi

echo ">>> Delegating AudioTest build to container '$CONTAINER_NAME' ..."
docker exec \
    -e WINEHUA_AUDIO_TEST_IN_CONTAINER=1 \
    "$CONTAINER_NAME" \
    bash -lc "cd \"$CONTAINER_SRC/Test/AudioTest\" && ./build.sh"
