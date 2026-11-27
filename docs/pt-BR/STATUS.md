# Estado do projeto

[English](../en/STATUS.md) · **Português** · Atualizado em 26/11/2026 · Versão 0.6.3

Esta página é o resumo atual. Investigações datadas, medições e evidências por issue ficam no [registro técnico](log/README.md). As pendências estão no [roteiro](ROADMAP.md).

## Qualificação

`gameplay_verified` e `multiplayer_verified` continuam `false`. A qualificação exige, juntos: menu, corrida inteira, reinício/save, imagem, áudio, controle e velocidade do relógio. O multijogador exige validação separada em duas máquinas. Os marcos abaixo são observações, não um percentual de conclusão.

| Área | Progresso atual | Próximo marco |
| --- | --- | --- |
| Launcher | Janela nativa, preparação guiada com fonte alternativa para baixar o Zig no Windows, instalador, atualizações, desinstalação, relatórios; inglês, português do Brasil e espanhol | Atualização a partir de versões anteriores em mais PCs |
| Corrida solo | Taça Cogumelo inteira, com replays e progresso preservados após reinício; batalhas, MKTV e Contra o Relógio alcançados; as 160 pistas (48 base, 48 DLC, 64 CTGP) carregadas no Contra o Relógio | Qualificação completa de imagem, áudio, controle e reinício/save |
| Mii | Criador com opções numeradas quando a prévia 3D está indisponível, perfis e o renderer Mii do próprio jogo (retratos, roupas, piloto, minimapa) | Mii nos lobbies LAN, comparação com recursos do console |
| Multijogador | Matchmaking nativo Nextendo pelo Online Play; salas Steam com convites; testes LAN locais | Uma corrida Nextendo até o resultado; uma corrida Steam completa entre dois PCs |
| Gráficos | Runtime Vulkan com correção de bloom em resoluções ampliadas em Vancouver Velocity; FSR 1, FSR 3.1.6 (executado na RX 580), FSR 4 / DLSS (só testes de capacidade); ultrawide (também com geração de quadros) e geração de quadros experimentais | DLSS em RTX, FSR 4 em RDNA 4; mais pistas e GPUs |
| Desempenho | Cerca de 60 FPS nas janelas de corrida medidas no PC de teste | Estabilidade de VRAM em copas repetidas, engasgos com cache frio, mais hardware |
| Controles | Direção por movimento via SDL e leitura automática experimental com um Switch Pro Controller via Bluetooth e a Entrada Steam no Windows | [Verificação de controles](log/controllers.md#movimento-do-switch-pro-controller-com-entrada-steam-164) |
| Linux | Construído da árvore principal; AppImage/tar.gz no CI em Ubuntu 24.04; renderização em Intel Iris Xe | Corrida em Steam Deck físico |

## Mudanças após a 0.6.3

- A tela cheia oculta por padrão o ponteiro do mouse quando fica parado. A preferência é compartilhada entre o launcher e os ajustes de vídeo do jogo. [Ocultação do cursor em tela cheia](log/graphics.md#ocultação-do-cursor-em-tela-cheia)

## Versão 0.6.3

- **0.6.3:** as corridas voltam a rodar a 60 FPS. Desde a 0.6.0, as verificações do upscaler e da geração de quadros liam variáveis de ambiente em todo draw; num Grand Prix com 12 pilotos numa RX 580, um terço da thread de renderização ia para isso, e a corrida ficava entre 27 e 45 FPS, em câmera lenta (#133). Agora cada opção é lida uma vez. A 0.6.3 também traz as correções da 0.6.2 abaixo.

- **0.6.2:** no Linux, o launcher volta a reconhecer a build (as capacidades de tela dividida e LAN eram lidas de uma DLL do Windows, #128); com a Steam no Wayland, o jogo usa XWayland para o Steam Controller sair do modo desktop (#131); resultados de oclusão indisponíveis no driver não encerram mais o jogo (#127).

- **0.6.1:** o Nextendo passou a ter 45 s para renovar o login e buscar o token do jogo; antes, com conexão lenta, o token nem era pedido e o jogo fechava com código 2 (#125). O log registra o motivo e o launcher explica a falha.

## Versão 0.6.0

- **Linux na mesma árvore:** o porte Linux ([PR #83](https://github.com/Vnnc1/MK8-online/pull/83)) agora é construído a partir da árvore principal, junto com o Windows. O launcher Linux procura um Python 3.12 de 64 bits (#112, #119) e inicializa os controles pelo SDL (#120).
- **PRs integradas:** temas do Menu (#92), distância da câmera (#107), 60 FPS em 3–4 jogadores (#108), opção de bloom (#109), entrada Steam com 1–2 jogadores (#110), controles (#75), correções Vulkan (#102, #104, #105, #106), recursos Mii após mover a pasta portátil (#116), mods de RomFS (#117) e documentação (#100).
- **Correções desta rodada:** cena 3D preta na primeira corrida com o cache vazio; idioma do navegador no launcher e no Menu; chinês simplificado e tradicional (#99); ultrawide mantido com geração de quadros; estouro de VRAM em resolução alta; vídeos substituíveis por mods; Pascal/GTX 10 (#114).
- **Testes:** 140 testes Rust com formatação e Clippy; 196 testes Python em 48 módulos das áreas alteradas; fluxo completo pelo launcher numa instalação limpa (ferramentas, build, DLC, Mii, corrida).

## Última validação local

- Build limpa com jogo, atualização, DLC e chaves de uma vez: 26,7 min, DLC importada em cerca de 20 s, corrida aberta pelo launcher a 60 FPS.
- RX 580 2048SP: Mario Kart Stadium a 60 FPS com o cache vazio; Twisted Mansion em 2× (4K interno) sem esgotar a VRAM em 3 de 3 corridas (antes, 3 de 4 esgotavam); ultrawide 32:9 com 120 de 120 quadros gerados.
- Menu principal em chinês simplificado; mod de RomFS servindo o vídeo do menu.

## Pendências

- Retângulo de luz com bordas retas no piso de pistas escuras (Twisted Mansion) e luzes dos karts no CTGP.
- A resolução dinâmica do jogo refaz os alvos de renderização a cada quadro quando a GPU não mantém 60 FPS.
- Prévia do Mii diferente depois de trocar o save por outro (o save guarda uma cópia do Mii).
- GPUs integradas Intel, monitor 4K em tela cheia e monitor 32:9 real precisam de reteste com logs de jogadores.
- Corrida Steam entre dois PCs e reconexão no Nextendo.

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
