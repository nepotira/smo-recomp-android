# Resolução de Problemas e Auditoria Técnica (TROUBLESHOOTING)

Data de criação: 2026-11-07  
Projeto: SMO-Recomp-Android  
Ambiente Alvo: Xiaomi Poco F7 (Snapdragon 8s Gen 4 / Adreno 825 / HyperOS 4 / Android 17)

---

## 1. Auditoria das Próprias Alegações (v0.1.0 e v0.2.0)

| Afirmação Anterior | Classificação Real | Evidência / Status Factual |
| :--- | :--- | :--- |
| *"Causa-raiz comprovada da tela preta na v0.1.0"* | **HIPÓTESE (Refutada ou Incompleta)** | **Evidência:** O usuário instalou a v0.2.0 e a tela preta persistiu com as barras do sistema visíveis. O fato de os arquivos não estarem na pasta pode ser um fator, mas NÃO é a causa única nem suficiente. A causa real está sob investigação aguardando logs. |
| *"v0.2.0 100% funcional"* | **HIPÓTESE (Não Verificada)** | **Evidência:** O CI verde no GitHub Actions apenas comprova que o código compilou e linkou. Não houve teste físico no aparelho do operador. Status correto: `not tested`. |
| *"Instala por cima ou compartilha diretamente os dados"* | **HIPÓTESE (Falsa / Alto Risco)** | **Evidência:** Nosso APK foi assinado com a nossa chave (`smo_recomp_key` em `android/sign_apk.sh`). O app funcional no aparelho do operador foi assinado pela release do upstream (`release.keystore` de `Deez-coder`). O Android **rejeita** a instalação por cima (`INSTALL_FAILED_UPDATE_INCOMPATIBLE`). Desinstalar o app funcional para instalar o nosso apagará todos os dados extraídos pelo operador. |
| *"libsmo.so de 71 MB pronta para release pública"* | **ALERTA CRÍTICO DE LICENÇA** | **Evidência:** A biblioteca contém as 257 funções geradas pelo lifter a partir do binário comercial do jogo (obra derivada). Distribuí-la em release pública no GitHub viola a regra de direitos autorais e a política de distribuição limpa. |

---

## 2. Análise de Risco: `applicationId` e Chaves de Assinatura

1. **O Risco da Reinstalação:**
   - Se o nosso APK usa `applicationId = "com.mobileodyssey.game"`, o Android o identifica como o mesmo aplicativo da referência funcional.
   - Porém, como as chaves criptográficas de assinatura são diferentes:
     - Tentar instalar o nosso APK por cima da referência resulta em falha: `INSTALL_FAILED_UPDATE_INCOMPATIBLE`.
     - O único jeito que o Android permitiria instalar seria **desinstalando** previamente o app da referência funcional.
     - **PERIGO:** Ao desinstalar o app funcional, o Android **apaga sumariamente** a pasta `/sdcard/Android/data/com.mobileodyssey.game/files/`, destruindo todos os gigabytes de dados que o usuário já havia extraído!
2. **Recomendação Segura:**
   - **NÃO desinstalar** o app de referência.
   - Realizar backup completo dos dados existentes via ADB para o computador antes de qualquer tentativa.
   - Retornar o nosso aplicativo para um `applicationId` próprio (ex.: `com.nepo.smorecomp`) e utilizar seleção de pasta via Storage Access Framework (SAF) ou importador de dados em vez de colisões perigosas de namespace.

---

## 3. Análise de Licença: Origem e Conteúdo da `libsmo.so` (71 MB)

1. **Origem:** Extraída da release pública `B0.2` do projeto `Deez-coder/SMO-android-port`.
2. **Conteúdo:** Contém o runtime em C linkado com centenas de objetos `.obj` transpilados diretamente do binário AArch64 de *Super Mario Odyssey*.
3. **Implicação Jurídica:**
   - Binários transpilados de jogos proprietários constituem **obras derivadas** de software comercial protegido por direitos autorais.
   - A regra universal da Constituição (§8 e `global/skills/licencas-e-direitos/`) proíbe hospedar ou publicar código derivado de software proprietário em repositórios ou releases públicas.
4. **Conduta Recomendada ao Usuário:**
   - Não publicar o binário `libsmo.so` pré-compilado na release pública do GitHub.
   - Adotar o modelo limpo: a release pública deve conter apenas o APK com o runtime de emulação/compatibilidade, exigindo que as funções transpiladas sejam geradas ou fornecidas localmente pelo usuário a partir de sua própria cópia legal do jogo.

---

## 4. Estado da Investigação da Tela Preta na v0.2.0

* **Sintoma Observado pelo Usuário:** O aplicativo abre, as barras de navegação/status do sistema permanecem visíveis, e a tela fica completamente preta/escura sem iniciar o jogo. O mesmo jogo abre normalmente no app de referência funcional.
* **Status da Investigação:** Reaberta. Nenhuma causa-raiz será afirmada sem evidência factual.
* **Ação Imediata:** Aguardar o envio do log completo do logcat gerado pelo operador via ADB no computador Linux conectado ao Xiaomi Poco F7.
