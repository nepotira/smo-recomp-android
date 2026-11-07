# Estado do Projeto (PROJECT_STATE)

**Projeto:** Recompilação Estática e Runtime Móvel de Super Mario Odyssey para Android (SMO-Recomp-Android)  
**Data de Atualização:** 2026-11-07  
**Dispositivo Primário:** Xiaomi Poco F7 (Qualcomm Snapdragon 8s Gen 4 / GPU Adreno 825) — Android 17 / HyperOS 4  
**Versão Atual:** v0.0.1-dev (Inicialização do Repositório e Linha de Base Arquitetural)  
**Repositório:** `/home/nepo/Downloads/SWITCH/smo-recomp-android/` (branch `main`)  

---

## 1. Visão Geral
Construção do zero absoluto do projeto de recompilação estática de *Super Mario Odyssey* (v1.4.1) para Android ARM64, estruturado em arquitetura modular desacoplada:
* `libmain.so`: Camada de compatibilidade em tempo real contendo Vulkan 1.3, AAudio nativo de 32ms, pacing não-blocante com `sleep_until`, unswizzler SIMD NEON e injeção do Mesa Turnip 8xx via `libadrenotools`.
* `smogame.so`: Módulo de código de jogo transpilado antecipadamente (AOT) a partir das 54.214 funções do binário original via Clang NDK.
* Patch de aspect ratio nativo Hor+ 20:9 injetado em `al::Projection` para visualização em tela cheia sem esticar.

---

## 2. Estado Atual dos Componentes

| Componente | Estado | Evidência / Verificação |
| :--- | :--- | :--- |
| **Passo 1: Inicialização do Workspace** | **Concluído** | Repositório Git criado, `.gitignore`, documentação bilíngue e esqueleto de pastas. |
| **Passo 2: Runtime Móvel (`libmain.so`)** | **Concluído** | Base Beta 0.2 importada, AAudio Exclusive Sink (~32ms), ARM NEON SIMD 128-bit unswizzler, pacing não-blocante e CMakeLists.txt configurado. |
| **Passo 3: Lifter AOT & Patch 20:9** | Planejado | Próximo passo de execução. |
| **Passo 4: Transpilação `smogame.so`** | Planejado | Script `build_gamelib.sh` a ser implementado. |
| **Passo 5: Projeto Android APK** | Planejado | Estrutura Gradle e SDL3 NativeActivity a ser criada. |
| **Passo 6: Safe-Zone 2D & Empacotamento** | Planejado | Ajuste ortográfico e empacotamento assinado. |
| **Passo 7: Validação em Silício Poco F7** | Planejado | Teste em hardware real e publicação da Release v0.1.0. |
