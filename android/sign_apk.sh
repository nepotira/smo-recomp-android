#!/usr/bin/env bash
# sign_apk.sh — Automated Zipalign & Cryptographic Signing for SMO Recomp APK
# Target: Android ARM64 (API 28-35 / HyperOS 4 / Poco F7)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

INPUT_APK="${1:-}"
OUTPUT_APK="${2:-${ROOT_DIR}/SMO-Recomp-v0.1.0-android-arm64.apk}"

if [ -z "${INPUT_APK}" ]; then
    if [ -f "${SCRIPT_DIR}/app/build/outputs/apk/release/app-release.apk" ]; then
        INPUT_APK="${SCRIPT_DIR}/app/build/outputs/apk/release/app-release.apk"
    elif [ -f "${SCRIPT_DIR}/app/build/outputs/apk/release/app-release-unsigned.apk" ]; then
        INPUT_APK="${SCRIPT_DIR}/app/build/outputs/apk/release/app-release-unsigned.apk"
    elif [ -f "${ROOT_DIR}/android/app/build/outputs/apk/release/app-release.apk" ]; then
        INPUT_APK="${ROOT_DIR}/android/app/build/outputs/apk/release/app-release.apk"
    else
        INPUT_APK="${ROOT_DIR}/android/app/build/outputs/apk/release/app-release-unsigned.apk"
    fi
fi

KEYSTORE_DIR="${SCRIPT_DIR}/keystore"
KEYSTORE_PATH="${KEYSTORE_DIR}/release.keystore"
KEY_ALIAS="smo_recomp_key"
KEY_PASS="android_smorecomp_2026"

echo "=========================================================="
echo " SMO Recomp ARM64 — APK Zipalign & Signing Pipeline"
echo "=========================================================="
echo "Input APK  : ${INPUT_APK}"
echo "Output APK : ${OUTPUT_APK}"
echo "=========================================================="

# Locate Android SDK build tools
locate_tool() {
    local tool="$1"
    if command -v "$tool" &>/dev/null; then
        command -v "$tool"
        return 0
    fi
    for sdk in "${ANDROID_HOME:-}" "${ANDROID_SDK_ROOT:-}" "/home/nepo/Android/Sdk"; do
        if [ -n "$sdk" ] && [ -d "$sdk/build-tools" ]; then
            local found
            found=$(find "$sdk/build-tools" -name "$tool" 2>/dev/null | sort -V | tail -n 1)
            if [ -n "$found" ] && [ -x "$found" ]; then
                echo "$found"
                return 0
            fi
        fi
    done
    return 1
}

ZIPALIGN=$(locate_tool "zipalign" || echo "")
APKSIGNER=$(locate_tool "apksigner" || echo "")
KEYTOOL=$(command -v keytool || echo "")

# If tools aren't present locally, check if Python signer is available
if [ -z "$ZIPALIGN" ] || [ -z "$APKSIGNER" ]; then
    echo "[!] Android build-tools (zipalign / apksigner) not found in PATH or ANDROID_HOME."
    echo "[!] Delegating to Python APK signing pipeline: tools/sign_apk.py"
    python3 "${ROOT_DIR}/tools/sign_apk.py" "${INPUT_APK}" "${OUTPUT_APK}"
    exit 0
fi

# Ensure keystore directory exists
mkdir -p "${KEYSTORE_DIR}"

# Generate keystore if not already created
if [ ! -f "${KEYSTORE_PATH}" ]; then
    echo "[+] Generating release keystore at ${KEYSTORE_PATH}..."
    "${KEYTOOL}" -genkeypair \
        -keystore "${KEYSTORE_PATH}" \
        -storepass "${KEY_PASS}" \
        -alias "${KEY_ALIAS}" \
        -keypass "${KEY_PASS}" \
        -keyalg RSA \
        -keysize 2048 \
        -validity 10000 \
        -dname "CN=SMO Recomp, OU=Gaming, O=Nepo, L=SaoPaulo, ST=SP, C=BR"
fi

TMP_ALIGNED="${INPUT_APK}.aligned.apk"
rm -f "${TMP_ALIGNED}"

echo "[+] Executing zipalign (16-byte ELF alignment for Android 15/16 16KB kernels)..."
"${ZIPALIGN}" -p -f 4 "${INPUT_APK}" "${TMP_ALIGNED}"

echo "[+] Signing APK with apksigner (V1 + V2 + V3 signature schemes)..."
"${APKSIGNER}" sign \
    --ks "${KEYSTORE_PATH}" \
    --ks-key-alias "${KEY_ALIAS}" \
    --ks-pass "pass:${KEY_PASS}" \
    --key-pass "pass:${KEY_PASS}" \
    --v1-signing-enabled true \
    --v2-signing-enabled true \
    --v3-signing-enabled true \
    --out "${OUTPUT_APK}" \
    "${TMP_ALIGNED}"

rm -f "${TMP_ALIGNED}"

echo "[+] Verifying APK signature..."
"${APKSIGNER}" verify --verbose "${OUTPUT_APK}"

echo "=========================================================="
echo "[✓] Signed release APK ready: ${OUTPUT_APK}"
echo "=========================================================="
