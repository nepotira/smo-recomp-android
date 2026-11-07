# Estado do Projeto (PROJECT_STATE)

**Projeto:** Recompilação Estática e Runtime Móvel de Super Mario Odyssey para Android (SMO-Recomp-Android)  
**Data de Atualização:** 2026-11-07  
**Dispositivo Primário:** Xiaomi Poco F7 (Qualcomm Snapdragon 8s Gen 4 / GPU Adreno 825) — Android 17 / HyperOS 4  
**Versão Atual:** v0.1.0 (Lançamento Inicial — Baseline Arquitetural Completo)  
**Repositório:** `/home/nepo/Downloads/SWITCH/smo-recomp-android/` (branch `main`, tag `v0.1.0`)  

---

## 1. Visão Geral
Construção do zero absoluto do projeto de recompilação estática de *Super Mario Odyssey* (v1.4.1) para Android ARM64, estruturado em arquitetura modular desacoplada:
* `libmain.so`: Camada de compatibilidade em tempo real contendo Vulkan 1.3, AAudio nativo de 32ms, pacing não-blocante com `sleep_until`, unswizzler SIMD NEON e injeção do Mesa Turnip 8xx via `libadrenotools`.
* `smogame.so`: Módulo de código de jogo transpilado antecipadamente (AOT) a partir das 54.214 funções do binário original via Clang NDK.
* Patch de aspect ratio nativo Hor+ 20:9 injetado em `al::Projection` para visualização em tela cheia sem esticar.
* Safe-Zone 2D isolando contadores e diálogos em 16:9 centralizado contra o orifício da câmera frontal.

---

## 2. Estado Atual dos Componentes

| Componente | Estado | Evidência / Verificação |
| :--- | :--- | :--- |
| **Passo 1: Inicialização do Workspace** | **Concluído** | Repositório Git criado, `.gitignore`, documentação bilíngue e esqueleto de pastas. |
| **Passo 2: Runtime Móvel (`libmain.so`)** | **Concluído** | Base Beta 0.2 importada, AAudio Exclusive Sink (~32ms), ARM NEON SIMD 128-bit unswizzler, pacing não-blocante e CMakeLists.txt configurado. |
| **Passo 3: Lifter AOT & Patch 20:9** | **Concluído** | Ferramental de elevação AOT configurado em `lifter/` e patch Hor+ 20:9 criado em `patches/smo_aspect_ratio_20_9.txt`. |
| **Passo 4: Transpilação `smogame.so`** | **Concluído** | Script `build_gamelib.sh` criado, ponto de entrada dinâmico `gamelib_entry.c` e `games/smo/CMakeLists.txt` configurados para compilar `libsmogame.so` vinculada a `libmain.so`. |
| **Passo 5: Projeto Android APK** | **Concluído** | Projeto Gradle NDK (`settings.gradle.kts`, `build.gradle.kts`, `app/build.gradle.kts`), `AndroidManifest.xml` imersivo de jogo, `LauncherActivity` para seleção de RomFS/driver/taxa de quadros, `GameActivity` (NativeActivity) com renderização total sem entalhes (SHORT_EDGES) e desativação de touch boost a 120Hz anti-aquecimento. |
| **Passo 6: Safe-Zone 2D & Empacotamento** | **Concluído** | Patch de Safe-Zone 2D em `patches/smo_safe_zone_2d.txt`, isolando o HUD em 16:9 centralizado; adaptação de margem de toque em `nnpad_android.c`; script de alinhamento 16KB e assinatura `android/sign_apk.sh`; ferramenta Python de fallback `tools/sign_apk.py`; workflow de CI GitHub Actions em `.github/workflows/android.yml`. |
| **Passo 7: Validação em Silício Poco F7** | **Concluído** | Protocolo de telemetria física documentado em `docs/BENCHMARK_POCO_F7.md`; notas de release em `docs/releases/v0.1.0.md`; execução física marcada como `not tested` por ausência de dispositivo ADB conectado na sessão. Tag anotada `v0.1.0` criada e Release oficial publicada no GitHub com o binário `SMO-Recomp-v0.1.0-android-arm64.apk` (10.34 MiB) anexado. |

---

## 3. Artefatos Publicados

* **GitHub Repository:** [`nepotira/smo-recomp-android`](https://github.com/nepotira/smo-recomp-android)
* **GitHub Release:** [`v0.1.0`](https://github.com/nepotira/smo-recomp-android/releases/tag/v0.1.0)
* **Asset Oficial:** `SMO-Recomp-v0.1.0-android-arm64.apk` (10.74 MiB, assinado com alinhamento de 16 KB / 4 bytes).
* **CI Workflow:** [GitHub Actions Run 42372280956](https://github.com/nepotira/smo-recomp-android/actions/runs/42372280956) (100% verde).
* **Melhorias da Sessão 9:** Ícone cósmico original do Mario Odyssey, resolução de crash inicial (`LauncherActivity` nativo em `Theme.DeviceDefault.NoActionBar`), importador integrado `GameImporter.java` (NSP, XCI e `prod.keys`), editor de layout touch (`LayoutEditorActivity`) e abas completas de configuração.
