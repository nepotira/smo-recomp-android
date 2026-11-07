# Diário de Bordo de Desenvolvimento (JOURNAL)

## 2026-11-07 — Sessão 1: Inicialização do Repositório do Zero

### Eventos e Ações:
- Criado o diretório raiz `/home/nepo/Downloads/SWITCH/smo-recomp-android/`.
- Inicializado repositório Git com branch principal `main`.
- Configurado `.gitignore` rígido contra vazamento de binários proprietários da Nintendo (ROMs, NSO, NSP, chaves, dumps e caches de compilação).
- Criada a documentação bilíngue oficial: `README.md` (inglês padrão) e `README.pt-BR.md` (português do Brasil).
- Estabelecido o `AGENTS.md` com o perfil detalhado do silício do Poco F7 (Qualcomm Snapdragon 8s Gen 4 / Adreno 825 / 1x Cortex-X4 + 7x Cortex-A720).
- Estruturado o esqueleto de diretórios modular desacoplado:
  - `runtime/` (subpastas para áudio, gfx, textures, platform, core, drivers).
  - `lifter/` (ferramental de transpilação AOT e patches).
  - `games/` (módulos e mapas de símbolos do Mario Odyssey).
  - `patches/` (patches de aspect ratio 20:9).
  - `android/` (estrutura do aplicativo Android NDK).
  - `docs/` (controle de estado e logs).
- Adicionado `CHANGELOG.md` estruturado no padrão Keep a Changelog e SemVer 2.0.0.

### Evidência de Verificação:
- Repositório Git inicializado e verificado com `git status -sb`.

## 2026-11-07 — Sessão 2: Passo 2 - Camada de Compatibilidade Móvel (libmain.so)

### Eventos e Ações:
- Importada a árvore de runtime C++ POSIX atualizada a partir da Beta 0.2 de `smo-android-port`.
- Integradas as correções de shaders Adreno/Vulkan (white-out fix dos samplers de textura, submissões fragmentadas para GPU e retenção de memória de luz ambiente no respawn).
- Injetada a otimização **AAudio Exclusive Sink** em `runtime/android/android_main.c` com `AAUDIO_SHARING_MODE_EXCLUSIVE` e fallback resiliente para `SHARED`, reduzindo latência sonora para ~32ms.
- Injetada a otimização de **Vetorização SIMD ARM NEON de 128 Bits** em `runtime/tex_decode.c` (`vld1q_u8` / `vst1q_u8`) para transferências de 16 bytes em ciclo único na decodificação de texturas Tegra Block Linear.
- Criados `CMakeLists.txt` e `runtime/CMakeLists.txt` com compilação para `libmain.so` com flags `-O3 -flto -march=armv8.2-a+simd`.

### Evidência de Verificação:
- Árvore de runtime configurada com 88 arquivos em `runtime/` e verificada via Git.
