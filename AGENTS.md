# AGENTS.md — Perfil e Diretrizes do Projeto SMO-Recomp-Android

> **AVISO A QUALQUER AGENTE DE IA ENTRANDO NESTA SESSÃO:**  
> Este arquivo define o perfil de engenharia, restrições imutáveis de silício e os padrões de desenvolvimento deste repositório (`smo-recomp-android`). Leia este documento integralmente no início da sessão junto com `docs/PROJECT_STATE.md`.

---

## 1. Perfil do Projeto
* **Nome do Projeto:** Recompilação Estática e Runtime Móvel de Alta Performance de Super Mario Odyssey para Android (SMO-Recomp-Android).
* **Workspace Raiz:** `/home/nepo/Downloads/SWITCH/smo-recomp-android/`
* **Escopo de Domínio:** este projeto É a recompilação estática e runtime nativo de Super Mario Odyssey para Android ARM64, e NÃO É emulação genérica de Switch 1/2 (Eden/Lichii) nem engine C++ descompilada de PC.
* **Referência Funcional:** `Deez-coder/SMO-android-port` (tag `B0.2`, APK funcional `odyssey-0.2.apk`). Auditoria comparativa e vetores de conflito documentados em `docs/COMPARATIVE_AUDIT.md`.
* **Alvo de Jogo:** *Super Mario Odyssey* (v1.4.1), 54.214 funções mapeadas via tabela de símbolos RTTI e `.eh_frame`.
* **Arquitetura de Compilação:** Dual `.so` (`libmain.so` como runtime hospedeiro em `runtime/` + `smogame.so` gerado por AOT em `games/smo/`).
* **Operador / Desenvolvedor:** Nepo (@nepotira).
* **Idioma:** Português do Brasil nas conversas e documentações; commits, código e releases em inglês internacional.

---

## 2. Hardware e Ambiente Técnico Real (Imutável)
* **Dispositivo Primário:** Xiaomi Poco F7
  - **SoC:** Qualcomm Snapdragon 8s Gen 4 (SM8735), 4nm TSMC.
  - **CPU:** 8 núcleos Qualcomm Kryo com arquitetura padrão **ARMv9 Cortex puro** (NÃO possui núcleos Oryon):
    - 1x ARM Cortex-X4 (Prime) @ 3.21 GHz
    - 3x ARM Cortex-A720 (Performance) @ 3.00 GHz
    - 2x ARM Cortex-A720 (Mid) @ 2.80 GHz
    - 2x ARM Cortex-A720 (Efficiency) @ 2.02 GHz
  - **GPU:** Qualcomm Adreno 825
  - **Driver Gráfico Ativo:** Mesa Turnip Custom Adreno 8xx mantido em `nepotira/turnip-8xx-nepo` carregado via `libadrenotools`.
  - **Tela:** $2712 \times 1220$ pixels (Proporção $20:9 \approx 2.22295$).
  - **SO:** HyperOS 4 / Android 17 (Kernel Linux AArch64).
* **Hardware Co-Suportado:** Snapdragon 8 Elite (SM8750 / Adreno 830).

---

## 3. Os 7 Pilares de Otimização em Silício Integrados no Runtime
1. **AAudio Native Exclusive Sink:** Latência de ~32ms em modo exclusivo sem OpenSL ES.
2. **Pacing Vulkan Não-Blocante:** Sincronização precisa via `std::this_thread::sleep_until()`, eliminando spin-loops de CPU.
3. **Unswizzler SIMD ARM NEON de 128 Bits:** Instruções nativas `vld1q_u8` e `vst1q_u8` para layout Tegra Block Linear.
4. **Mesa Turnip 8xx UMA Zero-Copy:** Mapeamento de memória unificada host-visible eliminando cópias de 490MB de buffers por segundo.
5. **Afinidade de CPU:** Núcleo Prime Cortex-X4 exclusivo para o loop do jogo; workers fixados no cluster Cortex-A720.
6. **Patch Hor+ Nativo 20:9:** Substituição via `RECOMP_PATCHES` em `al::getSceneFrameBufferMainAspect` (`SceneUtil.cpp:140`) e `al::Projection::setAspect` (`Projection.cpp:34`).
7. **Safe-Zone de HUD 2D:** Render pass ortográfico centralizado em 16:9 preservando a interface de cortes da câmera frontal.

---

## 4. Regras Mandatórias de Operação
1. **Compilação: GitHub Primeiro e Somente por CLI (`gh`):**
   - Nunca compilar APK pesado localmente. Disparar via GitHub Actions por CLI: `gh workflow run`.
   - Acompanhar a compilação até o fim com monitoramento contínuo.
2. **Honestidade e Evidência:**
   - Nunca afirmar que algo funciona ou foi testado sem evidência física de comando ou relato do usuário. Sem evidência, escreva `not tested`.
3. **Proibição Absoluta de Suposições e Alucinações Técnicas:**
   - Todo diagnóstico ou limite de hardware deve ser fundamentado em evidência direta conferida no código-fonte real.
