#!/bin/bash
# WineHua Audio Test - 交叉编译脚本
#
# 环境要求:
#   x86_64-w64-mingw32-gcc  (Linux / WSL)
#
# 用法:  bash build.sh
# 产物:  winehua_audio_test.exe

set -e
cd "$(dirname "$0")"

# --- 下载 minimp3 (single-header, public domain) ---
if [ ! -f minimp3.h ]; then
    echo ">>> Downloading minimp3.h ..."
    if command -v curl &>/dev/null; then
        curl -sLo minimp3.h \
            "https://raw.githubusercontent.com/lieff/minimp3/master/minimp3.h"
    elif command -v wget &>/dev/null; then
        wget -qO minimp3.h \
            "https://raw.githubusercontent.com/lieff/minimp3/master/minimp3.h"
    else
        echo "ERROR: need curl or wget to download minimp3.h" >&2
        exit 1
    fi
    echo ">>> minimp3.h downloaded ($(wc -c < minimp3.h) bytes)"
fi

# --- 编译 ---
echo ">>> Compiling winehua_audio_test.exe ..."
x86_64-w64-mingw32-gcc -O2 \
    -o winehua_audio_test.exe \
    main.c \
    -lcomctl32 -lwinmm \
    -mwindows -static \
    -Wall -Wextra

echo ""
echo "=== DONE ==="
ls -lh winehua_audio_test.exe
echo ""
echo "Deploy:"
echo "  hdc file send winehua_audio_test.exe /data/storage/el2/base/files/wine/bin/"
echo "  hdc shell mkdir -p /data/storage/el2/base/files/wine/bin/music"
echo "  hdc file send <your-audio>.mp3 /data/storage/el2/base/files/wine/bin/music/"
