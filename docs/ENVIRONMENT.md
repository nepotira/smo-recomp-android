# Ambiente de Desenvolvimento (ENVIRONMENT)

## Plataforma Host (Desenvolvimento)
* **Sistema Operacional:** Linux x86_64 / Debian-based
* **Compilador:** Clang / LLVM 18+ e GCC 13+
* **Build Systems:** CMake 3.22+, Ninja
* **Ferramental Python:** Python 3.12+ (capstone, unicorn, lz4)
* **Controle de Versão:** Git 2.40+, GitHub CLI (`gh`) 2.60+

## Plataforma Alvo (Execução Móvel)
* **Aparelho:** Xiaomi Poco F7
* **SoC:** Qualcomm Snapdragon 8s Gen 4 (SM8735, TSMC 4nm)
* **CPU:** 8x núcleos Kryo ARMv9 (1x Cortex-X4 @ 3.21 GHz + 7x Cortex-A720)
* **GPU:** Qualcomm Adreno 825
* **Driver Vulkan:** Mesa Turnip Custom Adreno 8xx (`nepotira/turnip-8xx-nepo`)
* **Sistema Operacional:** Android 17 / HyperOS 4 (Kernel Linux AArch64)
* **Display:** $2712 \times 1220$ pixels (Proporção $20:9 \approx 2.22295$)
