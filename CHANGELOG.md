# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Native CMake integration for `libmain.so` compiled directly from `runtime/` and bridged dynamically to `libsmo.so` via ARM64 trampolines and dlsym resolution.
- Hardware ARM64 `frecpe` and `frsqrte` inline assembly instructions in `runtime/fpexact.h` replacing scalar fallback loops.

### Changed
- Polled completed GPU compute buffer slots via non-blocking `cb_poll()` at the start of `cb_sync(void)` in `runtime/nvn_vk_draw.c` to prevent redundant `vkWaitForFences` blocking stalls.
- Capped background shader worker threads (`g_pw_nthread`) to at most 3 on Android in `runtime/nvn_vk_m4.c` to prevent thread contention against Kryo CPU cores.
- Enabled asynchronous shader skip across all scenes on Android (`skip_active` decoupled from `fight_recent`) with default `o->shader_skip = 1` in graphics options.
- Restored standalone applicationId (com.nepo.smorecomp) and app label ("SMO Recomp") decoupled from the internal Java namespace (ADR-001).

### Fixed
- Disabled automatic generate_release_notes in GitHub Actions workflow to enforce CHANGELOG-based release notes.
- Eliminated 30-second main thread stalls in `nvnProgramSetShaders` and synchronized `GameActivity`'s "Preparing graphics..." loading overlay with worker shader completion.
- Expanded AAudio buffer capacity to >= 1920 frames (4x burst), adjusted audio ring buffer thresholds, and quadrupled cutscene movie sample queue (`QMAX = 1024`) to eliminate audio stuttering and audio/video desync.

## [0.2.0] - 2026-11-07

### Added
- Native library integration for arm64-v8a: packaged complete 71MB `libsmo.so` containing all 257 lifted game objects and `libadrenotools` hooks (`libfile_redirect_hook.so`, `libgsl_alloc_hook.so`, `libhook_impl.so`, `libmain_hook.so`) for Turnip 8xx custom driver support.
- Upstream binary fetch step in GitHub Actions CI workflow to seamlessly pull compiled native assets during cloud builds without bloating git repository history.
- Detailed missing game file diagnostics in `LauncherActivity`: explicit status indications (`missing: romfs`, `missing: npdm`, `missing: main.elf`) and dialog warnings replacing unhandled black screen deadlocks.
- Established Comparative Audit Methodology in `docs/COMPARATIVE_AUDIT_METHODOLOGY.md` and updated `.agents/memory/LESSONS.md` to prevent architectural collisions between upstream base code and zero-scratch optimizations.

### Changed
- Aligned Android package identifier to `com.mobileodyssey.game` across manifest, Gradle build configurations, and Java source packages to seamlessly share and access existing on-device game installations without redundant file duplication.
- Upgraded graphics defaults on fresh installs: Vulkan Mailbox presentation mode (`VK_PRESENT_MODE_MAILBOX_KHR`), CPU boost Always, Custom scale 100%, FXAA, and 75% sharpening.
- Integrated Beta 0.2 Adreno fragmented GPU draw submissions (~14 submissions per frame in `nvn_vk_draw.c`) preventing GPU governor downclocking and sustaining higher clocks on Snapdragon 8s Gen 4 (SM8735 / Adreno 825).
- Integrated stepped dynamic resolution limiter (`smo_quality.c`) and free frame pacing in 60 FPS mode (`display.c`).

### Fixed
- Fixed black screen deadlock on launch: prevented silent event loop idling in `android_main.c` and blocked invalid launch attempts when required game files are absent.

## [0.1.0] - 2026-11-07

### Added
- Project initialized from absolute scratch on branch `main`.
- Clean directory layout: `runtime/`, `lifter/`, `games/`, `patches/`, `android/`, and `docs/`.
- Bilingual documentation standard: `README.md` (English primary) and `README.pt-BR.md` (Brazilian Portuguese).
- Strict `.gitignore` preventing leaks of game ROMs, keys, and build artifacts.
- Target hardware profile established for Xiaomi Poco F7 (Qualcomm Snapdragon 8s Gen 4 / Adreno 825).
- Dual library architecture implemented (`libmain.so` runtime host + `smogame.so` lifted game code).
- Integrated 7 silicon optimization blueprints (AAudio 32ms sink, Vulkan sleep_until pacing, ARM NEON SIMD 128-bit unswizzler, UMA Zero-Copy with Mesa Turnip 8xx).
- Complete Android Gradle NDK project structure in `android/` (`settings.gradle.kts`, `build.gradle.kts`, `app/build.gradle.kts`).
- `AndroidManifest.xml` configured for Vulkan 1.3, sensorLandscape, and immersive gaming.
- `LauncherActivity.java` interface with SAF RomFS picker, Turnip 8xx driver toggle, FPS selector, and Hor+ 20:9 native aspect ratio switch.
- `GameActivity.java` NativeActivity host with display cutout handling (`SHORT_EDGES`) and 120Hz touch boost suppression to prevent SoC overheating.
- Standalone APK build and packaging orchestration script `android/build_apk.sh`.
- 2D UI Safe-Zone isolation patch in `patches/smo_safe_zone_2d.txt` keeping HUD elements in 16:9 safe bounds on 20:9 screens.
- Ultra-wide touch controls safe margin compensation in `runtime/android/nnpad_android.c`.
- Automated APK zipalign (16KB page alignment) and signing pipeline in `android/sign_apk.sh` with V1/V2/V3 signatures.
- Standalone Python APK alignment utility in `tools/sign_apk.py`.
- Cloud compilation and GitHub Release CI workflow in `.github/workflows/android.yml`.
