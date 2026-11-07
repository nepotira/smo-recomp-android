# Metodologia de Auditoria Comparativa: Upstream Base vs. Criações Nativas Zero-Scratch

Este documento regulamenta o protocolo mandatório de auditoria técnica sempre que incorporarmos ou avaliarmos funções vindas de bases da internet (projetos upstream, forks comunitários, recompiladores de referência) em relação às soluções de alto desempenho criadas do zero por nossa engenharia no projeto de Nintendo Switch / Android (SoC Snapdragon 8s Gen 4 / GPU Adreno 825 / HyperOS 4).

---

## 1. Princípio Fundamental de Não-Conflito
Nenhuma função externa de projeto de base deve ser adotada cegamente ou sobreposta ao nosso código sem passar pela **Matriz de Não-Conflito em 5 Vetores**. A premissa básica é que melhorias da comunidade foram frequentemente projetadas para SoCs genéricos ou drivers proprietários, enquanto nossas otimizações são calibradas para hardware de alta fidelidade e baixa latência (Kryo Cortex-A720/X4 e Adreno 825).

---

## 2. A Matriz de Não-Conflito em 5 Vetores

| Vetor de Auditoria | Pergunta de Triagem Factual | Risco se Conflitar | Resolução Obrigatória |
| :--- | :--- | :--- | :--- |
| **1. ABI e Símbolos** | O código upstream usa convenções ou stubs que anulam nossos símbolos nativos ou geram colisões de linker? | Falha de carregamento silencioso, chamadas indirect nulas, abort no dlopen. | Preservar símbolos fracos (`weak`), isolar namespaces e alinhar `-Wl,--wrap`. |
| **2. Pipeline Gráfico Vulkan** | A divisão de batches ou novos layouts de push descriptors conflitam com nosso flush assíncrono ou Z-buffer? | Queda de profundidade, Z-fighting, estouro de GMEM Adreno 8xx. | Testar interação entre fragmentação de draws (`nvn_vk_draw.c`) e retenção de cache. |
| **3. CPU Affinity e Scheduler** | O "CPU Boost" do upstream reseta a afinidade fixada no cluster Cortex-A720? | Thermal throttling prematuro do núcleo prime Cortex-X4. | Manter as threads de renderização e compilação de shaders travadas nos núcleos A720. |
| **4. Scoped Storage e Namespaces** | O `applicationId` ou o caminho `externalDataPath` diverge da pasta onde o usuário já extraiu os dados do jogo? | O app entra em loop infinito e fica em tela preta silenciosa (`ALooper_pollOnce`). | Unificar IDs de pacote (`com.mobileodyssey.game`) e implementar checagem ativa com aviso visual. |
| **5. Silício Real (Adreno 825 / SM8735)** | O patch foi validado no mesmo silício ou foi desenhado para Adreno 750 / Snapdragon 8 Gen 3? | Perda de performance ou clock da GPU incompatível com o governor do HyperOS 4. | Validar empiricamente a frequência da GPU e comportamento do driver Turnip 8xx. |

---

## 3. Estudo de Caso: Auditoria da Release Beta 0.2.0 (0.2) de Super Mario Odyssey Android
Avaliando a nova versão lançada pela comunidade em relação ao nosso projeto:

1. **Submissões Fragmentadas no Adreno (`nvn_vk_draw.c`):**
   - *Upstream:* Divide draws em ~14 submissões menores para manter o clock da GPU em 310 MHz em vez de 231 MHz.
   - *Análise de Conflito:* Benéfico e convergente com nossa arquitetura. Não conflita com o desentrelaçamento Block Linear NEON nem com o sink AAudio. **Aprovado.**
2. **Limitador de Resolução por Degraus (`smo_quality.c`):**
   - *Upstream:* Estabiliza o Dynamic Resolution Scaling para evitar 5 oscilações por segundo.
   - *Análise de Conflito:* Totalmente estável e evita thrashing de memória GMEM no Adreno 825. **Aprovado.**
3. **Pacing Livre a 60 FPS (`display.c`):**
   - *Upstream:* Descarta espera desnecessária de VSync quando um frame atrasa.
   - *Análise de Conflito:* Melhora a consistência de 1% lows. **Aprovado.**
4. **Precisão Matemática e NoContraction (`power-moon-recomp`):**
   - *Upstream:* Força operações precisas em shaders de vértice e geometria para evitar furos no modelo do Mario.
   - *Análise de Conflito:* Essencial para a integridade visual sem impacto no Snapdragon. **Aprovado.**
5. **Harmonização de Pacote e Scoped Storage:**
   - *Upstream:* `com.mobileodyssey.game` com dados em `/sdcard/Android/data/com.mobileodyssey.game/files`.
   - *Conflito Identificado:* Nosso build inicial usava `com.nepo.smorecomp`, que ficava cego aos arquivos já existentes no aparelho do operador, causando congelamento em tela preta.
   - *Correção Aplicada:* Alinhamento completo do `applicationId` para `com.mobileodyssey.game` e inclusão de checagem transparente de integridade dos arquivos no `LauncherActivity`.
