#!/usr/bin/env bash
# build_apk.sh — Orchestration script for compiling and packaging SMO Recomp Android APK
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

echo "=========================================================="
echo " Super Mario Odyssey Recomp ARM64 — Android APK Packaging"
echo "=========================================================="
echo "Target Platform: Android AArch64 (ARMv8.2-A+SIMD / ARMv9)"
echo "Silicon Targets: Snapdragon 8s Gen 4 (Adreno 825) / 8 Elite"
echo "Root Directory : ${ROOT_DIR}"
echo "=========================================================="

BUILD_TYPE="${1:-Release}"

cd "${SCRIPT_DIR}"

if command -v ./gradlew &>/dev/null; then
    echo "[+] Running Gradle wrapper to assemble APK (${BUILD_TYPE})..."
    if [ "${BUILD_TYPE}" = "Debug" ]; then
        ./gradlew assembleDebug
        echo "[✓] Debug APK generated at: app/build/outputs/apk/debug/app-debug.apk"
    else
        ./gradlew assembleRelease
        echo "[✓] Release APK generated at: app/build/outputs/apk/release/app-release-unsigned.apk"
    fi
else
    echo "[!] gradlew not executable. Attempting chmod +x..."
    chmod +x ./gradlew
    ./gradlew assembleRelease
fi

echo "=========================================================="
echo " Packaging completed successfully."
echo "=========================================================="
