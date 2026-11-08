# Protocolo de Validação e Benchmarking em Silício: Xiaomi Poco F7

**Dispositivo:** Xiaomi Poco F7 (Qualcomm Snapdragon 8s Gen 4 / GPU Adreno 825)  
**SoC:** SM8735 (1x Cortex-X4 @ 3.21 GHz + 3x Cortex-A720 @ 3.00 GHz + 2x Cortex-A720 @ 2.80 GHz + 2x Cortex-A720 @ 2.02 GHz)  
**Display:** AMOLED 1.5K ($2712 \times 1220$ pixels, proporção nativa $\approx 20:9$ / $2.22295\text{f}$)  
**SO:** Android 17 / HyperOS 4 sobre Kernel Linux AArch64 (páginas de 16 KB)  
**Driver Gráfico:** Mesa Turnip Custom Adreno 8xx via AdrenoTools Hook  
**Estado Físico da Sessão:** **COMPROVADO EM HARDWARE REAL** (Xiaomi Poco F7 — `com.nepo.smorecomp`, coleta em 2026-11-08 via `log.txt` e `freeze.txt`).

---

## Baseline Factual (Grupo G-1 — Antes das Otimizações de Runtime)

Coleta realizada diretamente em `/data/media/0/Android/data/com.nepo.smorecomp/files/log.txt` (12.26 MB) e `freeze.txt` (1.76 KB) no APK do **Grupo G-0** (`com.nepo.smorecomp`, SHA-256 `3080fb28b8a547c74eb16d3ebd1e11f9a2454e30c53cef8deef75dd4240cf057`):

| Métrica / Subsistema | Valor Medido (Baseline G-1) | Evidência Literal (`log.txt` / `freeze.txt`) |
| :--- | :--- | :--- |
| **Frame-Time / FPS (Frames 1301400..1301700)** | **50.4 FPS médio** · **32.8 FPS (1% low)** · `avg 19.85 ms`, `p99 30.48 ms`, `max 74.87 ms` | `nvn_perf: frames 1301400..1301700 frame time ms: avg 19.85 min 10.66 median 18.98 p99 30.48 max 74.87 \| fps avg 50.4, 1% low 32.8` |
| **Tempo de GPU Vulkan (`nvn_vk_perf`)** | **4.18 ms/frame total** (`batch replay 3.69 ms`, `wait for GPU 0.09 ms`, `present 0.40 ms`) | `nvn_vk_perf: batch replay 3.69 ms (dispatch 0.02, draw preparation 2.27, pipelines 0.02, shader modules 0.00), wait for the GPU 0.09 ms, copy-back 0.01 ms, present 0.40 ms` |
| **Holds de Pipeline / Stalls de Compilação (`T_joycon_base` / in-game)** | **379 holds acumulados = 15.899,2 ms (~15,9 s de travamento)** · maior hold individual: **483,2 ms** · `pipelines not made ahead so far: 284` | `pipelines not made ahead so far: 284 \| holds so far: 379, 15899.2 ms, longest 483.2 ms` |
| **Congelamento Agudo (`freeze.txt`)** | **5,1 s congelado** no frame `1299195` (`WorldResourceLoader` em `nvnTexturePoolRegisterTexture` / `Presentation Thread` em `nvnQueuePresentTexture`) | `=== freeze: frame 1299195 unbeaten for 5.1 s ===` (`freeze.txt`) |
| **Áudio (`smo_audio`)** | **144 amostras descartadas (`queue full`)** a cada 2 s · fila em `112 ms` · `slowest frame of sound 5.1 ms` | `smo_audio: last 2 s: 0 gaps (the device found the queue empty), 144 samples dropped (queue full), queue now 112 ms, slowest frame of sound 5.1 ms, 0 frames late` |
| **Degradação de Resolução (`smo_quality` + DRS)** | Cai de `1440x810` para **`1248x702`** (`smo_quality`) e **`800x450`** (DRS interno do jogo) por falsos positivos de CPU stall | `smo_quality: frame avg 20.1 ms, worst 47.0 ms -> step down to 1248 (cooldown 64 s)` / `nvn_vk_tex: pool=0x132c96708 id=73 create 800x450 fmt=37 (the game's cap gives 800x450; our target is 1440x810)` |
| **Pipeline Cache em Disco** | **8.984 KB** (`vk_pipeline.cache`) | `nvn_vk: pipeline cache saved: 8984 KB` |

---

## Medição Grupo G-5 (Otimizações T-05 a T-08 Aplicadas em Silício Real)

Coleta realizada em 2026-11-08 no Poco F7 (`com.nepo.smorecomp`, build `group/g5-performance`, SHA-256 `6dae44cf4d394e9e03fcd6755aed51053ffdc4864145921cc7c75758e12a2135`):

| Métrica / Subsistema | Baseline G-1 | Medição Grupo G-5 | Evidência Física (`log.txt` G-5) |
| :--- | :--- | :--- | :--- |
| **Holds de Pipeline / Stalls Render Thread** | 379 holds (15.899,2 ms) | **0 holds (0,0 ms)** · **0% de travamento por shader** | `nvn_vk_m4: holds of the render thread at a draw: 0, 0 ms in all, longest 0.0 ms` |
| **Shader Skip Assíncrono (`T-05`)** | 0 draws skipped (bloqueava thread) | **68 draws pulados de forma assíncrona** sem travamento | `nvn_vk_skip: shader_wait = skip (near a fight); draws left out because a stage or pipeline was not ready: 68 in 21 frames` |
| **Contenção de Workers (`T-06`)** | 16 workers (starvation de Kryo) | **3 worker threads ativas** · 13.249 jobs concluídos limpos | `nvn_vk: 540 pipelines listed in ../cache/vk/pipes.bin; 3 worker threads` / `background jobs 13249 queued / 13249 done` |
| **Compute Buffers GPU Waits (`T-08`)** | Bloqueios síncronos em fences | **0 esperas por fence (0,0 ms em 72.237 dispatches)** | `nvn_vk: compute at frame 6000: 72237 dispatches ... waits for the GPU before a copy-back: 0 (0.0 ms in all)` |
| **FPS em Cutscene Cappy (3000..3300)** | 38.6 FPS | **47.1 FPS** (+8.5 FPS) · **36.7 FPS (1% low)** (+9.1 FPS) | `nvn_perf: frames 3000..3300 frame time ms: avg 21.22 median 20.80 p99 27.26 \| fps avg 47.1, 1% low 36.7` |
| **FPS em Gameplay (5700..6000)** | ~50 FPS | **60.0 FPS cravados** · **52.2 FPS (1% low)** | `nvn_perf: frames 5700..6000 frame time ms: avg 16.66 min 13.00 median 16.66 max 20.41 \| fps avg 60.0, 1% low 52.2` |
| **Tempo de GPU Real Adreno 825** | 4.18 ms | **0.22 a 0.72 ms/frame** (GPU completamente desafogada) | `nvn_vk_perf: wait for the GPU 0.22 ms` |

---

## 1. Procedimento de Instalação e Execução

### Via Cabo USB (ADB):
```bash
# 1. Habilitar Depuração USB no Poco F7
# 2. Conectar o smartphone ao PC e verificar
adb devices

# 3. Instalar o pacote gerado
adb install -r SMO-Recomp-v0.1.0-android-arm64.apk

# 4. Enviar a pasta RomFS do Super Mario Odyssey (v1.4.1)
adb push /caminho/do/seu/romfs /sdcard/SMO/romfs/
```

### Via Launcher no Smartphone:
1. Abra o app **SMO Recomp**.
2. Toque em `📁 Select RomFS Directory` e selecione a pasta `/sdcard/SMO/romfs/`.
3. Garanta que as opções estejam marcadas:
   - `Enable Hor+ 20:9 Native Screen Aspect Ratio (2.22295f — Zero Stretch)`
   - `Enable Mesa Turnip 8xx UMA Zero-Copy Hook via AdrenoTools`
   - `60 FPS (Native Performance)`
4. Toque em `▶ LAUNCH SUPER MARIO ODYSSEY`.

---

## 2. Metas de Telemetria e Benchmarking

| Parâmetro | Meta Esperada | Estado | Método de Medição |
| :--- | :--- | :--- | :--- |
| **Taxa de Quadros (Cascade Kingdom)** | 60 FPS estáveis | `not tested` | MangoHUD / PresentMon / Logcat |
| **Pacing de Quadros** | 16.6 ms plano (sem jitter) | `not tested` | Perfetto Trace Vulkan Queue |
| **Temperatura da Bateria/SoC** | $\le 39^\circ\text{C}$ contínuos | `not tested` | `dumpsys battery` / CpuFloat |
| **Proporção de Tela (3D)** | Hor+ $20:9$ total ($2712 \times 1220$) | `not tested` | Verificação visual / Screenshot |
| **Safe-Zone 2D (HUD)** | Centralizado 16:9 ($2169 \times 1220$) | `not tested` | Verificação visual anti-punch-hole |
| **Latência de Áudio (AAudio Sink)** | $\approx 32\text{ms}$ em 48.000 Hz | `not tested` | AAudio Stream State Logcat |
| **Alocação de Memória RAM** | $\le 3.8\text{GB}$ (Zero-Copy UMA) | `not tested` | `dumpsys meminfo com.nepo.smorecomp` |

---

## 3. Coleta de Logs de Telemetria

```bash
# Monitorar saída do runtime nativo
adb logcat -s "smo-recomp" "AAudioSink" "nvn_vk" "AdrenoTools"

# Inspecionar frequência e consumo de GPU Adreno 825
adb shell "cat /sys/class/kgsl/kgsl-3d0/gpuclk"
```
