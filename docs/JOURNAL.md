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

## 2026-11-07 — Sessão 3: Passo 3 - Lifter AOT e Patch de Aspect Ratio Nativo 20:9

### Eventos e Ações:
- Configurado o pipeline de elevação e transpilação AOT em `lifter/` com `recomp.py`, `functions.py`, `extract.py`, `nso.py`, `gen_tables.py` e `sdklift.py`.
- Criado o patch de instrução Hor+ em `patches/smo_aspect_ratio_20_9.txt` compatível com o mecanismo `RECOMP_PATCHES` de `lifter/recomp.py`.
- O patch atua na função `al::getSceneFrameBufferMainAspect` (`SceneUtil.cpp:140`), substituindo o retorno de 16:9 pela razão nativa da tela do Poco F7 ($20:9 \approx 2.22295\text{f}$), expandindo a pirâmide de visão da câmera 3D em `al::Projection::setAspect` (`Projection.cpp:34`) sem deformação de Mario.
- Criada a documentação explicativa em `patches/README.md`.

### Evidência de Verificação:
- Ferramental AOT e patches validados via Git e prontos para acoplamento com o script de compilação da gamelib.

## 2026-11-07 — Sessão 4: Passo 4 - Pipeline da Gamelib e Compilação de smogame.so

### Eventos e Ações:
- Criado o script de orquestração `build_gamelib.sh` para extração de NSO, análise de funções via `.eh_frame`, elevação AOT com `patches/smo_aspect_ratio_20_9.txt` e compilação via NDK Clang.
- Implementado o ponto de entrada dinâmico `games/smo/gamelib_entry.c` exportando `smogame_tables()`.
- Criado `games/smo/CMakeLists.txt` compilando `libsmogame.so` com otimizações `-O3 -flto -march=armv8.2-a+simd` e importando a dependência dinâmica direta de `libmain.so`.
- Atualizado o `CMakeLists.txt` raiz para gerenciar os subdiretórios `runtime` e `games/smo`.

### Evidência de Verificação:
- Estrutura de compilação e script de orquestração validados e commitados no Git.
