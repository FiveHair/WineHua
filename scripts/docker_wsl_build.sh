#!/bin/bash
# WSL/Docker entrypoint for WineHua builds.
set -euo pipefail

IMAGE_NAME="${WINEHUA_DOCKER_IMAGE:-winehua-dev}"
CONTAINER_NAME="${WINEHUA_CONTAINER:-winehua-dev}"
CONTAINER_SRC="${WINEHUA_CONTAINER_SRC:-/data/src/winehua}"
CACHE_MODE="${WINEHUA_DOCKER_CACHE:-bind}"
DEFAULT_ARCH="${NATIVE_ARCH:-arm64-v8a}"

# ── 自动检测 HARMONY_TOOLS 路径 ──
resolve_harmony_tools() {
    if [ -n "${HARMONY_TOOLS:-}" ]; then
        return 0
    fi
    local candidate
    for candidate in /mnt/f/command-line-tools /mnt/c/command-line-tools /mnt/d/command-line-tools; do
        if [ -d "$candidate" ]; then
            HARMONY_TOOLS="$candidate"
            return 0
        fi
    done
    HARMONY_TOOLS="/mnt/f/command-line-tools"  # 最后 fallback，check 时会报错
}
resolve_harmony_tools
DEFAULT_DEVICE="${DEVICE_TYPE:-pad}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

usage() {
    cat <<EOF
Usage: $0 <command> [args...]

Commands:
  dev-build    构建开发镜像 (winehua-dev)
  dev-start    启动持久化开发容器
  dev-stop     停止并删除开发容器
  dev-exec     在运行中容器执行命令 (默认: bash)
  dev-status   查看容器运行状态
  build-image  构建镜像 (同 dev-build)
  check        验证容器内构建环境
  make         一次性容器构建 (完整流程)
  hap          一次性容器构建 (仅 HAP)
  shell        启动容器交互 shell
  submodules   初始化 thirdparty 子模块

Defaults:
  image:        $IMAGE_NAME
  container:    $CONTAINER_NAME
  source:       $ROOT -> $CONTAINER_SRC
  harmony SDK:  $HARMONY_TOOLS -> /apps/harmony
  target:       NATIVE_ARCH=$DEFAULT_ARCH DEVICE_TYPE=$DEFAULT_DEVICE

Environment:
  HARMONY_TOOLS=/path/to/command-line-tools  (默认自动检测 F/C/D 盘)
  WINEHUA_DOCKER_IMAGE=winehua-dev
  WINEHUA_DOCKER_CACHE=bind|volume
  NATIVE_ARCH=arm64-v8a|x86_64|all
  DEVICE_TYPE=pad|pc
EOF
}

die() {
    echo "error: $*" >&2
    exit 1
}

need_tool() {
    command -v "$1" >/dev/null 2>&1 || die "$1 is required"
}

docker_tty_args() {
    if [ -t 0 ] && [ -t 1 ]; then
        printf '%s\n' "-it"
    else
        printf '%s\n' "-i"
    fi
}

build_image() {
    need_tool docker
    echo "Building image: $IMAGE_NAME ..."
    docker build -t "$IMAGE_NAME" "$ROOT"
    echo "Image built: $IMAGE_NAME"
}

# ── 持久化开发容器命令 ──

dev_start() {
    need_tool docker
    check_harmony_tools

    if docker ps --format '{{.Names}}' | grep -qx "$CONTAINER_NAME"; then
        echo "Container '$CONTAINER_NAME' is already running."
        echo "  docker exec -it $CONTAINER_NAME bash"
        return 0
    fi

    # 清理已停止的同名容器
    if docker ps -a --format '{{.Names}}' | grep -qx "$CONTAINER_NAME"; then
        echo "Removing stopped container: $CONTAINER_NAME"
        docker rm "$CONTAINER_NAME" >/dev/null
    fi

    # 开发模式默认使用 volume 缓存
    local use_cache="${WINEHUA_DOCKER_CACHE:-volume}"
    local cache_args=""
    if [ "$use_cache" = "volume" ]; then
        for vol in winehua-build-cache winehua-hvigor-cache winehua-oh-modules winehua-entry-build; do
            docker volume create "$vol" >/dev/null 2>&1 || true
        done
        cache_args="$(cache_mount_args)"
    fi

    echo "Starting dev container: $CONTAINER_NAME"
    echo "  Source:    $ROOT -> $CONTAINER_SRC"
    echo "  SDK:       $HARMONY_TOOLS -> /apps/harmony (ro)"
    echo "  Cache:     $use_cache"

    # shellcheck disable=SC2086
    docker run -d --name "$CONTAINER_NAME" \
        --mount "type=bind,src=$ROOT,dst=$CONTAINER_SRC" \
        --mount "type=bind,src=$HARMONY_TOOLS,dst=/apps/harmony,readonly" \
        $cache_args \
        -w "$CONTAINER_SRC" \
        -e NATIVE_ARCH="$DEFAULT_ARCH" \
        -e DEVICE_TYPE="$DEFAULT_DEVICE" \
        "$IMAGE_NAME" \
        bash -c 'sudo service ssh start && echo "SSH ready." && sleep infinity'

    echo ""
    echo "Container started. Connect with:"
    echo "  docker exec -it $CONTAINER_NAME bash"
    echo "  bash $0 dev-exec"
}

dev_stop() {
    need_tool docker

    if ! docker ps -a --format '{{.Names}}' | grep -qx "$CONTAINER_NAME"; then
        echo "Container '$CONTAINER_NAME' not found."
        return 0
    fi

    echo "Stopping container: $CONTAINER_NAME"
    docker stop "$CONTAINER_NAME" >/dev/null 2>&1 || true
    docker rm "$CONTAINER_NAME" >/dev/null 2>&1 || true
    echo "Container stopped and removed."
}

dev_exec() {
    need_tool docker

    if ! docker ps --format '{{.Names}}' | grep -qx "$CONTAINER_NAME"; then
        die "Container '$CONTAINER_NAME' is not running. Run '$0 dev-start' first."
    fi

    if [ "$#" -eq 0 ]; then
        docker exec -it "$CONTAINER_NAME" bash
    else
        docker exec -it "$CONTAINER_NAME" "$@"
    fi
}

dev_status() {
    need_tool docker

    echo "Image:    $IMAGE_NAME"
    echo ""

    if docker ps --format '{{.Names}}' | grep -qx "$CONTAINER_NAME"; then
        echo "Container: $CONTAINER_NAME [RUNNING]"
        echo ""
        docker ps --filter "name=$CONTAINER_NAME" --format '  ID:     {{.ID}}'
        docker ps --filter "name=$CONTAINER_NAME" --format '  Image:  {{.Image}}'
        docker ps --filter "name=$CONTAINER_NAME" --format '  Status: {{.Status}}'
        echo ""
        echo "Mounts:"
        docker inspect "$CONTAINER_NAME" --format '{{range .Mounts}}  {{.Type}}: {{.Source}} -> {{.Destination}}{{println}}{{end}}'
        echo ""
        echo "Connect:  docker exec -it $CONTAINER_NAME bash"
    elif docker ps -a --format '{{.Names}}' | grep -qx "$CONTAINER_NAME"; then
        echo "Container: $CONTAINER_NAME [STOPPED]"
        echo "  Run '$0 dev-start' to restart."
    else
        echo "Container: $CONTAINER_NAME [NOT FOUND]"
        echo "  Run '$0 dev-start' to create."
    fi
}

check_harmony_tools() {
    [ -d "$HARMONY_TOOLS" ] || die "HARMONY_TOOLS not found: $HARMONY_TOOLS"
}

check_submodules() {
    need_tool git
    if git -C "$ROOT" submodule status | grep -q '^-'; then
        cat >&2 <<EOF
error: thirdparty submodules are not initialized.

Run this from a shell that has Git/SSH access:
  git submodule update --init --recursive

Note: thirdparty/wine and thirdparty/box64 use SSH URLs.
EOF
        exit 1
    fi
}

cache_mount_args() {
    if [ "$CACHE_MODE" != "volume" ]; then
        return 0
    fi

    cat <<EOF
--mount type=volume,src=winehua-build-cache,dst=$CONTAINER_SRC/build
--mount type=volume,src=winehua-hvigor-cache,dst=$CONTAINER_SRC/.hvigor
--mount type=volume,src=winehua-oh-modules,dst=$CONTAINER_SRC/oh_modules
--mount type=volume,src=winehua-entry-build,dst=$CONTAINER_SRC/entry/build
EOF
}

run_container() {
    need_tool docker
    check_harmony_tools

    local tty_arg
    tty_arg="$(docker_tty_args)"

    docker run --rm $tty_arg \
        --mount "type=bind,src=$ROOT,dst=$CONTAINER_SRC" \
        --mount "type=bind,src=$HARMONY_TOOLS,dst=/apps/harmony,readonly" \
        $(cache_mount_args) \
        -w "$CONTAINER_SRC" \
        -e NATIVE_ARCH="$DEFAULT_ARCH" \
        -e DEVICE_TYPE="$DEFAULT_DEVICE" \
        -e TOOL_HOME=/apps/harmony \
        -e OHOS_SDK=/apps/harmony/sdk/default/openharmony \
        "$IMAGE_NAME" "$@"
}

container_check() {
    run_container bash -lc '
set -euo pipefail
missing=0
for tool in gcc g++ make cmake ninja meson bison flex autoconf libtoolize pkg-config git python3 java node x86_64-w64-mingw32-gcc; do
    if command -v "$tool" >/dev/null 2>&1; then
        echo "ok: $tool"
    else
        echo "missing: $tool"
        missing=1
    fi
done
for mod in yaml mako markupsafe; do
    if python3 -c "import $mod" >/dev/null 2>&1; then
        echo "ok: python $mod"
    else
        echo "missing: python $mod"
        missing=1
    fi
done
test -x /apps/harmony/bin/hvigorw && echo "ok: hvigorw" || { echo "missing: hvigorw"; missing=1; }
test -x /apps/harmony/sdk/default/openharmony/native/llvm/bin/clang && echo "ok: OHOS clang" || { echo "missing: OHOS clang"; missing=1; }
/apps/harmony/sdk/default/hms/native/BiSheng/bin/ld.lld --version >/dev/null 2>&1 && echo "ok: ld.lld" || { echo "missing: ld.lld"; missing=1; }
exit "$missing"
'
}

copy_hap_to_dist_if_present='
hap="entry/build/default/outputs/default/entry-default-signed.hap"
if [ -f "$hap" ]; then
    mkdir -p dist
    cp -f "$hap" "dist/entry-default-signed-${NATIVE_ARCH}-${DEVICE_TYPE}.hap"
    echo "copied: dist/entry-default-signed-${NATIVE_ARCH}-${DEVICE_TYPE}.hap"
fi
'

run_make() {
    local make_cmd=""

    check_submodules
    if [ "$#" -eq 0 ]; then
        set -- "NATIVE_ARCH=$DEFAULT_ARCH" "DEVICE_TYPE=$DEFAULT_DEVICE"
    fi
    printf -v make_cmd '%q ' make "$@"
    run_container bash -lc "$make_cmd; $copy_hap_to_dist_if_present"
}

case "${1:-help}" in
    -h|--help|help)
        usage
        ;;
    dev-build|build-image)
        build_image
        ;;
    dev-start)
        shift || true
        dev_start "$@"
        ;;
    dev-stop)
        dev_stop
        ;;
    dev-exec)
        shift || true
        dev_exec "$@"
        ;;
    dev-status)
        dev_status
        ;;
    check)
        container_check
        ;;
    make)
        shift || true
        run_make "$@"
        ;;
    hap)
        shift || true
        run_make hap "NATIVE_ARCH=$DEFAULT_ARCH" "DEVICE_TYPE=$DEFAULT_DEVICE" "$@"
        ;;
    shell)
        run_container bash
        ;;
    submodules)
        need_tool git
        git -C "$ROOT" submodule update --init --recursive
        ;;
    *)
        run_make "$@"
        ;;
esac
