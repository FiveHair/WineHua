#!/bin/bash
# package.sh — HNP 打包 + HAP 构建 + 签名 + 部署
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"
PACKAGE_BACKUP_DIR=""
PACKAGE_RESTORE_FILES=()
PACKAGE_RESTORE_BACKUPS=()

backup_package_file() {
    local file="$1"
    local backup=""

    [ -f "$file" ] || err "打包配置文件不存在: $file"

    if [ -z "$PACKAGE_BACKUP_DIR" ]; then
        PACKAGE_BACKUP_DIR="$(mktemp -d /tmp/winehua-package-backup.XXXXXX)"
    fi

    backup="$PACKAGE_BACKUP_DIR/$(printf '%s' "$file" | sed 's|[/:\\]|_|g').bak"
    cp "$file" "$backup"
    PACKAGE_RESTORE_FILES+=("$file")
    PACKAGE_RESTORE_BACKUPS+=("$backup")
}

restore_package_files() {
    local i

    if [ "${#PACKAGE_RESTORE_FILES[@]}" -gt 0 ]; then
        for i in "${!PACKAGE_RESTORE_FILES[@]}"; do
            cp "${PACKAGE_RESTORE_BACKUPS[$i]}" "${PACKAGE_RESTORE_FILES[$i]}"
        done
        log "已恢复打包前配置"
    fi

    if [ -n "${PACKAGE_BACKUP_DIR:-}" ]; then
        rm -rf "$PACKAGE_BACKUP_DIR"
    fi

    PACKAGE_BACKUP_DIR=""
    PACKAGE_RESTORE_FILES=()
    PACKAGE_RESTORE_BACKUPS=()
    trap - EXIT
}

# ============================================================
# 工具函数: 动态设置 abiFilters
set_abi_filters() {
    # 根据 NATIVE_ARCH 写 build-profile.json5 的 abiFilters
    local profile="$WINEHUA/entry/build-profile.json5"
    if [ ! -f "$profile" ]; then
        err "build-profile.json5 未找到: $profile"
    fi

    local abi_value
    if [ "$NATIVE_ARCH" = "all" ]; then
        abi_value='"arm64-v8a", "x86_64"'
    else
        abi_value="\"$NATIVE_ARCH\""
    fi

    # 用 python 正则替换, 支持多行 abiFilters
    python3 -c "
import re
with open('$profile', 'r') as f:
    content = f.read()
content = re.sub(r'\"abiFilters\"\s*:\s*\[[^\]]*\]', '\"abiFilters\": [$abi_value]', content)
with open('$profile', 'w') as f:
    f.write(content)
"
    log "abiFilters: [$abi_value]"
}

has_real_signing_material() {
    python3 - "$WINEHUA/build-profile.json5" <<'PY'
import json
import os
import re
import sys

profile_path = sys.argv[1]
root = os.path.dirname(profile_path)
with open(profile_path, "r", encoding="utf-8") as fh:
    content = re.sub(r",\s*([}\]])", r"\1", fh.read())
profile = json.loads(content)
material = profile["app"]["signingConfigs"][0]["material"]
placeholders = {
    "your_key_alias",
    "YOUR_ENCRYPTED_KEY_PASSWORD",
    "YOUR_ENCRYPTED_STORE_PASSWORD",
}
for key in ("certpath", "profile", "storeFile"):
    value = material.get(key, "")
    if not value or "your_" in value or not os.path.isfile(os.path.join(root, value)):
        sys.exit(1)
for key in ("keyAlias", "keyPassword", "storePassword"):
    if material.get(key, "") in placeholders:
        sys.exit(1)
PY
}

sign_hap_with_local_debug_material() {
    local unsigned_hap="$1"
    local signed_hap="$2"
    local sign_jar="$TOOL_HOME/sdk/default/openharmony/toolchains/lib/hap-sign-tool.jar"
    local work_dir="$BUILD_DIR/local-debug-signing"
    local password="${WINEHUA_DEBUG_SIGN_PASSWORD:-WineHuaDebug123}"
    local root_alias="winehua_root_ca"
    local sub_alias="winehua_app_ca"
    local app_alias="winehua_debug"
    local profile_sub_alias="winehua_profile_ca"
    local profile_alias="winehua_profile"
    local root_subject="C=CN,O=WineHua,OU=Debug,CN=WineHua Root CA"
    local sub_subject="C=CN,O=WineHua,OU=Debug,CN=Application Debug Signature Service CA"
    local app_subject="C=CN,O=WineHua,OU=Debug,CN=WineHua Debug"
    local profile_sub_subject="C=CN,O=WineHua,OU=Debug,CN=Provision Profile Debug Signature Service CA"
    local profile_subject="C=CN,O=WineHua,OU=Debug,CN=WineHua Profile Debug"
    local keystore="$work_dir/debug.p12"
    local root_cert="$work_dir/root-ca.cer"
    local sub_cert="$work_dir/sub-app-ca.cer"
    local app_cert="$work_dir/app-debug-cert.cer"
    local profile_sub_cert="$work_dir/sub-profile-ca.cer"
    local profile_cert="$work_dir/profile-debug-cert.cer"
    local profile_json="$work_dir/profile.json"
    local profile_p7b="$work_dir/profile.p7b"
    local cert_b64

    mkdir -p "$work_dir"
    rm -f "$keystore" "$root_cert" "$sub_cert" "$app_cert" \
        "$profile_sub_cert" "$profile_cert" "$profile_json" "$profile_p7b"

    java -jar "$sign_jar" generate-ca \
        -keyAlias "$root_alias" -keyPwd "$password" \
        -keyAlg ECC -keySize NIST-P-256 \
        -subject "$root_subject" -validity 3650 -signAlg SHA256withECDSA \
        -keystoreFile "$keystore" -keystorePwd "$password" \
        -outFile "$root_cert" -pwdInputMode 0

    java -jar "$sign_jar" generate-ca \
        -keyAlias "$sub_alias" -keyPwd "$password" \
        -keyAlg ECC -keySize NIST-P-256 \
        -issuer "$root_subject" -issuerKeyAlias "$root_alias" -issuerKeyPwd "$password" \
        -subject "$sub_subject" -validity 3650 -signAlg SHA256withECDSA \
        -keystoreFile "$keystore" -keystorePwd "$password" \
        -outFile "$sub_cert" -pwdInputMode 0

    java -jar "$sign_jar" generate-ca \
        -keyAlias "$profile_sub_alias" -keyPwd "$password" \
        -keyAlg ECC -keySize NIST-P-256 \
        -issuer "$root_subject" -issuerKeyAlias "$root_alias" -issuerKeyPwd "$password" \
        -subject "$profile_sub_subject" -validity 3650 -signAlg SHA256withECDSA \
        -keystoreFile "$keystore" -keystorePwd "$password" \
        -outFile "$profile_sub_cert" -pwdInputMode 0

    java -jar "$sign_jar" generate-keypair \
        -keyAlias "$app_alias" -keyPwd "$password" \
        -keyAlg ECC -keySize NIST-P-256 \
        -keystoreFile "$keystore" -keystorePwd "$password" -pwdInputMode 0

    java -jar "$sign_jar" generate-app-cert \
        -keyAlias "$app_alias" -keyPwd "$password" \
        -issuer "$sub_subject" -issuerKeyAlias "$sub_alias" -issuerKeyPwd "$password" \
        -subject "$app_subject" -validity 3650 -signAlg SHA256withECDSA \
        -rootCaCertFile "$root_cert" -subCaCertFile "$sub_cert" \
        -keystoreFile "$keystore" -keystorePwd "$password" \
        -outForm certChain -outFile "$app_cert" -pwdInputMode 0

    java -jar "$sign_jar" generate-keypair \
        -keyAlias "$profile_alias" -keyPwd "$password" \
        -keyAlg ECC -keySize NIST-P-256 \
        -keystoreFile "$keystore" -keystorePwd "$password" -pwdInputMode 0

    java -jar "$sign_jar" generate-profile-cert \
        -keyAlias "$profile_alias" -keyPwd "$password" \
        -issuer "$profile_sub_subject" \
        -issuerKeyAlias "$profile_sub_alias" -issuerKeyPwd "$password" \
        -subject "$profile_subject" -validity 3650 -signAlg SHA256withECDSA \
        -rootCaCertFile "$root_cert" -subCaCertFile "$profile_sub_cert" \
        -keystoreFile "$keystore" -keystorePwd "$password" \
        -outForm certChain -outFile "$profile_cert" -pwdInputMode 0

    cert_b64="$(base64 -w0 "$app_cert")"
    python3 - "$profile_json" "$cert_b64" <<'PY'
import json
import sys

path, cert = sys.argv[1:3]
profile = {
    "version-name": "1.0.0",
    "version-code": 1,
    "app-distribution-type": "os_integration",
    "uuid": "00000000-0000-0000-0000-000000000001",
    "validity": {"not-before": 1704067200, "not-after": 4102444800},
    "type": "debug",
    "bundle-info": {
        "developer-id": "WineHuaDebug",
        "development-certificate": cert,
        "distribution-certificate": cert,
        "bundle-name": "app.hackeris.winehua",
        "apl": "normal",
        "app-feature": "hos_normal_app",
    },
    "acls": {"allowed-acls": []},
    "permissions": {"restricted-permissions": []},
    "debug-info": {"device-ids": ["*"]},
}
with open(path, "w", encoding="utf-8") as fh:
    json.dump(profile, fh, separators=(",", ":"))
PY

    java -jar "$sign_jar" sign-profile \
        -mode localSign \
        -keyAlias "$profile_alias" -keyPwd "$password" \
        -profileCertFile "$profile_cert" -inFile "$profile_json" \
        -signAlg SHA256withECDSA \
        -keystoreFile "$keystore" -keystorePwd "$password" \
        -outFile "$profile_p7b" -pwdInputMode 0

    java -jar "$sign_jar" verify-profile -inFile "$profile_p7b" \
        -outFile "$work_dir/profile-verify.json"

    java -jar "$sign_jar" sign-app \
        -mode localSign \
        -keyAlias "$app_alias" -keyPwd "$password" \
        -appCertFile "$app_cert" -profileFile "$profile_p7b" -profileSigned 1 \
        -inFile "$unsigned_hap" -signAlg SHA256withECDSA \
        -keystoreFile "$keystore" -keystorePwd "$password" \
        -outFile "$signed_hap" -compatibleVersion 22 -signCode 1 -pwdInputMode 0
}

sign_hap() {
    local unsigned_hap="$1"
    local signed_hap="$2"

    if has_real_signing_material; then
        python3 sign.py "$unsigned_hap" "$signed_hap"
    else
        warn "未找到真实 .ohos 签名材料，使用本地 debug 签名生成 HAP"
        sign_hap_with_local_debug_material "$unsigned_hap" "$signed_hap"
    fi
}

# ============================================================
package_hnp() {
    log "=== 打包 HNP ($NATIVE_ARCH) ==="
    mkdir -p "$BUILD_DIR"
    "$HNPCLI" pack -i "$STAGING_DIR" -o "$BUILD_DIR" -n winehua -v 0.1.0 || { err "hnpcli pack 失败"; return 1; }

    # HNP 按架构存放
    local hnp_dir="$WINEHUA/entry/hnp/$NATIVE_ARCH"
    mkdir -p "$hnp_dir"
    cp "$BUILD_DIR/winehua.hnp" "$hnp_dir/winehua.hnp"
    ls -lh "$hnp_dir/winehua.hnp"
}

# ============================================================
package_hap() {
    log "=== 打包 HAP ($NATIVE_ARCH) ==="
    local unsigned_hap="$WINEHUA/entry/build/default/outputs/default/entry-default-unsigned.hap"
    local signed_hap="$WINEHUA/entry/build/default/outputs/default/entry-default-signed.hap"

    backup_package_file "$WINEHUA/entry/build-profile.json5"
    backup_package_file "$WINEHUA/entry/src/main/module.json5"
    trap restore_package_files EXIT

    set_abi_filters

    # Pad: 移除 hnpPackages 配置 + 注入 PAD_MODE 编译宏
    if [ "$DEVICE_TYPE" = "pad" ]; then
        local module_json="$WINEHUA/entry/src/main/module.json5"
        python3 -c "
import re
with open('$module_json', 'r') as f:
    content = f.read()
# 移除 hnpPackages 块 (含前导逗号)
content = re.sub(r',?\s*\"hnpPackages\"\s*:\s*\[[^][]*\]', '', content)
with open('$module_json', 'w') as f:
    f.write(content)
"
        log "  已移除 hnpPackages 配置"

        # 通过 cppFlags 注入 PAD_MODE (hvigorw 不会透传环境变量给 CMake)
        local profile="$WINEHUA/entry/build-profile.json5"
        python3 -c "
import re
with open('$profile', 'r') as f:
    content = f.read()
# 仅当不存在时注入 -DPAD_MODE
if '-DPAD_MODE' not in content:
    content = re.sub(r'\"cppFlags\"\s*:\s*\"', '\"cppFlags\": \"-DPAD_MODE ', content)
with open('$profile', 'w') as f:
    f.write(content)
"
        log "  已注入 -DPAD_MODE 到 cppFlags"
    fi

    # 清理非目标架构的 native libs (hvigorw ProcessLibs 会打包所有 libs/)
    local libs_root="$WINEHUA/entry/libs"
    if [ "$NATIVE_ARCH" = "arm64-v8a" ]; then
        rm -rf "$libs_root/x86_64"
    elif [ "$NATIVE_ARCH" = "x86_64" ]; then
        rm -rf "$libs_root/arm64-v8a"
    fi
    # NATIVE_ARCH=all 时保留两个架构

    cd "$WINEHUA"
    if ! hvigorw --no-daemon -c ohos.buildDir=.hvigor-ohos-build assembleHap; then
        if [ -s "$unsigned_hap" ]; then
            warn "hvigorw assembleHap 未完成签名，但 unsigned HAP 已生成，继续使用项目签名流程"
        else
            err "hvigorw assembleHap 失败"
            return 1
        fi
    fi

    cd "$WINEHUA/entry"
    # 清理非目标架构的 HNP (hvigorw 不处理 hnp/, 所以这时清理即可)
    local hnp_root="$WINEHUA/entry/hnp"
    if [ "$DEVICE_TYPE" = "pad" ]; then
        # Pad: 完全移除 hnp/ 目录 (Pad 不支持 HNP)
        rm -rf "$hnp_root"
    else
        if [ "$NATIVE_ARCH" = "arm64-v8a" ]; then
            rm -rf "$hnp_root/x86_64"
        elif [ "$NATIVE_ARCH" = "x86_64" ]; then
            rm -rf "$hnp_root/arm64-v8a"
        fi
        # NATIVE_ARCH=all 时保留两个架构

        # 将 HNP 目录打包进 HAP (hvigorw 不会自动处理 hnp/)
        zip -r "$unsigned_hap" hnp
    fi

    cd "$WINEHUA"
    sign_hap "$unsigned_hap" "$signed_hap"

    ls -lh "$signed_hap"
    log "HAP 构建 + 签名完成 ($NATIVE_ARCH)"
    restore_package_files
}

# ============================================================
deploy() {
    local device="${1:-192.168.1.4:38879}"
    local hap="$WINEHUA/entry/build/default/outputs/default/entry-default-signed.hap"

    if [ ! -f "$hap" ]; then
        err "HAP 文件不存在: $hap"
    fi

    log "=== 部署到 $device ==="
    hdc tconn "$device" || { err "hdc tconn 失败"; }
    hdc shell bm uninstall -n app.hackeris.winehua 2>/dev/null || true
    hdc file send "$hap" /data/local/tmp/ || { err "hdc file send 失败"; }
    hdc shell bm install -p /data/local/tmp/entry-default-signed.hap -r || { err "bm install 失败"; }

    log "部署完成"
}

# ---- main ----
case "${1:-}" in
    hnp)
        if [ "$DEVICE_TYPE" = "pad" ]; then
            log "Pad 模式: 跳过 HNP 打包"
        else
            package_hnp
        fi
        ;;
    hap)  package_hap ;;
    deploy) deploy "${2:-}" ;;
    all)
        if [ "$DEVICE_TYPE" = "pad" ]; then
            log "Pad 模式: 跳过 HNP 打包"
            package_hap && deploy "${2:-}"
        else
            package_hnp && package_hap && deploy "${2:-}"
        fi
        ;;
    *)    echo "用法: $0 {hnp|hap|deploy|all} [device_ip]" >&2; exit 1 ;;
esac
