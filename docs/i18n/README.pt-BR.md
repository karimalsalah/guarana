**Idiomas:** [English](../../README.md) · [简体中文](README.zh-CN.md) · [Español](README.es.md) · [Português (Brasil)](README.pt-BR.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [Français](README.fr.md) · [Deutsch](README.de.md) · [Русский](README.ru.md) · [العربية](README.ar.md) · [Índice](README.md)

# Guarana

**Emulação de Nintendo Switch 2 ajustada para GPUs integradas.**

Guarana é uma bifurcação de código aberto do [Lichii](https://github.com/zortos293/Lichii) (branch `nx2`) para Windows x64. É voltada para notebooks e consoles portáteis com gráficos integrados: primeiro Intel Arc, depois AMD Radeon. O foco é um ritmo de quadros mais uniforme, geração de quadros, inicialização rápida e uma divisão mais eficiente do orçamento de energia.

> [!IMPORTANT]
> O Guarana **não inclui** chaves, firmware, jogos nem arquivos do sistema. Você precisa dos seus, extraídos de um console e de jogos que você possui. Não é afiliado à Nintendo nem ao projeto Lichii. Veja o aviso legal abaixo.

## O que é

- Um emulador gratuito e de código aberto sob a licença GPL-3.0-or-later.
- Ajustado para GPUs integradas, com memória compartilhada e energia limitada.
- Requer Windows x64, uma CPU com AVX2 e um driver de GPU atualizado.
- Não inclui chaves, firmware, jogos nem arquivos do sistema.

## Resultado medido

Em um notebook com Core Ultra 9 285H, GPU integrada Arc 140T, driver Intel 32.0.101.9175 e 32 GB de RAM, a correção do roteamento de memória global (`EDEN_NX2_GM=11`) levou Hyrule Field, em *Ocarina of Time*, de **14,8 fps** para **22,8 fps** de jogo. A medição foi feita no mesmo ponto, com a opção ligada e desligada, e com a geração de quadros desativada.

É um único dado, de um equipamento, um jogo e uma cena. Outros dispositivos, drivers, jogos e cenas não foram medidos. Você pode [ajudar a medir](../../docs/BENCHMARKING.md).

## Status de compatibilidade

Só a Arc 140T foi testada. As demais GPUs (Arc 140V, Arc B390, Radeon 780M, 880M, 890M e outras) ainda não foram testadas. Um [relatório de dispositivo](../../docs/DEVICES.md#send-a-device-report) seu ajuda. Equipamentos com 16 GB de memória compartilhada estão abaixo do mínimo confortável; veja [DEVICES.md](../../docs/DEVICES.md).

## Início rápido

1. Baixe o zip mais recente em [Releases](https://github.com/karimalsalah/guarana/releases). Essa página é o único download oficial.
2. Extraia o zip inteiro em uma pasta onde você tenha permissão de escrita e execute **`lichii.exe`**.
3. Instale suas chaves e firmware: **Tools → Install Decryption Keys** e depois **Tools → Install Firmware**.
4. Adicione sua pasta de jogos (clique duplo na lista vazia) e inicie um jogo. Na primeira vez em cada área, os shaders são compilados e há travadas; depois eles são carregados do cache.
5. Opcional: ative **Enable Frame Generation** em **Emulation → Configure… → Graphics**.

Configurações, saves e caches ficam em `%APPDATA%\lichii`, a mesma pasta usada pelo Lichii original. Faça um backup antes de alternar entre os dois. Todas as chaves de configuração estão em [docs/switches.md](../../docs/switches.md), e problemas comuns em [TROUBLESHOOTING.md](../../docs/TROUBLESHOOTING.md).

## Chaves, firmware e jogos (aviso legal)

- Use suas próprias chaves, firmware e jogos, extraídos de hardware que você possui. O Guarana não os fornece e não aceita que sejam compartilhados em issues, PRs ou discussões.
- Não atendemos pedidos nem links para arquivos protegidos por direitos autorais, nem perguntas sobre como obtê-los.
- O Guarana é independente. Não é afiliado nem patrocinado pela Nintendo ou pelo projeto Lichii. "Nintendo" e "Nintendo Switch" são marcas da Nintendo, citadas apenas para indicar a compatibilidade do software. Texto completo: [LEGAL.md](../../LEGAL.md).

## Como contribuir

- **Sem código:** rode o [benchmark](../../docs/BENCHMARKING.md) no seu equipamento e envie os resultados, ou envie um [relatório de dispositivo](../../docs/DEVICES.md#send-a-device-report).
- **Com código:** escolha uma issue marcada com `good first issue`, leia [CONTRIBUTING.md](../../CONTRIBUTING.md) e siga o guia de [compilação](../../docs/Build.md).
- **Agentes de IA:** leia antes [AGENTS.md](../../AGENTS.md), a [arquitetura](../../docs/ARCHITECTURE.md) e a [política de IA](../../docs/AI_POLICY.md). Uma pessoa é responsável por cada PR.

## Links

- [README em inglês (referência)](../../README.md) · [Site](https://karimalsalah.github.io/guarana/) · [Perguntas frequentes](../../docs/FAQ.md)
- [Roteiro](../../ROADMAP.md) · [Changelog](../../CHANGELOG.md) · [Licença](../../LICENSE.txt)

## Licença

O Guarana é software livre, distribuído sob a **GNU General Public License v3.0 ou posterior**.

Esta tradução pode estar desatualizada em relação à versão em inglês. Em caso de diferença, vale o README em inglês. A tradução ainda não foi revisada por um falante nativo.
