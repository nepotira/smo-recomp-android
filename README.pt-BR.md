# Recomp do Super Mario Odyssey para Android (SMO-Recomp-Android)

[English](README.md) | [Português do Brasil](README.pt-BR.md)

[![Licença: GPL v3](https://img.shields.io/badge/Licen%C3%A7a-GPLv3-blue.svg)](LICENSE)
[![Plataforma: Android](https://img.shields.io/badge/Plataforma-Android%20ARM64-green.svg)](https://developer.android.com)
[![Alvo: Poco F7](https://img.shields.io/badge/Aparelho-Snapdragon%208s%20Gen%204-orange.svg)](docs/PROJECT_STATE.md)

Um porte nativo para Android de alto desempenho de *Super Mario Odyssey* (v1.4.1), construído a partir de recompilação binária estática antecipada (AOT) com um runtime móvel em C++20 de latência ultrabaixa.

---

## Aviso Importante

> **Este repositório não contém nenhum código de jogo protegido por direitos autorais, assets, ROMs, chaves ou binários proprietários da Nintendo.**  
> Os usuários devem fornecer sua própria cópia extraída legalmente de *Super Mario Odyssey* (v1.4.1) do seu próprio console Nintendo Switch. O processo de compilação roda localmente no seu computador ou dispositivo e nunca distribui arquivos do jogo.

---

## Destaques Arquiteturais

* **Execução Nativa 100% ARM64:** Como o Nintendo Switch (Tegra X1) e os smartphones Android contemporâneos compartilham a mesma arquitetura de registradores (AArch64), o jogo roda diretamente nos núcleos físicos da CPU sem intermediários JIT ou interpretador.
* **Arquitetura Dual de Bibliotecas:**
  - `libmain.so`: O runtime da aplicação hospedeira fornecendo amarração com Android OS, apresentação Vulkan 1.3, AAudio exclusivo e carregador de driver Turnip.
  - `smogame.so`: O módulo de jogo transpilado antecipadamente a partir do dump do usuário via Clang NDK.
* **Proporção de Tela Nativa (Hor+):** Patch customizado de expansão de campo de visão horizontal para telas modernas (como 20:9 no Xiaomi Poco F7), impedindo achatamento e barras pretas.
* **AAudio Exclusive Sink:** Áudio de resposta instantânea (~32ms) contornando wrappers lentos como OpenSL ES.
* **Frame Pacing Não-Blocante:** Eliminação total de loops de espera da CPU (*busy-wait*) através de temporização POSIX precisa (`sleep_until`).
* **Unswizzler SIMD ARM NEON de 128 Bits:** Aceleração vetorial nativa (`vld1q_u8`/`vst1q_u8`) para texturas Tegra Block Linear.
* **Mesa Turnip 8xx UMA Zero-Copy:** Mapeamento direto de memória visível ao host evitando cópias repetitivas de buffers em GPUs Adreno 825/830.

---

## Especificações do Hardware Alvo

* **Hardware Primário:** Xiaomi Poco F7
  - **SoC:** Qualcomm Snapdragon 8s Gen 4 (SM8735, 4nm TSMC)
  - **CPU:** 8 núcleos Qualcomm Kryo ARMv9 (1x Cortex-X4 @ 3.21 GHz + 7x Cortex-A720)
  - **GPU:** Qualcomm Adreno 825
  - **Driver:** Mesa Turnip Custom Adreno 8xx via `libadrenotools`
  - **SO:** Android 17 / HyperOS 4
* **Hardware Co-Suportado:** Snapdragon 8 Elite (SM8750 / Adreno 830)

---

## Estrutura do Projeto

```text
smo-recomp-android/
├── runtime/              # Camada de compatibilidade móvel (libmain.so)
│   ├── audio/            # AAudio native exclusive sink
│   ├── gfx/              # Renderizador Vulkan 1.3 e frame pacing
│   ├── textures/         # Unswizzler SIMD ARM NEON de 128 bits
│   ├── platform/         # Ciclo de vida Android e ponte SDL3
│   ├── core/             # Afinidade de threads e fastmem
│   └── drivers/          # libadrenotools e injeção do Turnip
├── lifter/               # Transpilador AOT AArch64 -> C com RECOMP_PATCHES
├── games/                # Símbolos do jogo e módulos transpilados
├── patches/              # Patches de proporção de tela nativa Hor+ 20:9
├── android/              # Projeto Gradle Android e overlay de toque
└── docs/                 # Documentação técnica, diários e estado do projeto
```

---

## Licença

Este projeto é licenciado sob a **GNU General Public License v3.0 (GPL-3.0)**. Consulte o arquivo [LICENSE](LICENSE) para obter detalhes completos.
