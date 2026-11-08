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
