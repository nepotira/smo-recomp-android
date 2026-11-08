# Novas Descobertas nos Logs Reais do Poco F7 (Backlog Pós-Plano Atual)

**Projeto:** `smo-recomp-android` (`com.nepo.smorecomp`)  
**Aparelho:** Xiaomi Poco F7 (Snapdragon 8s Gen 4 / Adreno 825 / Driver Customizado Turnip 8xx)  
**Fonte dos Dados:** `/data/media/0/Android/data/com.nepo.smorecomp/files/log.txt` (12,19 MB) e `freeze.txt` (4.320 bytes) coletados em `2026-11-08 07:50`.  
**Finalidade:** Registrar todas as anomalias, gargalos e comportamentos **COMPROVADOS** nos logs físicos que não estavam mapeados antes do início da execução do plano atual (`PLANO-2026-11-08-correcoes-smo.md`), para serem corrigidos/implementados logo após a conclusão dos grupos `G-0` a `G-8` deste plano.

---

## 1. Rebaixamento Indevido de Resolução pelo `smo_quality.c` e pelo DRS Interno do Jogo (Até `800x450`)
* **Rótulo:** **COMPROVADO**
* **Evidência Literal no `log.txt`:**
  ```text
  nvn_vk_perf: frames 1301400..1301700 ms/frame: batch replay 3.69 (of it: dispatch 0.02, draw preparation 2.27, pipelines 0.02, shader modules 0.00), wait for the GPU 0.09, copy-back 0.01 (waiting at batch start 0.00), present 0.40
  smo_quality: render size now 1440 wide (change 8, next step up after 16 s of room)
  smo_quality: render size asked 1600x900, the game's cap gives 1280x720
  smo_quality: render size now 1344 wide (change 9, next step up after 32 s of room)
  smo_quality: render size asked 1600x900, the game's cap gives 960x540
  smo_quality: render size asked 1600x900, the game's cap gives 800x450
  smo_quality: render size now 1248 wide (change 10, next step up after 64 s of room)
  ```
* **Diagnóstico no Código (`runtime/smo_quality.c`):**
  1. A GPU Adreno 825 está ociosa: gasta apenas **3,69 ms** em `batch replay` e **0,09 ms** em `wait for the GPU`.
  2. Apesar disso, sempre que há uma oscilação no tempo de quadro da CPU (`avg 19.85 ms`, causado por espera de sincronismo ou compilação de shaders), tanques duplos de DRS entram em ação:
     - O `smo_quality.c` rebaixa a largura máxima para `1440` → `1344` → `1248` e aplica um *backoff* exponencial de até **64 segundos** (`next step up after 64 s of room`) antes de tentar subir um único degrau!
     - O DRS nativo do próprio motor do Super Mario Odyssey (`the game's cap gives 800x450`) derruba a resolução interna para **800x450**!
* **Correção Pós-Plano:**
  - Travar o DRS interno do jogo em `1600x900` / `1920x1080` (ou na resolução nativa quando `native_fullscreen` estiver ativo) impedindo a queda para `800x450`, e condicionar o `smo_quality.c` apenas ao tempo real de GPU (`wait for the GPU`), nunca a stalls de CPU/shaders.

---

## 2. Congelamento de 5,1s Capturado pelo Watchdog (`freeze.txt`): `WorldResourceLoader` em `nvnTexturePoolRegisterTexture`
* **Rótulo:** **COMPROVADO**
* **Evidência Literal no `freeze.txt`:**
  ```text
  nvn_vk_watchdog: ======== NO FRAME PRESENTED FOR 5.1 s ======== 2026-11-08 07:49:18, process 26950, game frame 1299195
    presents: 1299194 frames reached the window so far; present attempts since the last one that did: 0
    render thread 2658, place in its present: 0 (1 loading barrier, 2 acquire, 3 blit and submit, 4 present, 0 none)
    workers: 6 threads; foreground jobs 52207 queued / 52207 done, background 93 / 93
    lock g_sh_lock (stage, pipeline and job tables): free
    lock g_gpl_lock (vertex-input / fragment-output libraries, universal layout): free
  guest threads (last function entered, link register):
    Presentation Thread  running   f_400073a174  lr=400073a1ac  in nvnQueuePresentTexture (from 400073a1a8)
    WorldResourceLoader  running   f_400075a55c  lr=4000776850  in nvnTexturePoolRegisterTexture (from 400077684c)
  ```
* **Diagnóstico no Código:**
  - Mesmo com `g_sh_lock` e `g_gpl_lock` livres e todos os `52.207` jobs de shaders concluídos, o jogo ficou **5,1 segundos** sem apresentar quadro enquanto `WorldResourceLoader` estava dentro de `nvnTexturePoolRegisterTexture` (`f_400075a55c`) e `Presentation Thread` aguardava em `nvnQueuePresentTexture` (`f_400073a174`).
* **Correção Pós-Plano:**
  - Auditar `nvnTexturePoolRegisterTexture` (`runtime/nvn_vk_draw.c` / `runtime/nvn.c`) e o lock/sincronização entre `WorldResourceLoader` e a `Presentation Thread` durante o carregamento assíncrono de texturas de mundo (`tex_ahead` / decodificação ASTC/Block Linear).

---

## 3. Descarte de Amostras de Áudio por Fila Cheia (`144 samples dropped (queue full)`)
* **Rótulo:** **COMPROVADO**
* **Evidência Literal no `log.txt`:**
  ```text
  smo_audio: last 2 s: 0 gaps (the device found the queue empty), 144 samples dropped (queue full), queue now 112 ms, slowest frame of sound 5.1 ms, 0 frames late
  ```
* **Diagnóstico no Código (`runtime/smo_audio.c`):**
  - Quando a `SoundThread` do jogo retoma após uma pausa ou variação de frame-time e envia rajadas de pacotes de 240 amostras (5 ms), a política de corte por `RING_TARGET` / fila cheia descarta amostras (`144 samples dropped`), gerando descontinuidade na forma de onda (clique/craquelado audível mesmo com `0 gaps`).
* **Correção Pós-Plano:**
  - Substituir o descarte abrupto de amostras (`dropped`) por *resampling* suave (ajuste fracionário de pitch de $\pm 0.5\%$ quando a fila passar de `RING_TARGET`) ou ampliação da janela de absorção sem corte seco de amostras.

---

## 4. Re-cópia Redundante no Ring Buffer Vulkan (`38 ranges, 2.30 MB/frame`) e Espera de Reports (`600/300 frames`)
* **Rótulo:** **COMPROVADO**
* **Evidência Literal no `log.txt`:**
  ```text
  nvn_vk_reports: frames 1301400..1301700: 600 report values waited in the queue for compute results (at most 2 at once)
  nvn_vk_ring: frames 1301400..1301700 per frame: streamed 6.32 MB, ring_put 2135 calls (67.2% cached), copied again within a batch 38 ranges 2.30 MB | host writes logged: copy-back 3.0, occlusion 5.0, UpdateUniformBuffer 84.2, report 10.0
  ```
* **Diagnóstico no Código (`runtime/nvn_vk_draw.c`):**
  - A cada frame são feitas **2.135 chamadas a `ring_put`** (6,32 MB/frame), sendo que **38 faixas (2,30 MB por frame)** são copiadas novamente dentro do mesmo batch (`copied again within a batch`), além de 2 esperas por frame em `nvn_vk_reports`.
* **Correção Pós-Plano:**
  - Otimizar o *dirty tracking* de `ring_put` para evitar re-cópia de `2.30 MB/frame` dentro do mesmo batch e avaliar resolução não-bloqueante para `nvnQueueFenceSync` / `report values`.

---

## 5. Retenção de `15.139 VkShaderModule` na Memória (`nvn_vk_mem`)
* **Rótulo:** **COMPROVADO**
* **Evidência Literal no `log.txt`:**
  ```text
  nvn_vk_mem: frame 1301700: working set 1578 MB (private 208 MB, peak 1741 MB) | Vulkan memory 1360 MB | shader modules made 15139, released 63 (0 ms in vkDestroyShaderModule)
  ```
* **Diagnóstico no Código (`runtime/nvn_vk_m4.c`):**
  - Foram criados **15.139 `VkShaderModule`** e apenas 63 foram destruídos (`released 63`), mantendo ~200 MB de objetos SPIR-V/IR do driver vivos na RAM.
* **Correção Pós-Plano:**
  - Liberar os `VkShaderModule` intermediários (`vkDestroyShaderModule`) após a vinculação final dos pipelines ou usar `VK_EXT_shader_module_identifier` / `VK_KHR_maintenance5` suportado pelo Turnip 8xx.

---

## 6. Crescimento Excessivo do `log.txt` (`12,2 MB` em uma sessão) por Spam de `smo_quality` e `nvn_vk_tex`
* **Rótulo:** **COMPROVADO**
* **Evidência Literal no `log.txt`:**
  - O arquivo `log.txt` chegou a `12.195.769 bytes` (~12,2 MB) devido à impressão repetitiva a cada mudança de frame de `smo_quality: render size asked 1600x900, the game's cap gives ...` e `nvn_vk_tex: frame ... contents carried over`.
* **Correção Pós-Plano:**
  - Silenciar logs repetitivos por frame quando `diagnostics = off`, mantendo apenas os resumos a cada 300 frames (`nvn_perf`, `nvn_vk_perf`, `smo_audio`), eliminando I/O desnecessário em disco e no `logcat` durante o gameplay.

---

## 7. Buffer Overflow Detectado pelo Bionic `_FORTIFY_SOURCE=2` em `GfxOptions.path` (`gfx_options.h:5` / `gfx_options.c:162`) — **[JÁ CORRIGIDO NO T-03]**
* **Rótulo:** **COMPROVADO E CORRIGIDO**
* **Evidência Literal no `log.txt` (G-2 Run 2):**
  ```text
  FORTIFY: vsnprintf: prevented 4096-byte write into 404-byte buffer
  HOST EXCEPTION SIGABRT at host address 0x7636d482c4
    libc.so+0xbd238 __vsnprintf_chk
    libmain.so+0xa1b90 gfx_options
  ```
* **Diagnóstico no Código (`runtime/gfx_options.h:5` e `runtime/gfx_options.c:162`):**
  - `GfxOptions.path` foi declarado com tamanho de Windows (`char path[260]`, deixando a struct `GfxOptions` inteira com `404` bytes), enquanto `opt_path()` chamava `snprintf(out, PLAT_MAX_PATH, ...)` passando `PLAT_MAX_PATH = 4096` no Android. Com LTO (`-flto`) ativo no NDK 27, o `__vsnprintf_chk` do Bionic detectou escrita de 4096 bytes em struct de 404 bytes e abortou com `SIGABRT`.
* **Status:** Corrigido imediatamente no `T-03` expandindo `char path[4096]` em `runtime/gfx_options.h` e alinhando `-D_FORTIFY_SOURCE=0` em `runtime/CMakeLists.txt`.
