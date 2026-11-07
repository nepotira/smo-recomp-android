# Auditoria Comparativa de Projeto-Base: Upstream vs. SMO-Recomp-Android

Este documento registra a análise técnica comparativa entre o projeto de referência comunitário ([`Deez-coder/SMO-android-port`](https://github.com/Deez-coder/SMO-android-port), tag `B0.2`) e o nosso repositório ([`nepotira/smo-recomp-android`](https://github.com/nepotira/smo-recomp-android)), focado no hardware Xiaomi Poco F7 (Snapdragon 8s Gen 4 / Adreno 825 / HyperOS 4).

---

## 1. Referência Funcional
* **Projeto de Referência:** `Deez-coder/SMO-android-port`
* **Versão de Referência Ativa:** Release `B0.2` (`odyssey-0.2.apk`, 23.71 MiB).
* **Comportamento na Referência:** O jogo inicializa normalmente a partir dos dados descriptografados extraídos em `/sdcard/Android/data/com.mobileodyssey.game/files/`.

---

## 2. Vetores de Conflito Específicos do Projeto

### Vetor 1: Identidade, Assinatura e Armazenamento (Scoped Storage)
* **Upstream:** Usa `applicationId = "com.mobileodyssey.game"` e chave de assinatura de release própria gerada em `release.keystore`.
* **Nossa Base:** Inicialmente compilou com `com.nepo.smorecomp`, que foi isolado pelo Android 11+ em uma sandbox vazia (`/sdcard/Android/data/com.nepo.smorecomp/files/`).
* **Conflito Identificado:** Trocar simplesmente o `applicationId` para `com.mobileodyssey.game` no nosso APK sem a mesma chave de assinatura impede instalar por cima (erro `INSTALL_FAILED_UPDATE_INCOMPATIBLE`). Além disso, desinstalar o app funcional apaga permanentemente a pasta `/sdcard/Android/data/com.mobileodyssey.game/files/` com todos os gigabytes de dados extraídos pelo operador.
* **Resolução Obrigatória:** Manter a identidade do nosso app bem documentada e exigir backup prévio dos dados antes de qualquer alteração de pacote.

### Vetor 2: ABI, Símbolos e Biblioteca Nativa (`libsmo.so`)
* **Upstream:** Monta uma biblioteca monolítica `libsmo.so` (71 MB) contendo o runtime C, os 257 objetos `.obj` de código do jogo gerados pelo lifter e o shaderc.
* **Nossa Base:** Inicialmente separou em `libmain.so` com stubs fracos em `table_stubs.c` para permitir linkagem sem os objetos do jogo.
* **Risco Legal e de Licença:** O binário `libsmo.so` de 71 MB contém código transpilado do jogo original (obra derivada de software proprietário). A regra da Constituição (§8 e skill `licencas-e-direitos`) proíbe expressamente a publicação de código recompilado de jogos comerciais em releases públicas.
* **Resolução Obrigatória:** O repositório e as releases públicas devem distribuir apenas o runtime limpo.

### Vetor 3: Pipeline Gráfico Vulkan e Batching de Draw Calls
* **Upstream:** Introduziu fragmentação de draws em ~14 submissões menores por frame em `nvn_vk_draw.c`.
* **Análise de Silício:** No Adreno 750 (Galaxy S24 Ultra), isso impediu o governor de baixar o clock para 231 MHz. No Adreno 825 (Poco F7 com driver Mesa Turnip 8xx), o comportamento do governor do HyperOS 4 precisa ser medido com logs reais de GPU antes de assumir paridade.

### Vetor 4: CPU Scheduler e Afinidade de Núcleos
* **Upstream:** Usa rotina simples de "CPU Boost Always" em `android_perf.c`.
* **Nossa Base:** Possui afinidade explícita travando os workers de shaders no cluster de 5 núcleos Cortex-A720 (Mid/Performance) para blindar o núcleo Prime Cortex-X4 contra thermal throttling severo.
* **Não-Conflito:** Manter a nossa blindagem térmica em `common/thread.cpp`, evitando que o "CPU Boost" genérico superaqueça o Poco F7.

### Vetor 5: Shaders e Precisão Matemática
* **Upstream:** Incorporou patches do `power-moon-recomp` para forçar `NoContraction` (`precise`) em shaders de vértice e geometria (`mx_translate.c` e `vk_shaderc_android.c`).
* **Análise:** Essencial para evitar furos na geometria e falhas de Z-fighting entre depth pass e color pass. Totalmente convergente com a nossa base.
