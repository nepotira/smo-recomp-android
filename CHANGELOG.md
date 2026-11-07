# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Project initialized from absolute scratch on branch `main`.
- Clean directory layout: `runtime/`, `lifter/`, `games/`, `patches/`, `android/`, and `docs/`.
- Bilingual documentation standard: `README.md` (English primary) and `README.pt-BR.md` (Brazilian Portuguese).
- Strict `.gitignore` preventing leaks of game ROMs, keys, and build artifacts.
- Target hardware profile established for Xiaomi Poco F7 (Qualcomm Snapdragon 8s Gen 4 / Adreno 825).
- Dual library architecture planned (`libmain.so` runtime host + `smogame.so` lifted game code).
- Integrated 7 silicon optimization blueprints (AAudio 32ms sink, Vulkan sleep_until pacing, ARM NEON SIMD 128-bit unswizzler, UMA Zero-Copy with Mesa Turnip 8xx).
- Complete Android Gradle NDK project structure in `android/` (`settings.gradle.kts`, `build.gradle.kts`, `app/build.gradle.kts`).
- `AndroidManifest.xml` configured for Vulkan 1.3, sensorLandscape, and immersive gaming.
- `LauncherActivity.java` interface with SAF RomFS picker, Turnip 8xx driver toggle, FPS selector, and Hor+ 20:9 native aspect ratio switch.
- `GameActivity.java` NativeActivity host with display cutout handling (`SHORT_EDGES`) and 120Hz touch boost suppression to prevent SoC overheating.
- Standalone APK build and packaging orchestration script `android/build_apk.sh`.
