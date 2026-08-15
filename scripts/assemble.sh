#!/bin/bash
# assemble.sh — 组装 HAP 打包布局
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

# ============================================================
# 文件分流到 libs/ + rawfile/
# ============================================================
assemble_pad() {
    log "=== 组装布局 ($NATIVE_ARCH) ==="

    local wine_data="$STAGING_DIR/wine-data"
    local guest_arch="${GUEST_ARCH:-x86_64}"
    # Wine PE 目录 (arm64 原生: aarch64-windows 合并 arm64ec; x86_64: x86_64-windows)
    local wine_pe_dir="x86_64-windows"
    local pe_src_dirs="$wine_pe_dir"
    local smoke_src_dir="x86_64-windows"
    if [ "$WINE_ARCH" = "aarch64" ]; then
        wine_pe_dir="aarch64-windows"
        pe_src_dirs="aarch64-windows arm64ec-windows"
        # system DLL/exe 为 ARM64EC+ARM64 混合, 统一在 aarch64-windows;
        # arm64ec-windows 仅产 .o (ABI 编译验证) → smoke 也取 aarch64-windows
        smoke_src_dir="aarch64-windows"
    fi
    rm -rf "$STAGING_DIR"
    rm -rf "$wine_data"
    mkdir -p "$wine_data/bin/$wine_pe_dir"
    mkdir -p "$wine_data/bin/$WINE_ARCH-unix"
    mkdir -p "$wine_data/share/wine/nls"
    mkdir -p "$wine_data/share/wine/fonts"
    mkdir -p "$wine_data/share/wine/winmd"
    mkdir -p "$wine_data/share/wine/mono"
    mkdir -p "$wine_data/share/X11"

    # SoundFont (MIDI 音色库)
    local soundfont="$WINEHUA/entry/src/main/resources/rawfile/winehua-gm.sf2"
    if [ -f "$soundfont" ]; then
        mkdir -p "$wine_data/audio"
        cp "$soundfont" "$wine_data/audio/winehua-gm.sf2"
        log "    winehua-gm.sf2 → rawfile audio/"
    else
        warn "winehua-gm.sf2 not found; MIDI output will be unavailable"
    fi

    # -- 1. 原生 .so → libs/$NATIVE_ARCH/ (由各 build 脚本完成) --
    mkdir -p "$NATIVE_LIBS"

    if [ "$NATIVE_ARCH" = "x86_64" ]; then
        # x86_64 Pad: Wine .so 是原生架构, 直接放 libs/
        log "  → Wine .so → libs/x86_64/"

        # 所有 Wine Unix .so → libs/x86_64/ (系统 linker 通过文件名搜索)
        for so in "$BUILD_DIR/wine-ohos/dlls/"*/*.so; do
            cp "$so" "$NATIVE_LIBS/"
        done
        log "    Wine .so: $(ls "$BUILD_DIR/wine-ohos/dlls/"*/*.so 2>/dev/null | wc -l) files"

        # 交叉编译依赖 → libs/x86_64/
        # (系统 linker 自动搜索此路径, 无需 x86_64-unix 子目录)
        _pick_lib_pad() {
            local name="$1" soname="$2" linker="${3:-}"
            local dest="$NATIVE_LIBS"
            if [ -f "$SYSROOT_EXT_LIB/$soname" ]; then
                cp "$SYSROOT_EXT_LIB/$soname" "$dest/$soname"
            elif [ -f "$SYSROOT/usr/lib/$TARGET/$name" ]; then
                cp "$SYSROOT/usr/lib/$TARGET/$name" "$dest/$soname"
            else
                warn "$soname 未找到"
                return 0
            fi
            if [ -n "$linker" ] && [ ! -f "$dest/$linker" ]; then
                cp "$dest/$soname" "$dest/$linker"
            fi
        }
        _pick_lib_pad "libfreetype.so.6.20.2"       "libfreetype.so.6"   "libfreetype.so"
        _pick_lib_pad "libz.so"                      "libz.so"
        _pick_lib_pad "libwayland-client.so.0.22.0"  "libwayland-client.so.0"
        _pick_lib_pad "libwayland-egl.so.1.22.0"     "libwayland-egl.so.1"
        _pick_lib_pad "libxkbcommon.so.0.0.0"        "libxkbcommon.so.0"
        _pick_lib_pad "libxkbregistry.so.0.0.0"      "libxkbregistry.so.0"
        _pick_lib_pad "libxml2.so.2.12.0"            "libxml2.so.2"
        _pick_lib_pad "libffi.so.8.1.4"              "libffi.so.8"
        # GnuTLS 链 (schannel TLS 后端)
        _pick_lib_pad "libgnutls.so.30.37.1"         "libgnutls.so.30"   "libgnutls.so"
        _pick_lib_pad "libnettle.so.8.11"            "libnettle.so.8"
        _pick_lib_pad "libhogweed.so.6.11"           "libhogweed.so.6"
        _pick_lib_pad "libgmp.so.10.4.1"             "libgmp.so.10"
        _pick_lib_pad "libtasn1.so.6.6.4"            "libtasn1.so.6"
        _pick_lib_pad "libunistring.so.5.2.0"        "libunistring.so.5"
        _pick_lib_pad "libm.so"                      "libm.so"
        # GStreamer 链 (winegstreamer 后端)
        for so in libglib-2.0.so.0 libgobject-2.0.so.0 libgmodule-2.0.so.0 libgio-2.0.so.0 \
                  libgthread-2.0.so.0 libpcre2-8.so.0 libintl.so.8 libintl.so libm.so \
                  libgstreamer-1.0.so.0 libgstbase-1.0.so.0 libgstcontroller-1.0.so.0 \
                  libgstnet-1.0.so.0 libgstvideo-1.0.so.0 libgstaudio-1.0.so.0 \
                  libgsttag-1.0.so.0 libgstpbutils-1.0.so.0 libgstallocators-1.0.so.0 \
                  libgstapp-1.0.so.0 libgstfft-1.0.so.0 libgstriff-1.0.so.0 \
                  libgstrtp-1.0.so.0 libgstrtsp-1.0.so.0 libgstsdp-1.0.so.0; do
            _pick_lib_pad "$so" "$so"
        done
        log "    交叉编译依赖 → libs/x86_64/"

        # libc.so → libs/x86_64/
        cp "$SYSROOT/usr/lib/$TARGET/libc.so" "$NATIVE_LIBS/"

        # libfreetype 已由 _pick_lib_pad 放入 libs/x86_64/，系统 linker 可直接找到

        # libwineserver.so (Pad fork+dlopen 入口)
        if [ -f "$BUILD_DIR/wine_server/libwineserver.so" ]; then
            cp "$BUILD_DIR/wine_server/libwineserver.so" "$NATIVE_LIBS/"
            log "    libwineserver.so → libs/x86_64/"
        else
            warn "libwineserver.so 未找到！请先执行: bash scripts/build_wine.sh"
        fi
    else
        # arm64 原生 wine: Wine aarch64 .so 直接放 libs/ (与 x86_64 同构, 非 box64 整体模拟)
        log "  → Wine aarch64 .so → libs/$NATIVE_ARCH/"

        # 所有 Wine Unix .so → libs/ (系统 linker 通过文件名搜索)
        for so in "$BUILD_DIR/wine-ohos/dlls/"*/*.so; do
            cp "$so" "$NATIVE_LIBS/"
        done
        log "    Wine .so: $(ls "$BUILD_DIR/wine-ohos/dlls/"*/*.so 2>/dev/null | wc -l) files"

        # 交叉编译依赖 → libs/
        _pick_lib_pad() {
            local name="$1" soname="$2" linker="${3:-}"
            local dest="$NATIVE_LIBS"
            if [ -f "$SYSROOT_EXT_LIB/$soname" ]; then
                cp "$SYSROOT_EXT_LIB/$soname" "$dest/$soname"
            elif [ -f "$SYSROOT/usr/lib/$TARGET/$name" ]; then
                cp "$SYSROOT/usr/lib/$TARGET/$name" "$dest/$soname"
            else
                warn "$soname 未找到"
                return 0
            fi
            if [ -n "$linker" ] && [ ! -f "$dest/$linker" ]; then
                cp "$dest/$soname" "$dest/$linker"
            fi
        }
        _pick_lib_pad "libfreetype.so.6.20.2"       "libfreetype.so.6"   "libfreetype.so"
        _pick_lib_pad "libz.so"                      "libz.so"
        _pick_lib_pad "libwayland-client.so.0.22.0"  "libwayland-client.so.0"
        _pick_lib_pad "libwayland-egl.so.1.22.0"     "libwayland-egl.so.1"
        _pick_lib_pad "libxkbcommon.so.0.0.0"        "libxkbcommon.so.0"
        _pick_lib_pad "libxkbregistry.so.0.0.0"      "libxkbregistry.so.0"
        _pick_lib_pad "libxml2.so.2.12.0"            "libxml2.so.2"
        _pick_lib_pad "libffi.so.8.1.4"              "libffi.so.8"
        # GnuTLS 链 (schannel TLS 后端)
        _pick_lib_pad "libgnutls.so.30.37.1"         "libgnutls.so.30"   "libgnutls.so"
        _pick_lib_pad "libnettle.so.8.11"            "libnettle.so.8"
        _pick_lib_pad "libhogweed.so.6.11"           "libhogweed.so.6"
        _pick_lib_pad "libgmp.so.10.4.1"             "libgmp.so.10"
        _pick_lib_pad "libtasn1.so.6.6.4"            "libtasn1.so.6"
        _pick_lib_pad "libunistring.so.5.2.0"        "libunistring.so.5"
        _pick_lib_pad "libm.so"                      "libm.so"
        # GStreamer 链 (winegstreamer 后端)
        for so in libglib-2.0.so.0 libgobject-2.0.so.0 libgmodule-2.0.so.0 libgio-2.0.so.0 \
                  libgthread-2.0.so.0 libpcre2-8.so.0 libintl.so.8 libintl.so libm.so \
                  libgstreamer-1.0.so.0 libgstbase-1.0.so.0 libgstcontroller-1.0.so.0 \
                  libgstnet-1.0.so.0 libgstvideo-1.0.so.0 libgstaudio-1.0.so.0 \
                  libgsttag-1.0.so.0 libgstpbutils-1.0.so.0 libgstallocators-1.0.so.0 \
                  libgstapp-1.0.so.0 libgstfft-1.0.so.0 libgstriff-1.0.so.0 \
                  libgstrtp-1.0.so.0 libgstrtsp-1.0.so.0 libgstsdp-1.0.so.0; do
            _pick_lib_pad "$so" "$so"
        done
        log "    交叉编译依赖 → libs/$NATIVE_ARCH/"

        # libc.so → libs/
        cp "$SYSROOT/usr/lib/$TARGET/libc.so" "$NATIVE_LIBS/"

        # libwineserver.so (dlopen 入口)
        if [ -f "$BUILD_DIR/wine_server/libwineserver.so" ]; then
            cp "$BUILD_DIR/wine_server/libwineserver.so" "$NATIVE_LIBS/"
            log "    libwineserver.so → libs/$NATIVE_ARCH/"
        else
            warn "libwineserver.so 未找到！请先执行: bash scripts/build_wine.sh"
        fi

        # 注: 不需要 wine/wine-preloader ELF。winehua 部署以 wine 构建出的
        # ntdll.so 作为 wine 启动 (wine_child.cpp dlopen ntdll.so → __wine_main);
        # OHOS 下 spawn_process 走 ohos_broker_spawn_child (StartNativeChildProcess),
        # 不走 exec_wineloader (ntdll/unix/process.c #ifdef __OHOS__ 分支)。

        # FEX libarm64ecfex.dll → PE 目录 (arm64 原生转译 x64 应用)
        if [ -f "$BUILD_DIR/fex-ec/Bin/libarm64ecfex.dll" ]; then
            mkdir -p "$wine_data/bin/$wine_pe_dir"
            cp "$BUILD_DIR/fex-ec/Bin/libarm64ecfex.dll" "$wine_data/bin/$wine_pe_dir/"
            log "    libarm64ecfex.dll → rawfile $wine_pe_dir/"
        else
            warn "libarm64ecfex.dll 未找到！请先执行: bash scripts/build_fex.sh"
        fi
    fi

    # -- 2. PE DLL + 数据文件 → rawfile (两种架构共用) --
    # x86_64-windows/ — 复制所有运行时 PE 文件
    # .cpl (含 appwiz.cpl) 是否打包由 BUILD_WINE_MONO 决定:
    #   =1 (本地默认): 打包 cpl + build_deps 下载 mono msi, 保留 .NET/控制面板.
    #   =0 (CI 无 curl): 不打包 cpl. 若 mono 缺失, wineboot 初始化时 mscoree.dll
    #   会 CreateProcess "control.exe appwiz.cpl install_mono" 弹 DialogBoxW 模态框,
    #   OHOS 无头环境无人响应 → wineboot 永久阻塞 (mscoree WaitForSingleObject 无限
    #   等待). 去掉 appwiz.cpl 后 control.exe 加载 cpl 失败立即退出, mscoree 走
    #   "无 .NET 运行时"路径不卡死.
    local pe_exts="dll drv exe sys acm ax ocx tlb"
    if [ "${BUILD_WINE_MONO:-1}" = "1" ]; then
        pe_exts="$pe_exts cpl"
    fi
    for ext in $pe_exts; do
        for pe_src in $pe_src_dirs; do
            for f in "$BUILD_DIR/wine-ohos/dlls/"*/$pe_src/*.$ext; do
                [ -f "$f" ] && cp "$f" "$wine_data/bin/$wine_pe_dir/"
            done
        done
    done
    log "  $wine_pe_dir → $(ls "$wine_data/bin/$wine_pe_dir" | wc -l) files"

    # strip PE 调试符号 (DWARF .debug_*, 缩减 ~50%)
    log "  stripping debug symbols..."
    local pe_strip="x86_64-w64-mingw32-strip"
    [ "$WINE_ARCH" = "aarch64" ] && pe_strip="$LLVM_MINGW/bin/aarch64-w64-mingw32-strip"
    if command -v "$pe_strip" &>/dev/null || [ -x "$pe_strip" ]; then
        for f in "$wine_data/bin/$wine_pe_dir/"*.dll "$wine_data/bin/$wine_pe_dir/"*.drv "$wine_data/bin/$wine_pe_dir/"*.exe "$wine_data/bin/$wine_pe_dir/"*.sys; do
            [ -f "$f" ] && "$pe_strip" "$f" 2>/dev/null
        done
        log "  $wine_pe_dir PE stripped"
    else
        warn "  $pe_strip not found, skipping strip"
    fi
    # i386-windows/ (32-bit PE DLL for WoW64)
    # 主构建 --enable-archs=i386,x86_64 已产出全部 32-bit PE, 直接取自 wine-ohos,
    # 无需独立的 i686-mingw32 构建.
    # 注意: wineboot/rpcss/services/conhost 等服务程序只有 x86_64 版,
    # WoW64 下它们由 Wine 以 64 位进程拉起, 属上游 WoW64 的正常行为.
    mkdir -p "$wine_data/bin/i386-windows"
    # 与 x86_64 一致: cpl 仅当 BUILD_WINE_MONO=1 时打包 (见上方 pe_exts 注释)
    for ext in $pe_exts; do
        for f in "$BUILD_DIR/wine-ohos/dlls/"*/i386-windows/*.$ext; do
            [ -f "$f" ] && cp "$f" "$wine_data/bin/i386-windows/"
        done
    done
    log "  i386-windows → $(ls "$wine_data/bin/i386-windows" | wc -l) files (ALL)"

    # 32-bit exe stubs, 放在 bin/i386-windows/.
    # Wine 通过 WINEARCH 或 exe header 判断 32/64, 自动加载对应 DLL.
    for exe in "$BUILD_DIR/wine-ohos/programs/"*/i386-windows/*.exe; do
        [ -f "$exe" ] && cp "$exe" "$wine_data/bin/i386-windows/"
    done
    log "  i386 exe stubs → $(ls "$wine_data/bin/i386-windows"/*.exe 2>/dev/null | wc -l) files"

    # 32-bit PE strip (必须在 copy 之后)
    if command -v i686-w64-mingw32-strip &>/dev/null; then
        for f in "$wine_data/bin/i386-windows/"*.dll "$wine_data/bin/i386-windows/"*.drv "$wine_data/bin/i386-windows/"*.exe "$wine_data/bin/i386-windows/"*.sys; do
            [ -f "$f" ] && i686-w64-mingw32-strip "$f" 2>/dev/null
        done
        log "  32-bit PE stripped"
    else
        warn "  i686-w64-mingw32-strip not found, skipping strip"
    fi

    # *.exe stubs → rawfile
    # 注意: arm64 下 pe_src_dirs 含 arm64ec-windows, 该架构只产 .o 无 .exe
    # (system DLL 为 ARM64EC+ARM64 混合, 统一在 aarch64-windows) → 需 -f 保护
    for pe_src in $pe_src_dirs; do
        for exe in "$BUILD_DIR/wine-ohos/programs/"*/$pe_src/*.exe; do
            [ -f "$exe" ] && cp -f "$exe" "$wine_data/bin/"
        done
    done
    # graphics smoke test (OHOS 交叉编译产物, 不在 build-native/)
    for pe_src in $pe_src_dirs; do
        if [ -f "$BUILD_DIR/wine-ohos/programs/winehua_graphics_smoke/$pe_src/winehua_graphics_smoke.exe" ]; then
            cp "$BUILD_DIR/wine-ohos/programs/winehua_graphics_smoke/$pe_src/winehua_graphics_smoke.exe" "$wine_data/bin/$wine_pe_dir/"
            log "  winehua_graphics_smoke.exe → $wine_pe_dir/"
        fi
    done

    # Versioned, App-managed C:\smoke payload.  Keep it separate from Wine's
    # DLL search directories so a prefix refresh can update tests without
    # touching user files or relying on Explorer.
    local smoke_dir="$wine_data/smoke"
    mkdir -p "$smoke_dir/x64" "$smoke_dir/x86" "$smoke_dir/assets"
    local cube_source="$WINEHUA/smoke/winehua_d3d_switch_cube.c"
    x86_64-w64-mingw32-gcc -O2 -s -mwindows -o \
        "$smoke_dir/x64/winehua_d3d_switch_cube.exe" "$cube_source" \
        -ld3d9 -ld3d11 -ldxgi -ld3dcompiler -luuid -lshell32 -luser32 -lgdi32 -lm
    i686-w64-mingw32-gcc -O2 -s -mwindows -o \
        "$smoke_dir/x86/winehua_d3d_switch_cube.exe" "$cube_source" \
        -ld3d9 -ld3d11 -ldxgi -ld3dcompiler -luuid -lshell32 -luser32 -lgdi32 -lm
    local d3d8_source="$WINEHUA/smoke/winehua_d3d8_smoke.c"
    x86_64-w64-mingw32-gcc -O2 -s -mwindows -o \
        "$smoke_dir/x64/winehua_d3d8_smoke.exe" "$d3d8_source" \
        -luser32 -lgdi32
    i686-w64-mingw32-gcc -O2 -s -mwindows -o \
        "$smoke_dir/x86/winehua_d3d8_smoke.exe" "$d3d8_source" \
        -luser32 -lgdi32
    local win32_driver_source="$WINEHUA/smoke/winehua_win32_driver.c"
    x86_64-w64-mingw32-gcc -O2 -s -municode -mwindows -o \
        "$smoke_dir/x64/winehua_win32_driver.exe" "$win32_driver_source" \
        -lshell32 -luser32
    i686-w64-mingw32-gcc -O2 -s -municode -mwindows -o \
        "$smoke_dir/x86/winehua_win32_driver.exe" "$win32_driver_source" \
        -lshell32 -luser32
    # venus shader assets: 随 guest_vulkan bundle 打包 (aarch64/x86_64 源驱动; 无 bundle 则跳过)
    local guest_shader_root="$BUILD_DIR/guest_vulkan/$guest_arch/share/winehua"
    if [ -d "$guest_shader_root" ]; then
        local smoke_shader
        for smoke_shader in venus_storage_write venus_storage_read venus_image_fetch venus_combined_sample venus_separated_sample; do
            [ -f "$guest_shader_root/$smoke_shader.spv" ] || err "Wine Vulkan sampled-image shader missing: $guest_shader_root/$smoke_shader.spv"
            cp "$guest_shader_root/$smoke_shader.spv" "$smoke_dir/assets/$smoke_shader.spv"
        done
    fi
    local dxvk_root="$DXVK_BUILD_ROOT"
    [ -f "$dxvk_root/x64/bin/d3d11.dll" ] || err "DXVK Legacy x64 d3d11.dll missing: $dxvk_root/x64/bin/d3d11.dll"
    [ -f "$dxvk_root/x64/bin/dxgi.dll" ] || err "DXVK Legacy x64 dxgi.dll missing: $dxvk_root/x64/bin/dxgi.dll"
    [ -f "$dxvk_root/x86/bin/d3d11.dll" ] || err "DXVK Legacy x86 d3d11.dll missing: $dxvk_root/x86/bin/d3d11.dll"
    [ -f "$dxvk_root/x86/bin/dxgi.dll" ] || err "DXVK Legacy x86 dxgi.dll missing: $dxvk_root/x86/bin/dxgi.dll"
    mkdir -p "$wine_data/dxvk/legacy/x64" "$wine_data/dxvk/legacy/x86"
    cp "$dxvk_root/x64/bin/d3d11.dll" "$wine_data/dxvk/legacy/x64/d3d11.dll"
    cp "$dxvk_root/x64/bin/dxgi.dll" "$wine_data/dxvk/legacy/x64/dxgi.dll"
    cp "$dxvk_root/x86/bin/d3d11.dll" "$wine_data/dxvk/legacy/x86/d3d11.dll"
    cp "$dxvk_root/x86/bin/dxgi.dll" "$wine_data/dxvk/legacy/x86/dxgi.dll"
    # The DXVK binaries are runtime-owned overlays.  Do not place them next
    # to the smoke executables: that would make the test layout look like a
    # game distribution and would force real games to carry WineHua-specific
    # DLLs.  SpawnWineProgram exposes this versioned directory through
    # WINEDLLPATH only for a selected dxvk_* backend.
    local smoke_program
    for smoke_program in winehua_audio_smoke winehua_graphics_smoke winehua_vulkan_smoke winehua_d3d11_smoke; do
        local smoke64="$BUILD_DIR/wine-ohos/programs/$smoke_program/$smoke_src_dir/$smoke_program.exe"
        local smoke32="$BUILD_DIR/wine-i386-pe/programs/$smoke_program/i386-windows/$smoke_program.exe"
        if [ ! -f "$smoke32" ]; then
            smoke32="$BUILD_DIR/wine-ohos/programs/$smoke_program/i386-windows/$smoke_program.exe"
        fi
        [ -f "$smoke64" ] || err "managed smoke x64 artifact missing: $smoke64"
        [ -f "$smoke32" ] || err "managed smoke x86 artifact missing: $smoke32"
        cp "$smoke64" "$smoke_dir/x64/$smoke_program.exe"
        cp "$smoke32" "$smoke_dir/x86/$smoke_program.exe"
    done
    local audio64_sha graphics64_sha vulkan64_sha d3d1164_sha d3d864_sha cube64_sha driver64_sha
    local audio32_sha graphics32_sha vulkan32_sha d3d1132_sha d3d832_sha cube32_sha driver32_sha
    local storage_write_sha storage_read_sha image_fetch_sha combined_sample_sha separated_sample_sha
    audio64_sha="$(sha256sum "$smoke_dir/x64/winehua_audio_smoke.exe" | awk '{print $1}')"
    graphics64_sha="$(sha256sum "$smoke_dir/x64/winehua_graphics_smoke.exe" | awk '{print $1}')"
    vulkan64_sha="$(sha256sum "$smoke_dir/x64/winehua_vulkan_smoke.exe" | awk '{print $1}')"
    d3d1164_sha="$(sha256sum "$smoke_dir/x64/winehua_d3d11_smoke.exe" | awk '{print $1}')"
    d3d864_sha="$(sha256sum "$smoke_dir/x64/winehua_d3d8_smoke.exe" | awk '{print $1}')"
    cube64_sha="$(sha256sum "$smoke_dir/x64/winehua_d3d_switch_cube.exe" | awk '{print $1}')"
    driver64_sha="$(sha256sum "$smoke_dir/x64/winehua_win32_driver.exe" | awk '{print $1}')"
    audio32_sha="$(sha256sum "$smoke_dir/x86/winehua_audio_smoke.exe" | awk '{print $1}')"
    graphics32_sha="$(sha256sum "$smoke_dir/x86/winehua_graphics_smoke.exe" | awk '{print $1}')"
    vulkan32_sha="$(sha256sum "$smoke_dir/x86/winehua_vulkan_smoke.exe" | awk '{print $1}')"
    d3d1132_sha="$(sha256sum "$smoke_dir/x86/winehua_d3d11_smoke.exe" | awk '{print $1}')"
    d3d832_sha="$(sha256sum "$smoke_dir/x86/winehua_d3d8_smoke.exe" | awk '{print $1}')"
    cube32_sha="$(sha256sum "$smoke_dir/x86/winehua_d3d_switch_cube.exe" | awk '{print $1}')"
    driver32_sha="$(sha256sum "$smoke_dir/x86/winehua_win32_driver.exe" | awk '{print $1}')"
    # venus shader 资产: 随 guest_vulkan bundle 打包 (源驱动)
    if [ -f "$smoke_dir/assets/venus_storage_write.spv" ]; then
        storage_write_sha="$(sha256sum "$smoke_dir/assets/venus_storage_write.spv" | awk '{print $1}')"
        storage_read_sha="$(sha256sum "$smoke_dir/assets/venus_storage_read.spv" | awk '{print $1}')"
        image_fetch_sha="$(sha256sum "$smoke_dir/assets/venus_image_fetch.spv" | awk '{print $1}')"
        combined_sample_sha="$(sha256sum "$smoke_dir/assets/venus_combined_sample.spv" | awk '{print $1}')"
        separated_sample_sha="$(sha256sum "$smoke_dir/assets/venus_separated_sample.spv" | awk '{print $1}')"
    else
        storage_write_sha="" storage_read_sha="" image_fetch_sha="" combined_sample_sha="" separated_sample_sha=""
    fi
    local dxvk_commit
    dxvk_commit="$(git -c safe.directory="$DXVK_SRC" -C "$DXVK_SRC" rev-parse HEAD 2>/dev/null || echo unknown)"
    local dxvk64_d3d11_sha dxvk64_dxgi_sha dxvk32_d3d11_sha dxvk32_dxgi_sha
    dxvk64_d3d11_sha="$(sha256sum "$wine_data/dxvk/legacy/x64/d3d11.dll" | awk '{print $1}')"
    dxvk64_dxgi_sha="$(sha256sum "$wine_data/dxvk/legacy/x64/dxgi.dll" | awk '{print $1}')"
    dxvk32_d3d11_sha="$(sha256sum "$wine_data/dxvk/legacy/x86/d3d11.dll" | awk '{print $1}')"
    dxvk32_dxgi_sha="$(sha256sum "$wine_data/dxvk/legacy/x86/dxgi.dll" | awk '{print $1}')"
    cat > "$wine_data/dxvk/manifest.json" <<EOF
{
  "schemaVersion": 1,
  "backend": "dxvk",
  "profile": "legacy",
  "runtimeRoot": "dxvk",
  "version": "1.10.3",
  "commit": "$dxvk_commit",
  "requiredCapabilities": {
    "vulkanApi": "1.1",
    "bcFormats": false,
    "descriptorIndexing": false
  },
  "runtimes": {
    "x64": {"d3d11.dll": "$dxvk64_d3d11_sha", "dxgi.dll": "$dxvk64_dxgi_sha"},
    "x86": {"d3d11.dll": "$dxvk32_d3d11_sha", "dxgi.dll": "$dxvk32_dxgi_sha"}
  }
}
EOF
    cat > "$smoke_dir/manifest.json" <<EOF
{
  "schemaVersion": 1,
  "suiteVersion": "phase2-vulkan-dxvk-legacy-v6-d3d8-d3d9",
  "enabledSuites": ["core", "audio", "opengl", "wine-vulkan", "d3d8", "d3d9", "dxvk"],
  "managedRoot": "C:\\\\smoke",
  "files": {
    "x64/winehua_audio_smoke.exe": "$audio64_sha",
    "x64/winehua_graphics_smoke.exe": "$graphics64_sha",
    "x64/winehua_vulkan_smoke.exe": "$vulkan64_sha",
    "x64/winehua_d3d11_smoke.exe": "$d3d1164_sha",
    "x64/winehua_d3d8_smoke.exe": "$d3d864_sha",
    "x64/winehua_d3d_switch_cube.exe": "$cube64_sha",
    "x64/winehua_win32_driver.exe": "$driver64_sha",
    "x86/winehua_audio_smoke.exe": "$audio32_sha",
    "x86/winehua_graphics_smoke.exe": "$graphics32_sha",
    "x86/winehua_vulkan_smoke.exe": "$vulkan32_sha",
    "x86/winehua_d3d11_smoke.exe": "$d3d1132_sha",
    "x86/winehua_d3d8_smoke.exe": "$d3d832_sha",
    "x86/winehua_d3d_switch_cube.exe": "$cube32_sha",
    "x86/winehua_win32_driver.exe": "$driver32_sha",
    "assets/venus_storage_write.spv": "$storage_write_sha",
    "assets/venus_storage_read.spv": "$storage_read_sha",
    "assets/venus_image_fetch.spv": "$image_fetch_sha",
    "assets/venus_combined_sample.spv": "$combined_sample_sha",
    "assets/venus_separated_sample.spv": "$separated_sample_sha"
  }
}
EOF
    log "  managed smoke payload → smoke/{x64,x86}"

    # fonts
    cp "$WINE_SRC/fonts/"*.ttf "$wine_data/share/wine/fonts/"
    # NLS
    cp "$BUILD_DIR/wine-ohos/nls/"*.nls "$wine_data/share/wine/nls/"
    # winmd
    cp "$BUILD_DIR/wine-ohos/include/"*.winmd "$wine_data/share/wine/winmd/"
    # Wine Mono (.NET 运行时)
    if ls "$BUILD_DIR/wine-ohos/share/wine/mono/"*.msi >/dev/null 2>&1; then
        cp "$BUILD_DIR/wine-ohos/share/wine/mono/"*.msi "$wine_data/share/wine/mono/"
        log "    wine-mono.msi → rawfile share/wine/mono/"
    fi
    # wine.inf (含 OHOS font substitutes)
    cp "$BUILD_DIR/wine-ohos/loader/wine.inf" "$wine_data/share/wine/"
    sed_i '/^\[MCI\]$/i\
;; OHOS font substitutes\
HKLM,%FontSubStr%,"System",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"MS Sans Serif",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"MS Shell Dlg",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"MS Shell Dlg 2",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Arial",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Arial Black",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Calibri",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Cambria",,"Noto Serif"\
HKLM,%FontSubStr%,"Candara",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Comic Sans MS",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Constantia",,"Noto Serif"\
HKLM,%FontSubStr%,"Corbel",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Impact",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Palatino Linotype",,"Noto Serif"\
HKLM,%FontSubStr%,"Segoe UI",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Tahoma",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Trebuchet MS",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Verdana",,"HarmonyOS Sans SC"\
;; Latin: 衬线 (serif)\
HKLM,%FontSubStr%,"Georgia",,"Noto Serif"\
HKLM,%FontSubStr%,"Times New Roman",,"Noto Serif"\
;; CJK: 简体中文\
HKLM,%FontSubStr%,"Microsoft JhengHei",,"HarmonyOS Sans TC"\
HKLM,%FontSubStr%,"Microsoft JhengHei UI",,"HarmonyOS Sans TC"\
HKLM,%FontSubStr%,"Microsoft YaHei",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Microsoft YaHei UI",,"HarmonyOS Sans SC"\
;; CJK: 宋体/楷体 (serif)\
HKLM,%FontSubStr%,"SimSun",,"Noto Serif CJK SC"\
HKLM,%FontSubStr%,"NSimSun",,"Noto Serif CJK SC"\
HKLM,%FontSubStr%,"SimHei",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"FangSong",,"Noto Serif CJK SC"\
HKLM,%FontSubStr%,"KaiTi",,"Noto Serif CJK SC"\
HKLM,%FontSubStr%,"YouYuan",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"LiSu",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"DengXian",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"STSong",,"Noto Serif CJK SC"\
HKLM,%FontSubStr%,"STKaiti",,"Noto Serif CJK SC"\
HKLM,%FontSubStr%,"STFangsong",,"Noto Serif CJK SC"\
HKLM,%FontSubStr%,"STHeiti",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"STXihei",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"STLiti",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"STXingkai",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"STXinwei",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"STHupo",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"STCaiyun",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"STZhongSong",,"Noto Serif CJK SC"\
HKLM,%FontSubStr%,"STBaoli",,"Noto Serif CJK SC"\
HKLM,%FontSubStr%,"FZShuTi",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"FZYaoti",,"HarmonyOS Sans SC"\
;; CJK: 繁体中文\
HKLM,%FontSubStr%,"MingLiU",,"HarmonyOS Sans TC"\
HKLM,%FontSubStr%,"PMingLiU",,"HarmonyOS Sans TC"\
HKLM,%FontSubStr%,"DFKai-SB",,"Noto Serif CJK TC"\
HKLM,%FontSubStr%,"Consolas",,"Noto Sans Mono"\
HKLM,%FontSubStr%,"Courier",,"Noto Sans Mono"\
HKLM,%FontSubStr%,"Courier New",,"Noto Sans Mono"\
HKLM,%FontSubStr%,"Fixedsys",,"Noto Sans Mono"\
HKLM,%FontSubStr%,"Lucida Console",,"Noto Sans Mono"' "$wine_data/share/wine/wine.inf"
    # XKB
    if [ -d "$SYSROOT_EXT_SHARE/X11/xkb" ]; then
        cp -r "$SYSROOT_EXT_SHARE/X11/xkb" "$wine_data/share/X11/"
    fi

    # guest GPU 库 (Mesa/VirGL, 供 GraphicsBroker 注入到 Wine LD_LIBRARY_PATH)
    if [ -d "$BUILD_DIR/guest_gfx/$guest_arch/lib" ]; then
        mkdir -p "$wine_data/bin/guest_gfx"
        cp -a "$BUILD_DIR/guest_gfx/$guest_arch/"* "$wine_data/bin/guest_gfx/"
        log "  guest_gfx ($guest_arch): $(ls "$wine_data/bin/guest_gfx/lib"/*.so* 2>/dev/null | wc -l) .so files"
    else
        if [ "${BUILD_GUEST_GFX:-0}" = "1" ]; then
            err "BUILD_GUEST_GFX=1 but build/guest_gfx/$guest_arch/lib is missing"
        fi
        log "  guest_gfx: SKIP (build/guest_gfx/$guest_arch/lib not found)"
    fi

    # guest Mesa 库必须可被系统 dlopen (el1 bundle libs); el2 数据区 dlopen 被拒 (ENOENT).
    # libwayland-*/libffi/libz 复用 el1 已有 host 版本; libc++_shared 设备 el1/系统都没有,
    # 但 guest libgallium (C++) 动态依赖它, 必须随 guest 一起复制到 el1, 否则 guest libEGL
    # dlopen 失败 → Wine 内 OpenGL 初始化失败 (ChoosePixelFormat 失败). aarch64/x86_64 都执行.
    if [ -d "$BUILD_DIR/guest_gfx/$guest_arch/lib" ]; then
        log "  guest_gfx -> entry/libs/$NATIVE_ARCH (el1 dlopen)"
        mkdir -p "$ROOT/entry/libs/$NATIVE_ARCH"
        for pattern in libEGL.so libGLESv2.so libGLESv1_CM.so libgallium-*.so libdrm.so libc++_shared.so; do
            for f in "$BUILD_DIR/guest_gfx/$guest_arch/lib"/$pattern*; do
                [ -f "$f" ] && cp -a "$f" "$ROOT/entry/libs/$NATIVE_ARCH/"
            done
        done
        for f in "$BUILD_DIR/guest_gfx/$guest_arch/lib"/dri/*.so; do
            [ -f "$f" ] && cp -a "$f" "$ROOT/entry/libs/$NATIVE_ARCH/"
        done
        log "  guest_gfx el1: $(ls "$ROOT/entry/libs/$NATIVE_ARCH"/libEGL.so* "$ROOT/entry/libs/$NATIVE_ARCH"/libgallium-*.so 2>/dev/null | wc -l) libs + $(ls "$ROOT/entry/libs/$NATIVE_ARCH"/*_dri.so 2>/dev/null | wc -l) dri drivers"
    fi

    # Guest Linux Vulkan runtime is intentionally outside C:\smoke: Loader + Venus
    # ICD + offscreen smoke, 按 GUEST_ARCH (aarch64/x86_64) 打包.
    if [ -f "$BUILD_DIR/guest_vulkan/$guest_arch/manifest.json" ]; then
        mkdir -p "$wine_data/bin/guest_vulkan"
        cp -a "$BUILD_DIR/guest_vulkan/$guest_arch/"* "$wine_data/bin/guest_vulkan/"
        log "  guest_vulkan ($guest_arch): Loader + Venus ICD + offscreen smoke"
    elif [ "${BUILD_GUEST_VULKAN:-0}" = "1" ]; then
        err "BUILD_GUEST_VULKAN=1 but build/guest_vulkan/$guest_arch/manifest.json is missing"
    else
        log "  guest_vulkan: SKIP"
    fi

    # guest_vulkan 关键 dlopen 库 → el1 bundle (loader + venus ICD + smoke 程序).
    # arm64 必须: el2 data 区 dlopen 被拒 (ENOENT); x86_64 一并复制以支持 NCP dlopen smoke.
    if [ -d "$BUILD_DIR/guest_vulkan/$guest_arch/lib" ]; then
        log "  guest_vulkan -> entry/libs/$NATIVE_ARCH (el1 dlopen)"
        mkdir -p "$ROOT/entry/libs/$NATIVE_ARCH"
        # guest Vulkan Loader (libvulkan.so.1) 放 el1 顶层: guest smoke 进程的
        # DT_NEEDED libvulkan.so.1 靠 LD_LIBRARY_PATH(el1 顶层) 名字搜索命中.
        # host vkr 不再担心遮蔽 — vkr_library.c 已改为绝对路径
        # dlopen("/system/lib64/libvulkan.so") 加载宿主系统 Vulkan (Maleoon 935),
        # 不走名字搜索, 顶层 guest loader 不会遮蔽 host 驱动. libvirglrenderer.so.1
        # 的 NEEDED 只有 libepoxy/libc (纯 dlopen vulkan), 无 NEEDED 绑定问题.
        # 注意只清理旧布局残留: 不能碰 libvulkan_virtio.so (venus ICD, 放顶层).
        rm -f "$ROOT/entry/libs/$NATIVE_ARCH"/libvulkan.so \
              "$ROOT/entry/libs/$NATIVE_ARCH"/libvulkan.so.1
        rm -rf "$ROOT/entry/libs/$NATIVE_ARCH/guest-vulkan-loader"
        for f in "$BUILD_DIR/guest_vulkan/$guest_arch"/lib/libvulkan.so.1 \
                 "$BUILD_DIR/guest_vulkan/$guest_arch"/lib/libvulkan.so; do
            [ -f "$f" ] && cp -a "$f" "$ROOT/entry/libs/$NATIVE_ARCH/"
        done
        for f in "$BUILD_DIR/guest_vulkan/$guest_arch"/lib/libvulkan_virtio.so \
                 "$BUILD_DIR/guest_vulkan/$guest_arch"/bin/libwinehua_guest_vulkan_smoke.so \
                 "$BUILD_DIR/guest_vulkan/$guest_arch"/bin/libvenus_sampled_image_probe.so \
                 "$BUILD_DIR/guest_vulkan/$guest_arch"/bin/libvenus_spirv_replay.so \
                 "$BUILD_DIR/guest_vulkan/$guest_arch"/bin/libvenus_heaven_material_replay.so; do
            [ -f "$f" ] && cp -a "$f" "$ROOT/entry/libs/$NATIVE_ARCH/"
        done
        log "  guest_vulkan el1: $(ls "$ROOT/entry/libs/$NATIVE_ARCH"/libvulkan.so* "$ROOT/entry/libs/$NATIVE_ARCH"/libvulkan_virtio.so 2>/dev/null | wc -l) vulkan libs (loader 顶层)"
    fi

    # Native offscreen replay runs in the App/NCP security domain and links the
    # system Host Vulkan loader. Captured resources remain in guest_vulkan so
    # there is one authoritative exact-replay input set for the Host/Venus A/B.
    local host_vulkan_root="$BUILD_DIR/host_vulkan/$NATIVE_ARCH"
    [ -f "$host_vulkan_root/manifest.json" ] || \
        err "Host Vulkan replay manifest missing: $host_vulkan_root/manifest.json"
    [ -f "$host_vulkan_root/bin/heaven_exact_host_replay" ] || \
        err "Host Vulkan replay marker missing: $host_vulkan_root/bin/heaven_exact_host_replay"
    [ -f "$host_vulkan_root/lib/libwinehua_host_heaven_replay.so" ] || \
        err "Host Vulkan replay module missing: $host_vulkan_root/lib/libwinehua_host_heaven_replay.so"
    mkdir -p "$wine_data/bin/host_vulkan"
    cp -a "$host_vulkan_root/"* "$wine_data/bin/host_vulkan/"
    log "  host_vulkan ($NATIVE_ARCH): native exact replay"

    # -- 3. 打包 zip → rawfile (不带 wine-data/ 前缀) --
    local rawfile_dir="$WINEHUA/entry/src/main/resources/rawfile"
    mkdir -p "$rawfile_dir"
    local zip_name="wine-data.zip"
    cd "$wine_data"
    rm -f "$STAGING_DIR/$zip_name"
    zip -r "$STAGING_DIR/$zip_name" . -x '*.git*'
    cp "$STAGING_DIR/$zip_name" "$rawfile_dir/"
    local payload_sha
    payload_sha="$(sha256sum "$rawfile_dir/$zip_name" | awk '{print $1}')"
    cat > "$rawfile_dir/wine-runtime-manifest.json" <<EOF
{
  "schemaVersion": 1,
  "payload": "wine-data.zip",
  "payloadSha256": "$payload_sha",
  "smokeSuiteVersion": "phase2-vulkan-b3-v1"
}
EOF
    log "  $zip_name → rawfile/ ($(du -h "$rawfile_dir/$zip_name" | cut -f1))"

    log "Pad 布局组装完成 ($NATIVE_ARCH)"
    echo ""
    echo "  libs/$NATIVE_ARCH/"
    ls -la "$NATIVE_LIBS/" 2>/dev/null || echo "    (empty)"
    echo "  rawfile/$zip_name"
}

log "=== 组装布局 ($NATIVE_ARCH) ==="

# 统一使用 rawfile zip 布局
assemble_pad
