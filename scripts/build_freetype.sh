#!/bin/bash
# build_freetype.sh — FreeType 交叉编译 → sysroot-ext
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

FT_SRC="$ROOT/thirdparty/freetype"
FT_BUILD="$BUILD_DIR/freetype_build"

log "=== 构建 FreeType (x86_64) ==="

if [ -f "$SYSROOT_EXT_LIB/libfreetype.so.6" ] \
   && [ -f "$SYSROOT_EXT_LIB/libfreetype.so" ] \
   && [ -d "$SYSROOT_EXT_INC/freetype2" ] \
   && [ -f "$SYSROOT_EXT_PC/freetype2.pc" ]; then
    log "FreeType 已就绪，跳过"
    exit 0
fi

rm -rf "$FT_BUILD"

mkdir -p "$SYSROOT_EXT_INC" "$SYSROOT_EXT_LIB" "$SYSROOT_EXT_PC"
mkdir -p "$FT_BUILD"
cd "$FT_BUILD"

cmake "$FT_SRC" \
    -GNinja \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR=x86_64 \
    -DCMAKE_C_COMPILER="$CLANG" \
    -DCMAKE_C_COMPILER_TARGET="$TARGET" \
    -DCMAKE_SYSROOT="$SYSROOT" \
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
    -DCMAKE_C_FLAGS="--target=$TARGET --sysroot=$SYSROOT -fPIC -D__MUSL__" \
    -DCMAKE_SHARED_LINKER_FLAGS="--target=$TARGET --sysroot=$SYSROOT -fuse-ld=lld" \
    -DCMAKE_EXE_LINKER_FLAGS="--target=$TARGET --sysroot=$SYSROOT -fuse-ld=lld" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON \
    -DFT_DISABLE_ZLIB=ON \
    -DFT_DISABLE_BROTLI=ON \
    -DFT_DISABLE_HARFBUZZ=ON \
    -DFT_DISABLE_PNG=ON \
    -DFT_DISABLE_BZIP2=ON \
    -DBUILD_SHARED_LIBS=ON \
    -DCMAKE_INSTALL_PREFIX="$FT_BUILD/install"

ninja

# 安装到 sysroot-ext (文件名 = SONAME)
[ -f "$FT_BUILD/libfreetype.so.6.20.2" ] || err "FreeType 产物缺失: $FT_BUILD/libfreetype.so.6.20.2"
cp "$FT_BUILD/libfreetype.so.6.20.2" "$SYSROOT_EXT_LIB/libfreetype.so.6"
ln -sf libfreetype.so.6 "$SYSROOT_EXT_LIB/libfreetype.so"
rm -rf "$SYSROOT_EXT_INC/freetype2"
mkdir -p "$SYSROOT_EXT_INC/freetype2"
cp "$FT_SRC/include/ft2build.h" "$SYSROOT_EXT_INC/freetype2/"
cp -r "$FT_SRC/include/freetype" "$SYSROOT_EXT_INC/freetype2/"
cat > "$SYSROOT_EXT_PC/freetype2.pc" << EOF
prefix=$SYSROOT_EXT/usr
includedir=\${prefix}/include
libdir=\${prefix}/lib/x86_64-linux-ohos

Name: FreeType 2
Description: A free, high-quality, and portable font engine.
Version: 2.13.3
Libs: -L\${libdir} -lfreetype
Cflags: -I\${includedir}/freetype2
EOF

log "FreeType → sysroot-ext ($SYSROOT_EXT_LIB/libfreetype.so.6)"
