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
