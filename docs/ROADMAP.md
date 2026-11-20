# Pendências e roteiro

O MK8-online é **experimental**: ele chega ao menu, completa corridas e já rodou duas instâncias multijogador numa máquina, mas tem defeitos conhecidos. Nada aqui está qualificado (`gameplay_verified=false`). Os detalhes técnicos e as evidências de cada item ficam em [STATUS.md](STATUS.md).

Legenda: **[bloqueia]** impede o uso normal · **[visível]** atrapalha, mas dá para testar · **[validação]** funciona em teste, falta provar no uso real.

## Ultrawide e menu nativo — entrega experimental

- **Implementado e revisado no candidato isolado:** opção Original/Esticar/Ultrawide no launcher e no jogo; câmeras solo validadas entre 16:9 e 32:9, HUD proporcional nas bordas, transições e banner de classe. Menu nativo opt-in, recursos derivados privados, persistência e integração CTGP. A integração está incluída na 0.4.0.
- **Verificação automática concluída:** regressões nativas/navegador, Rust/formatação/Clippy, build host/guest com consumidores conferidos e revisão independente. O candidato novo percorreu Stadium até Water Park e persistiu uma preferência de Settings entre dois processos reais. A qualificação humana de áudio/controle/relógio e recarga de progresso novo permanece pendente; evidência manual da combinação anterior não substitui esse reteste. Manter fallback 16:9 nos caminhos sem suporte demonstrado; matriz real em STATUS.
- **Extensão TT:** o close-up inicial foi reproduzido em 16:9 e incluído com identidade/modo/flag específicos; regressões nativas e rebuild concluídos, com close-up/corrida/Settings exercitados em 21:9 e 32:9. Manter fallback nos demais caminhos desconhecidos e qualificação completa pendente.
- **Validação futura:** todas as pistas/efeitos, equivalência da tinta, MKTV/replays, Batalha/VS com equipes, online e tela dividida; outra GPU e monitor ultrawide físico. Geração de quadros com câmera ampla exige validação própria antes de habilitar. Não extrapolar evidência de uma corrida para todos os modos.
- **Câmera futura:** investigar distância do personagem separada de proporção/FOV, depois primeira pessoa opt-in; preservar o padrão e testar clipping, culling, traseira, boost, água, planador, antigravidade e resgate antes de expor opções.
- **Movimento:** ampliar o teste de direção por movimento para outros controles após a validação física de Switch Pro Controller e Joy-Con.
- **Multiplayer adiado pelo mantenedor:** o laptop tem o jogo, mas falta uma conta Steam separada. Exigir versões/DLC/CTGP compatíveis, duas contas, corrida inteira/resultados e reconexão em dois PCs, com logs sanitizados. Não bloqueia a entrega solo experimental.
- **Direção por movimento e vibração HD.** Em corrida, o piloto confirmou a direção e o planador por movimento com o Switch Pro Controller via Bluetooth. Dois Joy-Con horizontais também permitiram dirigir por movimento de forma independente na mesma corrida. Os testes físicos cobriram a separação dos lados, a mudança de frequência e a parada da vibração. Os resultados estão em [STATUS.md](STATUS.md).

## Prioridades após a 0.4.0 experimental

A 0.3.0 reúne CTGP no perfil 4.0.1 (64 pistas/16 copas adicionais), redesign padrão, DLC opcional, Miis e as PRs aceitas da fix-chore. A 0.4.0 acrescenta portabilidade no Windows, controles, melhorias gráficas e correções de saves. Veja as [notas bilíngues](releases/0.4.0.md). Não representa compatibilidade completa; o plugin CTGP original não é executado.

- **[bloqueia]** Corrigir a descoberta de DLLs Steam quando uma cópia local incompatível oculta outra instalada. Validar as interfaces SteamUser e SteamFriends nas configurações das issues #69–71.
- **[bloqueia]** Renovar corretamente a sessão autenticada na reconexão Steam à mesma sala (#32), preservando proteção antirreplay, e validar retorno/corrida entre dois PCs.
- **[bloqueia]** Investigar falha de inicialização SDL/Vulkan apesar do preflight aprovado (#5) e crashes de memória em Intel/AMD integradas (#11/#17).
- **[visível]** Retestar os defeitos gráficos #8/#26 e perfis de desempenho #7; pausas de shaders ainda existem. Mais pistas CTGP, GPUs e três/quatro jogadores precisam de cobertura.
- **[validação]** Upgrade 0.3.0 → 0.4.0, reconstrução do jogo, saves/reinício, peças Mii, DLC e bibliotecas locais em máquinas dos testadores. Corrida completa, áudio/controle/relógio e multiplayer permanecem sem qualificação global.

Os registros por versão abaixo são históricos; a revisão atual de todas as Issues está em STATUS.

Preparação 0.1.7: Python 3.12 x64 privado baixado automaticamente, com reparo de ambientes incompatíveis/incompletos e backup. Resta acompanhar instalações em outras máquinas; detalhes em STATUS.md.

Preparação 0.1.8 (testes): recuperação de caches, isolamento Python/pip, coordenação de processos e tratamento de arquivos bloqueados/unidades indisponíveis. Carregador Vulkan incluído. Resta validar em máquinas dos relatos e com antivírus/dispositivos reais; não altera a qualificação de gameplay.

Republicação 0.2.0: setup aprovado pelo usuário em 13/11, com download verificado da biblioteca Mii, preparação Python sem PowerShell e conclusão da conversão Mii acompanhada na tela Jogar. GameSir pelo receptor USB confirmado com a Steam conectada após desativar Entrada Steam para Spacewar e reiniciar jogo e launcher. Acompanhar outros controles e instalações; isso não qualifica uma corrida multiplayer.

## 1. Multijogador

- **[validação — 0.2.0]** A interface pública usa salas Steam, convites de amigos e entrada automática experimental no lobby LAN. Anfitrião Steam e dois jogos com transporte local no mesmo PC foram exercitados. Falta confirmar o convidado por outra conta/PC e corridas inteiras entre duas máquinas; AppID 480 e `steam_api64.dll` local continuam requisitos. LAN/relay/P2P abaixo permanecem caminhos de desenvolvimento pela API.

- **[gameplay online confirmado pelo mantenedor em teste]** Nextendo: em 16/11, o mantenedor conseguiu jogar com outras pessoas em MK8 4.0.1, com Miis atuais e sem CTGP. OAuth/NEX e TLS MK8/ZNC registrados; pins temporários por serviço, sem alterar a confiança do Windows. Cliente próprio, endpoints atuais e tokens somente em memória. Faltam confiança TLS definitiva, integração pública no launcher, qualificação de corrida inteira/resultados/reinício, reconexão, teste controlado entre duas instalações e amigos. Cloud saves fora do protótipo. Evidências e limites em [STATUS.md](STATUS.md); fontes/licenças em [THIRD_PARTY.md](THIRD_PARTY.md).

- **[validação]** Um erro de comunicação apareceu uma vez com duas instâncias na mesma máquina (não reproduzido na repetição). Se aparecer entre duas máquinas, testar com `MK8_ASYNC_SUBMIT=0` e enviar os relatórios dos dois PCs.

- **[validação]** Corrida entre **duas máquinas** pela LAN direta (UDP 47820 no firewall do anfitrião). O transporte e o roteiro já existem; falta repetir com dois PCs reais.
- **[validação]** Sala pela internet com **relay público** HTTPS/UDP (`mk8-relay`). Até agora só foi testado em loopback; NAT, latência e MTU não foram medidos.
- Verificado em 08/11/2026 com a build atual: relay em loopback e LAN direta com dois jogos na mesma corrida, 60 FPS, sem erro de comunicação. Corrida VS e Batalha funcionam. Vídeos dos menus corrigidos.
- **[visível]** No lobby LAN, o nome do segundo jogador aparece espelhado.
- **[visível]** Tela dividida: depois do applet de controles, a seleção de personagens para 2 jogadores ignora os botões dos dois jogadores, embora o jogo leia e receba os dois controles (diagnósticos em `toolchain/split_screen.py`).
- **[validação]** Mais de quatro jogos reais (até 12), com perda/atraso de rede e reconexão.

## 2. Imagem

- **[visível]** O halo de bloom do letreiro do estádio some em cerca de 1 a cada 4 quadros (parâmetros da cadeia secundária de bloom).
- **[visível]** Pontos coloridos, sombras e fidelidade geral da imagem.
- Corrigido: alternância de brilho da cena a 30 Hz (cubemap de ambiente lido num ID de textura reaproveitado).
- Corrigido: exposição estourada (reflexo especular BC3 gerado por shader no início da corrida escrito numa view inválida; `toolchain/bc_render.py`). 7 de 7 GPs limpos.

## 3. Desempenho (prioridade atual: VRAM)

- **[visível]** A memória de vídeo cresce ao trocar de pista. Foram corrigidas listas de comandos que sobreviviam à finalização de seus buffers/pools; algumas imagens antigas grandes continuam pertencendo a texturas não finalizadas pelo jogo. O candidato com blocos VMA de 128 MiB percorreu quatro pistas e voltou à seleção de copas, com ~60 FPS nos trechos medidos e pico reservado de 1.648 MiB no heap 0. Falta isolar o efeito dos blocos menores e verificar estabilização repetindo copas/pistas; a memória ativa caiu no menu, mas a reserva permaneceu.
- **[validação]** A liberação direta de imagens de pools permanece opt-in (`MK8_RELEASE_POOL_TEXTURES=1`). A possível regressão de FPS no GP de Thwomp Ruins não se repetiu no Contra o Relógio parcial, que não teve despachos de compute no trecho medido; a dependência específica ainda não foi identificada.

- **[visível]** Primeiro uso de efeitos: com a espera limitada (padrão, 4 ms por quadro), o efeito pode faltar por alguns quadros em vez de travar a tela; o cache em disco resolve nas corridas seguintes.
- **[visível]** Picos menores de 25–45 ms ao entrar em áreas novas (prefetch de shaders) e na tradução de comandos.
- Melhorado: anel de 32 envios, tradução de comandos numa thread própria e espera limitada de shaders. No GP completo: 59,99 FPS, p95 17,3 ms, p99 17,6 ms, pior quadro 38 ms (no início do dia: 57,9 FPS, p95 21,2 ms, picos de 160–370 ms). O jogo roda a 99,7% da velocidade real.
- Corrigido: travamento (`c0000005`) no início da corrida ao comparar samplers copiados pelo jogo.
- **[validação]** 60 FPS estáveis e velocidade correta do jogo: o MK8 avança um passo por quadro, então abaixo de 60 FPS o jogo fica em câmera lenta.

## 4. Qualificação da corrida

- **[validação]** Áudio, controle físico, reinício/save, relógio e imagem verificados juntos numa corrida inteira.
- **[validação — 0.2.0]** Criador Mii com miniaturas, prévia do corpo do jogo e preparação de recursos para o renderer original: retratos, Mii Suits, piloto e minimapa observados localmente. Falta qualificar todas as peças, o lobby LAN, comparação com recursos de console e corrida completa; a biblioteca AFL é baixada do Internet Archive com hashes verificados ou selecionada localmente pelo usuário, sem ser incluída no GitHub/instalador.

## 5. Launcher e distribuição

- Incluído no setup 0.1.6: inglês e PT-BR no launcher e instalador, com seleção e preferência persistente. Os READMEs acompanham os dois idiomas.
- **[validação]** Antes de tornar o repositório público, revisar nome/e-mail de autoria e commit no histórico Git. A revisão de 08/11 encontrou esses metadados pessoais nos 21 commits alcançáveis; a varredura de conteúdo não encontrou credenciais pelos padrões utilizados. O histórico não foi reescrito.

- **[validação]** Atualização pelo botão **Atualizar agora** contra um Release publicado de verdade (os testes unitários cobrem versão, asset e SHA-256).
- Instância única: duas janelas do Launcher competiriam pela mesma pasta de dados.
- Fechar a janela não interrompe uma preparação ou o jogo em andamento.
- Importação XCI/XCZ, DLC e outras versões do jogo estão fora do escopo até o percurso atual ser qualificado.

## Como ajudar

1. Instale pelo `setup.exe` do Release mais recente e prepare com os arquivos do **seu** console.
2. Ao encontrar um problema: **Suporte → Gerar relatório**. Abra uma issue com o `.zip`, o que você fez e o que esperava.
3. Para testar multijogador em duas máquinas, siga **Multijogador → Como testar em duas máquinas** e envie os relatórios dos dois PCs.
