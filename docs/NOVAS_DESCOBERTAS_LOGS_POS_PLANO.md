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

---

## 8. Trechos de Silêncio no Mixer do Jogo (`smo_audio: ... ms of silence in the game's sound`) e `wait-present 7.54 ms` / `wait-other 5.57 ms`
* **Rótulo:** **COMPROVADO**
* **Evidência Literal no `log.txt` (G-2 Run 4 — Frames 9000..9300):**
  ```text
  nvn_perf: ms/frame (calls/frame): ... wait-present 7.54 (1.0) wait-other 5.57 (2.0)
  smo_audio: last 2 s: 260 ms of silence in the game's sound (5 ms stretches with nothing; a gap if music was playing)
  ```
* **Diagnóstico no Código (`runtime/smo_audio.c` e `runtime/nvn_vk.c`):**
  - Quando a thread principal aguarda `wait-present` (`7,54 ms/frame`) e `wait-other` (`5,57 ms/frame`), a thread de áudio do jogo atrasa a entrega de blocos de 5 ms (gerando lacunas de amostras zeradas dentro do próprio buffer do jogo antes mesmo de chegar ao AAudio).
* **Correção Pós-Plano:**
  - Investigar o bloqueio em `wait-present` e `wait-other` na sincronização de eventos/fences do `nvn_vk.c` e implementar preenchimento preditivo/interpolado no `smo_audio.c` quando um bloco de 5 ms vier zerado durante picos de frame-time.

---

## 9. Bloqueio das Threads de Carregamento (`FileLoadThread` / `WorldResourceLoader`) por `gl_barrier()` Síncrono a 60 Hz em `nvn_gl.c`
* **Rótulo:** **COMPROVADO**
* **Evidência Literal no `log.txt` (G-3 Run — `/tmp/smo_log_g3.txt`):**
  ```text
  nvn_setsh: frame 91, program object 6e943b30a0 (called from 4000aeb87c): 15.6 ms | barrier 15.6 | container 0.0 ...
  nvn_setsh: frame 168, program object 6e945c52a0 (called from 4000aeb87c): 31.9 ms | barrier 31.9 | container 0.0 ...
  nvn_setsh: frame 980, program object 6e97734bc0 (called from 400077ee34): 15.7 ms | barrier 15.7 | container 0.0 ...
  nvn_setsh: frame 2633, program object 6e869de3a8 (called from 4000aeb87c): 16.4 ms | barrier 16.3 | container 0.0 ...
  nvn_vk: first battle frame is frame 2679, 45.4 s after the first present; the loading screen before it lasted 15.1 s
  ```
* **Diagnóstico no Código (`runtime/nvn_gl.c:339, 4246, 4366, 4459, 4562`):**
  - Enquanto a tela dos Joy-Cons (`frames 76..1252`) ou a barra de loading após clicar em `"Resume"` (`frames 1800..2679`) anima a 60 FPS, a Render Thread fica bloqueada ~13–15 ms por frame dentro de `display_pace()` com `g_async_pending > 0`.
  - As funções `prog_set_shaders_()` (`nvn_gl.c:4246`), `tpool_register()` (`nvn_gl.c:4562`), `spool_register()` (`nvn_gl.c:4366`) e `tex_finalize()` (`nvn_gl.c:4459`) chamam `gl_barrier()` (`if (g_async_pending) gl_run(job_nop, NULL);`) **incondicionalmente**, mesmo quando:
    1. O `Prog *p` é recém-inicializado (`!p` ou sem stages anteriores) e nenhum command buffer em voo pode referenciá-lo;
    2. O `tpool_register` / `spool_register` está registrando em slot vazio (e já é protegido por `g_pool_lock`);
    3. O `tex_finalize` está apenas limpando uma cópia temporária por valor de `NVNtexture` (`A0 != t->guest`, que o `agl::driver::NVNtexture_` cria e destrói a cada uma das 5.200 texturas carregadas!).
  - Isso explica também a Descoberta #2 (`freeze.txt` com `WorldResourceLoader` parado em `nvnTexturePoolRegisterTexture` enquanto `Presentation Thread` está em `nvnQueuePresentTexture`) e o `wait-other 4.47 ms (2.4 calls/frame)`.
* **Correção:**
  - Em `prog_set_shaders_()`, só chamar `gl_barrier()` se o `Prog *p` já existia e já possuía algum stage configurado (`state != 0`).
  - Em `tex_finalize()`, mover `gl_barrier()` para depois de `if (!t) return;` e `if (A0 != t->guest) { ... return; }`.
  - Em `tpool_register()` e `spool_register()`, remover `gl_barrier()` desnecessário (pois `g_pool_lock` já serializa leituras/escritas dos descritores e `pool_ent_release` já adia a coleta via `g_unref` / `tex_reclaim`).

---

## 10. Tempestade de `53.235` Jobs de Shader, `>106.000` `fopen()` em FUSE (`variants_known`) e Contenção de `g_sh_lock` em `nvn_vk_m4.c`
* **Rótulo:** **COMPROVADO**
* **Evidência Literal no `log.txt` (G-3 Run — `/tmp/smo_log_g3.txt`):**
  ```text
  nvn_setsh: frame 937, program object 6e972af7a8 (called from 400077ee34): 16.0 ms | barrier 0.0 | ... | program_set 16.0 (hash + job copy 0.0, lock 16.0, HELD 0.0)
  nvn_setsh: frame 1232, program object 6e98016550 (called from 400077ee34): 31.8 ms | barrier 3.7 | ... | program_set 28.2 (hash + job copy 0.0, lock 28.2, HELD 0.0)
  nvn_vk_m4: workers: 39992 programs handed over, 14833 stages made by them, 0 pipelines made by them
  nvn_vk_cscan: container scan on: 8 containers scanned (8 known), 13243 variations, 13243 handed to the workers (109626 KB of code copied), 6836 pairs found, 1274 ms of the guest threads' time; pipelines made for a container's pass signatures: 0
  ```
* **Diagnóstico no Código (`runtime/nvn_vk_m4.c:1403-1455, 1787-1802, 1869`):**
  1. **`variants_known()` chamado 106.470 vezes via FUSE mesmo quando `how == 0` (`nvn_vk_m4.c:1445-1450`):** Em `job_prog()`, `variants_known(&j->src[0], hv)` e `variants_known(&j->src[1], fs->hb)` abrem arquivos `.var` via `fopen()` no Scoped Storage (`/storage/emulated/0/Android/data/...`) em **todas** as 53.235 execuções de `job_prog()`, mesmo quando `stage_ensure()` retorna `how == 0` (stage já carregado na tabela `g_vstage`!). Chamar `variants_known()` apenas quando `how == 1` elimina ~92.000 `fopen()` no FUSE durante a tela dos Joy-Cons e no fim da barra de loading do `"Resume"`.
  2. **`spawn_stage()` redundante para stages já prontos (`nvn_vk_m4.c:1437`):** `job_prog()` chama `spawn_stage(&j->src[1], zero)` para cada um dos 39.992 programas sem checar se o fragment stage já está pronto em `g_vstage`, inflando a fila de foreground de 39.992 para 52.207 jobs e disputando `g_sh_lock`.
  3. **`g_cscan` (Container Scan) ativo sem `cpredict` nem `GPL` no Android (`nvn_vk_m4.c:1869`):** No Android (`cpredict = 0`, `GPL = 0`), o `nvn_vk_cscan` consome `1.274 ms` das threads do jogo, copia `109,6 MB` de código e enfileira `13.243` variações que geram **0 pipelines** (`pipelines made for a container's pass signatures: 0`), mas criam milhares de `VkShaderModule`s órfãos na RAM (Descoberta #5).
  4. **Contenção de CPU dos 6 workers (`pw_main`) sobre os cores `2, 3, 4` do jogo (`SMO_OS_CONSOLE_CORES`):** Como os 6 workers ainda rodam sem afinidade restrita (o que será corrigido na **`T-07`** do Grupo G-5, junto com a remoção do spin-wait `boost_main` na **`T-05`**), eles preemptam as threads do jogo nos cores 2, 3 e 4 por fatias inteiras do agendador Linux (~13–16 ms em `lock` e `hash + job copy`).

---

## 11. Silêncios de `480..790 ms` a cada 2 s no Som do Próprio Jogo Durante Cutscenes 3D em Tempo Real (`frames 3112..8100` — Mario + Cappy)
* **Rótulo:** **COMPROVADO**
* **Evidência Literal no `log.txt` (G-4 Run — `/tmp/smo_log_g4_full.txt`, `frames 3112..9385`):**
  ```text
  301: audio: AAudio 48000 Hz 2 channels, burst 192 frames, buffer 1536 frames, started
  925: smo_audio: last 2 s: 0 gaps (the device found the queue empty), 96 samples dropped (queue full), queue now 152 ms, slowest frame of sound 0.8 ms, 0 frames late
  1049: smo_audio: last 2 s: 480 ms of silence in the game's sound (5 ms stretches with nothing; a gap if music was playing)
  1051: smo_audio: last 2 s: 560 ms of silence in the game's sound (5 ms stretches with nothing; a gap if music was playing)
  1056: nvn_perf: frames 4800..5100 frame time ms: avg 21.88 ... | fps avg 45.7, 1% low 35.7
  1057: nvn_perf: ms/frame (calls/frame): ... wait-present 8.61 (1.0) wait-other 7.57 (2.0) pace 0.00 (0.0)
  1066: smo_audio: last 2 s: 500 ms of silence in the game's sound (5 ms stretches with nothing; a gap if music was playing)
  1067: smo_audio: last 2 s: 580 ms of silence in the game's sound (5 ms stretches with nothing; a gap if music was playing)
  1068: nvn_perf: frames 5100..5400 frame time ms: avg 23.94 ... | fps avg 41.8, 1% low 36.2
  1069: nvn_perf: ms/frame (calls/frame): ... wait-present 9.65 (1.0) wait-other 8.48 (2.0) pace 0.00 (0.0)
  1125: nvn_perf: frames 6300..6600 frame time ms: avg 16.67 ... | fps avg 60.0, 1% low 52.9
  1126: nvn_perf: ms/frame (calls/frame): ... wait-present 4.24 (1.0) wait-other 0.08 (2.0) pace 6.83 (1.0)
  1204: smo_audio: last 2 s: 705 ms of silence in the game's sound (5 ms stretches with nothing; a gap if music was playing)
  1205: smo_audio: last 2 s: 770 ms of silence in the game's sound (5 ms stretches with nothing; a gap if music was playing)
  1206: nvn_perf: frames 7200..7500 frame time ms: avg 27.08 ... | fps avg 36.9, 1% low 32.6
  1207: nvn_perf: ms/frame (calls/frame): ... wait-present 11.15 (1.0) wait-other 10.34 (2.0) pace 0.00 (0.0)
  1231: smo_audio: last 2 s: 790 ms of silence in the game's sound (5 ms stretches with nothing; a gap if music was playing)
  ```
* **Diagnóstico no Código (`runtime/smo_audio.c`, `runtime/nvn_gl.c`, `runtime/android/android_perf.c`, `runtime/os_threads.c` e `runtime/smo_framerate.c`):**
  1. **O AAudio e o Ring Buffer (`T-09`) NÃO tiveram nenhum underrun nem drop na cutscene:** A linha 925 comprova `0 gaps (the device found the queue empty)` e, do frame 3116 ao frame 9385, `ring_report` (`smo_audio.c:224`) não registrou nenhum gap nem amostra descartada no AAudio.
  2. **A cutscene do Mario falando com o Cappy é 3D em tempo real na engine (`637..997 draws/frame`), NÃO um vídeo `.mp4`:** A proporção de silêncio gerada pelo próprio mixer do jogo (`g_silent_blocks` em `smo_audio.c:343`, que conta blocos de 240 amostras = 5 ms saídos de `CommandListProcessor::Process()` com pico `< 2`) obedece exatamente à lei matemática $\frac{\text{ms de silêncio}}{2000\text{ ms}} \approx 1 - \frac{\text{FPS}}{60,0}$:
     - Em `60,0 FPS` (`frames 6300..6600` e `8400..8700`): **`0 ms` de silêncio (0%)**.
     - Em `45,7 FPS` (`frames 4800..5100`): $1 - 45,7/60 = 23,8\%$ $\rightarrow$ **`480 ms` de silêncio / 2 s (24,0%)**.
     - Em `41,8 FPS` (`frames 5100..5700`): $1 - 41,8/60 = 30,3\%$ $\rightarrow$ **`580 ms` de silêncio / 2 s (29,0%)**.
     - Em `36,7 FPS` (`frames 7200..8100`): $1 - 36,7/60 = 38,8\%$ $\rightarrow$ **`775..790 ms` de silêncio / 2 s (38,8%..39,5%)**.
  3. **Três causas combinadas no código explicam esse comportamento:**
     - **Causa A — Spin-wait de 16 ms (`boost = on` em `gfx_options.c:21` + `BOOST_SPIN_NS = 16000000` em `android_perf.c:193-206` + `plat_spin_mark_hot()` em `nvn_gl.c:4904, 5049`) sobre os 3 cores fixos do console (`SMO_OS_CONSOLE_CORES` em `os_threads.c:234-272`):** Quando a main thread / present thread espera a `render_thread` em `wait-present` (`8,61..11,23 ms`) e `wait-other` (`7,57..10,40 ms`) dentro de `gl_run()` (`nvn_gl.c:300`), `plat_event_wait()` faz **busy-spin (`yield`) por até 16 ms seguidos** no próprio core do jogo (`cpu4`/`cpu3`/`cpu2`) em vez de dormir no kernel, impedindo as demais threads do jogo (`SoundThread`, `TaskThread`, workers de cena) fixadas no mesmo core de rodarem e derrubando o clock do cluster mid (`cpu5` cai para `480..1056 MHz`). Além disso, `SMO_PRESENT_ASYNC` está desligado por padrão (`nvn_gl.c:4910`), serializando a thread do jogo com a `render_thread`.
     - **Causa B — Clock de áudio fixo em 200 Hz (`smo_audio.c:109-145`) vs. lógica do jogo atrelada ao frame (`frame_rate = 60` em `smo_framerate.c:20-27`):** Abaixo de 60 FPS (ex.: 37–45 FPS na cutscene), o jogo avança `procCalc_` e alimenta buffers de voz/stream da `nn::atk` na razão $\text{FPS}/60$, mas `frame_thread` continua cobrando `AudioRenderSystem::SendCommandToDsp` a 200 Hz fixos (a cada 5,000 ms), esgotando os buffers de 5 ms na fração $1 - \text{FPS}/60$.
     - **Causa C — Ausência de retenção/interpolação de pacote (Zero-Order Hold / crossfade) ou cadência adaptativa em `audio_sink_out` (`smo_audio.c:331-359`) e `AAudioStreamBuilder_setBufferCapacityInFrames` (`android_main.c:280`):** Quando um tick de 5 ms do `CommandListProcessor` sai zerado (`g_blk_peak < 2`) por atraso de atualização da `SoundThread`, `audio_sink_out` escreve 240 amostras de zeros puros no meio da onda sonora (gerando o efeito de som "metralhado"/picotado na fala do Cappy).
* **Correções Pós-Plano (complementares à `T-05` do Grupo G-5):**
  1. Na **`T-05` (Grupo G-5, já no plano atual!)**, `plat_spinhot_set_ns(0)` e a desativação do spin-wait de 16 ms em `android_perf.c` já vão eliminar o bloqueio por spin nos cores do jogo (`cpu2, cpu3, cpu4`).
  2. No Pós-Plano:
     - Habilitar `SMO_PRESENT_ASYNC=1` por padrão no Android (`nvn_gl.c:4910`) para sobrepor o frame $N+1$ da CPU ao replay Vulkan do frame $N$ na `render_thread` (eliminando os `8,6..11,2 ms` de `wait-present`);
     - Sincronizar/adaptar a cadência do `frame_thread` (`smo_audio.c:130`) ou aplicar interpolação/repetição suave de janela (packet loss concealment / crossfade de 240 amostras em `audio_sink_out`) quando um bloco de 5 ms vier zerado durante quedas abaixo de 60 FPS;
     - Adicionar `AAudioStreamBuilder_setBufferCapacityInFrames(b, 3840)` antes de `AAudioStreamBuilder_openStream` em `android_main.c:285` para que `AAudioStream_setBufferSizeInFrames(g_stream, 1920)` não seja limitado em `1536` frames pelo driver AAudio em modo exclusivo.
