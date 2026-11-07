# Super Mario Odyssey Recomp for Android (SMO-Recomp-Android)

[English](README.md) | [Português do Brasil](README.pt-BR.md)

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)
[![Platform: Android](https://img.shields.io/badge/Platform-Android%20ARM64-green.svg)](https://developer.android.com)
[![Target: Poco F7](https://img.shields.io/badge/Device-Snapdragon%208s%20Gen%204-orange.svg)](docs/PROJECT_STATE.md)

A high-performance, native Android port of *Super Mario Odyssey* (v1.4.1), built on ahead-of-time static binary recompilation (AOT) with an ultra-low-latency C++20 mobile runtime.

---

## Important Notice

> **This repository contains no copyrighted game code, assets, ROMs, keys, or proprietary Nintendo binaries.**  
> Users must provide their own legally dumped copy of *Super Mario Odyssey* (v1.4.1) from their own Nintendo Switch console. The build process runs locally on your computer or device and never distributes game assets.

---

## Architectural Highlights

* **100% Native ARM64 Execution:** Since the Nintendo Switch (Tegra X1) and modern Android phones share the same AArch64 instruction set architecture, the game runs directly on the device's CPU cores with zero interpreter and zero JIT overhead.
* **Dual Library Architecture:**
  - `libmain.so`: The host application runtime delivering Android OS bindings, Vulkan 1.3 presentation, exclusive AAudio, and Turnip driver loading.
  - `smogame.so`: The lifted game module translated ahead-of-time from the player's own game dump via Clang NDK.
* **True Native Aspect Ratio (Hor+):** Custom horizontal field-of-view expansion patch for modern Android displays (such as 20:9 on Xiaomi Poco F7) preventing geometric distortion and pillarboxing.
* **AAudio Exclusive Sink:** Sub-32ms sound delivery directly bypassing slow OpenSL ES wrappers.
* **Non-Blocking Frame Pacing:** Eliminated CPU spin-loops using high-resolution POSIX timing (`sleep_until`).
* **ARM NEON SIMD Accelerated Texture Unswizzler:** 128-bit vector pipelines (`vld1q_u8`/`vst1q_u8`) for Tegra Block Linear textures.
* **Turnip 8xx UMA Zero-Copy:** Direct host-visible memory mapping eliminating per-frame buffer copies on Qualcomm Adreno 825/830 GPUs.

---

## Target Hardware Specifications

* **Primary Hardware:** Xiaomi Poco F7
  - **SoC:** Qualcomm Snapdragon 8s Gen 4 (SM8735, 4nm TSMC)
  - **CPU:** 8-core Qualcomm Kryo ARMv9 (1x Cortex-X4 @ 3.21 GHz + 7x Cortex-A720)
  - **GPU:** Qualcomm Adreno 825
  - **Driver:** Mesa Turnip Custom Adreno 8xx via `libadrenotools`
  - **OS:** Android 17 / HyperOS 4
* **Secondary Supported Hardware:** Snapdragon 8 Elite (SM8750 / Adreno 830)

---

## Project Structure

```text
smo-recomp-android/
├── runtime/              # Mobile compatibility layer (libmain.so)
│   ├── audio/            # AAudio native exclusive sink
│   ├── gfx/              # Vulkan 1.3 renderer and frame pacing
│   ├── textures/         # ARM NEON SIMD 128-bit unswizzler
│   ├── platform/         # Android lifecycle and SDL3 bridge
│   ├── core/             # CPU thread affinity and fastmem
│   └── drivers/          # libadrenotools and Turnip injection
├── lifter/               # AOT AArch64 -> C transpiler with RECOMP_PATCHES
├── games/                # Game specific symbols and lifted modules
├── patches/              # Hor+ 20:9 native display aspect ratio patches
├── android/              # Android Gradle project and touch controls overlay
└── docs/                 # Technical documentation, logs, and project state
```

---

## License

This project is licensed under the **GNU General Public License v3.0 (GPL-3.0)**. See the [LICENSE](LICENSE) file for complete details.
