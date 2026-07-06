#!/usr/bin/env bash
set -euo pipefail

REPO="${REPO:-/mnt/d/Gits/WineHua}"
SDK_ZIP="${SDK_ZIP:-/mnt/d/commandline-tools-linux-x64-6.1.1.290.zip}"
HARMONY_TOOLS_HOST="${HARMONY_TOOLS_HOST:-/mnt/d/Gits/.winehua-harmony-tools}"
NATIVE_ARCH="${NATIVE_ARCH:-arm64-v8a}"
DEVICE_TYPE="${DEVICE_TYPE:-pad}"
SKIP_SUBMODULES="${SKIP_SUBMODULES:-0}"

cd "$REPO"

log() {
    printf '\n[%s] %s\n' "$(date '+%H:%M:%S')" "$*"
}

prepare_harmony_tools() {
    if [ -x "$HARMONY_TOOLS_HOST/bin/hvigorw" ] &&
       [ -d "$HARMONY_TOOLS_HOST/sdk/default/openharmony" ]; then
        repair_harmony_tool_symlinks
        log "Harmony tools already ready: $HARMONY_TOOLS_HOST"
        return 0
    fi

    [ -f "$SDK_ZIP" ] || {
        echo "error: SDK zip not found: $SDK_ZIP" >&2
        exit 1
    }

    if [ -e "$HARMONY_TOOLS_HOST" ]; then
        echo "error: $HARMONY_TOOLS_HOST exists but is incomplete" >&2
        echo "Move it aside or delete it, then rerun." >&2
        exit 1
    fi

    command -v unzip >/dev/null 2>&1 || {
        echo "error: unzip is missing in WSL" >&2
        exit 1
    }

    local tmp root
    tmp="$(mktemp -d /mnt/d/Gits/.winehua-harmony-tools.extract.XXXXXX)"
    log "Extracting Harmony tools to $tmp"
    unzip -q "$SDK_ZIP" -d "$tmp"

    root="$(find "$tmp" -type f -path '*/bin/hvigorw' -printf '%h\n' | sed 's#/bin$##' | head -n1)"
    [ -n "$root" ] || {
        echo "error: bin/hvigorw not found in SDK zip" >&2
        exit 1
    }

    mv "$root" "$HARMONY_TOOLS_HOST"
    rmdir "$tmp"/command-line-tools 2>/dev/null || true
    rmdir "$tmp" 2>/dev/null || true

    test -x "$HARMONY_TOOLS_HOST/bin/hvigorw"
    test -d "$HARMONY_TOOLS_HOST/sdk/default/openharmony"
    repair_harmony_tool_symlinks
    log "Harmony tools ready: $HARMONY_TOOLS_HOST"
}

repair_alias() {
    local dir="$1"
    local alias="$2"
    local target="$3"

    [ -d "$dir" ] || return 0
    [ -e "$dir/$target" ] || return 0
    if [ -L "$dir/$alias" ]; then
        return 0
    fi
    if [ -e "$dir/$alias" ] && [ ! -s "$dir/$alias" ]; then
        rm -f "$dir/$alias"
        ln -s "$target" "$dir/$alias"
    fi
}

repair_llvm_bin_dir() {
    local dir="$1"
    repair_alias "$dir" clang clang-15
    repair_alias "$dir" clang++ clang-15
    repair_alias "$dir" clang-cl clang-15
    repair_alias "$dir" clang-cpp clang-15
    repair_alias "$dir" bisheng-clang clang-15
    repair_alias "$dir" bisheng-clang++ clang-15
    repair_alias "$dir" ld.lld lld
    repair_alias "$dir" ld64.lld lld
    repair_alias "$dir" lld-link lld
    repair_alias "$dir" llvm-lib llvm-ar
    repair_alias "$dir" llvm-ranlib llvm-ar
    repair_alias "$dir" llvm-strip llvm-objcopy
    repair_alias "$dir" llvm-readelf llvm-readobj
    repair_alias "$dir" llvm-addr2line llvm-symbolizer
}

repair_llvm_lib_dir() {
    local dir="$1"
    repair_alias "$dir" libLLVM.so libLLVM-15.so
    repair_alias "$dir" libLLVM-15.0.4.so libLLVM-15.so
    repair_alias "$dir" libLTO.so libLTO.so.15
    repair_alias "$dir" libRemarks.so libRemarks.so.15
    repair_alias "$dir" libclang-cpp.so libclang-cpp.so.15
    repair_alias "$dir" libclang.so libclang.so.15.0.4
    repair_alias "$dir" libclang.so.15 libclang.so.15.0.4
    repair_alias "$dir" liblldb.so liblldb.so.15.0.4
    repair_alias "$dir" liblldb.so.15 liblldb.so.15.0.4
    repair_alias "$dir" liblldbIntelFeatures.so liblldbIntelFeatures.so.15
    repair_alias "$dir" libxml2.so libxml2.so.2.14.0
    repair_alias "$dir" libxml2.so.16 libxml2.so.2.14.0
}

repair_harmony_tool_symlinks() {
    repair_llvm_bin_dir "$HARMONY_TOOLS_HOST/sdk/default/openharmony/native/llvm/bin"
    repair_llvm_lib_dir "$HARMONY_TOOLS_HOST/sdk/default/openharmony/native/llvm/lib"
    repair_llvm_bin_dir "$HARMONY_TOOLS_HOST/sdk/default/hms/native/BiSheng/bin"
    repair_llvm_lib_dir "$HARMONY_TOOLS_HOST/sdk/default/hms/native/BiSheng/lib"
}

configure_proxy() {
    local host_ip proxy_url
    host_ip="$(ip route | awk '/default/ {print $3; exit}')"
    [ -n "$host_ip" ] || {
        echo "error: failed to resolve Windows host IP from WSL route" >&2
        exit 1
    }

    proxy_url="http://${host_ip}:8080"
    export http_proxy="$proxy_url"
    export https_proxy="$proxy_url"
    export HTTP_PROXY="$proxy_url"
    export HTTPS_PROXY="$proxy_url"
    export no_proxy="localhost,127.0.0.1,::1,gitee.com,*.gitee.com"
    export NO_PROXY="$no_proxy"

    mkdir -p "$HOME/.docker"
    python3 - "$proxy_url" <<'PY'
import json
import os
import sys

proxy = sys.argv[1]
path = os.path.expanduser("~/.docker/config.json")
data = {}
if os.path.exists(path):
    try:
        with open(path, "r", encoding="utf-8") as fh:
            data = json.load(fh)
    except Exception:
        data = {}
data.setdefault("proxies", {})
data["proxies"]["default"] = {
    "httpProxy": proxy,
    "httpsProxy": proxy,
    "noProxy": "localhost,127.0.0.1,::1",
}
with open(path, "w", encoding="utf-8") as fh:
    json.dump(data, fh, indent=2)
    fh.write("\n")
PY

    log "Proxy configured: $proxy_url (Gitee direct)"
}

init_submodules() {
    if [ "$SKIP_SUBMODULES" = "1" ]; then
        log "Skipping submodule initialization"
        return 0
    fi

    log "Initializing submodules"
    if bash scripts/docker_wsl_build.sh submodules; then
        return 0
    fi

    log "Submodules failed with original URLs; retrying GitHub SSH URLs via HTTPS"
    git config url.https://github.com/.insteadOf git@github.com:
    bash scripts/docker_wsl_build.sh submodules
}

run_build() {
    command -v docker >/dev/null 2>&1 || {
        echo "error: docker CLI is missing in WSL" >&2
        exit 1
    }
    docker info >/dev/null

    log "Building Docker image"
    bash scripts/docker_wsl_build.sh build-image

    log "Checking Docker build environment"
    HARMONY_TOOLS="$HARMONY_TOOLS_HOST" bash scripts/docker_wsl_build.sh check

    log "Building HAP: NATIVE_ARCH=$NATIVE_ARCH DEVICE_TYPE=$DEVICE_TYPE"
    WINEHUA_DOCKER_CACHE=volume \
    HARMONY_TOOLS="$HARMONY_TOOLS_HOST" \
    NATIVE_ARCH="$NATIVE_ARCH" \
    DEVICE_TYPE="$DEVICE_TYPE" \
    bash scripts/docker_wsl_build.sh make
}

verify_outputs() {
    log "Verifying artifacts"
    test -s dist/entry-default-signed-"$NATIVE_ARCH"-"$DEVICE_TYPE".hap
    test -s entry/build/default/outputs/default/entry-default-signed.hap
    ls -lh dist/entry-default-signed-"$NATIVE_ARCH"-"$DEVICE_TYPE".hap
    ls -lh entry/build/default/outputs/default/entry-default-signed.hap
    log "Git status"
    git status --short
}

prepare_harmony_tools
configure_proxy
init_submodules
run_build
verify_outputs
