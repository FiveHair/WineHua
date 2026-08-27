#!/bin/bash
# build_wine.sh — Wine 引擎构建入口 (winehua/wine fork 直建)
# 共享实现在 build_engine.sh; Proton flavor 见 build_proton.sh
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ENGINE=wine bash "$SCRIPT_DIR/build_engine.sh"
