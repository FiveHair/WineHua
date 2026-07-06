param(
    [string]$Distro = "Ubuntu",
    [string]$RepoWslPath = "/mnt/d/Gits/WineHua",
    [string]$SdkZipWindowsPath = "D:\commandline-tools-linux-x64-6.1.1.290.zip",
    [switch]$SkipSubmodules
)

$ErrorActionPreference = "Stop"

function Invoke-Wsl {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Arguments
    )

    & wsl.exe -d $Distro @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "wsl.exe failed with exit code $LASTEXITCODE"
    }
}

Write-Host "Checking WSL distro: $Distro"
& wsl.exe -d $Distro -e bash -lc "printf 'user=' && whoami && printf ' pwd=' && pwd && printf '\n'"
if ($LASTEXITCODE -ne 0) {
    throw "Cannot enter WSL distro '$Distro'. Run 'wsl --list --verbose' from this same PowerShell session."
}

if (-not (Test-Path -LiteralPath $SdkZipWindowsPath)) {
    throw "SDK zip not found: $SdkZipWindowsPath"
}

$SdkZipWslPath = (& wsl.exe -d $Distro -e wslpath -a $SdkZipWindowsPath).Trim()
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($SdkZipWslPath)) {
    throw "Failed to convert SDK zip path to WSL path: $SdkZipWindowsPath"
}

$SkipSubmodulesValue = if ($SkipSubmodules) { "1" } else { "0" }
$BashScript = @'
#!/usr/bin/env bash
set -euo pipefail

repo="$1"
sdk_zip="$2"
skip_submodules="$3"

cd "$repo"

host_ip="$(ip route | awk '/default/ {print $3; exit}')"
if [ -z "$host_ip" ]; then
    echo "error: failed to resolve Windows host IP from WSL route" >&2
    exit 1
fi

proxy_url="http://${host_ip}:8080"
export http_proxy="$proxy_url"
export https_proxy="$proxy_url"
export HTTP_PROXY="$proxy_url"
export HTTPS_PROXY="$proxy_url"
export no_proxy="localhost,127.0.0.1,::1"
export NO_PROXY="$no_proxy"

echo "Repo:      $repo"
echo "SDK zip:   $sdk_zip"
echo "Proxy:     $proxy_url"
echo "Target:    NATIVE_ARCH=arm64-v8a DEVICE_TYPE=pad"

if [ ! -f "$sdk_zip" ]; then
    echo "error: SDK zip not found in WSL: $sdk_zip" >&2
    exit 1
fi

if [ ! -x /apps/harmony/bin/hvigorw ] || [ ! -d /apps/harmony/sdk/default/openharmony ]; then
    echo "Preparing Harmony tools in /apps/harmony ..."
    if ! command -v unzip >/dev/null 2>&1; then
        echo "Installing unzip in WSL ..."
        sudo apt-get update
        sudo apt-get install -y unzip
    fi

    tmp_dir="$(mktemp -d)"
    cleanup() {
        rm -rf "$tmp_dir"
    }
    trap cleanup EXIT

    unzip -q "$sdk_zip" -d "$tmp_dir"
    tool_root="$(find "$tmp_dir" -type f -path '*/bin/hvigorw' -printf '%h\n' | sed 's#/bin$##' | head -n1)"
    if [ -z "$tool_root" ]; then
        echo "error: could not find bin/hvigorw inside SDK zip" >&2
        exit 1
    fi

    sudo mkdir -p /apps/harmony
    if command -v rsync >/dev/null 2>&1; then
        sudo rsync -a "$tool_root"/ /apps/harmony/
    else
        sudo cp -a "$tool_root"/. /apps/harmony/
    fi
fi

test -x /apps/harmony/bin/hvigorw
test -d /apps/harmony/sdk/default/openharmony

if ! command -v docker >/dev/null 2>&1; then
    echo "error: docker CLI is not available inside WSL." >&2
    echo "Install Docker Desktop and enable WSL integration for this distro, or install Docker Engine inside WSL." >&2
    exit 1
fi

docker info >/dev/null

git config http.proxy "$proxy_url"
git config https.proxy "$proxy_url"

mkdir -p "$HOME/.docker"
if command -v python3 >/dev/null 2>&1; then
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
else
    echo "warning: python3 missing; skipped Docker client proxy config merge"
fi

if [ "$skip_submodules" != "1" ]; then
    if ! bash scripts/docker_wsl_build.sh submodules; then
        echo "Submodule init failed with original URLs; retrying GitHub SSH URLs via HTTPS proxy ..."
        git config url.https://github.com/.insteadOf git@github.com:
        bash scripts/docker_wsl_build.sh submodules
    fi
fi

bash scripts/docker_wsl_build.sh build-image
HARMONY_TOOLS=/apps/harmony bash scripts/docker_wsl_build.sh check
WINEHUA_DOCKER_CACHE=volume HARMONY_TOOLS=/apps/harmony NATIVE_ARCH=arm64-v8a DEVICE_TYPE=pad bash scripts/docker_wsl_build.sh make

test -s dist/entry-default-signed-arm64-v8a-pad.hap
test -s entry/build/default/outputs/default/entry-default-signed.hap

echo ""
echo "Artifacts:"
ls -lh dist/entry-default-signed-arm64-v8a-pad.hap
ls -lh entry/build/default/outputs/default/entry-default-signed.hap
echo ""
echo "Git status:"
git status --short
'@

$TempScript = Join-Path $PSScriptRoot "..\build\codex-wsl-pad-build.sh"
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $TempScript) | Out-Null
$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($TempScript, $BashScript.Replace("`r`n", "`n"), $Utf8NoBom)
$TempScriptWslPath = (& wsl.exe -d $Distro -e wslpath -a $TempScript).Trim()
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($TempScriptWslPath)) {
    throw "Failed to convert temp script path to WSL path: $TempScript"
}

Write-Host "Running WSL Docker Pad build ..."
Invoke-Wsl -Arguments @(
    "-e", "bash", $TempScriptWslPath,
    $RepoWslPath,
    $SdkZipWslPath,
    $SkipSubmodulesValue
)
