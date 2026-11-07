#!/usr/bin/env bash
# ==============================================================================
# build_gamelib.sh — Orquestrador de Transpilação e Compilação de smogame.so
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DUMP_DIR="${1:-}"

echo "======================================================================"
echo "SMO-Recomp-Android: Pipeline de Transpilação AOT & Gamelib"
echo "======================================================================"

if [[ -z "${DUMP_DIR}" || ! -d "${DUMP_DIR}" ]]; then
    echo "Uso: ./build_gamelib.sh <caminho_para_pasta_com_dump_v1.4.1>"
    echo ""
    echo "A pasta do dump deve conter os arquivos descriptografados:"
    echo "  - main (executável NSO ou ELF)"
    echo "  - subsdk0 (se aplicável)"
    echo "  - sdk (se aplicável)"
    echo "  - RomFS descompactada"
    exit 1
fi

export SMO_LAYOUT="android"
export RECOMP_BASE="0x4000000000"
export RECOMP_PATCHES="${SCRIPT_DIR}/patches/smo_aspect_ratio_20_9.txt"

OUT_GEN="${SCRIPT_DIR}/games/smo/generated"
mkdir -p "${OUT_GEN}"

echo "[1/4] Extraindo módulos e seções NSO/ELF a partir de: ${DUMP_DIR}"
python3 "${SCRIPT_DIR}/lifter/extract.py" --dump "${DUMP_DIR}" --out "${SCRIPT_DIR}/build/elf"

echo "[2/4] Mapeando limites exatos de funções a partir de .eh_frame e RTTI..."
python3 "${SCRIPT_DIR}/lifter/functions.py" --elf "${SCRIPT_DIR}/build/elf/main.elf" --out "${SCRIPT_DIR}/build/functions"

echo "[3/4] Transpilando AArch64 para C++ com Patch Hor+ 20:9 ativo..."
python3 "${SCRIPT_DIR}/lifter/recomp.py" full "${OUT_GEN}"

echo "[4/4] Compilando biblioteca compartilhada libsmogame.so via NDK CMake..."
mkdir -p "${SCRIPT_DIR}/build/gamelib"
cd "${SCRIPT_DIR}/build/gamelib"
cmake -DCMAKE_BUILD_TYPE=Release \
      -DANDROID_ABI=arm64-v8a \
      -DCMAKE_TOOLCHAIN_FILE="${ANDROID_NDK}/build/cmake/android.toolchain.cmake" \
      "${SCRIPT_DIR}/games/smo"
cmake --build . --config Release -j$(nproc)

echo "======================================================================"
echo "Sucesso: libsmogame.so compilada com sucesso e vinculada a libmain.so!"
echo "======================================================================"
