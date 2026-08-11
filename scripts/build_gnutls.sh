#!/bin/bash
# build_gnutls.sh — GnuTLS 链交叉编译 → sysroot-ext (供 Wine schannel 使用)
#
# 依赖链 (全 autotools, 安装到统一 staging, 最后复制到 sysroot-ext):
#   gmp → nettle(→hogweed) → gnutls
#   libtasn1 ──────────────┘
#   libunistring ──────────┘
#
# 交叉目标 x86_64-linux-ohos (Wine unix 层), 仿 build_libffi.sh 模式:
#   CC="$CLANG --target=$TARGET --sysroot=$SYSROOT" + --host=x86_64-linux-gnu
#   (configure 只看 host 判平台特性, 实际编译 target 由 --target= 控制)
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

STAGING="$BUILD_DIR/gnutls_staging"
GNULIB_DIR="$ROOT/thirdparty/gnutls/gnulib"   # gnutls 的 gnulib submodule, 共享给 libtasn1

# 幂等跳过: 5 个库的关键产物全部就位
idempotent_done() {
    [ -f "$SYSROOT_EXT_LIB/libgnutls.so.30" ] \
        && [ -f "$SYSROOT_EXT_LIB/libnettle.so.8" ] \
        && [ -f "$SYSROOT_EXT_LIB/libhogweed.so.6" ] \
        && [ -f "$SYSROOT_EXT_LIB/libgmp.so.10" ] \
        && [ -f "$SYSROOT_EXT_LIB/libtasn1.so.6" ] \
        && [ -f "$SYSROOT_EXT_LIB/libunistring.so.5" ] \
        && [ -f "$SYSROOT_EXT_INC/gnutls/gnutls.h" ]
}

if idempotent_done; then
    log "GnuTLS 链已就绪，跳过"
    exit 0
fi

log "=== 构建 GnuTLS 链 (gmp/nettle/libtasn1/libunistring/gnutls, x86_64) → sysroot-ext ==="

mkdir -p "$SYSROOT_EXT_INC" "$SYSROOT_EXT_LIB" "$SYSROOT_EXT_PC" "$STAGING/include" "$STAGING/lib" "$STAGING/lib/pkgconfig"

CROSS_CFLAGS="-O2 -fPIC -D__MUSL__"
CROSS_LDFLAGS="-fuse-ld=lld --sysroot=$SYSROOT --target=$TARGET"
export PKG_CONFIG_PATH="$STAGING/lib/pkgconfig"
export CFLAGS="-I$STAGING/include $CROSS_CFLAGS"
export LDFLAGS="-L$STAGING/lib $CROSS_LDFLAGS"
export CC="$CLANG --target=$TARGET --sysroot=$SYSROOT"

# 收集 staging 产物到 sysroot-ext (libtool .so 保留 SONAME 符号链接)
stage_libs() {
    cp -P "$STAGING"/lib/"$1".so* "$SYSROOT_EXT_LIB/"
}
stage_headers() {
    cp -r "$STAGING"/include/"$1" "$SYSROOT_EXT_INC/"
}
stage_pc() {
    cp "$STAGING"/lib/pkgconfig/"$1" "$SYSROOT_EXT_PC/" 2>/dev/null || true
}

# configure 生成 (git 树无 configure 时才跑; bootstrap 只跑一次)
bootstrap_source() {
    local src="$1" mode="$2"
    [ -f "$src/configure" ] && return 0
    log "--- 生成 $src configure ($mode) ---"
    case "$mode" in
        nettle)
            (cd "$src" && ./.bootstrap)
            ;;
        autogen)
            (cd "$src" && GNULIB_SRCDIR="$GNULIB_DIR" ./autogen.sh)
            ;;
        gnulib)
            # --no-git: 不递归 clone/update submodule (devel/libtasn1 等无用子模块)
            (cd "$src" && ./bootstrap --skip-po --no-git --gnulib-srcdir="$GNULIB_DIR")
            ;;
    esac
}

build_one() {
    local name="$1" src="$2" bootstrap="$3"; shift 3
    local build="$BUILD_DIR/${name}_build"
    log "--- 构建 $name ---"
    bootstrap_source "$src" "$bootstrap"
    rm -rf "$build"
    mkdir -p "$build"
    cd "$build"
    CC="$CC" CFLAGS="$CFLAGS" LDFLAGS="$LDFLAGS" \
    "$src/configure" --host=x86_64-linux-gnu --prefix="$STAGING" --disable-static \
        "$@"
    # tests/fuzz 是无条件 SUBDIRS: libtasn1 的 all 目标会运行交叉 asn1Parser
    # 生成头文件 (宿主无法执行 x86_64-ohos 二进制), libunistring 的 tests 编译
    # glibc 扩展宏 (musl 无 PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP)
    # → 按库剔除测试子目录; 再 touch Makefile 防止 automake 自动 regen
    # (源码 mtime 比 build Makefile 新时 config.status 会重新生成覆盖 sed)
    case "$name" in
        libtasn1)
            sed -i 's/^SUBDIRS = .*/SUBDIRS = lib src/' "$build/Makefile"
            touch "$build/Makefile"
            ;;
        libunistring)
            sed -i 's/^SUBDIRS = .*/SUBDIRS = doc gnulib-local lib/' "$build/Makefile"
            touch "$build/Makefile"
            ;;
        gnutls)
            # ASN.1 tab 生成需要 asn1Parser (交叉二进制, 宿主不可运行)。
            # 复用同版本 (3.8.3) CrossOver release 树的预生成文件 → build 树,
            # touch 保证比 .asn 新, 防止 make 重新生成。
            mkdir -p "$build/lib"
            for t in gnutls_asn1_tab.c pkix_asn1_tab.c; do
                if [ -f "$ROOT/.temp/crossover/gnutls/gnutls/lib/$t" ] \
                   && [ ! -f "$build/lib/$t" ]; then
                    cp "$ROOT/.temp/crossover/gnutls/gnutls/lib/$t" "$build/lib/$t"
                fi
                [ -f "$build/lib/$t" ] && touch "$build/lib/$t"
            done
            # inih (第三方 INI 解析) 无 config.h include, gnulib 替换头
            # (getdelim/getline 被替换声明) 要求 config.h 先行 → 复制到
            # build 树首行插入 (automake VPATH 优先 build 树文件, 不污染 submodule)
            # ini.h 必须同放 (build 树 #include "ini.h" 相对当前文件目录解析)
            if [ -f "$src/lib/inih/ini.c" ]; then
                mkdir -p "$build/lib/inih"
                cp "$src/lib/inih/ini.c" "$build/lib/inih/ini.c"
                cp "$src/lib/inih/ini.h" "$build/lib/inih/ini.h"
                sed -i '1i #include "config.h"' "$build/lib/inih/ini.c"
                touch "$build/lib/inih/ini.c" "$build/lib/inih/ini.h"
            fi
            ;;
    esac
    make -j$JOBS
    make install
    cd "$SCRIPT_DIR"
}

# ── 1. gmp (nettle 的 bignum 后端) ──
build_one gmp "$ROOT/thirdparty/gmp" none \
    --disable-assembly --enable-cxx=no --disable-dependency-tracking
stage_libs libgmp
stage_headers gmp.h
stage_pc gmp.pc

# ── 2. libtasn1 (gnutls 的 ASN.1 解析) ──
build_one libtasn1 "$ROOT/thirdparty/libtasn1" gnulib \
    --disable-doc --disable-dependency-tracking --disable-tests
stage_libs libtasn1
stage_headers libtasn1.h
stage_pc libtasn1.pc

# ── 3. libunistring (gnutls 的字符串/IDN 依赖) ──
build_one libunistring "$ROOT/thirdparty/libunistring" autogen \
    --disable-dependency-tracking --without-libiconv-prefix
stage_libs libunistring
stage_headers unistring
stage_pc libunistring.pc

# ── 4. nettle (+hogweed, gnutls 的 crypto 后端) ──
build_one nettle "$ROOT/thirdparty/nettle" nettle \
    --disable-documentation --disable-openssl --disable-assembler \
    --disable-dependency-tracking
stage_libs libnettle
stage_libs libhogweed
stage_headers nettle
stage_pc nettle.pc
stage_pc hogweed.pc

# ── 5. gnutls (schannel 的 TLS 后端) ──
build_one gnutls "$ROOT/thirdparty/gnutls" gnulib \
    --disable-doc --disable-tools --disable-tests --disable-full-test-suite --disable-gtk-doc --disable-cxx \
    --disable-guile --disable-valgrind-tests --disable-code-coverage \
    --without-p11-kit --without-tpm --without-brotli --without-zstd \
    --without-libpsl --without-idn --without-libidn2 --without-unbound \
    --without-libxml2 --disable-dependency-tracking
stage_libs libgnutls
stage_headers gnutls
stage_pc gnutls.pc

log "GnuTLS 链就绪: $SYSROOT_EXT"
