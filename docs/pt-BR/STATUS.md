# Estado do projeto

[English](../en/STATUS.md) · **Português** · Atualizado em 20/11/2026 · Versão 0.5.0

Esta página é o resumo atual. Investigações datadas, medições e evidências por issue ficam no [registro técnico](log/README.md). As pendências estão no [roteiro](ROADMAP.md).

## Qualificação

`gameplay_verified` e `multiplayer_verified` continuam `false`. A qualificação exige, juntos: menu, corrida inteira, reinício/save, imagem, áudio, controle e velocidade do relógio. O multijogador exige validação separada em duas máquinas. Os marcos abaixo são observações, não um percentual de conclusão.

| Área | Progresso atual | Próximo marco |
| --- | --- | --- |
| Launcher | Janela nativa, preparação guiada, instalador, atualizações, desinstalação, relatórios; inglês, português do Brasil e espanhol | Atualização a partir de versões anteriores em mais PCs |
| Corrida solo | Taça Cogumelo inteira, com replays e progresso preservados após reinício; batalhas, MKTV e Contra o Relógio alcançados; as 160 pistas (48 base, 48 DLC, 64 CTGP) carregadas no Contra o Relógio | Qualificação completa de imagem, áudio, controle e reinício/save |
| Mii | Criador, perfis e o renderer Mii do próprio jogo (retratos, roupas, piloto, minimapa) | Mii nos lobbies LAN, comparação com recursos do console |
| Multijogador | Matchmaking nativo Nextendo pelo Online Play; salas Steam com convites; testes LAN locais | Uma corrida Nextendo até o resultado; uma corrida Steam completa entre dois PCs |
| Gráficos | Runtime Vulkan; FSR 1, FSR 3.1.6 (executado na RX 580), FSR 4 / DLSS (só testes de capacidade); ultrawide e geração de quadros experimentais | DLSS em RTX, FSR 4 em RDNA 4; mais pistas e GPUs |
| Desempenho | Cerca de 60 FPS nas janelas de corrida medidas no PC de teste | Estabilidade de VRAM em copas repetidas, engasgos com cache frio, mais hardware |
| Linux | AppImage/tar.gz compilados e testados no CI em Ubuntu 24.04; renderização em Intel Iris Xe | Corrida em Steam Deck físico |

## Versão 0.5.0

- **Windows:** o instalador e o ZIP portátil são a revisão validada pelo mantenedor. O ZIP abriu de uma pasta temporária e reutilizou o runtime pré-compilado após conferir chave e hashes.
- **Linux:** o AppImage e o tar.gz vêm do porte de mitch030504 ([PR #83](https://github.com/Vnnc1/MK8-online/pull/83)). O CI em Ubuntu 24.04 passou nos testes Rust e de ELF, gerou os dois pacotes, iniciou o launcher e o importador empacotados e executou atualização e reversão headless do tar.gz.
- **Testes:** passaram 109 testes Rust e 35 testes de navegador do launcher e instalador, com formatação e Clippy. O teste Rust optativo que cria uma sala Steam real não foi executado.

## Última validação local

- Uma build limpa recompilou as 484 unidades do MK8 4.0.1 em 31 min 47 s; a repetição imediata levou 46 s.
- 430 de 431 testes Python descobertos passaram no Windows; a bateria dedicada de gráficos passou nos 46 casos.
- Varredura em Contra o Relógio a 1920 × 1080 numa RX 580 2048SP: mediana de 60 FPS nas pistas base e CTGP, 59,1–60 no DLC. Pior janela de ~2 s: 48,1 FPS (DLC). Ainda há rajadas de compilação com cache frio (carregamento de 6,3 s com 503 compilações em Squeaky Clean Sprint).
- Dois shaders de partículas do DLC que falhavam na consulta de mip `TMML.B` agora são traduzidos e compilados.

## Pendências

- Votação de pistas com modelos Mii ausentes (relato com RTX 5070 Ti): corrigido localmente, falta reteste na máquina do jogador.
- Crash ao abrir o solo sem DLC numa RTX 4090 (driver 621.24): não reproduzido na RX 580; falta confirmação nesse hardware.
- Corrida Steam entre dois PCs e reconexão no Nextendo.
- Crescimento de VRAM entre pistas e engasgos de shaders no primeiro uso.

A lista completa e priorizada está no [roteiro](ROADMAP.md).

## Medições

Xeon E5-2666 v3 / 16 GB RAM / RX 580 2048SP, trechos de corrida de uma Taça Cogumelo completa:

| Pista | FPS médio | Tempo de quadro p95 |
| --- | ---: | ---: |
| Mario Kart Stadium | 59,99 | 17,46 ms |
| Water Park | 59,98 | 17,45 ms |
| Sweet Sweet Canyon | 59,97 | 17,46 ms |
| Thwomp Ruins | 59,99 | 17,33 ms |

Excluem as janelas de captura e não garantem 60 FPS em toda cena nem em outros PCs.

## Como atualizar esta página

Mantenha esta página curta e atual: substitua linhas desatualizadas em vez de acrescentar. Detalhes datados (relatórios, logs, medições, raciocínio) vão para o registro técnico, uma seção por tema, a mais recente primeiro. Atualize o [resumo em inglês](../en/STATUS.md) na mesma mudança.
