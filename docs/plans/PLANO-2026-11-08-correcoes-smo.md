# PLANO DE CORREÇÃO E ENGENHARIA — `smo-recomp-android` (2026-11-08)

**Projeto:** `smo-recomp-android` (`/home/nepo/Downloads/SWITCH/smo-recomp-android`)  
**Repositório Remoto:** `https://github.com/nepotira/smo-recomp-android`  
**Dispositivo Alvo:** Xiaomi Poco F7 — Qualcomm Snapdragon 8s Gen 4 (SM8735, 1x Cortex-X4 @ 3.21 GHz + 7x Cortex-A720) — GPU Adreno 825 — Driver Customizado Mesa Turnip 8xx (`nepotira/turnip-8xx-nepo`) — HyperOS 4 / Android 17  
**Arquiteto do Plano:** Modelo Forte (Revisão Integral de Código)  
**Executor Designado:** Gemini 3.8 Flash (Execução Literal Determinística por Grupos e Portões)  
**Data:** `2026-11-08`

---

## 1. CONTRATO DO EXECUTOR E LIMITES DO FLASH (OBRIGATÓRIO PARA O GEMINI 3.8 FLASH)

Você é o **Executor Determinístico (Gemini 3.8 Flash)**. Você **NÃO** deve tomar decisões de arquitetura, **NÃO** deve improvisar e **NÃO** deve alterar uma única vírgula fora do que está escrito literalmente neste plano.

### 1.1. Limites Estritos do Gemini 3.8 Flash
1. **Não decide quando compilar:** Só compila exatamente nos pontos determinados pelo portão do grupo no plano.
2. **Não junta grupos:** Executa apenas um grupo (`G-N`) por vez, mantendo os grupos, a ordem e a numeração de tarefas intocados.
3. **Não pula portão:** Ao final de cada grupo, é obrigatório executar o **Portão do Grupo (`G-N`)**, entregar o **Relatório de Grupo** e **PARAR IMEDIATAMENTE** aguardando a resposta explícita do usuário (`"OK"` ou `"FALHOU"` com o log).
4. **Não publica release:** Builds intermediários nos portões são apenas artefatos do GitHub Actions CI. A release oficial (`v0.3.0`) só ocorre depois do último grupo (`G-8`), seguindo a skill `release-semver`, com a decisão de direitos autorais resolvida.
5. **Não trata `"compilou"` como `"funciona"`:** CI verde e APK gerado nunca significam que funciona. Enquanto o usuário não responder `"OK"` no teste físico do portão, a funcionalidade permanece como `not tested`.
6. **Não segue para o próximo grupo sem a resposta do usuário:** Nunca inicie o próximo grupo nem faça merge do ramo do grupo na `main` antes de receber o `"OK"` explícito do usuário.

### 1.2. Regra de Ramos (Tarefa → Grupo → `main`)
1. **Um ramo por tarefa:** Cada tarefa (`T-NN`) é feita no seu próprio ramo individual (ex.: `refactor/01-restore-nepo-application-id`).
2. **Merge no ramo do grupo:** Ao concluir cada tarefa com `PASSOU`, o ramo da tarefa é mesclado (`git merge --no-ff`) no **ramo do grupo** correspondente (`group/g0-safety-and-identity`, `group/g2-runtime-bridge`, etc.).
3. **Merge na `main` somente após o `"OK"` do usuário:** O ramo do grupo **SÓ** é mesclado na `main` depois que o usuário testar o APK do portão no celular e responder `"OK"`. **Nunca faça commit direto na `main`.**

### 1.3. Regra de Execução de Cada Portão de Grupo (`G-0` a `G-8`)
Ao chegar no fim de cada grupo, o Gemini 3.8 Flash deve seguir literalmente estes 5 passos (`a` a `e`):
- **a.** Terminar todas as tarefas do grupo, cada uma com relatório individual `PASSOU`. Se qualquer tarefa falhar na pré-checagem, edição ou pós-checagem, **PARE IMEDIATAMENTE**, reporte e **não compile**.
- **b.** Compilar pelo `gh` (`gh workflow run android.yml --repo nepotira/smo-recomp-android --ref <ramo-do-grupo>`), monitorar até terminar (`gh run watch`), baixar o APK e informar o link do artefato no GitHub Actions e o `sha256sum` do arquivo `.apk`.
- **c.** **NÃO publicar release** (nenhum `gh release create` nem tag `v*` nos grupos intermediários).
- **d.** Entregar ao usuário o **Relatório de Grupo** contendo o guia de teste passo a passo no celular (instalar o APK, passos um por vez, o que deve aparecer, métricas a observar e comandos `adb logcat` + `log.txt` para coletar o log). Depois **PARAR e ESPERAR** a resposta do usuário (`"OK"` ou `"FALHOU"` com o log).
- **e.** **Com `"OK"` do usuário:** Fazer o merge do ramo do grupo na `main` (`git checkout main && git merge --no-ff <ramo-do-grupo> && git push origin main`) e só então iniciar o grupo seguinte. **Com `"FALHOU"` do usuário:** Analisar o log enviado, reverter a **ÚLTIMA** tarefa do grupo usando o comando exato escrito no portão (`git revert --no-edit <commit>`), recompilar pelo `gh` e testar de novo, voltando uma tarefa por vez até achar a culpada. **Nunca avance com grupo falho.**

### 1.4. Regra de Medição nos Grupos de Desempenho
- Em grupos de desempenho (`G-5`), o portão inclui a medição **DEPOIS**, comparada com a base já definida no plano (`G-1` / `T-02`), na mesma cena, mesmo aparelho (Xiaomi Poco F7) e mesmo método.
- **Sem ganho mensurável:** O Flash reverte a tarefa imediatamente com o comando exato do plano (`git revert --no-edit HEAD`) e registra `"SEM GANHO — REVERTIDO"` no `docs/JOURNAL.md` e `docs/BENCHMARK_POCO_F7.md`.

---

## 2. TABELA DE COBERTURA TOTAL DE PEDIDOS (`P-1` a `P-9`)

Nenhum pedido do usuário foi omitido ou descartado:

| ID do Pedido | Descrição Completa do Pedido do Usuário | Seção do Relatório | Tarefas que Atendem | Grupo / Portão |
| :--- | :--- | :--- | :--- | :--- |
| **P-1** | Compilar de fato o nosso `runtime/` C dentro do APK (`libmain.so`) sobrepondo o runtime da `libsmo.so` monolítica sem precisar regerar as 74.822 funções do jogo | Seção 0 + Resumo #1 | **T-03** | **Grupo G-2** (`Portão G-2`) |
| **P-2** | Corrigir a tela `"Preparing graphics..."` que some cedo demais no 1º boot e eliminar o congelamento de até 30s na tela dos Joy-Cons (`SMO_VK_FRAGONLY=0` e remoção do `WD_SLEEP` de 30s em `nvnProgramSetShaders`) | Seção 1A, 1B + Resumo #2 | **T-04** | **Grupo G-3** (`Portão G-3`) |
| **P-3** | Resolver o desempenho baixo (33–44 FPS) e travadas em jogo no Poco F7 (Turnip 8xx), com medição de baseline e mudanças isoladas 1 a 1 (`skip_active` assíncrono real, redução de contenção de `g_sh_lock`, `fpexact.h` com instruções hardware ARM64 `FRSQRTE`/`FRECPE`, e `cb_poll` preventivo em `cb_sync`) | Seção 2A, 2B + Resumo #2 e #3 | **T-02** (Baseline), **T-05**, **T-06**, **T-07**, **T-08** | **Grupo G-1** (`Portão G-1`) e **Grupo G-5** (`Portões G-5A..G-5D`) |
| **P-4** | Corrigir a cutscene de introdução (tela preta com som adiantado, depois vídeo sem som, depois 10–40 FPS travando) aumentando a fila do decodificador (`QMAX = 1024`) em `android_movie.c` + desbloqueio de shaders da `T-04` | Seção 3A + Resumo #4 | **T-04**, **T-09** | **Grupo G-3** e **Grupo G-4** (`Portão G-4`) |
| **P-5** | Corrigir o áudio soluçando e craquelando (`AAudio` buffer de `2 * burst` para `>= 1920 frames / 4 * burst` em `android_main.c` e ajuste de `RING_START` / `RING_TARGET` em `smo_audio.c`) | Seção 3B + Resumo #4 | **T-09** | **Grupo G-4** (`Portão G-4`) |
| **P-6** | Implementar a chave **Native Fullscreen Resolution** no Launcher que detecta dinamicamente a resolução e proporção exatas da tela do celular em que está rodando, preenchendo 100% da tela sem barras pretas e sem esticar a imagem (câmera 3D Hor+ em `f_40009cf324` e `f_40009c06f8`) | Seção 4 + Resumo #5 | **T-10** | **Grupo G-6** (`Portão G-6`) |
| **P-7** | Restaurar imediatamente a identidade própria do nosso aplicativo (`applicationId = "com.nepo.smorecomp"`, `android:label="SMO Recomp"`), separando `namespace` Java de `applicationId`, documentando em ADR-001 e iniciando o app do zero (sem copiar dados antigos de outro pacote) | Resumo #6 + Decisão **D-1B** | **T-01** | **Grupo G-0** (`Portão G-0`) |
| **P-8** | Decisão sobre `libsmo.so` no CI/Releases e texto do `README.md` | Regra I + Decisão **D-2C** | **T-12** (Dispensada por **D-2C**) | **Grupo G-7** (`Portão G-7`) |
| **P-9** | Resolver todas as pendências anteriores registradas no `JOURNAL.md` e `PROJECT_STATE.md`: (a) remover `generate_release_notes: true` do `.github/workflows/android.yml`; (b) corrigir o endereço real `0x009cf324` em `patches/smo_aspect_ratio_20_9.txt`; (c) preencher `docs/BENCHMARK_POCO_F7.md` com dados reais medidos no Poco F7 | `JOURNAL.md` (Sessões 3, 7 e 11) | **T-00**, **T-10**, **T-13** | **Grupo G-0**, **Grupo G-6** e **Grupo G-8** (`Portão G-8`) |

---

## 3. CLASSIFICAÇÃO RIGOROSA DE EVIDÊNCIA (COMPROVADO vs. HIPÓTESE)

| # | Afirmação Técnica | Rótulo | Evidência no Código / Método de Confirmação |
| :--- | :--- | :--- | :--- |
| **E-0** | O código C da nossa pasta `runtime/` **não** foi compilado no APK `v0.2.0`; o APK executa o binário monolítico `libsmo.so` baixado de `Deez-coder/SMO-android-port` (`B0.2`). | **COMPROVADO** | `android/app/build.gradle.kts` (sem bloco `externalNativeBuild`), `.github/workflows/android.yml:61-63` (baixa `odyssey-0.2.apk` e copia `libsmo.so`), `AndroidManifest.xml:61` (`android.app.lib_name="smo"`). |
| **E-1** | As 74.822 funções `f_40*` em `libsmo.so` (`0x0143c6e0..0x03c0e910`) são *stateless* (não acessam `.data`/`.bss` via `ADRP`) e só saltam para fora do seu bloco em **7 funções de runtime** (`stx_diag`, `call_indirect`, `guest_trap`, `host_import`, `__wrap_f_400072e61c`, `__wrap_f_4000731fdc`, `__wrap_f_4000a58394`). | **COMPROVADO** | Varredura binária completa das instruções `BL`/`B` e `ADRP` do segmento `.text` de `libsmo.so` via `readelf -l` e `nm -D`. |
| **E-2** | A tela `"Preparing graphics..."` (`GameActivity.showLoading()`) fecha 600 ms após o 1º frame quando `pipes.bin` não existe (`st[2] == 0`), mesmo com milhares de shaders ainda na fila `g_jq_queued`. | **COMPROVADO** | `GameActivity.java:106-108` (`if (shownAt >= 0 && now - shownAt > 600) { hideLoading(); return; }`) e `runtime/nvn_vk_m4.c:1271-1275` (`*total = g_pw_n`, que é `0` sem `pipes.bin`). |
| **E-3** | Durante a tela dos Joy-Cons, `nvn_vk_program_set` tem `SMO_VK_FRAGONLY` ativo por padrão (`on = 1`, dobrando os estágios no boot de 3.872 para 7.740) e bloqueia a thread chamadora em `WD_SLEEP` por até 30 segundos sempre que `g_jq_queued - g_jq_done > g_pw_nthread`. | **COMPROVADO** | `runtime/nvn_vk_m4.c:1777` e `1803-1809` (`while ((g_jq_queued - g_jq_done > g_pw_nthread ...) && tk() - t0 < 30 * fq.QuadPart) WD_SLEEP(&g_sh_cv, &g_sh_lock, 100);`). |
| **E-4** | Remover o `WD_SLEEP` de 30s em `nvn_vk_program_set` e desligar `SMO_VK_FRAGONLY` reduzirá drasticamente o tempo de espera na tela dos Joy-Cons no Poco F7. | **HIPÓTESE** | Será confirmada no **Portão G-3 (`T-04`)** cronometrando no Poco F7 o tempo em segundos desde o aparecimento da tela dos Joy-Cons até a entrada na tela título do Mario (Antes vs. Depois). |
| **E-5** | O celular quase não esquenta e oscila entre 33 e 44 FPS porque a thread de renderização dorme em bloqueios síncronos (`shader_wait = hold`, `skip_active()` restrito a `fight_recent()`, contenção em `g_sh_lock` com os 6 workers do Container Scan e `cb_sync()`). | **HIPÓTESE** | Os bloqueios existem no código (`nvn_vk_m4.c:410`, `2390`, `nvn_vk_draw.c:2270`), mas seu impacto exato no FPS e na temperatura do Poco F7 é **HIPÓTESE** até medirmos cada mudança isoladamente no **Grupo G-5 (`T-05`, `T-06`, `T-08`)** contra a baseline da **T-02**. |
| **E-6** | A emulação escalar de `recip_sqrt_estimate` (loop `while` de até 256 iterações em `runtime/fpexact.h:26-32`) degrada o tempo de CPU na física/animação em comparação à instrução ARM64 nativa `FRSQRTE` (`frsqrte`). | **HIPÓTESE** | O loop escalar em `fpexact.h:26-32` é **COMPROVADO** no código, mas o ganho real de FPS no Poco F7 é **HIPÓTESE** até ser medido isoladamente na **T-07**. |
| **E-7** | A dessincronia da cutscene (áudio na frente com tela preta, depois vídeo sem som, depois 10–40 FPS) e os soluços do áudio decorrem do bloqueio de shaders na abertura da cena + buffer AAudio de apenas `2 * burst` (`android_main.c:293`) + threshold de partida `RING_START` em `smo_audio.c:200` + `QMAX = 256` em `android_movie.c:49`. | **HIPÓTESE** | As constantes são **COMPROVADAS** no código; a eliminação dos estalos (`underruns`) e da dessincronia será verificada em hardware no **Portão G-4 (`T-09`)**. |
| **E-8** | A função `al::getSceneFrameBufferMainAspect` no binário v1.4.1 está em `0x40009cf324` (`f_40009cf324`) e `al::Projection::setAspect` está em `0x40009c06f8` (`f_40009c06f8`), enquanto o passe de apresentação em `nvn_vk_draw.c:5460-5473` força barras pretas 16:9. | **COMPROVADO** | Tabela `dyn_syms` de `libsmo.so` (`_ZN2al29getSceneFrameBufferMainAspectEPKNS_5SceneE = 0x9cf324`, `_ZN2al10Projection9setAspectEf = 0x9c06f8`) e `runtime/nvn_vk_draw.c:5460-5473`. |

---

## 4. DECISÕES DO USUÁRIO JÁ TOMADAS E VINCULANTES (`D-1` e `D-2`)

### Decisão `D-1`: **ESCOLHIDA OPÇÃO `D-1B` COM INICIALIZAÇÃO DO ZERO (SEM COPIAR DADOS ANTIGOS)**
* **Determinação exata do usuário:**
  > *"D1-B. E não vou copiar nenhum dado desses 5gb aí não. É instalar o apk, iniciar o app do zero como qualquer pessoa faria do zero."*
* **Regra para o Flash:** Executar a restauração do `applicationId = "com.nepo.smorecomp"` e `android:label="SMO Recomp"` na **T-01** (`Grupo G-0`), sem copiar nada da pasta antiga `com.mobileodyssey.game`. O usuário instalará o APK do nosso pacote (`com.nepo.smorecomp`) no Portão `G-0` e fará a configuração inicial dentro do próprio app **SMO Recomp** do zero (selecionando `prod.keys`, o `.nsp`/`.xci` da ROM e o `.zip` do driver Turnip customizado).

### Decisão `D-2`: **ESCOLHIDA OPÇÃO `D-2C` (NÃO TOCAR EM NADA SOBRE `libsmo.so` E `README.md`)**
* **Determinação exata do usuário:**
  > *"D2-C"* (Deixar tanto o workflow quanto o `README.md` exatamente como estão hoje, mantendo `libsmo.so` empacotada no APK e sem alterar os avisos do `README.md`).

---

## 5. TABELA DE ORDEM DE EXECUÇÃO, GRUPOS, RAMOS E DEPENDÊNCIAS

| Grupo / Portão | Ramo do Grupo | Tarefa | Ramo da Tarefa | Título Curto | Pedido | Depende de | Risco |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **G-0 (Fase 0: Segurança e Identidade)** | `group/g0-safety-and-identity` | **T-00** | `chore/00-safety-tag-and-ci-notes` | Tag de segurança, limpeza da árvore e remoção de `generate_release_notes` no CI | P-9 | — | Baixo |
| **G-0 (Fase 0: Segurança e Identidade)** | `group/g0-safety-and-identity` | **T-01** | `refactor/01-restore-nepo-application-id` | Restaurar `applicationId = "com.nepo.smorecomp"` (`D-1B`), gravar ADR-001 e iniciar app do zero | P-7 | T-00 | Baixo |
| **G-1 (Fase 1: Baseline)** | `group/g1-baseline` | **T-02** | `docs/02-baseline-measurements` | Coleta da Baseline Factual no Poco F7 em `com.nepo.smorecomp` | P-2, P-3, P-4, P-5 | Portão G-0 ("OK") | Baixo |
| **G-2 (Fase 2: Ativação do Runtime)** | `group/g2-runtime-bridge` | **T-03** | `feat/03-compile-custom-runtime-bridge` | Compilar nosso `runtime/` (`libmain.so`) e vincular via ponte dinâmica + trampolins ARM64 à `libsmo.so` | P-1 | Portão G-1 ("OK") | Médio |
| **G-3 (Fase 3: Boot e Joy-Cons)** | `group/g3-boot-joycons` | **T-04** | `fix/04-joycon-stall-and-loading-screen` | Destravar a tela dos Joy-Cons e sincronizar a tela `"Preparing graphics..."` | P-2, P-4 | Portão G-2 ("OK") | Baixo |
| **G-4 (Fase 4: Áudio e Vídeo)** | `group/g4-audio-video` | **T-09** | `fix/09-audio-underruns-and-cutscene-queue` | Eliminar soluços do áudio (`AAudio 4x burst`) e aumentar fila de vídeo das cutscenes (`QMAX = 1024`) | P-4, P-5 | Portão G-3 ("OK") | Baixo |
| **G-5 (Fase 5: Desempenho 1 a 1)** | `group/g5-performance` | **T-05** | `perf/05-async-shader-skip-always` | Ativar compilação assíncrona real de shaders/pipelines (`skip_active`) fora de combate | P-3 | Portão G-4 ("OK") | Baixo |
| **G-5 (Fase 5: Desempenho 1 a 1)** | `group/g5-performance` | **T-06** | `perf/06-reduce-shader-worker-contention` | Reduzir contenção do mutex `g_sh_lock` causada pelos workers do Container Scan | P-3 | T-05 (medida) | Baixo |
| **G-5 (Fase 5: Desempenho 1 a 1)** | `group/g5-performance` | **T-07** | `perf/07-arm64-hardware-recpe-rsqrte` | Acelerar `recpe` / `rsqrte` em `fpexact.h` com instruções nativas ARM64 (`frecpe` / `frsqrte`) | P-3 | T-06 (medida) | Baixo |
| **G-5 (Fase 5: Desempenho 1 a 1)** | `group/g5-performance` | **T-08** | `perf/08-compute-buffer-nonblocking-poll` | Otimizar `cb_poll` antes de `cb_sync` para evitar `vkWaitForFences` desnecessário em Compute Buffers | P-3 | T-07 (medida) | Médio |
| **G-6 (Fase 6: Tela Cheia Nativa)** | `group/g6-native-fullscreen` | **T-10** | `feat/10-native-fullscreen-resolution-toggle` | Implementar a chave **Native Fullscreen Resolution** (proporção e resolução exatas da tela do aparelho, Hor+ 3D sem barras e sem esticar) | P-6, P-9 | Portão G-5 ("OK") | Médio |
| **G-7 (Fase 7: Registro de Decisões)** | `group/g7-decisions-record` | **T-11**, **T-12** | `docs/12-record-d2c-decision` | `T-11` já concluída na `T-01` (`D-1B`); `T-12` dispensada por `D-2C` (registro formal no `JOURNAL.md`) | P-7, P-8 | Portão G-6 ("OK") | Baixo |
| **G-8 (Fase 8: Fechamento e Release)** | `group/g8-release-v030` | **T-13** | `chore/13-release-v0.3.0` | Consolidar telemetria real em `docs/BENCHMARK_POCO_F7.md`, atualizar `PROJECT_STATE.md` e publicar Release `v0.3.0` | P-9 | Portão G-7 ("OK") | Baixo |

---

## 6. ESPECIFICAÇÃO DETALHADA DOS GRUPOS, TAREFAS (`T-00` a `T-13`) E PORTÕES (`G-0` a `G-8`)

---

# GRUPO G-0 — Fase 0: Segurança e Identidade (`T-00`, `T-01`)
**Ramo do Grupo:** `group/g0-safety-and-identity`  
```bash
cd /home/nepo/Downloads/SWITCH/smo-recomp-android && git checkout main && git checkout -b group/g0-safety-and-identity
```

### T-00 — Criar tag de segurança, limpar árvore Git e desativar `generate_release_notes` no CI
- **Pedido atendido:** P-9 · **Depende de:** — · **Risco:** baixo
- **Ramo da tarefa:** `chore/00-safety-tag-and-ci-notes` (criado a partir de `group/g0-safety-and-identity` e mesclado de volta em `group/g0-safety-and-identity`).
  ```bash
  cd /home/nepo/Downloads/SWITCH/smo-recomp-android && git tag -f -a backup/pre-plan-2026-11-08 7c35ab6 -m "Safety snapshot before SMO correction plan" && git checkout -b chore/00-safety-tag-and-ci-notes
  ```
- **Arquivos permitidos:**
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/.github/workflows/android.yml`
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/docs/TROUBLESHOOTING.md`
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/CHANGELOG.md`
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/docs/JOURNAL.md`
- **Evidência do diagnóstico:** **COMPROVADO** (`/home/nepo/Downloads/SWITCH/smo-recomp-android/.github/workflows/android.yml` continha `generate_release_notes: true`).
- **Pré-checagem:**
  ```bash
  cd /home/nepo/Downloads/SWITCH/smo-recomp-android && grep -n "generate_release_notes:" .github/workflows/android.yml
  ```
  **Saída esperada literal (antes da edição):** `87:          generate_release_notes: true`
- **Mudança:**
  Em `/home/nepo/Downloads/SWITCH/smo-recomp-android/.github/workflows/android.yml`:
  - **Antes (âncora exata):**
    ```yaml
          - name: Create GitHub Release on Tag
            if: startsWith(github.ref, 'refs/tags/v')
            uses: softprops/action-gh-release@v2
            with:
              files: SMO-Recomp-v0.2.0-android-arm64.apk
              generate_release_notes: true
              draft: false
              prerelease: false
    ```
  - **Depois:**
    ```yaml
          - name: Create GitHub Release on Tag
            if: startsWith(github.ref, 'refs/tags/v')
            uses: softprops/action-gh-release@v2
            with:
              files: SMO-Recomp-v0.2.0-android-arm64.apk
              generate_release_notes: false
              draft: false
              prerelease: false
    ```
- **Pós-checagem:**
  ```bash
  cd /home/nepo/Downloads/SWITCH/smo-recomp-android && grep -n "generate_release_notes: false" .github/workflows/android.yml && git tag -l "backup/pre-plan-2026-11-08"
  ```
  **Saída esperada literal:**
  ```text
  87:          generate_release_notes: false
  backup/pre-plan-2026-11-08
  ```
- **Critério de aceite:** Tag `backup/pre-plan-2026-11-08` apontando para `7c35ab6` e `generate_release_notes: false` confirmado na linha 87 de `.github/workflows/android.yml`.
- **Se falhar:** `git restore .github/workflows/android.yml` e PARE.
- **Commit e Merge no Ramo do Grupo:**
  - Commit: `ci(release): disable automatic generate_release_notes in workflow`
  - Merge no ramo do grupo:
    ```bash
    git checkout group/g0-safety-and-identity && git merge --no-ff chore/00-safety-tag-and-ci-notes -m "Merge task T-00 into group/g0-safety-and-identity"
    ```
- **Registro:** `CHANGELOG.md [Unreleased]` e `docs/JOURNAL.md`.

---

### T-01 — Restaurar imediatamente `applicationId = "com.nepo.smorecomp"` (Decisão `D-1B`), gravar ADR-001 e iniciar do zero
- **Pedido atendido:** P-7 (Decisão `D-1B` aprovada pelo usuário) · **Depende de:** T-00 · **Risco:** baixo
- **Ramo da tarefa:** `refactor/01-restore-nepo-application-id` (criado a partir de `group/g0-safety-and-identity` e mesclado de volta em `group/g0-safety-and-identity`).
  ```bash
  cd /home/nepo/Downloads/SWITCH/smo-recomp-android && git checkout group/g0-safety-and-identity && git checkout -b refactor/01-restore-nepo-application-id
  ```
- **Arquivos permitidos:**
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/android/app/build.gradle.kts`
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/android/app/src/main/AndroidManifest.xml`
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/docs/adr/ADR-001-application-id.md` (novo arquivo autorizado)
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/CHANGELOG.md`
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/docs/JOURNAL.md`
- **Evidência do diagnóstico:** **COMPROVADO** (`android/app/build.gradle.kts:10` tinha `applicationId = "com.mobileodyssey.game"`).
- **Pré-checagem:**
  ```bash
  cd /home/nepo/Downloads/SWITCH/smo-recomp-android
  grep -n 'applicationId = "com.mobileodyssey.game"' android/app/build.gradle.kts
  grep -n 'android:label="Odyssey"' android/app/src/main/AndroidManifest.xml
  ```
  **Saída esperada literal:**
  ```text
  10:        applicationId = "com.mobileodyssey.game"
  20:        android:label="Odyssey"
  ```
- **Mudança:**

  #### 1) Em `/home/nepo/Downloads/SWITCH/smo-recomp-android/android/app/build.gradle.kts`:
  - **Antes (âncora exata):**
    ```kotlin
        defaultConfig {
            applicationId = "com.mobileodyssey.game"
            minSdk = 30
    ```
  - **Depois:**
    ```kotlin
        defaultConfig {
            applicationId = "com.nepo.smorecomp"
            minSdk = 30
    ```

  #### 2) Em `/home/nepo/Downloads/SWITCH/smo-recomp-android/android/app/src/main/AndroidManifest.xml`:
  - **Antes (âncora exata):**
    ```xml
        <application
            android:label="Odyssey"
            android:icon="@mipmap/ic_launcher"
    ```
  - **Depois:**
    ```xml
        <application
            android:label="SMO Recomp"
            android:icon="@mipmap/ic_launcher"
    ```

  #### 3) Criar `/home/nepo/Downloads/SWITCH/smo-recomp-android/docs/adr/ADR-001-application-id.md`:
  ```markdown
  # ADR-001: Restauração do `applicationId` próprio (`com.nepo.smorecomp`) separado do `namespace` Java

  * **Status:** Aceito (Decisão D-1B)
  * **Data:** 2026-11-08
  * **Decisor:** Nepo (@nepotira)

  ## Contexto
  No commit `fb6be60`, o `applicationId` foi alterado para `com.mobileodyssey.game`. Isso descaracterizou a identidade própria do projeto e gerou conflito de assinatura (`INSTALL_FAILED_UPDATE_INCOMPATIBLE`) com o APK de referência.

  ## Decisão
  1. Restaurar imediatamente `applicationId = "com.nepo.smorecomp"` em `android/app/build.gradle.kts` e `android:label="SMO Recomp"` em `AndroidManifest.xml`.
  2. Manter `namespace = "com.mobileodyssey.game"` para preservar os pacotes das classes Java sem refatoração invasiva.
  3. Iniciar e validar o aplicativo do zero (extraindo `prod.keys` + `.nsp` e importando o driver Turnip diretamente pela interface do nosso app, como um usuário novo).

  ## Consequências
  * O nosso APK instala de forma limpa e independente sob `com.nepo.smorecomp`.
  * Todo o fluxo de extração inicial e armazenamento em `/sdcard/Android/data/com.nepo.smorecomp/files/` é validado do zero.
  ```

- **Pós-checagem:**
  ```bash
  cd /home/nepo/Downloads/SWITCH/smo-recomp-android
  grep -n 'namespace = "com.mobileodyssey.game"' android/app/build.gradle.kts
  grep -n 'applicationId = "com.nepo.smorecomp"' android/app/build.gradle.kts
  grep -n 'android:label="SMO Recomp"' android/app/src/main/AndroidManifest.xml
  test -f docs/adr/ADR-001-application-id.md && echo "ADR OK"
  ```
  **Saída esperada literal:**
  ```text
  6:    namespace = "com.mobileodyssey.game"
  10:        applicationId = "com.nepo.smorecomp"
  20:        android:label="SMO Recomp"
  ADR OK
  ```
- **Critério de aceite:** Pós-checagem idêntica à saída esperada.
- **Se falhar:** `git restore android/app/build.gradle.kts android/app/src/main/AndroidManifest.xml && rm -f docs/adr/ADR-001-application-id.md` e PARE.
- **Commit e Merge no Ramo do Grupo:**
  - Commit: `refactor(android): restore com.nepo.smorecomp applicationId and record ADR-001`
  - Merge no ramo do grupo:
    ```bash
    git checkout group/g0-safety-and-identity && git merge --no-ff refactor/01-restore-nepo-application-id -m "Merge task T-01 into group/g0-safety-and-identity"
    ```

### 🛑 PORTÃO NO FIM DO GRUPO G-0 (`T-00`, `T-01`)
1. **a. Checagem de Tarefas:** Confirme que `T-00` e `T-01` terminaram com relatório `PASSOU`. Se qualquer uma falhou, **PARE**, reporte e não compile.
2. **b. Compilação via `gh` e SHA-256:**
   ```bash
   git push -u origin group/g0-safety-and-identity
   gh workflow run android.yml --repo nepotira/smo-recomp-android --ref group/g0-safety-and-identity
   sleep 10
   RUN_ID=$(gh run list --repo nepotira/smo-recomp-android --branch group/g0-safety-and-identity --limit 1 --json databaseId -q '.[0].databaseId')
   gh run watch "$RUN_ID" --repo nepotira/smo-recomp-android --exit-status > /tmp/ci_g0.log 2>&1
   tail -n 15 /tmp/ci_g0.log
   rm -rf /tmp/apk_g0 && mkdir -p /tmp/apk_g0
   gh run download "$RUN_ID" --repo nepotira/smo-recomp-android --dir /tmp/apk_g0
   sha256sum /tmp/apk_g0/SMO-Recomp-v0.2.0-android-arm64.apk
   ```
   *(Nota: se o build idêntico já foi executado no run `42445668729` em `/tmp/apk_t01/SMO-Recomp-v0.2.0-android-arm64.apk`, aponte `group/g0-safety-and-identity` para o commit de `T-01` e informe o link do run `https://github.com/nepotira/smo-recomp-android/actions/runs/42445668729` e o `sha256sum` real).*
3. **c. Proibido publicar release:** Não crie tag de versão nem release no GitHub.
4. **d. Guia de Teste do Grupo G-0 (entregue ao usuário no Relatório de Grupo):**
   - **Passo 1 (no terminal do PC):** Conecte o Poco F7 via USB e instale o APK do Grupo G-0:
     ```bash
     adb install -r /tmp/apk_t01/SMO-Recomp-v0.2.0-android-arm64.apk
     ```
   - **Passo 2 (no celular):** Verifique se o aplicativo instalou com o nome **SMO Recomp** (pacote `com.nepo.smorecomp`) e abra-o.
   - **Passo 3 (no celular — configuração do zero):**
     1. Na aba **Game**, toque em **Select prod.keys** e selecione seu arquivo `prod.keys`.
     2. Toque em **Unpack game (.nsp / .xci)**, selecione o `.nsp` do Super Mario Odyssey e aguarde terminar a extração até aparecer **Ready**.
     3. Na aba **Graphics**, em **Vulkan driver (Adreno)**, toque em **Import .zip / .adpkg** e selecione o seu driver Turnip customizado.
   - **O que deve aparecer:** Status **Ready** em verde na aba **Game** e o nome do seu driver Turnip ativo na aba **Graphics**.
   - **Comandos `adb logcat` (caso ocorra qualquer erro):**
     ```bash
     adb logcat -d -v time > /tmp/logcat_g0.txt && grep -iE "smorecomp|mobileodyssey|AndroidRuntime|FATAL" /tmp/logcat_g0.txt | tail -n 40
     ```
   - **PARE E ESPERE** a resposta do usuário (`"OK"` ou `"FALHOU"` com o log).
5. **e. Ação pós-resposta do usuário:**
   - **Com `"OK"`:** Faça merge do ramo `group/g0-safety-and-identity` na `main`:
     ```bash
     git checkout main && git merge --no-ff group/g0-safety-and-identity -m "Merge group G-0 (T-00, T-01) after hardware verification" && git push origin main
     ```
     Só então inicie o **Grupo G-1**.
   - **Com `"FALHOU"`:** Analise o log, reverta a última tarefa (`T-01`) no ramo do grupo:
     ```bash
     git checkout group/g0-safety-and-identity && git revert --no-edit HEAD
     ```
     Recompile e teste novamente. Nunca avance com grupo falho.

---

# GRUPO G-1 — Fase 1: Baseline Factual (`T-02`)
**Ramo do Grupo:** `group/g1-baseline`  
```bash
cd /home/nepo/Downloads/SWITCH/smo-recomp-android && git checkout main && git pull origin main && git checkout -b group/g1-baseline
```

### T-02 — Medição da Baseline Factual no Poco F7 em `com.nepo.smorecomp`
- **Pedido atendido:** P-2, P-3, P-4, P-5 · **Depende de:** Portão G-0 aprovado com `"OK"` · **Risco:** baixo
- **Ramo da tarefa:** `docs/02-baseline-measurements`
  ```bash
  cd /home/nepo/Downloads/SWITCH/smo-recomp-android && git checkout group/g1-baseline && git checkout -b docs/02-baseline-measurements
  ```
- **Arquivos permitidos:**
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/docs/BENCHMARK_POCO_F7.md`
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/docs/JOURNAL.md`
- **Evidência do diagnóstico:** **COMPROVADO** (Toda tarefa de desempenho exige medição BASE antes na mesma cena, mesmo aparelho e mesmo método).
- **Pré-checagem:**
  ```bash
  adb shell pm list packages | grep "com.nepo.smorecomp"
  ```
  **Saída esperada literal:** `package:com.nepo.smorecomp`
- **Mudança:** Preencher em `docs/BENCHMARK_POCO_F7.md` a seção `## Baseline Factual (Grupo G-1 — Antes das Otimizações de Runtime)` com os valores medidos pelo usuário no Portão G-1.
- **Pós-checagem:**
  ```bash
  grep -n "Baseline Factual (Grupo G-1" /home/nepo/Downloads/SWITCH/smo-recomp-android/docs/BENCHMARK_POCO_F7.md
  ```
  **Saída esperada:** 1 ocorrência encontrada.
- **Critério de aceite:** As 4 métricas base (`T_joycon_base`, `FPS_cutscene_base`, `FPS_cap_base` e contagem de `gaps`/`held` de `log.txt`) gravadas em `docs/BENCHMARK_POCO_F7.md`.
- **Se falhar:** `git restore docs/BENCHMARK_POCO_F7.md` e PARE.
- **Commit e Merge no Ramo do Grupo:**
  - Commit: `docs(benchmark): record factual baseline metrics on Poco F7 (com.nepo.smorecomp)`
  - Merge no ramo do grupo:
    ```bash
    git checkout group/g1-baseline && git merge --no-ff docs/02-baseline-measurements -m "Merge task T-02 into group/g1-baseline"
    ```

### 🛑 PORTÃO NO FIM DO GRUPO G-1 (`T-02`)
1. **a. Checagem de Tarefas:** Como o Grupo G-1 é puramente a coleta da Baseline no APK já instalado no Grupo G-0, não há alteração de binário C/Java para recompilar no CI (usa o mesmo APK validado no Portão G-0).
2. **b. Guia de Coleta da Baseline no Celular (entregue ao usuário um passo por vez):**
   - **Passo 1 (no celular):** Abra o app **SMO Recomp** (`com.nepo.smorecomp`) e toque em **Play**.
   - **Passo 2 (Métrica 1 — `T_joycon_base`):** Cronometre ou observe quantos segundos o jogo fica parado na tela **"Try playing with detached Joy-Con controllers"** até entrar na tela inicial do Mario.
   - **Passo 3 (Métrica 2 — `FPS_cutscene_base`):** Inicie um **New Game** e observe a cutscene de abertura (se há tela preta com som adiantado, soluços no áudio e qual a faixa de FPS mostrada no canto superior esquerdo).
   - **Passo 4 (Métrica 3 — `FPS_cap_base`):** Assim que controlar o Mario em **Cap Kingdom (Bonneton)** parado olhando para a torre central por 30 segundos, observe a faixa de FPS.
   - **Passo 5 (no terminal do PC — Coleta de `log.txt` e `adb logcat`):**
     ```bash
     adb pull /sdcard/Android/data/com.nepo.smorecomp/files/log.txt /tmp/smo_log_g1_base.txt
     grep -E "nvn_vk:|smo_audio:|frame-time:|held" /tmp/smo_log_g1_base.txt | tail -n 30
     adb logcat -d -v time > /tmp/logcat_g1_base.txt
     ```
   - **PARE E ESPERE** o usuário informar as métricas e responder `"OK"` (ou `"FALHOU"`).
3. **c. Ação pós-resposta do usuário:**
   - **Com `"OK"`:** Grave os números exatos em `docs/BENCHMARK_POCO_F7.md`, finalize o commit de `T-02`, mescle `group/g1-baseline` na `main`:
     ```bash
     git checkout main && git merge --no-ff group/g1-baseline -m "Merge group G-1 (T-02 baseline) into main" && git push origin main
     ```
     Só então inicie o **Grupo G-2**.

---

# GRUPO G-2 — Fase 2: Ativação do Nosso Runtime C (`T-03`)
**Ramo do Grupo:** `group/g2-runtime-bridge`  
```bash
cd /home/nepo/Downloads/SWITCH/smo-recomp-android && git checkout main && git pull origin main && git checkout -b group/g2-runtime-bridge
```

### T-03 — Compilar nosso `runtime/` (`libmain.so`) no APK e vincular à `libsmo.so` via ponte dinâmica e trampolins ARM64
- **Pedido atendido:** P-1 · **Depende de:** Portão G-1 aprovado com `"OK"` · **Risco:** médio
- **Ramo da tarefa:** `feat/03-compile-custom-runtime-bridge`
  ```bash
  cd /home/nepo/Downloads/SWITCH/smo-recomp-android && git checkout group/g2-runtime-bridge && git checkout -b feat/03-compile-custom-runtime-bridge
  ```
- **Arquivos permitidos:**
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/runtime/CMakeLists.txt`
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/runtime/table_stubs.c`
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/runtime/hostrt.c`
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/runtime/android/vk_shaderc_android.c`
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/runtime/android/android_driver.c`
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/runtime/android/android_main.c`
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/android/app/build.gradle.kts`
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/android/app/src/main/AndroidManifest.xml`
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/CHANGELOG.md`
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/docs/JOURNAL.md`
- **Evidência do diagnóstico:** **COMPROVADO** (`build.gradle.kts:1-64`, `AndroidManifest.xml:61`, `runtime/gamedll.c:1-45`, `runtime/hostrt.c:227-232`).
- **Pré-checagem:**
  ```bash
  cd /home/nepo/Downloads/SWITCH/smo-recomp-android
  grep -n -- "-DSMO_LAYOUT=android" runtime/CMakeLists.txt
  grep -n "if ((uint32_t)(e >> 32) == key)" runtime/hostrt.c
  grep -n '"libshaderc.so",' runtime/android/vk_shaderc_android.c
  grep -n "#ifndef SMO_ADRENOTOOLS" runtime/android/android_driver.c
  grep -n '"**/libmain_hook.so"' android/app/build.gradle.kts
  grep -n 'android:name="android.app.lib_name"' android/app/src/main/AndroidManifest.xml
  ```
  **Saída esperada literal:**
  ```text
  16:    -DSMO_LAYOUT=android
  227:        if ((uint32_t)(e >> 32) == key) fn = (void (*)(Ctx *))(uintptr_t)((intptr_t)(uintptr_t)call_indirect + (int32_t)(uint32_t)e);
  119:        "libshaderc.so",
  83:#ifndef SMO_ADRENOTOOLS
  35:                "**/libmain_hook.so"
  61:            <meta-data android:name="android.app.lib_name" android:value="smo" />
  ```
- **Mudança:**

  #### 1) Em `/home/nepo/Downloads/SWITCH/smo-recomp-android/runtime/CMakeLists.txt`:
  - **Antes (âncora exata):**
    ```cmake
        -D_GNU_SOURCE
        -DSMO_LAYOUT=android
        -D__ANDROID__
        -ffunction-sections
        -fdata-sections
    )
    ```
  - **Depois:**
    ```cmake
        -D_GNU_SOURCE
        -DSMO_LAYOUT=android
        -D__ANDROID__
        -Dfn_table=\(*game_fn_table_p\)
        -Dfn_table_count=\(*game_fn_table_count_p\)
        -Ddyn_syms=\(*game_dyn_syms_p\)
        -Ddyn_sym_count=\(*game_dyn_sym_count_p\)
        -ffunction-sections
        -fdata-sections
    )
    ```

  #### 2) Em `/home/nepo/Downloads/SWITCH/smo-recomp-android/runtime/table_stubs.c` (substituição integral das 14 linhas do arquivo):
  - **Antes (âncora exata):**
    ```c
    /* table_stubs.c: Default weak symbol definitions for lifted tables and wrapped functions.
     * When the lifted game module (smogame.so or lifted objects) is linked, its strong
     * definitions override these weak stubs seamlessly.
     */
    #include "runtime.h"

    __attribute__((weak)) const FnEntry fn_table[1] = {{0, 0}};
    __attribute__((weak)) const int fn_table_count = 0;
    __attribute__((weak)) const DynSym dyn_syms[1] = {{"", 0, 0}};
    __attribute__((weak)) const int dyn_sym_count = 0;

    __attribute__((weak)) void __real_f_400072e61c(Ctx *c) { (void)c; }
    __attribute__((weak)) void __real_f_4000731fdc(Ctx *c) { (void)c; }
    __attribute__((weak)) void __real_f_4000a58394(Ctx *c) { (void)c; }
    ```
  - **Depois:**
    ```c
    /* table_stubs.c: Dynamic bridge between libmain.so (our compiled C runtime) and libsmo.so
     * (the pre-lifted 74,822 game functions and tables).
     */
    #include <stdio.h>
    #include <stdint.h>
    #include <dlfcn.h>
    #include <sys/mman.h>
    #include <unistd.h>
    #include "runtime.h"

    #undef fn_table
    #undef fn_table_count
    #undef dyn_syms
    #undef dyn_sym_count

    static const FnEntry k_empty_fn[1] = {{0, 0}};
    static const int k_zero = 0;
    static const DynSym k_empty_sym[1] = {{"", 0, 0}};

    const FnEntry (*game_fn_table_p)[] = &k_empty_fn;
    const int *game_fn_table_count_p = &k_zero;
    const DynSym (*game_dyn_syms_p)[] = &k_empty_sym;
    const int *game_dyn_sym_count_p = &k_zero;

    static void (*g_real_72e61c)(Ctx *);
    static void (*g_real_731fdc)(Ctx *);
    static void (*g_real_a58394)(Ctx *);
    static void *g_libsmo_handle;

    void *smo_bridge_handle(void) { return g_libsmo_handle; }

    void __real_f_400072e61c(Ctx *c) { if (g_real_72e61c) g_real_72e61c(c); }
    void __real_f_4000731fdc(Ctx *c) { if (g_real_731fdc) g_real_731fdc(c); }
    void __real_f_4000a58394(Ctx *c) { if (g_real_a58394) g_real_a58394(c); }

    void __wrap_f_400072e61c(Ctx *c);
    void __wrap_f_4000731fdc(Ctx *c);
    void __wrap_f_4000a58394(Ctx *c);

    int smo_patch_arm64_jump(void *src, void *dst) {
        if (!src || !dst || src == dst) return 0;
        uintptr_t p = (uintptr_t)src;
        uintptr_t page_start = p & ~(uintptr_t)0xfffULL;
        uintptr_t page_end = (p + 16 + 0xfffULL) & ~(uintptr_t)0xfffULL;
        if (mprotect((void *)page_start, page_end - page_start, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) return 0;
        uint32_t *ins = (uint32_t *)src;
        ins[0] = 0x58000050u; /* LDR X16, #8 */
        ins[1] = 0xd61f0200u; /* BR  X16     */
        uint64_t target = (uint64_t)(uintptr_t)dst;
        memcpy(&ins[2], &target, 8);
        __builtin___clear_cache((char *)src, (char *)src + 16);
        return 1;
    }

    __attribute__((constructor)) static void smo_bridge_init(void) {
        g_libsmo_handle = dlopen("libsmo.so", RTLD_NOW | RTLD_GLOBAL);
        if (!g_libsmo_handle) return;
        const FnEntry *ft = (const FnEntry *)dlsym(g_libsmo_handle, "fn_table");
        const int *ftc = (const int *)dlsym(g_libsmo_handle, "fn_table_count");
        const DynSym *ds = (const DynSym *)dlsym(g_libsmo_handle, "dyn_syms");
        const int *dsc = (const int *)dlsym(g_libsmo_handle, "dyn_sym_count");
        if (ft && ftc && ds && dsc) {
            game_fn_table_p = (const FnEntry (*)[])ft;
            game_fn_table_count_p = ftc;
            game_dyn_syms_p = (const DynSym (*)[])ds;
            game_dyn_sym_count_p = dsc;
        }
        g_real_72e61c = (void (*)(Ctx *))dlsym(g_libsmo_handle, "f_400072e61c");
        g_real_731fdc = (void (*)(Ctx *))dlsym(g_libsmo_handle, "f_4000731fdc");
        g_real_a58394 = (void (*)(Ctx *))dlsym(g_libsmo_handle, "f_4000a58394");

        struct { const char *name; void *dst; } hooks[] = {
            {"call_indirect", (void *)call_indirect},
            {"host_import", (void *)host_import},
            {"guest_trap", (void *)guest_trap},
            {"guest_unimpl", (void *)guest_unimpl},
            {"guest_svc", (void *)guest_svc},
            {"stx_diag", (void *)stx_diag},
            {"__wrap_f_400072e61c", (void *)__wrap_f_400072e61c},
            {"__wrap_f_4000731fdc", (void *)__wrap_f_4000731fdc},
            {"__wrap_f_4000a58394", (void *)__wrap_f_4000a58394},
        };
        int patched = 0;
        for (unsigned i = 0; i < sizeof(hooks) / sizeof(hooks[0]); i++) {
            void *sym = dlsym(g_libsmo_handle, hooks[i].name);
            if (smo_patch_arm64_jump(sym, hooks[i].dst)) patched++;
        }
        fprintf(stderr, "smo: [NEPO-RUNTIME-ACTIVE] libmain.so bound to libsmo.so (%d fns, %d syms, %d bridges)\n",
                ftc ? *ftc : 0, dsc ? *dsc : 0, patched);
    }
    ```

  #### 3) Em `/home/nepo/Downloads/SWITCH/smo-recomp-android/runtime/hostrt.c`:
  - **Antes (âncora exata):**
    ```c
            if ((uint32_t)(e >> 32) == key) fn = (void (*)(Ctx *))(uintptr_t)((intptr_t)(uintptr_t)call_indirect + (int32_t)(uint32_t)e);
            else {
                fn = lookup_guest(addr);
                if (!fn) guest_trap("indirect call to an address that is not a known function", addr);
                intptr_t d = (intptr_t)(uintptr_t)fn - (intptr_t)(uintptr_t)call_indirect;
                if (d == (int32_t)d) __atomic_store_n(&g_icache[k], (uint64_t)key << 32 | (uint32_t)(int32_t)d, __ATOMIC_RELAXED);
            }
    ```
  - **Depois:**
    ```c
            intptr_t base_anchor = (intptr_t)(uintptr_t)(fn_table_count > 0 ? fn_table[0].fn : (void (*)(Ctx *))call_indirect);
            if ((uint32_t)(e >> 32) == key) fn = (void (*)(Ctx *))(uintptr_t)(base_anchor + (int32_t)(uint32_t)e);
            else {
                fn = lookup_guest(addr);
                if (!fn) guest_trap("indirect call to an address that is not a known function", addr);
                intptr_t d = (intptr_t)(uintptr_t)fn - base_anchor;
                if (d == (int32_t)d) __atomic_store_n(&g_icache[k], (uint64_t)key << 32 | (uint32_t)(int32_t)d, __ATOMIC_RELAXED);
            }
    ```

  #### 4) Em `/home/nepo/Downloads/SWITCH/smo-recomp-android/runtime/android/vk_shaderc_android.c`:
  - **Antes (âncora exata):**
    ```c
    static void init_once(void) {
        const char *libs[] = {
            "libshaderc.so",
            "libshaderc_shared.so",
            "libshaderc_combined.so",
            "/data/data/com.nepo.smorecomp/lib/libshaderc.so",
            "/data/data/com.nepo.smorecomp/lib/libshaderc_shared.so",
            NULL
        };
    ```
  - **Depois:**
    ```c
    static void init_once(void) {
        const char *libs[] = {
            "libsmo.so",
            "libshaderc.so",
            "libshaderc_shared.so",
            "libshaderc_combined.so",
            "/data/data/com.nepo.smorecomp/lib/libshaderc.so",
            "/data/data/com.nepo.smorecomp/lib/libshaderc_shared.so",
            NULL
        };
    ```

  #### 5) Em `/home/nepo/Downloads/SWITCH/smo-recomp-android/runtime/android/android_driver.c`:
  - **Antes (âncora exata):**
    ```c
    #ifndef SMO_ADRENOTOOLS
        printf("nvn_vk: %s is there, but this build has no libadrenotools: the system's Vulkan driver\n", src);
        return NULL;
    #else
    ```
  - **Depois:**
    ```c
    #if 1
        typedef void *(*pfn_adrenotools_open_libvulkan)(int, int, const char *, const char *, const char *, const char *, const char *, void *);
        void *smo_bridge_handle(void);
        void *bh = smo_bridge_handle();
        pfn_adrenotools_open_libvulkan fn_at_open = (pfn_adrenotools_open_libvulkan)(bh ? dlsym(bh, "adrenotools_open_libvulkan") : dlsym(RTLD_DEFAULT, "adrenotools_open_libvulkan"));
        if (!fn_at_open) {
            printf("nvn_vk: %s is there, but adrenotools_open_libvulkan symbol was not found: the system's Vulkan driver\n", src);
            return NULL;
        }
        #define ADRENOTOOLS_DRIVER_CUSTOM 1
        #define adrenotools_open_libvulkan fn_at_open
    ```

  #### 6) Em `/home/nepo/Downloads/SWITCH/smo-recomp-android/android/app/build.gradle.kts`:
  - **Antes (âncora exata):**
    ```kotlin
            ndk {
                abiFilters.add("arm64-v8a")
            }
        }

        sourceSets {
    ```
  - **Depois:**
    ```kotlin
            ndk {
                abiFilters.add("arm64-v8a")
            }

            externalNativeBuild {
                cmake {
                    arguments("-DANDROID_STL=c++_shared")
                }
            }
        }

        externalNativeBuild {
            cmake {
                path = file("../../runtime/CMakeLists.txt")
                version = "3.22.1"
            }
        }

        sourceSets {
    ```
  - E no bloco `pickFirsts` de `android/app/build.gradle.kts`:
    - **Antes (âncora exata):**
      ```kotlin
                  pickFirsts.addAll(listOf(
                      "**/libsmo.so",
                      "**/libfile_redirect_hook.so",
                      "**/libgsl_alloc_hook.so",
                      "**/libhook_impl.so",
                      "**/libmain_hook.so"
                  ))
      ```
    - **Depois:**
      ```kotlin
                  pickFirsts.addAll(listOf(
                      "**/libmain.so",
                      "**/libsmo.so",
                      "**/libfile_redirect_hook.so",
                      "**/libgsl_alloc_hook.so",
                      "**/libhook_impl.so",
                      "**/libmain_hook.so"
                  ))
      ```

  #### 7) Em `/home/nepo/Downloads/SWITCH/smo-recomp-android/android/app/src/main/AndroidManifest.xml`:
  - **Antes (âncora exata):**
    ```xml
                <meta-data android:name="android.app.lib_name" android:value="smo" />
    ```
  - **Depois:**
    ```xml
                <meta-data android:name="android.app.lib_name" android:value="main" />
    ```

  #### 8) Em `/home/nepo/Downloads/SWITCH/smo-recomp-android/runtime/android/android_main.c`:
  - **Antes (âncora exata):**
    ```c
        ANativeActivity_setWindowFlags(app->activity, AWINDOW_FLAG_FULLSCREEN | AWINDOW_FLAG_KEEP_SCREEN_ON, 0);
        printf("smo: Android host, data folder %s\n", g_root);
    ```
  - **Depois:**
    ```c
        ANativeActivity_setWindowFlags(app->activity, AWINDOW_FLAG_FULLSCREEN | AWINDOW_FLAG_KEEP_SCREEN_ON, 0);
        printf("smo: [NEPO-RUNTIME-ACTIVE] Android host running compiled libmain.so, data folder %s\n", g_root);
    ```

- **Pós-checagem:**
  ```bash
  cd /home/nepo/Downloads/SWITCH/smo-recomp-android
  grep -n "NEPO-RUNTIME-ACTIVE" runtime/table_stubs.c runtime/android/android_main.c
  grep -n 'android:value="main"' android/app/src/main/AndroidManifest.xml
  grep -n 'path = file("../../runtime/CMakeLists.txt")' android/app/build.gradle.kts
  ```
  **Saída esperada literal:**
  ```text
  runtime/table_stubs.c:83:    fprintf(stderr, "smo: [NEPO-RUNTIME-ACTIVE] libmain.so bound to libsmo.so (%d fns, %d syms, %d bridges)\n",
  runtime/android/android_main.c:418:    printf("smo: [NEPO-RUNTIME-ACTIVE] Android host running compiled libmain.so, data folder %s\n", g_root);
  61:            <meta-data android:name="android.app.lib_name" android:value="main" />
  29:            path = file("../../runtime/CMakeLists.txt")
  ```
- **Critério de aceite:** Pós-checagem idêntica à saída esperada.
- **Se falhar:** `git restore runtime/CMakeLists.txt runtime/table_stubs.c runtime/hostrt.c runtime/android/vk_shaderc_android.c runtime/android/android_driver.c runtime/android/android_main.c android/app/build.gradle.kts android/app/src/main/AndroidManifest.xml` e PARE.
- **Commit e Merge no Ramo do Grupo:**
  - Commit: `feat(runtime): compile libmain.so via CMake and bridge dynamically to libsmo.so`
  - Merge no ramo do grupo:
    ```bash
    git checkout group/g2-runtime-bridge && git merge --no-ff feat/03-compile-custom-runtime-bridge -m "Merge task T-03 into group/g2-runtime-bridge"
    ```

### 🛑 PORTÃO NO FIM DO GRUPO G-2 (`T-03`)
1. **a. Checagem de Tarefas:** Confirme que `T-03` terminou com relatório `PASSOU`. Se falhou, **PARE**, reporte e não compile.
2. **b. Compilação via `gh` e SHA-256:**
   ```bash
   git push -u origin group/g2-runtime-bridge
   gh workflow run android.yml --repo nepotira/smo-recomp-android --ref group/g2-runtime-bridge
   sleep 10
   RUN_ID=$(gh run list --repo nepotira/smo-recomp-android --branch group/g2-runtime-bridge --limit 1 --json databaseId -q '.[0].databaseId')
   gh run watch "$RUN_ID" --repo nepotira/smo-recomp-android --exit-status > /tmp/ci_g2.log 2>&1
   tail -n 15 /tmp/ci_g2.log
   rm -rf /tmp/apk_g2 && mkdir -p /tmp/apk_g2
   gh run download "$RUN_ID" --repo nepotira/smo-recomp-android --dir /tmp/apk_g2
   unzip -l /tmp/apk_g2/*.apk | grep -E "libmain\.so|libsmo\.so"
   sha256sum /tmp/apk_g2/*.apk
   ```
3. **c. Proibido publicar release:** Não crie tag de versão nem release no GitHub.
4. **d. Guia de Teste do Grupo G-2 (entregue ao usuário no Relatório de Grupo):**
   - **Passo 1 (no terminal do PC):** Instale o APK do Grupo G-2 por cima do atual:
     ```bash
     adb install -r /tmp/apk_g2/SMO-Recomp-v0.2.0-android-arm64.apk
     ```
   - **Passo 2 (no celular):** Abra o app **SMO Recomp**, toque em **Play**, aguarde entrar na tela dos Joy-Cons (ou por 10 segundos) e feche o app.
   - **Passo 3 (no terminal do PC — verificar log e `adb logcat`):**
     ```bash
     adb shell grep "NEPO-RUNTIME-ACTIVE" /sdcard/Android/data/com.nepo.smorecomp/files/log.txt
     adb logcat -d -v time > /tmp/logcat_g2.txt && grep -iE "NEPO-RUNTIME-ACTIVE|smorecomp|FATAL" /tmp/logcat_g2.txt | tail -n 30
     ```
   - **O que deve aparecer:** A linha `smo: [NEPO-RUNTIME-ACTIVE] Android host running compiled libmain.so, data folder /storage/emulated/0/Android/data/com.nepo.smorecomp/files` comprovando que o nosso código C está executando!
   - **PARE E ESPERE** a resposta do usuário (`"OK"` ou `"FALHOU"` com o log).
5. **e. Ação pós-resposta do usuário:**
   - **Com `"OK"`:** Faça merge do ramo `group/g2-runtime-bridge` na `main`:
     ```bash
     git checkout main && git merge --no-ff group/g2-runtime-bridge -m "Merge group G-2 (T-03 runtime bridge) after hardware verification" && git push origin main
     ```
     Só então inicie o **Grupo G-3**.
   - **Com `"FALHOU"`:** Analise o log, reverta `T-03` (`git checkout group/g2-runtime-bridge && git revert --no-edit HEAD`), corrija dentro do escopo permitido, recompile e teste novamente.

---

# GRUPO G-3 — Fase 3: Boot e Joy-Cons (`T-04`)
**Ramo do Grupo:** `group/g3-boot-joycons`  
```bash
cd /home/nepo/Downloads/SWITCH/smo-recomp-android && git checkout main && git pull origin main && git checkout -b group/g3-boot-joycons
```

### T-04 — Destravar a tela dos Joy-Cons e sincronizar a tela `"Preparing graphics..."`
- **Pedido atendido:** P-2, P-4 · **Depende de:** Portão G-2 aprovado com `"OK"` · **Risco:** baixo
- **Ramo da tarefa:** `fix/04-joycon-stall-and-loading-screen`
  ```bash
  cd /home/nepo/Downloads/SWITCH/smo-recomp-android && git checkout group/g3-boot-joycons && git checkout -b fix/04-joycon-stall-and-loading-screen
  ```
- **Arquivos permitidos:**
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/runtime/nvn_vk_m4.c`
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/android/app/src/main/java/com/mobileodyssey/game/GameActivity.java`
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/CHANGELOG.md`
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/docs/JOURNAL.md`
- **Pré-checagem:**
  ```bash
  cd /home/nepo/Downloads/SWITCH/smo-recomp-android
  grep -n "int n = g_pw_n, next = g_pw_next;" runtime/nvn_vk_m4.c
  grep -n 'if (on < 0) on = !(getenv("SMO_VK_FRAGONLY")' runtime/nvn_vk_m4.c
  grep -n "int hs = hold_scan();" runtime/nvn_vk_m4.c
  grep -n "if (shownAt >= 0 && now - shownAt > 600)" android/app/src/main/java/com/mobileodyssey/game/GameActivity.java
  ```
  **Saída esperada literal:**
  ```text
  1272:    int n = g_pw_n, next = g_pw_next;
  1777:        if (on < 0) on = !(getenv("SMO_VK_FRAGONLY") && !atoi(getenv("SMO_VK_FRAGONLY")));
  1802:    int hs = hold_scan();
  107:                if (shownAt >= 0 && now - shownAt > 600) { hideLoading(); return; }     /* the game's own frames for a moment */
  ```
- **Mudança:**

  #### 1) Em `/home/nepo/Downloads/SWITCH/smo-recomp-android/runtime/nvn_vk_m4.c`:
  - **Antes (âncora exata):**
    ```c
    int nvn_vk_load_progress(long long *done, long long *total) {
        int n = g_pw_n, next = g_pw_next;
        *total = n; *done = next < n ? next : n;
        return g_pw_t0 != 0 && g_pw_left == 0;
    }
    ```
  - **Depois:**
    ```c
    int nvn_vk_load_progress(long long *done, long long *total) {
        int n = g_pw_n, next = g_pw_next;
        if (n > 0) {
            *total = n; *done = next < n ? next : n;
            return g_pw_t0 != 0 && g_pw_left == 0;
        }
        int64_t q = g_jq_queued, d = g_jq_done;
        *total = q; *done = d < q ? d : q;
        return q > 0 && d >= q;
    }
    ```

  #### 2) Em `/home/nepo/Downloads/SWITCH/smo-recomp-android/runtime/nvn_vk_m4.c`:
  - **Antes (âncora exata):**
    ```c
            static int on = -1;
            if (on < 0) on = !(getenv("SMO_VK_FRAGONLY") && !atoi(getenv("SMO_VK_FRAGONLY")));
            if (on) { static const uint32_t zero[4]; if (holdlog_on()) { plat_mutex_lock(&g_sh_lock); ho_note(ssrc_hb(&s[1]), 0); plat_mutex_unlock(&g_sh_lock); } spawn_stage_fo(&s[1], zero); }
            return;
        }
        if (!has[0] && !has[2]) return;
        int64_t ta = tk();
        size_t need = 0;
        for (int i = 0; i < 3; i++) if (has[i]) need += (size_t)s[i].code_len + s[i].cdata_size + 16;
        Job *j = calloc(1, sizeof *j + need);
        uint8_t *d = j->data;
        j->sshade = p->sample_shading;
        for (int i = 0; i < 3; i++) if (has[i]) job_copy_src(j, &d, i, &s[i]);
        uint64_t hn[2] = {0, 0};
        if (holdlog_on()) for (int i = 0; i < 2; i++) if (has[i]) hn[i] = ssrc_hb(&s[i]);      /* session 8: hashed before the lock is taken */
        int64_t tb = tk();
        nvn_setsh_part[5] += tb - ta;
        plat_mutex_lock(&g_sh_lock);
        nvn_setsh_part[6] += tk() - tb;
        if (holdlog_on()) for (int i = 0; i < 2; i++) if (has[i]) ho_note(hn[i], 0);
        job_push(j);
        /* session 6: outside a fight (no compute dispatch in the last frame) the caller is also held while the container scan
         * still has work: loading ends when everything the container holds is ready */
        int fr = fight_recent();
        /* session 7: in a fight the caller (the game's main thread) is not held at all: a draw that needs the program waits for it
         * on the render thread, a program not drawn in this frame costs nothing */
        int hs = hold_scan();
        if (!fr && (g_jq_queued - g_jq_done > g_pw_nthread || (hs && g_bq_queued > g_bq_done))) {
            int64_t t0 = tk();
            plat_li fq; ((fq).QuadPart = plat_qpf());
            while ((g_jq_queued - g_jq_done > g_pw_nthread || (hs && g_bq_queued > g_bq_done)) && tk() - t0 < 30 * fq.QuadPart) WD_SLEEP(&g_sh_cv, &g_sh_lock, 100);
            g_set_held++; g_set_held_tk += tk() - t0;
            nvn_setsh_part[7] += tk() - t0;
        }
        plat_mutex_unlock(&g_sh_lock);
    ```
  - **Depois:**
    ```c
            static int on = -1;
            if (on < 0) { const char *e = getenv("SMO_VK_FRAGONLY"); on = e ? atoi(e) != 0 : 0; }
            if (on) { static const uint32_t zero[4]; if (holdlog_on()) { plat_mutex_lock(&g_sh_lock); ho_note(ssrc_hb(&s[1]), 0); plat_mutex_unlock(&g_sh_lock); } spawn_stage_fo(&s[1], zero); }
            return;
        }
        if (!has[0] && !has[2]) return;
        int64_t ta = tk();
        size_t need = 0;
        for (int i = 0; i < 3; i++) if (has[i]) need += (size_t)s[i].code_len + s[i].cdata_size + 16;
        Job *j = calloc(1, sizeof *j + need);
        uint8_t *d = j->data;
        j->sshade = p->sample_shading;
        for (int i = 0; i < 3; i++) if (has[i]) job_copy_src(j, &d, i, &s[i]);
        uint64_t hn[2] = {0, 0};
        if (holdlog_on()) for (int i = 0; i < 2; i++) if (has[i]) hn[i] = ssrc_hb(&s[i]);      /* session 8: hashed before the lock is taken */
        int64_t tb = tk();
        nvn_setsh_part[5] += tb - ta;
        plat_mutex_lock(&g_sh_lock);
        nvn_setsh_part[6] += tk() - tb;
        if (holdlog_on()) for (int i = 0; i < 2; i++) if (has[i]) ho_note(hn[i], 0);
        job_push(j);
        int hs = hold_scan();
        if (hs && g_bq_queued > g_bq_done) {
            int64_t t0 = tk();
            plat_li fq; ((fq).QuadPart = plat_qpf());
            while (g_bq_queued > g_bq_done && tk() - t0 < 30 * fq.QuadPart) WD_SLEEP(&g_sh_cv, &g_sh_lock, 100);
            g_set_held++; g_set_held_tk += tk() - t0;
            nvn_setsh_part[7] += tk() - t0;
        }
        plat_mutex_unlock(&g_sh_lock);
    ```

  #### 3) Em `/home/nepo/Downloads/SWITCH/smo-recomp-android/android/app/src/main/java/com/mobileodyssey/game/GameActivity.java`:
  - **Antes (âncora exata):**
    ```java
                    if (st[0] > 0 && shownAt < 0) shownAt = now;
                    if (shownAt >= 0 && now - shownAt > 600) { hideLoading(); return; }     /* the game's own frames for a moment */
    ```
  - **Depois:**
    ```java
                    if (st[0] > 0 && (st[2] == 0 || st[3] != 0) && shownAt < 0) shownAt = now;
                    if (shownAt >= 0 && now - shownAt > 600) { hideLoading(); return; }     /* the game's own frames once initial shader queue finishes */
    ```

- **Pós-checagem:**
  ```bash
  cd /home/nepo/Downloads/SWITCH/smo-recomp-android
  grep -n 'if (on < 0) { const char \*e = getenv("SMO_VK_FRAGONLY"); on = e ? atoi(e) != 0 : 0; }' runtime/nvn_vk_m4.c
  grep -n "if (st\[0\] > 0 && (st\[2\] == 0 || st\[3\] != 0) && shownAt < 0)" android/app/src/main/java/com/mobileodyssey/game/GameActivity.java
  ```
- **Critério de aceite:** 1 linha em `runtime/nvn_vk_m4.c` e 1 linha em `GameActivity.java`.
- **Se falhar:** `git restore runtime/nvn_vk_m4.c android/app/src/main/java/com/mobileodyssey/game/GameActivity.java` e PARE.
- **Commit e Merge no Ramo do Grupo:**
  - Commit: `fix(vulkan): remove 30s main thread stall on nvnProgramSetShaders and sync loading screen`
  - Merge no ramo do grupo:
    ```bash
    git checkout group/g3-boot-joycons && git merge --no-ff fix/04-joycon-stall-and-loading-screen -m "Merge task T-04 into group/g3-boot-joycons"
    ```

### 🛑 PORTÃO NO FIM DO GRUPO G-3 (`T-04`)
1. **a. Checagem de Tarefas:** Confirme que `T-04` terminou com relatório `PASSOU`. Se falhou, **PARE**, reporte e não compile.
2. **b. Compilação via `gh` e SHA-256:**
   ```bash
   git push -u origin group/g3-boot-joycons
   gh workflow run android.yml --repo nepotira/smo-recomp-android --ref group/g3-boot-joycons
   sleep 10
   RUN_ID=$(gh run list --repo nepotira/smo-recomp-android --branch group/g3-boot-joycons --limit 1 --json databaseId -q '.[0].databaseId')
   gh run watch "$RUN_ID" --repo nepotira/smo-recomp-android --exit-status > /tmp/ci_g3.log 2>&1
   tail -n 15 /tmp/ci_g3.log
   rm -rf /tmp/apk_g3 && mkdir -p /tmp/apk_g3
   gh run download "$RUN_ID" --repo nepotira/smo-recomp-android --dir /tmp/apk_g3
   sha256sum /tmp/apk_g3/*.apk
   ```
3. **c. Proibido publicar release:** Não crie tag nem release no GitHub.
4. **d. Guia de Teste do Grupo G-3 (entregue ao usuário no Relatório de Grupo):**
   - **Passo 1 (no terminal do PC):** Instale o APK do Grupo G-3:
     ```bash
     adb install -r /tmp/apk_g3/SMO-Recomp-v0.2.0-android-arm64.apk
     ```
   - **Passo 2 (no celular):** Abra o app **SMO Recomp**, toque em **Play** e meça o tempo na tela dos Joy-Cons (`T_joycon_t04`) comparado a `T_joycon_base`.
   - **Passo 3 (no terminal do PC — coletar log):**
     ```bash
     adb pull /sdcard/Android/data/com.nepo.smorecomp/files/log.txt /tmp/smo_log_g3.txt
     adb logcat -d -v time > /tmp/logcat_g3.txt
     ```
   - **PARE E ESPERE** a resposta do usuário (`"OK"` ou `"FALHOU"` com o log).
5. **e. Ação pós-resposta do usuário:**
   - **Com `"OK"`:** Faça merge de `group/g3-boot-joycons` na `main` (`git checkout main && git merge --no-ff group/g3-boot-joycons && git push origin main`) e inicie o **Grupo G-4**.
   - **Com `"FALHOU"`:** Reverta `T-04` (`git checkout group/g3-boot-joycons && git revert --no-edit HEAD`), recompile e teste novamente.

---

# GRUPO G-4 — Fase 4: Áudio e Vídeo das Cutscenes (`T-09`)
**Ramo do Grupo:** `group/g4-audio-video`  
```bash
cd /home/nepo/Downloads/SWITCH/smo-recomp-android && git checkout main && git pull origin main && git checkout -b group/g4-audio-video
```

### T-09 — Corrigir áudio soluçando (`AAudio 4x burst`) e aumentar fila de decodificação de vídeo das cutscenes (`QMAX = 1024`)
- **Pedido atendido:** P-4, P-5 · **Depende de:** Portão G-3 aprovado com `"OK"` · **Risco:** baixo
- **Ramo da tarefa:** `fix/09-audio-underruns-and-cutscene-queue`
  ```bash
  cd /home/nepo/Downloads/SWITCH/smo-recomp-android && git checkout group/g4-audio-video && git checkout -b fix/09-audio-underruns-and-cutscene-queue
  ```
- **Arquivos permitidos:**
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/runtime/android/android_main.c`
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/runtime/smo_audio.c`
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/runtime/android/android_movie.c`
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/CHANGELOG.md`
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/docs/JOURNAL.md`
- **Pré-checagem:**
  ```bash
  cd /home/nepo/Downloads/SWITCH/smo-recomp-android
  grep -n "AAudioStream_setBufferSizeInFrames(g_stream, AAudioStream_getFramesPerBurst(g_stream) \* 2);" runtime/android/android_main.c
  grep -n "#define RING_START 2880" runtime/smo_audio.c
  grep -n "#define QMAX 256" runtime/android/android_movie.c
  ```
  **Saída esperada literal:**
  ```text
  293:    AAudioStream_setBufferSizeInFrames(g_stream, AAudioStream_getFramesPerBurst(g_stream) * 2);
  200:#define RING_START 2880
  49:#define QMAX 256                                      /* decoded samples queued per track */
  ```
- **Mudança:**

  #### 1) Em `/home/nepo/Downloads/SWITCH/smo-recomp-android/runtime/android/android_main.c`:
  - **Antes (âncora exata):**
    ```c
        AAudioStream_setBufferSizeInFrames(g_stream, AAudioStream_getFramesPerBurst(g_stream) * 2);
    ```
  - **Depois:**
    ```c
        int32_t burst = AAudioStream_getFramesPerBurst(g_stream);
        int32_t target_frames = burst * 4 < 1920 ? 1920 : burst * 4;
        AAudioStream_setBufferSizeInFrames(g_stream, target_frames);
    ```

  #### 2) Em `/home/nepo/Downloads/SWITCH/smo-recomp-android/runtime/smo_audio.c`:
  - **Antes (âncora exata):**
    ```c
    #define RING_FRAMES 32768
    #define RING_TARGET 5760
    #define RING_START 2880
    ```
  - **Depois:**
    ```c
    #define RING_FRAMES 32768
    #define RING_TARGET 7680
    #define RING_START 3840
    ```

  #### 3) Em `/home/nepo/Downloads/SWITCH/smo-recomp-android/runtime/android/android_movie.c`:
  - **Antes (âncora exata):**
    ```c
    #define QMAX 256                                      /* decoded samples queued per track */
    ```
  - **Depois:**
    ```c
    #define QMAX 1024                                     /* decoded samples queued per track (expanded to prevent cutscene audio/video desync) */
    ```

- **Pós-checagem:**
  ```bash
  cd /home/nepo/Downloads/SWITCH/smo-recomp-android
  grep -n "int32_t target_frames = burst \* 4 < 1920 ? 1920 : burst \* 4;" runtime/android/android_main.c
  grep -n "#define RING_START 3840" runtime/smo_audio.c
  grep -n "#define QMAX 1024" runtime/android/android_movie.c
  ```
- **Critério de aceite:** 1 ocorrência em cada um dos 3 arquivos.
- **Se falhar:** `git restore runtime/android/android_main.c runtime/smo_audio.c runtime/android/android_movie.c` e PARE.
- **Commit e Merge no Ramo do Grupo:**
  - Commit: `fix(audio,movie): expand AAudio burst buffer to >=1920 frames and increase movie decode queue`
  - Merge no ramo do grupo:
    ```bash
    git checkout group/g4-audio-video && git merge --no-ff fix/09-audio-underruns-and-cutscene-queue -m "Merge task T-09 into group/g4-audio-video"
    ```

### 🛑 PORTÃO NO FIM DO GRUPO G-4 (`T-09`)
1. **a. Checagem de Tarefas:** Confirme que `T-09` terminou com relatório `PASSOU`. Se falhou, **PARE**, reporte e não compile.
2. **b. Compilação via `gh` e SHA-256:**
   ```bash
   git push -u origin group/g4-audio-video
   gh workflow run android.yml --repo nepotira/smo-recomp-android --ref group/g4-audio-video
   sleep 10
   RUN_ID=$(gh run list --repo nepotira/smo-recomp-android --branch group/g4-audio-video --limit 1 --json databaseId -q '.[0].databaseId')
   gh run watch "$RUN_ID" --repo nepotira/smo-recomp-android --exit-status > /tmp/ci_g4.log 2>&1
   tail -n 15 /tmp/ci_g4.log
   rm -rf /tmp/apk_g4 && mkdir -p /tmp/apk_g4
   gh run download "$RUN_ID" --repo nepotira/smo-recomp-android --dir /tmp/apk_g4
   sha256sum /tmp/apk_g4/*.apk
   ```
3. **c. Proibido publicar release:** Não crie tag nem release no GitHub.
4. **d. Guia de Teste do Grupo G-4 (entregue ao usuário no Relatório de Grupo):**
   - **Passo 1 (no terminal do PC):** Instale o APK do Grupo G-4:
     ```bash
     adb install -r /tmp/apk_g4/SMO-Recomp-v0.2.0-android-arm64.apk
     ```
   - **Passo 2 (no celular):** Abra o app **SMO Recomp**, toque em **Play**, inicie **New Game** e observe a cutscene de abertura (sincronia som/vídeo e ausência de estalos no áudio).
   - **Passo 3 (no terminal do PC — coletar log):**
     ```bash
     adb pull /sdcard/Android/data/com.nepo.smorecomp/files/log.txt /tmp/smo_log_g4.txt
     grep "smo_audio:" /tmp/smo_log_g4.txt | tail -n 15
     adb logcat -d -v time > /tmp/logcat_g4.txt
     ```
   - **PARE E ESPERE** a resposta do usuário (`"OK"` ou `"FALHOU"` com o log).
5. **e. Ação pós-resposta do usuário:**
   - **Com `"OK"`:** Faça merge de `group/g4-audio-video` na `main` (`git checkout main && git merge --no-ff group/g4-audio-video && git push origin main`) e inicie o **Grupo G-5**.
   - **Com `"FALHOU"`:** Reverta `T-09` (`git checkout group/g4-audio-video && git revert --no-edit HEAD`), recompile e teste novamente.

---

# GRUPO G-5 — Fase 5: Otimizações de Desempenho 1 a 1 (`T-05`, `T-06`, `T-07`, `T-08`)
**Ramo do Grupo:** `group/g5-performance`  
```bash
cd /home/nepo/Downloads/SWITCH/smo-recomp-android && git checkout main && git pull origin main && git checkout -b group/g5-performance
```
*(Regra de Medição do Grupo G-5: cada tarefa `T-05`, `T-06`, `T-07` e `T-08` é medida contra a etapa anterior na mesma cena em Cap Kingdom no Poco F7. Se uma tarefa não trouxer ganho mensurável, é revertida com `git revert --no-edit HEAD` e registrada como `"sem ganho"`).*

### T-05 — Desempenho #1: Ativar compilação assíncrona real de shaders/pipelines (`skip_active`) fora de combate
- **Pedido atendido:** P-3 (Mudança isolada #1 de desempenho) · **Depende de:** Portão G-4 aprovado com `"OK"` · **Risco:** baixo
- **Ramo da tarefa:** `perf/05-async-shader-skip-always`
  ```bash
  cd /home/nepo/Downloads/SWITCH/smo-recomp-android && git checkout group/g5-performance && git checkout -b perf/05-async-shader-skip-always
  ```
- **Arquivos permitidos:**
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/runtime/nvn_vk_m4.c`
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/runtime/gfx_options.c`
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/CHANGELOG.md`
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/docs/JOURNAL.md`
- **Pré-checagem:**
  ```bash
  cd /home/nepo/Downloads/SWITCH/smo-recomp-android
  grep -n "static int skip_active(void) { return skip_on() && fight_recent(); }" runtime/nvn_vk_m4.c
  grep -n "o->present = 3; o->scale = 1.0f; o->boost = 2; o->fxaa = 1; o->sharpen = 0.75f;" runtime/gfx_options.c
  ```
  **Saída esperada literal:**
  ```text
  410:static int skip_active(void) { return skip_on() && fight_recent(); }
  171:    o->present = 3; o->scale = 1.0f; o->boost = 2; o->fxaa = 1; o->sharpen = 0.75f;      /* as DEF_* above: a file without these keys */
  ```
- **Mudança:**

  #### 1) Em `/home/nepo/Downloads/SWITCH/smo-recomp-android/runtime/nvn_vk_m4.c`:
  - **Antes (âncora exata):**
    ```c
    static int skip_active(void) { return skip_on() && fight_recent(); }
    ```
  - **Depois:**
    ```c
    static int skip_active(void) { return skip_on(); }
    ```

  #### 2) Em `/home/nepo/Downloads/SWITCH/smo-recomp-android/runtime/gfx_options.c`:
  - **Antes (âncora exata):**
    ```c
    #ifdef __ANDROID__
        o->present = 3; o->scale = 1.0f; o->boost = 2; o->fxaa = 1; o->sharpen = 0.75f;      /* as DEF_* above: a file without these keys */
    #endif
    ```
  - **Depois:**
    ```c
    #ifdef __ANDROID__
        o->present = 3; o->scale = 1.0f; o->boost = 2; o->fxaa = 1; o->sharpen = 0.75f; o->shader_skip = 1;      /* as DEF_* above: async shader skip on by default on Android */
    #endif
    ```

- **Pós-checagem:**
  ```bash
  cd /home/nepo/Downloads/SWITCH/smo-recomp-android
  grep -n "static int skip_active(void) { return skip_on(); }" runtime/nvn_vk_m4.c
  grep -n "o->shader_skip = 1;" runtime/gfx_options.c
  ```
- **Critério de aceite:** 1 linha em `runtime/nvn_vk_m4.c` e 1 linha em `runtime/gfx_options.c`.
- **Se falhar:** `git restore runtime/nvn_vk_m4.c runtime/gfx_options.c` e PARE.
- **Commit e Merge no Ramo do Grupo:**
  - Commit: `perf(vulkan): enable asynchronous shader skip across all scenes on Android`
  - Merge no ramo do grupo:
    ```bash
    git checkout group/g5-performance && git merge --no-ff perf/05-async-shader-skip-always -m "Merge task T-05 into group/g5-performance"
    ```

---

### T-06 — Desempenho #2: Reduzir contenção do mutex `g_sh_lock` causada pelos workers do Container Scan em background
- **Pedido atendido:** P-3 (Mudança isolada #2 de desempenho) · **Depende de:** T-05 · **Risco:** baixo
- **Ramo da tarefa:** `perf/06-reduce-shader-worker-contention`
  ```bash
  cd /home/nepo/Downloads/SWITCH/smo-recomp-android && git checkout group/g5-performance && git checkout -b perf/06-reduce-shader-worker-contention
  ```
- **Arquivos permitidos:**
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/runtime/nvn_vk_m4.c`
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/CHANGELOG.md`
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/docs/JOURNAL.md`
- **Pré-checagem:**
  ```bash
  cd /home/nepo/Downloads/SWITCH/smo-recomp-android
  grep -n "if (nthread > 6) nthread = 6;" runtime/nvn_vk_m4.c
  ```
  **Saída esperada:** 1 ocorrência de `if (nthread > 6) nthread = 6;`.
- **Mudança:**
  Em `/home/nepo/Downloads/SWITCH/smo-recomp-android/runtime/nvn_vk_m4.c`:
  - **Antes (âncora exata):**
    ```c
        int nthread = (int)plat_cpu_count() - 2;
        if (nthread < 2) nthread = 2;
        if (nthread > 6) nthread = 6;
    ```
  - **Depois:**
    ```c
        int nthread = (int)plat_cpu_count() - 2;
        if (nthread < 2) nthread = 2;
    #ifdef __ANDROID__
        if (nthread > 3) nthread = 3;
    #else
        if (nthread > 6) nthread = 6;
    #endif
    ```
- **Pós-checagem:**
  ```bash
  cd /home/nepo/Downloads/SWITCH/smo-recomp-android
  grep -n "if (nthread > 3) nthread = 3;" runtime/nvn_vk_m4.c
  ```
- **Critério de aceite:** 1 linha correspondente em `runtime/nvn_vk_m4.c`.
- **Se falhar:** `git restore runtime/nvn_vk_m4.c` e PARE.
- **Commit e Merge no Ramo do Grupo:**
  - Commit: `perf(vulkan): cap background shader worker threads to 3 on Android to reduce lock contention`
  - Merge no ramo do grupo:
    ```bash
    git checkout group/g5-performance && git merge --no-ff perf/06-reduce-shader-worker-contention -m "Merge task T-06 into group/g5-performance"
    ```

---

### T-07 — Desempenho #3: Substituir loops escalares de `recpe` / `rsqrte` em `fpexact.h` por instruções de hardware ARM64 (`frecpe` / `frsqrte`)
- **Pedido atendido:** P-3 (Mudança isolada #3 de desempenho) · **Depende de:** T-06 · **Risco:** baixo
- **Ramo da tarefa:** `perf/07-arm64-hardware-recpe-rsqrte`
  ```bash
  cd /home/nepo/Downloads/SWITCH/smo-recomp-android && git checkout group/g5-performance && git checkout -b perf/07-arm64-hardware-recpe-rsqrte
  ```
- **Arquivos permitidos:**
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/runtime/fpexact.h`
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/CHANGELOG.md`
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/docs/JOURNAL.md`
- **Pré-checagem:**
  ```bash
  cd /home/nepo/Downloads/SWITCH/smo-recomp-android
  grep -n "static inline float recpe_f(float x)" runtime/fpexact.h
  ```
  **Saída esperada literal:**
  ```text
  82:static inline float recpe_f(float x) { uint32_t b; memcpy(&b, &x, 4); b = (uint32_t)fp_recpe_bits(b, 0); memcpy(&x, &b, 4); return x; }
  ```
- **Mudança:**
  Em `/home/nepo/Downloads/SWITCH/smo-recomp-android/runtime/fpexact.h`:
  - **Antes (âncora exata):**
    ```c
    static inline float recpe_f(float x) { uint32_t b; memcpy(&b, &x, 4); b = (uint32_t)fp_recpe_bits(b, 0); memcpy(&x, &b, 4); return x; }
    static inline double recpe_d(double x) { uint64_t b; memcpy(&b, &x, 8); b = fp_recpe_bits(b, 1); memcpy(&x, &b, 8); return x; }
    static inline float rsqrte_f(float x) { uint32_t b; memcpy(&b, &x, 4); b = (uint32_t)fp_rsqrte_bits(b, 0); memcpy(&x, &b, 4); return x; }
    static inline double rsqrte_d(double x) { uint64_t b; memcpy(&b, &x, 8); b = fp_rsqrte_bits(b, 1); memcpy(&x, &b, 8); return x; }
    ```
  - **Depois:**
    ```c
    #if defined(__aarch64__)
    static inline float recpe_f(float x) { float r; __asm__("frecpe %s0, %s1" : "=w"(r) : "w"(x)); return r; }
    static inline double recpe_d(double x) { double r; __asm__("frecpe %d0, %d1" : "=w"(r) : "w"(x)); return r; }
    static inline float rsqrte_f(float x) { float r; __asm__("frsqrte %s0, %s1" : "=w"(r) : "w"(x)); return r; }
    static inline double rsqrte_d(double x) { double r; __asm__("frsqrte %d0, %d1" : "=w"(r) : "w"(x)); return r; }
    #else
    static inline float recpe_f(float x) { uint32_t b; memcpy(&b, &x, 4); b = (uint32_t)fp_recpe_bits(b, 0); memcpy(&x, &b, 4); return x; }
    static inline double recpe_d(double x) { uint64_t b; memcpy(&b, &x, 8); b = fp_recpe_bits(b, 1); memcpy(&x, &b, 8); return x; }
    static inline float rsqrte_f(float x) { uint32_t b; memcpy(&b, &x, 4); b = (uint32_t)fp_rsqrte_bits(b, 0); memcpy(&x, &b, 4); return x; }
    static inline double rsqrte_d(double x) { uint64_t b; memcpy(&b, &x, 8); b = fp_rsqrte_bits(b, 1); memcpy(&x, &b, 8); return x; }
    #endif
    ```
- **Pós-checagem:**
  ```bash
  cd /home/nepo/Downloads/SWITCH/smo-recomp-android
  grep -n '__asm__("frsqrte %s0, %s1"' runtime/fpexact.h
  ```
- **Critério de aceite:** 1 linha correspondente em `runtime/fpexact.h`.
- **Se falhar:** `git restore runtime/fpexact.h` e PARE.
- **Commit e Merge no Ramo do Grupo:**
  - Commit: `perf(math): use native AArch64 frecpe and frsqrte instructions in fpexact.h`
  - Merge no ramo do grupo:
    ```bash
    git checkout group/g5-performance && git merge --no-ff perf/07-arm64-hardware-recpe-rsqrte -m "Merge task T-07 into group/g5-performance"
    ```

---

### T-08 — Desempenho #4: Otimizar `cb_poll` antes de `cb_sync` para evitar `vkWaitForFences` desnecessário em Compute Buffers
- **Pedido atendido:** P-3 (Mudança isolada #4 de desempenho) · **Depende de:** T-07 · **Risco:** médio
- **Ramo da tarefa:** `perf/08-compute-buffer-nonblocking-poll`
  ```bash
  cd /home/nepo/Downloads/SWITCH/smo-recomp-android && git checkout group/g5-performance && git checkout -b perf/08-compute-buffer-nonblocking-poll
  ```
- **Arquivos permitidos:**
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/runtime/nvn_vk_draw.c`
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/CHANGELOG.md`
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/docs/JOURNAL.md`
- **Pré-checagem:**
  ```bash
  cd /home/nepo/Downloads/SWITCH/smo-recomp-android
  grep -n "__attribute__((noinline)) static void cb_sync(void)" runtime/nvn_vk_draw.c
  ```
  **Saída esperada literal:**
  ```text
  2270:__attribute__((noinline)) static void cb_sync(void) {      /* guest memory must hold what compute wrote, now */
  ```
- **Mudança:**
  Em `/home/nepo/Downloads/SWITCH/smo-recomp-android/runtime/nvn_vk_draw.c`:
  - **Antes (âncora exata):**
    ```c
    __attribute__((noinline)) static void cb_sync(void) {      /* guest memory must hold what compute wrote, now */
        int mid = g_cb_live != 0;
        if (!g_cb_live && !g_cb_pending) return;
    ```
  - **Depois:**
    ```c
    __attribute__((noinline)) static void cb_sync(void) {      /* guest memory must hold what compute wrote, now */
        cb_poll();
        int mid = g_cb_live != 0;
        if (!g_cb_live && !g_cb_pending) return;
    ```
- **Pós-checagem:**
  ```bash
  cd /home/nepo/Downloads/SWITCH/smo-recomp-android
  sed -n '2270,2276p' runtime/nvn_vk_draw.c
  ```
- **Critério de aceite:** `cb_poll();` na primeira linha de `cb_sync(void)`.
- **Se falhar:** `git restore runtime/nvn_vk_draw.c` e PARE.
- **Commit e Merge no Ramo do Grupo:**
  - Commit: `perf(vulkan): poll completed GPU slots in cb_sync before blocking on fences`
  - Merge no ramo do grupo:
    ```bash
    git checkout group/g5-performance && git merge --no-ff perf/08-compute-buffer-nonblocking-poll -m "Merge task T-08 into group/g5-performance"
    ```

### 🛑 PORTÃO NO FIM DO GRUPO G-5 (`T-05`, `T-06`, `T-07`, `T-08`)
1. **a. Checagem de Tarefas:** Confirme que `T-05`, `T-06`, `T-07` e `T-08` terminaram com relatório `PASSOU`. Se qualquer uma falhou, **PARE**, reporte e não compile.
2. **b. Compilação via `gh` e SHA-256:**
   ```bash
   git push -u origin group/g5-performance
   gh workflow run android.yml --repo nepotira/smo-recomp-android --ref group/g5-performance
   sleep 10
   RUN_ID=$(gh run list --repo nepotira/smo-recomp-android --branch group/g5-performance --limit 1 --json databaseId -q '.[0].databaseId')
   gh run watch "$RUN_ID" --repo nepotira/smo-recomp-android --exit-status > /tmp/ci_g5.log 2>&1
   tail -n 15 /tmp/ci_g5.log
   rm -rf /tmp/apk_g5 && mkdir -p /tmp/apk_g5
   gh run download "$RUN_ID" --repo nepotira/smo-recomp-android --dir /tmp/apk_g5
   sha256sum /tmp/apk_g5/*.apk
   ```
3. **c. Proibido publicar release:** Não crie tag nem release no GitHub.
4. **d. Guia de Teste e Medição do Grupo G-5 (entregue ao usuário no Relatório de Grupo):**
   - **Passo 1 (no terminal do PC):** Instale o APK do Grupo G-5:
     ```bash
     adb install -r /tmp/apk_g5/SMO-Recomp-v0.2.0-android-arm64.apk
     ```
   - **Passo 2 (no celular):** Na aba **Advanced**, verifique se **When a shader is not ready** está em **Skip the draw**.
   - **Passo 3 (no celular — Medição DEPOIS vs. Baseline G-1):** Entre na cutscene de abertura e em **Cap Kingdom (Bonneton)** (mesma posição da Baseline G-1 por 30 segundos) e anote `FPS_cutscene_g5` e `FPS_cap_g5`.
   - **Passo 4 (no terminal do PC — coletar telemetria de `log.txt` e `adb logcat`):**
     ```bash
     adb pull /sdcard/Android/data/com.nepo.smorecomp/files/log.txt /tmp/smo_log_g5.txt
     grep -E "nvn_vk:|frame-time:|held" /tmp/smo_log_g5.txt | tail -n 30
     adb logcat -d -v time > /tmp/logcat_g5.txt
     ```
   - **PARE E ESPERE** a resposta do usuário (`"OK"` com o FPS medido ou `"FALHOU"` / `"SEM GANHO"`).
5. **e. Ação pós-resposta do usuário:**
   - **Com `"OK"` (ganho mensurável confirmado):** Grave a medição em `docs/BENCHMARK_POCO_F7.md`, faça merge de `group/g5-performance` na `main`:
     ```bash
     git checkout main && git merge --no-ff group/g5-performance -m "Merge group G-5 (T-05..T-08 performance) after hardware verification" && git push origin main
     ```
     Só então inicie o **Grupo G-6**.
   - **Com `"FALHOU"` ou `"SEM GANHO"`:** Reverta a última tarefa do grupo (`T-08`, depois `T-07`, `T-06`, `T-05`, uma por vez) usando:
     ```bash
     git checkout group/g5-performance && git revert -m 1 --no-edit HEAD
     ```
     Recompile pelo `gh` e teste novamente até isolar qualquer tarefa sem ganho ou culpada, registrando `"sem ganho"` em `docs/JOURNAL.md`.

---

# GRUPO G-6 — Fase 6: Chave de Resolução Nativa e Tela Cheia (`T-10`)
**Ramo do Grupo:** `group/g6-native-fullscreen`  
```bash
cd /home/nepo/Downloads/SWITCH/smo-recomp-android && git checkout main && git pull origin main && git checkout -b group/g6-native-fullscreen
```

### T-10 — Implementar a Chave de Resolução Nativa e Tela Cheia Dinâmica (`Native Fullscreen Resolution`) sem bordas pretas e sem esticar
- **Pedido atendido:** P-6, P-9 · **Depende de:** Portão G-5 aprovado com `"OK"` · **Risco:** médio
- **Ramo da tarefa:** `feat/10-native-fullscreen-resolution-toggle`
  ```bash
  cd /home/nepo/Downloads/SWITCH/smo-recomp-android && git checkout group/g6-native-fullscreen && git checkout -b feat/10-native-fullscreen-resolution-toggle
  ```
- **Arquivos permitidos:**
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/runtime/gfx_options.h`
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/runtime/gfx_options.c`
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/runtime/nvn_vk_draw.c`
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/runtime/smo_quality.c`
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/runtime/table_stubs.c`
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/android/app/src/main/java/com/mobileodyssey/game/LauncherActivity.java`
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/patches/smo_aspect_ratio_20_9.txt`
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/CHANGELOG.md`
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/docs/JOURNAL.md`
- **Pré-checagem:**
  ```bash
  cd /home/nepo/Downloads/SWITCH/smo-recomp-android
  grep -n "int boost;" runtime/gfx_options.h
  grep -n 'else if (!plat_stricmp(key, "shader_wait"))' runtime/gfx_options.c
  grep -n "android_touch_aspect((float)W / (float)(H > 0 ? H : 1));" runtime/nvn_vk_draw.c
  grep -n "void __wrap_f_4000a58394(Ctx \*c)" runtime/smo_quality.c
  grep -n 'r.addView(header("Resolution"));' android/app/src/main/java/com/mobileodyssey/game/LauncherActivity.java
  ```
  **Saída esperada literal:**
  ```text
  26:    int boost;               /* boost = auto (default, 1) | on (2, always) | off (0): the frame's threads keep their CPUs' clocks up while frames are late (Android, android_perf.c) */
  208:        else if (!plat_stricmp(key, "shader_wait")) o->shader_skip = !plat_stricmp(val, "skip");
  5472:                android_touch_aspect((float)W / (float)(H > 0 ? H : 1));
  62:void __wrap_f_4000a58394(Ctx *c) {
  263:        r.addView(header("Resolution"));
  ```
- **Mudança:**

  #### 1) Em `/home/nepo/Downloads/SWITCH/smo-recomp-android/runtime/gfx_options.h`:
  - **Antes (âncora exata):**
    ```c
        int boost;               /* boost = auto (default, 1) | on (2, always) | off (0): the frame's threads keep their CPUs' clocks up while frames are late (Android, android_perf.c) */
    } GfxOptions;
    ```
  - **Depois:**
    ```c
        int boost;               /* boost = auto (default, 1) | on (2, always) | off (0): the frame's threads keep their CPUs' clocks up while frames are late (Android, android_perf.c) */
        int native_fullscreen;   /* native_fullscreen = on (default on Android) | off: renders at the device's exact screen aspect ratio and native resolution without black bars or stretching */
    } GfxOptions;
    ```

  #### 2) Em `/home/nepo/Downloads/SWITCH/smo-recomp-android/runtime/gfx_options.c`:
  - **Antes (âncora exata):**
    ```c
    #ifdef __ANDROID__
        o->present = 3; o->scale = 1.0f; o->boost = 2; o->fxaa = 1; o->sharpen = 0.75f; o->shader_skip = 1;      /* as DEF_* above: async shader skip on by default on Android */
    #endif
    ```
  - **Depois:**
    ```c
    #ifdef __ANDROID__
        o->present = 3; o->scale = 1.0f; o->boost = 2; o->fxaa = 1; o->sharpen = 0.75f; o->shader_skip = 1; o->native_fullscreen = 1;      /* as DEF_* above: async shader skip & native fullscreen on by default */
    #endif
    ```
  - E no parser de `opt_load` em `runtime/gfx_options.c`:
    - **Antes (âncora exata):**
      ```c
              else if (!plat_stricmp(key, "shader_wait")) o->shader_skip = !plat_stricmp(val, "skip");
              else if (!plat_stricmp(key, "diagnostics")) o->diag_off = plat_stricmp(val, "on") != 0;
      ```
    - **Depois:**
      ```c
              else if (!plat_stricmp(key, "shader_wait")) o->shader_skip = !plat_stricmp(val, "skip");
              else if (!plat_stricmp(key, "diagnostics")) o->diag_off = plat_stricmp(val, "on") != 0;
              else if (!plat_stricmp(key, "native_fullscreen")) o->native_fullscreen = !plat_stricmp(val, "on") || !strcmp(val, "1");
      ```

  #### 3) Em `/home/nepo/Downloads/SWITCH/smo-recomp-android/runtime/nvn_vk_draw.c`:
  - **Antes (âncora exata):**
    ```c
    static float rs_scale(void) {
        if (g_rs < 0) {
            const char *e = getenv("SMO_VK_SCALE");
            float v = e && atof(e) > 0 ? (float)atof(e) : gfx_options()->scale;
            if (v <= 0) v = gfx_options()->handheld ? 1.0f : 1.2f;      /* Android port, resolution_scale = auto: docked 1920x1080 (SMO draws 1600x900), handheld 1280x720 */
            if (v < 0.25f) v = 0.25f;
            if (v > 2.0f) v = 2.0f;                    /* Android port: up to 2.0, drawing above the game's size as an emulator's resolution multiplier does */
            g_rs = v;
        }
        return g_rs;
    }
    ```
  - **Depois:**
    ```c
    float nvn_vk_display_aspect(int *out_w, int *out_h) {
        int w = (int)V.extent.width, h = (int)V.extent.height;
        if (w <= 0 || h <= 0) { plat_window_size(NULL, &w, &h); }
        if (w > 0 && h > 0 && w < h) { int t = w; w = h; h = t; }
        if (out_w) *out_w = w > 0 ? w : 1920;
        if (out_h) *out_h = h > 0 ? h : 1080;
        return (w > 0 && h > 0) ? (float)w / (float)h : (16.0f / 9.0f);
    }
    static float rs_scale(void) {
        if (g_rs < 0) {
            const char *e = getenv("SMO_VK_SCALE");
            float v = e && atof(e) > 0 ? (float)atof(e) : gfx_options()->scale;
            if (gfx_options()->native_fullscreen && (!e || !e[0]) && v <= 0.0f) {
                int dw = 0, dh = 0;
                nvn_vk_display_aspect(&dw, &dh);
                float base_h = gfx_options()->handheld ? 720.0f : 900.0f;
                v = dh > 0 ? (float)dh / base_h : 1.2f;
            } else if (v <= 0) v = gfx_options()->handheld ? 1.0f : 1.2f;
            if (v < 0.25f) v = 0.25f;
            if (v > 2.0f) v = 2.0f;
            g_rs = v;
        }
        return g_rs;
    }
    ```
  - E no bloco `present_pass` em `runtime/nvn_vk_draw.c`:
    - **Antes (âncora exata):**
      ```c
      #ifdef __ANDROID__
                  {   /* Android port: the picture keeps its shape (16:9) in a window of another one (a 20:9 phone): bars at the sides
                       * or above and below, cleared black; the touch layer is told where the picture is */
                      int W = (int)V.extent.width, H = (int)V.extent.height, w = W, h = H;
                      if (s4[2] > 0 && s4[3] > 0) { if ((int64_t)W * s4[3] > (int64_t)H * s4[2]) w = (int)((int64_t)H * s4[2] / s4[3]); else h = (int)((int64_t)W * s4[3] / s4[2]); }
                      if (w != W || h != H) {
                          VkClearColorValue z; memset(&z, 0, sizeof z); z.float32[3] = 1.0f;
                          vkCmdClearColorImage(R.cmd, V.images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &sr);
                          VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT};
                          vkCmdPipelineBarrier(R.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
                          b.dstOffsets[0].x = (W - w) / 2; b.dstOffsets[0].y = (H - h) / 2; b.dstOffsets[1].x = b.dstOffsets[0].x + w; b.dstOffsets[1].y = b.dstOffsets[0].y + h;
                      }
                      android_touch_aspect((float)W / (float)(H > 0 ? H : 1));
                  }
      #endif
      ```
    - **Depois:**
      ```c
      #ifdef __ANDROID__
                  {   /* Android port: when native_fullscreen is off, keep 16:9 with black bars; when native_fullscreen is on, fill 100% of the display (Hor+ camera handles aspect) */
                      int W = (int)V.extent.width, H = (int)V.extent.height, w = W, h = H;
                      if (!gfx_options()->native_fullscreen && s4[2] > 0 && s4[3] > 0) {
                          if ((int64_t)W * s4[3] > (int64_t)H * s4[2]) w = (int)((int64_t)H * s4[2] / s4[3]);
                          else h = (int)((int64_t)W * s4[3] / s4[2]);
                      }
                      if (w != W || h != H) {
                          VkClearColorValue z; memset(&z, 0, sizeof z); z.float32[3] = 1.0f;
                          vkCmdClearColorImage(R.cmd, V.images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &sr);
                          VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT};
                          vkCmdPipelineBarrier(R.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
                          b.dstOffsets[0].x = (W - w) / 2; b.dstOffsets[0].y = (H - h) / 2; b.dstOffsets[1].x = b.dstOffsets[0].x + w; b.dstOffsets[1].y = b.dstOffsets[0].y + h;
                      }
                      android_touch_aspect((float)W / (float)(H > 0 ? H : 1));
                  }
      #endif
      ```

  #### 4) Em `/home/nepo/Downloads/SWITCH/smo-recomp-android/runtime/smo_quality.c`:
  - **Antes (âncora exata):**
    ```c
    void __wrap_f_4000a58394(Ctx *c) {
    ```
  - **Depois:**
    ```c
    float nvn_vk_display_aspect(int *out_w, int *out_h);

    /* al::getSceneFrameBufferMainAspect(const al::Scene*) [0x40009cf324] -> returns float aspect in S0 */
    void __wrap_f_40009cf324(Ctx *c) {
        c->fn = 0x40009cf324ULL;
        float aspect = 16.0f / 9.0f;
        if (gfx_options()->native_fullscreen) {
            int dw = 0, dh = 0;
            aspect = nvn_vk_display_aspect(&dw, &dh);
        }
        vzero(&c->v[0]);
        c->v[0].f[0] = aspect;
    }

    /* al::Projection::setAspect(float) [0x40009c06f8] -> updates projection aspect at +0xb0 and +0x1e0 */
    void __wrap_f_40009c06f8(Ctx *c) {
        c->fn = 0x40009c06f8ULL;
        uint64_t proj = c->x[0];
        float a = c->v[0].f[0];
        if (gfx_options()->native_fullscreen && a > 1.70f && a < 1.85f) {
            a = nvn_vk_display_aspect(NULL, NULL);
            c->v[0].f[0] = a;
        }
        uint32_t bits = c->v[0].w[0];
        c->x[8] = 1;
        wr32(proj + 0xb0, bits);
        wr32(proj + 0x1e0, bits);
        wr8(proj + 0x8, 1);
    }

    void __wrap_f_4000a58394(Ctx *c) {
    ```

  #### 5) Em `/home/nepo/Downloads/SWITCH/smo-recomp-android/runtime/table_stubs.c`:
  - **Antes (âncora exata):**
    ```c
    void __wrap_f_400072e61c(Ctx *c);
    void __wrap_f_4000731fdc(Ctx *c);
    void __wrap_f_4000a58394(Ctx *c);
    ```
  - **Depois:**
    ```c
    void __wrap_f_400072e61c(Ctx *c);
    void __wrap_f_4000731fdc(Ctx *c);
    void __wrap_f_4000a58394(Ctx *c);
    void __wrap_f_40009cf324(Ctx *c);
    void __wrap_f_40009c06f8(Ctx *c);
    ```
  - E na tabela `hooks[]` de `runtime/table_stubs.c`:
    - **Antes (âncora exata):**
      ```c
              {"__wrap_f_400072e61c", (void *)__wrap_f_400072e61c},
              {"__wrap_f_4000731fdc", (void *)__wrap_f_4000731fdc},
              {"__wrap_f_4000a58394", (void *)__wrap_f_4000a58394},
          };
      ```
    - **Depois:**
      ```c
              {"__wrap_f_400072e61c", (void *)__wrap_f_400072e61c},
              {"__wrap_f_4000731fdc", (void *)__wrap_f_4000731fdc},
              {"__wrap_f_4000a58394", (void *)__wrap_f_4000a58394},
              {"f_40009cf324", (void *)__wrap_f_40009cf324},
              {"f_40009c06f8", (void *)__wrap_f_40009c06f8},
          };
      ```

  #### 6) Em `/home/nepo/Downloads/SWITCH/smo-recomp-android/android/app/src/main/java/com/mobileodyssey/game/LauncherActivity.java`:
  - **Antes (âncora exata):**
    ```java
            LinearLayout r = card();
            r.addView(header("Resolution"));
            r.addView(choice(g, "console_mode", "Console mode", new String[]{"docked", "handheld"}, new String[]{"Docked", "Handheld"}, "docked",
    ```
  - **Depois:**
    ```java
            android.util.DisplayMetrics dm = getResources().getDisplayMetrics();
            int natW = Math.max(dm.widthPixels, dm.heightPixels), natH = Math.min(dm.widthPixels, dm.heightPixels);
            if (android.os.Build.VERSION.SDK_INT >= 30) {
                android.graphics.Rect bnd = getWindowManager().getMaximumWindowMetrics().getBounds();
                natW = Math.max(bnd.width(), bnd.height());
                natH = Math.min(bnd.width(), bnd.height());
            }
            float natRatio = natH > 0 ? (float) natW / (float) natH : (16f / 9f);
            String ratioLabel = String.format(java.util.Locale.US, "%dx%d (%.2f:9)", natW, natH, natRatio * 9f);
            LinearLayout r = card();
            r.addView(header("Resolution"));
            r.addView(toggle(g, "native_fullscreen", "Native fullscreen (" + ratioLabel + ")", "on", "off", "on",
                    "Detected display: " + ratioLabel + ". Expands the 3D camera field of view (Hor+) to your phone's exact aspect ratio and fills 100% of the screen without black bars or stretching."));
            r.addView(choice(g, "console_mode", "Console mode", new String[]{"docked", "handheld"}, new String[]{"Docked", "Handheld"}, "docked",
    ```

  #### 7) Em `/home/nepo/Downloads/SWITCH/smo-recomp-android/patches/smo_aspect_ratio_20_9.txt`:
  - **Antes (âncora exata):**
    ```text
    0x0045e41c 0x1e211800 0x1e271000  # al::getSceneFrameBufferMainAspect: replace 16:9 FDIV with 20:9 native aspect ratio
    ```
  - **Depois:**
    ```text
    0x009cf324 0x1e211800 0x1e271000  # al::getSceneFrameBufferMainAspect (0x40009cf324): replace 16:9 FDIV with native aspect ratio
    ```

- **Pós-checagem:**
  ```bash
  cd /home/nepo/Downloads/SWITCH/smo-recomp-android
  grep -n "int native_fullscreen;" runtime/gfx_options.h
  grep -n "void __wrap_f_40009cf324(Ctx \*c)" runtime/smo_quality.c
  grep -n '"f_40009cf324"' runtime/table_stubs.c
  grep -n '"native_fullscreen"' android/app/src/main/java/com/mobileodyssey/game/LauncherActivity.java
  ```
- **Critério de aceite:** 1 ocorrência em cada arquivo listado.
- **Se falhar:** `git restore runtime/gfx_options.h runtime/gfx_options.c runtime/nvn_vk_draw.c runtime/smo_quality.c runtime/table_stubs.c android/app/src/main/java/com/mobileodyssey/game/LauncherActivity.java patches/smo_aspect_ratio_20_9.txt` e PARE.
- **Commit e Merge no Ramo do Grupo:**
  - Commit: `feat(graphics): add dynamic Native Fullscreen Resolution toggle with Hor+ 3D projection`
  - Merge no ramo do grupo:
    ```bash
    git checkout group/g6-native-fullscreen && git merge --no-ff feat/10-native-fullscreen-resolution-toggle -m "Merge task T-10 into group/g6-native-fullscreen"
    ```

### 🛑 PORTÃO NO FIM DO GRUPO G-6 (`T-10`)
1. **a. Checagem de Tarefas:** Confirme que `T-10` terminou com relatório `PASSOU`. Se falhou, **PARE**, reporte e não compile.
2. **b. Compilação via `gh` e SHA-256:**
   ```bash
   git push -u origin group/g6-native-fullscreen
   gh workflow run android.yml --repo nepotira/smo-recomp-android --ref group/g6-native-fullscreen
   sleep 10
   RUN_ID=$(gh run list --repo nepotira/smo-recomp-android --branch group/g6-native-fullscreen --limit 1 --json databaseId -q '.[0].databaseId')
   gh run watch "$RUN_ID" --repo nepotira/smo-recomp-android --exit-status > /tmp/ci_g6.log 2>&1
   tail -n 15 /tmp/ci_g6.log
   rm -rf /tmp/apk_g6 && mkdir -p /tmp/apk_g6
   gh run download "$RUN_ID" --repo nepotira/smo-recomp-android --dir /tmp/apk_g6
   sha256sum /tmp/apk_g6/*.apk
   ```
3. **c. Proibido publicar release:** Não crie tag nem release no GitHub.
4. **d. Guia de Teste do Grupo G-6 (entregue ao usuário no Relatório de Grupo):**
   - **Passo 1 (no terminal do PC):** Instale o APK do Grupo G-6:
     ```bash
     adb install -r /tmp/apk_g6/SMO-Recomp-v0.2.0-android-arm64.apk
     ```
   - **Passo 2 (no celular):** Abra o app **SMO Recomp**, toque na aba **Graphics** no topo e verifique no card **Resolution** a nova chave **"Native fullscreen (2712x1220 (20.01:9))"**.
   - **Passo 3 (no celular):** Com a chave **ligada**, toque em **Play** e verifique se o jogo preenche 100% da tela do Poco F7 sem barras pretas laterais e sem esticar o Mario.
   - **Passo 4 (no celular):** Feche o jogo, desligue a chave **Native fullscreen**, toque em **Play** e verifique se volta para 16:9 com barras laterais.
   - **Passo 5 (no terminal do PC — coletar log):**
     ```bash
     adb pull /sdcard/Android/data/com.nepo.smorecomp/files/log.txt /tmp/smo_log_g6.txt
     adb logcat -d -v time > /tmp/logcat_g6.txt
     ```
   - **PARE E ESPERE** a resposta do usuário (`"OK"` ou `"FALHOU"` com o log).
5. **e. Ação pós-resposta do usuário:**
   - **Com `"OK"`:** Faça merge de `group/g6-native-fullscreen` na `main` (`git checkout main && git merge --no-ff group/g6-native-fullscreen && git push origin main`) e inicie o **Grupo G-7**.
   - **Com `"FALHOU"`:** Reverta `T-10` (`git checkout group/g6-native-fullscreen && git revert --no-edit HEAD`), recompile e teste novamente.

---

# GRUPO G-7 — Fase 7: Registro Formal das Decisões `D-1B` e `D-2C` (`T-11`, `T-12`)
**Ramo do Grupo:** `group/g7-decisions-record`  
```bash
cd /home/nepo/Downloads/SWITCH/smo-recomp-android && git checkout main && git pull origin main && git checkout -b group/g7-decisions-record
```

### T-11 — Concluída Antecipadamente na `T-01` (Decisão `D-1B` Confirmada)
- **Pedido atendido:** P-7 · **Status:** **JÁ ATENDIDA NA T-01 (GRUPO G-0)**

### T-12 — Dispensada por Decisão Expressa `D-2C` do Usuário (`libsmo.so` e `README.md` mantidos inalterados)
- **Pedido atendido:** P-8 · **Depende de:** Portão G-6 aprovado com `"OK"` · **Risco:** baixo
- **Ramo da tarefa:** `docs/12-record-d2c-decision`
  ```bash
  cd /home/nepo/Downloads/SWITCH/smo-recomp-android && git checkout group/g7-decisions-record && git checkout -b docs/12-record-d2c-decision
  ```
- **Arquivos permitidos:**
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/docs/JOURNAL.md`
- **Mudança:** Adicionar 1 linha em `docs/JOURNAL.md` registrando:
  `- T-11 / T-12: T-11 concluída antecipadamente na T-01 (D-1B); T-12 dispensada por decisão expressa D-2C do usuário (mantidos libsmo.so no APK e README.md atual).`
- **Commit e Merge no Ramo do Grupo:**
  - Commit: `docs(journal): record completion of D-1B in T-01 and user waiver D-2C for T-12`
  - Merge no ramo do grupo:
    ```bash
    git checkout group/g7-decisions-record && git merge --no-ff docs/12-record-d2c-decision -m "Merge task T-12 into group/g7-decisions-record"
    ```

### 🛑 PORTÃO NO FIM DO GRUPO G-7 (`T-11`, `T-12`)
1. Como `T-11` já foi compilada e validada no Grupo G-0 e `T-12` apenas registra a dispensa `D-2C` em `docs/JOURNAL.md` (sem mudança de código), mescle `group/g7-decisions-record` na `main` após confirmação do usuário e avance para o **Grupo G-8**.

---

# GRUPO G-8 — Fase 8: Fechamento de Telemetria e Release Oficial `v0.3.0` (`T-13`)
**Ramo do Grupo:** `group/g8-release-v030`  
```bash
cd /home/nepo/Downloads/SWITCH/smo-recomp-android && git checkout main && git pull origin main && git checkout -b group/g8-release-v030
```

### T-13 — Consolidar telemetria real em `docs/BENCHMARK_POCO_F7.md`, atualizar `PROJECT_STATE.md` e preparar Release `v0.3.0`
- **Pedido atendido:** P-9 · **Depende de:** Grupos G-0 a G-7 aprovados com `"OK"` pelo usuário · **Risco:** baixo
- **Ramo da tarefa:** `chore/13-release-v0.3.0`
  ```bash
  cd /home/nepo/Downloads/SWITCH/smo-recomp-android && git checkout group/g8-release-v030 && git checkout -b chore/13-release-v0.3.0
  ```
- **Arquivos permitidos:**
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/docs/BENCHMARK_POCO_F7.md`
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/docs/PROJECT_STATE.md`
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/docs/JOURNAL.md`
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/CHANGELOG.md`
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/android/app/build.gradle.kts`
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/android/app/src/main/AndroidManifest.xml`
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/.github/workflows/android.yml`
  - `/home/nepo/Downloads/SWITCH/smo-recomp-android/docs/releases/v0.3.0.md` (novo arquivo autorizado)
- **Pré-checagem:**
  ```bash
  cd /home/nepo/Downloads/SWITCH/smo-recomp-android
  grep -n 'versionName = "0.2.0"' android/app/build.gradle.kts
  grep -n 'android:versionName="0.2.0"' android/app/src/main/AndroidManifest.xml
  ```
  **Saída esperada literal:**
  ```text
  14:        versionName = "0.2.0"
  5:    android:versionName="0.2.0">
  ```
- **Mudança:**
  1. Atualizar `versionCode` de `11` para `12` e `versionName` de `"0.2.0"` para `"0.3.0"` em `android/app/build.gradle.kts` e `android/app/src/main/AndroidManifest.xml`, e o nome do artefato em `.github/workflows/android.yml` para `SMO-Recomp-v0.3.0-android-arm64.apk`.
  2. Consolidar em `docs/BENCHMARK_POCO_F7.md` a tabela comparativa final Antes (`G-1`) vs. Depois (`G-6`) medida pelo usuário no Poco F7.
  3. Fechar a seção `## [0.3.0] - 2026-11-08` no `CHANGELOG.md`, extrair o texto para `docs/releases/v0.3.0.md` e atualizar `docs/PROJECT_STATE.md`.
- **Pós-checagem:**
  ```bash
  grep -n 'versionName = "0.3.0"' android/app/build.gradle.kts
  grep -n 'android:versionName="0.3.0"' android/app/src/main/AndroidManifest.xml
  ```
- **Commit e Merge no Ramo do Grupo:**
  - Commit: `chore(release): bump version to v0.3.0 and finalize Poco F7 benchmark report`
  - Merge no ramo do grupo:
    ```bash
    git checkout group/g8-release-v030 && git merge --no-ff chore/13-release-v0.3.0 -m "Merge task T-13 into group/g8-release-v030"
    ```

### 🛑 PORTÃO NO FIM DO GRUPO G-8 (`T-13` — Verificação Final e Publicação da Release `v0.3.0`)
1. **a. Checagem de Tarefas:** Confirme que `T-13` terminou com relatório `PASSOU`. Se falhou, **PARE**, reporte e não compile.
2. **b. Compilação via `gh` e SHA-256:**
   ```bash
   git push -u origin group/g8-release-v030
   gh workflow run android.yml --repo nepotira/smo-recomp-android --ref group/g8-release-v030
   sleep 10
   RUN_ID=$(gh run list --repo nepotira/smo-recomp-android --branch group/g8-release-v030 --limit 1 --json databaseId -q '.[0].databaseId')
   gh run watch "$RUN_ID" --repo nepotira/smo-recomp-android --exit-status > /tmp/ci_g8.log 2>&1
   tail -n 15 /tmp/ci_g8.log
   rm -rf /tmp/apk_g8 && mkdir -p /tmp/apk_g8
   gh run download "$RUN_ID" --repo nepotira/smo-recomp-android --dir /tmp/apk_g8
   sha256sum /tmp/apk_g8/*.apk
   ```
3. **c. Guia de Teste Final (entregue ao usuário no Relatório de Grupo):**
   - Instale `/tmp/apk_g8/SMO-Recomp-v0.3.0-android-arm64.apk`, confira que a versão `0.3.0` inicia e opera perfeitamente no Poco F7.
   - **PARE E ESPERE** a resposta `"OK"` do usuário.
4. **d. Publicação da Release Oficial `v0.3.0` (SOMENTE após o `"OK"` final do usuário no Portão G-8, via skill `release-semver`):**
   ```bash
   git checkout main && git merge --no-ff group/g8-release-v030 -m "Merge group G-8 (Release v0.3.0) into main" && git push origin main
   git tag -a v0.3.0 -m "Release v0.3.0"
   git push origin v0.3.0
   gh release create v0.3.0 /tmp/apk_g8/SMO-Recomp-v0.3.0-android-arm64.apk \
     --repo nepotira/smo-recomp-android \
     --title "SMO Recomp Android v0.3.0 — Native Fullscreen, Runtime Bridge & Stutter Fixes" \
     --notes-file docs/releases/v0.3.0.md
   ```

---

## 7. MODELOS DE RELATÓRIO OBRIGATÓRIOS PARA O FLASH

### 7.1. Relatório Padrão por Tarefa (`T-NN`)
```markdown
T-NN: PASSOU | FALHOU | BLOQUEADA | PULADA (escalar)
Ramo da Tarefa: <nome-do-ramo-da-tarefa> → mesclado em <nome-do-ramo-do-grupo>
Pré-checagem: <comando> → <saída literal>
Pós-checagem: <comando> → <saída literal>
Commit: <hash> <mensagem>
Não verificado: <listar explicitamente tudo o que depende do teste físico no Portão do Grupo — marcado como "not tested">
```

### 7.2. Relatório de Grupo (entregue obrigatoriamente em cada Portão `G-N`)
```markdown
### RELATÓRIO DE GRUPO — G-N
- **Status do Grupo:** `G-N: PRONTO PARA TESTE | FALHOU | BLOQUEADO`
- **Ramo do Grupo:** `<group/gN-...>`
- **Lista de Tarefas do Grupo:**
  - `T-XX`: `PASSOU | FALHOU` (`<hash>`)
- **Link do Artefato no GitHub Actions:** `https://github.com/nepotira/smo-recomp-android/actions/runs/<RUN_ID>`
- **SHA-256 do APK (`sha256sum`):** `<hash sha256 completo>`
- **Guia de Teste no Celular (um passo por vez):**
  1. Instalar o APK: `adb install -r /tmp/apk_gN/...apk`
  2. Passos no aparelho: `<passos claros um por vez>`
  3. O que deve aparecer / Métricas a observar: `<descrição exata>`
  4. Comandos para coletar o log (`adb logcat` e `log.txt`):
     ```bash
     adb pull /sdcard/Android/data/com.nepo.smorecomp/files/log.txt /tmp/smo_log_gN.txt
     adb logcat -d -v time > /tmp/logcat_gN.txt && grep -iE "smorecomp|mobileodyssey|AndroidRuntime|FATAL" /tmp/logcat_gN.txt | tail -n 40
     ```

**OK ou FALHOU?**
```
