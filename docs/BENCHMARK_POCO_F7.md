# Protocolo de Validação e Benchmarking em Silício: Xiaomi Poco F7

**Dispositivo:** Xiaomi Poco F7 (Qualcomm Snapdragon 8s Gen 4 / GPU Adreno 825)  
**SoC:** SM8735 (1x Cortex-X4 @ 3.21 GHz + 3x Cortex-A720 @ 3.00 GHz + 2x Cortex-A720 @ 2.80 GHz + 2x Cortex-A720 @ 2.02 GHz)  
**Display:** AMOLED 1.5K ($2712 \times 1220$ pixels, proporção nativa $\approx 20:9$ / $2.22295\text{f}$)  
**SO:** Android 17 / HyperOS 4 sobre Kernel Linux AArch64 (páginas de 16 KB)  
**Driver Gráfico:** Mesa Turnip Custom Adreno 8xx via AdrenoTools Hook  
**Estado Físico da Sessão:** `not tested` (Dispositivo não conectado via ADB durante a execução; procedimentos de validação detalhados abaixo).

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
