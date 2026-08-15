#!/bin/bash
# build_gstreamer.sh — GStreamer 链交叉编译 → sysroot-ext (供 Wine winegstreamer 使用)
#
# 依赖链 (pcre2/glib 用 autotools+meson, gstreamer 系用 meson):
#   pcre2 ─→ glib(2.78) ─→ gstreamer core(1.24.4) ─→ gst-plugins-base
#   zlib(OHOS sysroot)                              (出 gstreamer-video/audio/tag .pc)
#   libffi(已编)
#
# Wine configure 探测 gstreamer-1.0/video/audio/tag 四个 .pc:
#   core 出 gstreamer-1.0 / gstreamer-base-1.0, base 的 gst-libs 出 video/audio/tag。
# 全部直接装 sysroot-ext (prefix=$SYSROOT_EXT/usr, libdir=lib/x86_64-linux-ohos),
# 与 gnutls 链不同 (那套走 staging 中转, 因库间用 pkg-config 互相找)。
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

GLIB_SRC="$ROOT/thirdparty/glib"
GST_SRC="$ROOT/thirdparty/gstreamer"
PCRE2_SRC="$ROOT/thirdparty/pcre2"
# gst-plugins-base: fd.o 独立仓库停在 1.12, 1.24 只在 monorepo subproject。
# gstreamer 子模块已直接跟踪 subprojects/gst-plugins-base (1.24.4, 与 core 同源),
# 直接用它, 无需外部 .temp/crossover staging。
BASE_SRC="$ROOT/thirdparty/gstreamer/subprojects/gst-plugins-base"
GOOD_SRC="$ROOT/thirdparty/gstreamer/subprojects/gst-plugins-good"
LIBAV_SRC="$ROOT/thirdparty/gstreamer/subprojects/gst-libav"
FFMPEG_SRC="$BUILD_DIR/ffmpeg_src"

GST_PREFIX="$SYSROOT_EXT/usr"
GST_LIBDIR="$GST_PREFIX/lib/x86_64-linux-ohos"

# 幂等跳过: 4 个 .pc + 关键 .so 齐全
idempotent_done() {
    [ -f "$SYSROOT_EXT_PC/gstreamer-1.0.pc" ] \
        && [ -f "$SYSROOT_EXT_PC/gstreamer-video-1.0.pc" ] \
        && [ -f "$SYSROOT_EXT_PC/glib-2.0.pc" ] \
        && [ -f "$SYSROOT_EXT_LIB/libgstreamer-1.0.so.0" ] \
        && [ -f "$SYSROOT_EXT_LIB/libglib-2.0.so.0" ] \
        && [ -f "$SYSROOT_EXT_INC/glib.h" ]
}

log "=== 构建 GStreamer 链 (core + plugins-base/good + libav, x86_64) → sysroot-ext ==="

mkdir -p "$SYSROOT_EXT_INC" "$SYSROOT_EXT_LIB" "$SYSROOT_EXT_PC" "$BUILD_DIR"

CROSS_CFLAGS="-O2 -fPIC -D__MUSL__"
CROSS_LDFLAGS="-fuse-ld=lld --sysroot=$SYSROOT --target=$TARGET"
# pkgconfigdir = libdir/pkgconfig (x86_64-linux-ohos 子目录) — 两个路径都要
export PKG_CONFIG_PATH="$SYSROOT_EXT_PC:$GST_LIBDIR/pkgconfig:$SYSROOT/usr/lib/pkgconfig"
export CFLAGS="-I$SYSROOT_EXT_INC $CROSS_CFLAGS"
export LDFLAGS="-L$SYSROOT_EXT_LIB $CROSS_LDFLAGS"
export CC="$CLANG --target=$TARGET --sysroot=$SYSROOT"

# OHOS sysroot 有 libz 但无 .pc (glib meson dependency('zlib') 需要)
if [ ! -f "$SYSROOT_EXT_PC/zlib.pc" ]; then
    cat > "$SYSROOT_EXT_PC/zlib.pc" << EOF
prefix=$SYSROOT/usr
includedir=\${prefix}/include
libdir=\${prefix}/lib/x86_64-linux-ohos
Name: zlib
Description: zlib compression library
Version: 1.2.11
Libs: -L\${libdir} -lz
Cflags: -I\${includedir}
EOF
fi
# musl 的 libintl 是 libc 内建 stub → 提供 .pc 防 glib 触发 proxy-libintl wrap 下载
if [ ! -f "$SYSROOT_EXT_PC/libintl.pc" ]; then
    cat > "$SYSROOT_EXT_PC/libintl.pc" << EOF
prefix=$SYSROOT/usr
includedir=\${prefix}/include
libdir=\${prefix}/lib/x86_64-linux-ohos
Name: libintl
Description: GNU gettext (musl stub)
Version: 0.22
Libs: -L\${libdir} -lc
Cflags: -I\${includedir}
EOF
fi

# 每库 install 后 .pc 复制到 $SYSROOT_EXT_PC (wine configure 只搜那里)
stage_pcs() {
    cp "$GST_LIBDIR"/pkgconfig/*.pc "$SYSROOT_EXT_PC/" 2>/dev/null || true
}

# ── 1. pcre2 (glib 正则后端, autotools) ──
if [ ! -f "$SYSROOT_EXT_LIB/libpcre2-8.so.0" ]; then
    log "--- 构建 pcre2 ---"
    # git 树无 configure, 首次跑 autogen.sh (autotools 全套)
    [ -f "$PCRE2_SRC/configure" ] || (cd "$PCRE2_SRC" && ./autogen.sh)
    build="$BUILD_DIR/pcre2_build"
    rm -rf "$build" && mkdir -p "$build" && cd "$build"
    CC="$CC" CFLAGS="$CFLAGS" LDFLAGS="$LDFLAGS" \
    "$PCRE2_SRC/configure" --host=x86_64-linux-gnu --prefix="$GST_PREFIX" \
        --libdir="$GST_LIBDIR" --disable-static --disable-pcre2grep --disable-pcre2test \
        --enable-jit=no
    make -j$JOBS
    make install
    cd "$SCRIPT_DIR"
    stage_pcs
else
    log "pcre2 已就绪，跳过"
fi

# ── 2. glib 2.78 (meson) ──
if [ ! -f "$SYSROOT_EXT_LIB/libglib-2.0.so.0" ]; then
    log "--- 构建 glib ---"
    # gnulib works 检测: cross file 的 needs_exe_wrapper=true 使
    # meson.can_run_host_binaries() 返回 false, 检测自动走 else 分支
    # (works=true, 不依赖 cc.run), 无需 sed 源码。
    # -Werror=format=2 含 format-security: G_DBUS_ERROR 宏的 format 参数非字面量
    # 触发 (gdebugcontrollerdbus.c) → 以 patch 方式追加 -Wno-error 豁免。
    # 改动仅 2 行, 不值得提交 submodule (开分支/push/指针更新), patch 随构建走。
    PATCH="$SCRIPT_DIR/patches/glib-format-security.patch"
    if ! git -C "$GLIB_SRC" apply --reverse --check "$PATCH" 2>/dev/null; then
        git -C "$GLIB_SRC" apply "$PATCH"
        log "  已应用 patch: $(basename "$PATCH")"
    fi
    build="$BUILD_DIR/glib_build"
    rm -rf "$build"
    # --wrap-mode=nodownload: 防 wrap 下载 (pcre2/zlib/libffi 已由 .pc 提供, 不触发)
    meson setup "$build" "$GLIB_SRC" --cross-file "$(gen_cross_file)" \
        --prefix="$GST_PREFIX" -Dlibdir=lib/x86_64-linux-ohos --wrap-mode=nodownload \
        -Dc_args="--target=$TARGET --sysroot=$SYSROOT -I$SYSROOT_EXT_INC -D__MUSL__" \
        -Dselinux=disabled -Dxattr=false -Dlibmount=disabled -Dman=false \
        -Ddtrace=false -Dsystemtap=false -Dgtk_doc=false -Dtests=false \
        -Dinstalled_tests=false -Dlibelf=disabled \
        -Dnls=disabled
    meson compile -C "$build" -j "$JOBS"
    DESTDIR=/ meson install -C "$build"
    stage_pcs
else
    log "glib 已就绪，跳过"
fi

# ── 3. gstreamer core 1.24.4 (meson, 只需 core 本身) ──
if [ ! -f "$SYSROOT_EXT_LIB/libgstreamer-1.0.so.0" ]; then
    log "--- 构建 gstreamer core ---"
    build="$BUILD_DIR/gstreamer_build"
    rm -rf "$build"
    meson setup "$build" "$GST_SRC" --cross-file "$(gen_cross_file)" \
        --prefix="$GST_PREFIX" -Dlibdir=lib/x86_64-linux-ohos --wrap-mode=nodownload \
        -Dc_args="--target=$TARGET --sysroot=$SYSROOT -I$SYSROOT_EXT_INC -D__MUSL__" \
        -Dtests=disabled -Dexamples=disabled -Dtools=disabled \
        -Dintrospection=disabled -Ddoc=disabled -Dgtk_doc=disabled -Dorc=disabled \
        -Dbase=disabled -Dgood=disabled -Dbad=disabled -Dugly=disabled -Dlibav=disabled
    meson compile -C "$build" -j "$JOBS"
    DESTDIR=/ meson install -C "$build"
    stage_pcs
else
    log "gstreamer core 已就绪，跳过"
fi

# ── 4. gst-plugins-base 1.24.4 (meson, monorepo subproject) ──
# 既出 gst-libs 的 .pc (wine configure 探测用), 也编基础插件
# (typefind/playback/app/audioconvert 等, winegstreamer 运行时必需)。
if [ ! -f "$SYSROOT_EXT_PC/gstreamer-video-1.0.pc" ] || \
   [ ! -d "$GST_LIBDIR/gstreamer-1.0" ]; then
    log "--- 构建 gst-plugins-base ---"
    build="$BUILD_DIR/gst_base_build"
    rm -rf "$build"
    meson setup "$build" "$BASE_SRC" --cross-file "$(gen_cross_file)" \
        --prefix="$GST_PREFIX" -Dlibdir=lib/x86_64-linux-ohos --wrap-mode=nodownload \
        -Dc_args="--target=$TARGET --sysroot=$SYSROOT -I$SYSROOT_EXT_INC -D__MUSL__" \
        -Dtests=disabled -Dexamples=disabled -Dintrospection=disabled -Ddoc=disabled \
        -Dorc=disabled -Dnls=disabled \
        -Dcompositor=disabled -Ddebugutils=disabled -Dencoding=disabled \
        -Doverlaycomposition=disabled -Ddrm=disabled -Dgl=disabled -Dalsa=disabled \
        -Dcdparanoia=disabled -Dlibvisual=disabled -Dogg=disabled -Dopus=disabled \
        -Dpango=disabled -Dtheora=disabled -Dtremor=disabled -Dvorbis=disabled \
        -Dxshm=disabled -Dxi=disabled
    meson compile -C "$build" -j "$JOBS"
    DESTDIR=/ meson install -C "$build"
    stage_pcs
else
    log "gst-plugins-base 已就绪，跳过"
fi

# include 平铺 symlink: wine configure 的 gint64 检查 (AC_COMPILE_IFELSE) 只用
# 全局 CFLAGS (-I$SYSROOT_EXT_INC), 不含 pkg-config 的 -Iglib-2.0/-Igstreamer-1.0
for f in "$SYSROOT_EXT_INC"/glib-2.0/*; do
    b="$(basename "$f")"
    [ -e "$SYSROOT_EXT_INC/$b" ] || ln -sfn "glib-2.0/$b" "$SYSROOT_EXT_INC/$b"
done
# glibconfig.h 是构建产物头, meson 按 libdir 装 (lib/x86_64-linux-ohos/glib-2.0/include/)
ln -sfn ../lib/x86_64-linux-ohos/glib-2.0/include/glibconfig.h "$SYSROOT_EXT_INC/glibconfig.h"
ln -sfn gstreamer-1.0/gst "$SYSROOT_EXT_INC/gst"

# wine 直接 -l 链接 4 个包 (动态链接不看 Libs.private) → Libs 补全依赖链,
# 否则 winegstreamer.so 链接报 g_*/g_object_* undefined
for pc in gstreamer-1.0 gstreamer-video-1.0 gstreamer-audio-1.0 gstreamer-tag-1.0; do
    f="$SYSROOT_EXT_PC/$pc.pc"
    grep -q "lgstbase-1.0" "$f" || \
        sed -i "/^Libs:/ s/\$/ -lgstbase-1.0 -lgstpbutils-1.0 -lglib-2.0 -lgobject-2.0 -lgmodule-2.0 -lgio-2.0/" "$f"
done

# ── 5. gst-plugins-good (demuxer: qtdemux/matroskademux/typefind 等) ──
if [ ! -d "$GST_LIBDIR/gstreamer-1.0" ] || \
   [ ! -f "$GST_LIBDIR/gstreamer-1.0/libgstqtdemux.so" ]; then
    log "--- 构建 gst-plugins-good ---"
    build="$BUILD_DIR/gst_good_build"
    rm -rf "$build"
    # zlib 来自 OHOS sysroot; vpx 插件已由 libvpx 提供。禁用外部音频/图像依赖。
    meson setup "$build" "$GOOD_SRC" --cross-file "$(gen_cross_file)" \
        --prefix="$GST_PREFIX" -Dlibdir=lib/x86_64-linux-ohos --wrap-mode=nodownload \
        -Dc_args="--target=$TARGET --sysroot=$SYSROOT -I$SYSROOT_EXT_INC -D__MUSL__" \
        -Dcairo=disabled -Ddv=disabled -Dflac=disabled -Djack=disabled \
        -Djpeg=disabled -Dlame=disabled -Dlibcaca=disabled -Dmpg123=disabled \
        -Dpulse=disabled -Dshout2=disabled -Dspeex=disabled -Dtaglib=disabled \
        -Dtwolame=disabled -Dvpx=disabled -Dwavpack=disabled -Daalib=disabled \
        -Damrnb=disabled -Damrwbdec=disabled -Ddv1394=disabled -Dgtk3=disabled \
        -Doss=disabled -Doss4=disabled -Dosxaudio=disabled -Dosxvideo=disabled \
        -Dqt5=disabled -Dqt6=disabled -Drpicamsrc=disabled -Dsoup=disabled \
        -Dv4l2=disabled -Dximagesrc=disabled -Ddirectsound=disabled
    meson compile -C "$build" -j "$JOBS"
    DESTDIR=/ meson install -C "$build"
    stage_pcs
else
    log "gst-plugins-good 已就绪，跳过"
fi

# ── 6. FFmpeg + gst-libav (通用解码: H.264/VP9/AAC 等) ──
if [ ! -f "$SYSROOT_EXT_LIB/libavcodec.so" ] || \
   [ ! -f "$GST_LIBDIR/gstreamer-1.0/libgstlibav.so" ]; then
    log "--- 构建 FFmpeg (meson-ports) ---"
    if [ ! -d "$FFMPEG_SRC/.git" ]; then
        if [ ! -f "$FFMPEG_SRC/meson.build" ]; then
            rm -rf "$FFMPEG_SRC"
            if [ -f "$ROOT/build/ffmpeg-meson.tar.gz" ]; then
                mkdir -p "$FFMPEG_SRC"
                tar -xzf "$ROOT/build/ffmpeg-meson.tar.gz" -C "$FFMPEG_SRC" --strip-components=1
            else
                git clone --depth 1 --branch meson-6.1 \
                    https://gitlab.freedesktop.org/gstreamer/meson-ports/ffmpeg.git "$FFMPEG_SRC" \
                    || { echo "FFmpeg clone 失败, 跳过 libav"; FFMPEG_SRC=""; }
            fi
        fi
    fi
    if [ -n "${FFMPEG_SRC:-}" ] && [ -f "$FFMPEG_SRC/meson.build" ]; then
        build="$BUILD_DIR/ffmpeg_build"
        rm -rf "$build"
        meson setup "$build" "$FFMPEG_SRC" --cross-file "$(gen_cross_file)" \
            --prefix="$GST_PREFIX" -Dlibdir=lib/x86_64-linux-ohos \
            -Ddefault_library=shared \
            -Dc_args="--target=$TARGET --sysroot=$SYSROOT -I$SYSROOT_EXT_INC -D__MUSL__" \
            -Dc_link_args="--target=$TARGET --sysroot=$SYSROOT -L$SYSROOT_EXT_LIB -lz" \
            -Dprograms=disabled -Dtests=disabled \
            -Dzlib=enabled -Diconv=disabled \
            -Dlibx264=disabled -Dlibvpx=disabled -Dlibopus=disabled
        meson compile -C "$build" -j "$JOBS"
        DESTDIR=/ meson install -C "$build"
        stage_pcs
    fi
    if [ -n "${FFMPEG_SRC:-}" ] && [ -f "$FFMPEG_SRC/meson.build" ] && \
       [ -f "$SYSROOT_EXT_LIB/libavcodec.so" ]; then
        log "--- 构建 gst-libav ---"
        build="$BUILD_DIR/gst_libav_build"
        rm -rf "$build"
        meson setup "$build" "$LIBAV_SRC" --cross-file "$(gen_cross_file)" \
            --prefix="$GST_PREFIX" -Dlibdir=lib/x86_64-linux-ohos --wrap-mode=nodownload \
            -Dc_args="--target=$TARGET --sysroot=$SYSROOT -I$SYSROOT_EXT_INC -D__MUSL__" \
            -Dtests=disabled -Ddoc=disabled
        meson compile -C "$build" -j "$JOBS"
        DESTDIR=/ meson install -C "$build"
        stage_pcs
    fi
else
    log "gst-libav 已就绪，跳过"
fi

log "GStreamer 链就绪: $SYSROOT_EXT"
