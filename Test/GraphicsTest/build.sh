#!/bin/bash
# WineHua Graphics Test - 交叉编译脚本

set -e
cd "$(dirname "$0")"

echo ">>> Compiling winehua_graphics_test.exe ..."
x86_64-w64-mingw32-gcc -O2 \
    -I../../shared/graphics \
    -o winehua_graphics_test.exe \
    main.c \
    -lopengl32 -lgdi32 -luser32 -lkernel32 -lm \
    -mconsole -static \
    -Wall -Wextra

echo ""
echo "=== DONE ==="
ls -lh winehua_graphics_test.exe