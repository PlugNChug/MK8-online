# Registro técnico — CTGP e DLC

[Índice](README.md) · [English](../../en/log/ctgp-dlc.md)

## Crash ao abrir o menu principal com CTGP e configurações nativas na 0.4.0

O relatório de 19/11 registra uma build concluída em 65,1 s e o pré-teste Vulkan concluído. Com CTGP Deluxe, Booster Course Pass, recursos Mii e menu nativo ativos, a criação do menu principal terminou em acesso nulo na função guest `f_7000732a30` (exceção `0xc0000005`, saída do processo `0xc0000409`). O runtime publicado reproduziu a mesma falha no mesmo ponto com uma cópia dos saves. Nos controles, tanto CTGP com o menu nativo desligado quanto o menu nativo sem CTGP passaram pela abertura.

A preparação da 0.4.0 acrescentava os componentes das configurações ao layout compartilhado `cm_Page_Dummy`. A correção na `fix-chore` mantém esse recurso original e deriva uma página exclusiva para o seletor. Com os mesmos executáveis e os layouts regenerados, CTGP e o DLC ativos, o botão da chave e F1 abriram o seletor em duas sessões isoladas. Foi possível fechar, salvar e voltar ao menu; ambas terminaram com código 0. Uma tentativa separada parou no início com a notificação SDL `0x406d1388`, antes de carregar o menu; o reteste de F1 completou. Esta investigação cobre a abertura do menu.

Com o jogo base e os layouts corrigidos, F1 também abriu e fechou o menu com saída 0. Passaram 24 testes de configurações e 14 de CTGP, incluindo o teste das texturas dos ícones das copas executado com o arquivo local do MK8 4.0.1.

No instalador original da 0.4.0, **Configurações → desmarcar “Ativar o menu nativo de configurações”** permite manter o CTGP ligado. A correção depende de regenerar os layouts privados com a ferramenta atualizada.

## Importação de DLC em NSP combinado — #36

Quatro testes de navegador do módulo DLC também aprovados com o launcher debug existente e o Python de desenvolvimento (a primeira tentativa no Python empacotado não iniciou por ausência de Playwright). Eles verificam controles/estados/API da interface; não exercem descriptografia de pacote comercial.

O importador recusava um pacote com o par de NCAs verificado sempre que existisse qualquer NCA adicional. Agora exige a presença dos dois nomes fixados e extrai apenas esses arquivos, conferindo os mesmos tamanhos e hashes. Seleciona somente o ticket com prefixo do título DLC; zero ou múltiplos tickets correspondentes continuam sendo erro. Outros conteúdos/tickets ficam no pacote, sem extração. A validação LibHac e os hashes do RomFS permanecem obrigatórios. Não habilita versões desconhecidas, DLC comprimido ou bypass de integridade.

Regressão com NSP autoral combinado falhou no código anterior e passou após a mudança. Onze testes de DLC aprovados, incluindo pacote independente, conteúdo ausente/adulterado, ticket ausente/ambíguo, preservação de configuração/backup e seis casos do serviço nativo. Teste de importação usa extrator simulado e arquivos autorais; não valida pacote comercial combinado. Nenhum arquivo privado foi importado/reinstalado. #36 continua sem metadados do pacote do relator, portanto esta correção de compatibilidade não encerra o relato. Sem mudança de runtime/Rust ou novo setup.

## Desempenho CTGP e seleção local — 16/11/2026

Investigação do relatório privado `mk8-report-1794858711.zip`, com reprodução em Luigi Circuit (CTGP29) e Koopa Troopa Beach (CTGP21). O relatório registra 18–40 FPS e pausas de até 1,76 s na entrada de CTGP29, junto de centenas de compilações de shaders, seguidas de trechos a 60 FPS. Não demonstra sozinho vazamento de VRAM nem identifica um crash novo. Testes usam cópias de saves/configurações/cache, uma única instância e o guest já compilado; nenhum dado comercial é publicado.

**Espera de shaders:** `toolchain/shader_budget.py` substitui o timed wait de Winpthreads pelo temporizador monotônico de alta resolução já disponível na plataforma. No controle nativo, o código anterior leva cerca de 16 ms para uma espera limitada a 4 ms e falha no novo teste de latência. A correção fica perto de 4 ms. O orçamento continua compartilhado por quadro; compute, probes, iluminação e alvos desconhecidos preservam a espera completa. Não há remoção indiscriminada de draws nem mudança de ABI/guest.

**Sessão manual e tela dividida:** sessão privada de aproximadamente 13 minutos em GPDX Kingdom Way (CTGP53), Alpine Skyway (CTGP54) e Dry Dry Falls (CTGP45, dois jogadores), com RX 580 2048SP, tela cheia 1080p, SMAA e frame generation desligada. O usuário confirmou engasgos na primeira passagem, ausência deles ao revisitar os mesmos trechos e nenhum crash; o processo terminou com código 0. O log mostra pausas de até 2,43 s associadas a compilações completas. A reserva VMA no heap principal chegou a 1648 MiB e depois caiu para 1136 MiB; a alocação atingiu 852 MiB e terminou perto de 438 MiB. Isso não demonstra vazamento contínuo nem mede toda a VRAM do driver; o HUD em MB mostra memória do processo.

O diagnóstico optativo `MK8_TRACE_SHADER_TARGETS=1` identificou esperas de 60–311 ms na cena dividida: alvos de cor/profundidade 8:9, de 720×810 a 960×1080, não recebem a marca `scalable` reservada a 16:9. O orçamento agora reconhece esse par específico do MK8 4.0.1, com validação de dimensões, proporção arredondada, formatos, flags, amostras e backends. Alvos auxiliares, cubos, probes, combinações desconhecidas e compute continuam síncronos; não há mudança da resolução. Teste nativo executa a fila real e verifica tanto o retorno limitado quanto a espera completa para alvos excluídos. Três/quatro jogadores e outras proporções não foram qualificados por essa extensão.

Comparação automatizada em Dry Dry Falls, dois jogadores, 1080p/SMAA, 9000 quadros por execução, cache da aplicação inicialmente vazio nas duas: nos quadros 3004–8990, a média passou de **54,96 para 59,21 FPS**, o pior quadro de **518,81 para 60,54 ms** e os quadros acima de 50 ms de **35 para 2**. No trecho 6100–8990, com aceleração contínua, foram 54,29 → 59,93 FPS e 19 → 2 quadros acima de 50 ms. Capturas e seus três quadros vizinhos excluídos. As posições variam com a simulação e o cache do driver não foi isolado: não é garantia de 60 FPS em todas as pistas. Não houve falha de processo; capturas 5000/7000 confirmam pista, karts e iluminação visíveis. Pausas obrigatórias de preparação antes desse trecho persistem. Evidência privada: `race-grand-prix-vuxzye_7` (anterior) e `race-grand-prix-chrwe8_a` (corrigido). Uma tentativa independente de normalizar chaves de pipelines foi descartada por não mostrar ganho (59,74 contra 59,73 FPS em solo); não integra o runtime.

**Seleção local:** havia sucesso com apenas um jogador quando o jogo solicitava dois. O guest reabria ShowControllerSupport a cada poucos quadros, embora o renderer continuasse a 60 FPS. Agora retorna `ControllerSupportCanceled` e preserva a notificação de foco. Na reprodução real, uma solicitação insuficiente voltou à classe de motor, com uma única chamada ao applet. Dois jogadores com entradas automatizadas escolheram Luigi/Yoshi e avançaram para os karts, sem o ciclo. Para teclado P1 e um controle P2, usar a opção existente **Controles começam no jogador 2**; não foi alterada a preferência pessoal do usuário.

**Medições e limites:** Luigi Circuit manteve 60,00 FPS médios nos quadros 3600–5990 em ambos os runtimes. Esse trecho inclui permanência fora da pista, portanto não representa uma volta completa. Em Koopa Troopa Beach, quadros 2400–4490, incluindo carregamento: original 58,99 FPS/pico 361,73 ms; corrigido com cache de aplicação vazio 53,76 FPS/pico 2358,66 ms; repetição corrigida reaproveitando seu cache 59,40 FPS/pico 262,12 ms. O cache do driver não foi isolado e o cache de tradução é invalidado ao trocar o runtime: esses números não demonstram ganho geral de FPS. A repetição confirma o peso da primeira compilação, mas pausas obrigatórias persistem. Capturas próximas são excluídas pelo `scripts/frame-stats.py`; imagens das duas pistas e da seleção/karts foram inspecionadas. Evidências locais nos diretórios `race-time-trial-*`, receitas em `local/ctgp-perf-check`.

A sessão manual acima acrescentou várias pistas consecutivas, memória e encerramento normal. Ainda falta qualificar voltas completas com imagem, áudio, controle, velocidade do relógio e reinício/save, além dos demais crashes relatados. Sem crash reproduzido nesses ensaios, sem qualificação de multiplayer entre PCs. `gameplay_verified=false`. A geração de quadros 120 já existe, mantendo simulação a 60; FPS nativo acima de 60 exige investigação separada dos relógios e da lógica guest, não apenas retirar o limitador.

**Regressões:** nova rodada com 25 testes da área aprovados (applet, orçamento de shaders incluindo cena dividida e alvos excluídos, stalls/cache/lifetime, filas/tabelas de shaders, prefetch e pré-compilados). O controle negativo com a implementação anterior falhou como esperado: mediana 15,94 ms/p95 16,05 ms para 4 ms solicitados; corrigido mediana 4,23 ms/p95 4,68 ms. Na nova rodada, mediana 4,11 ms/p95 4,43 ms. O teste usa as funções reais de temporização Windows. Os harnesses de filas precisaram declarar a nova dependência da plataforma; seus dublês abortam se a temporização, fora do escopo desses casos, for chamada. A dependência Zstandard ausente no Python global foi resolvida usando o ambiente privado já instalado. Runtime de host compilado e ligado com 220 unidades; guest preservado.

**Setup para teste:** `MK8-online-0.2.0-setup.exe`, 59.518.464 bytes, SHA-256 `d67bae4173969a711f458f3d35835d8483411b2666df971012102cbfbbc47a77`. Launcher release recompilado e host embarcado com chave `b70c0d85c11163cde636056ae750c10d2e612d00c3d36bae7189c216a36d27c7`. Auditoria dos 966 arquivos aprovada: fontes atuais, manifestos/hashes conferidos, sem dados de jogo/chaves/saves/caminhos pessoais. Instalação, abertura e desinstalação isoladas aprovadas, mantendo os dados de teste e restaurando registro/atalhos da instalação pessoal. Sem mudança visual nesta rodada; os testes de UI completos do redesign permanecem os registrados abaixo. Reinstalar manualmente e atualizar a build do jogo para aplicar na instalação existente; versão continua `0.2.0`. Sem commit, tag ou publicação.

## CTGP Deluxe como pacote de pistas opcional (Fase A) — 15/11/2026

**O que é:** as 64 pistas do CTGP Deluxe 1.1.1 nas vagas de pista existentes do 4.0.1, sem código do mod (o plugin é ARM de 32 bits, para o 3.0.3). Opcional e reversível: o launcher importa, liga e desliga. Jogo base, DLC e saves não são alterados, e as quatro combinações funcionam (base; base + DLC; base + CTGP; base + DLC + CTGP).

**Partes:**
- `toolchain/ctgp_pack.py` / `import_ctgp.py` montam `<dados>/ctgp/pack`:
  - a partir da pasta extraída do ZIP oficial, com hardlinks no mesmo disco (4,6 s, sem cópia);
  - CTGP fica só na pasta de dados do jogador; o repositório guarda apenas números (CT → rótulo de nome, lidos estaticamente do plugin 1.1.1, `PLUGIN_111`) e nomes de arquivo.
- **Vagas:**
  - copas 1–12 do CTGP nas 12 copas do jogo base (48 pistas);
  - copas 13–16 nas 4 primeiras copas do Booster Course Pass (`Cnsw_11`–`28`), visíveis só com o DLC ativo, pois as pistas do BCP estão no RomFS base e o DLC só as libera.
- **O que vai em cada vaga:**
  - a pasta da pista (sem o banco de sons próprio do CTGP; fica o da vaga);
  - as músicas N/F e variantes (LAP2, REPLAY);
  - a imagem de seleção, os ícones de copa;
  - os nomes de copa/pista no `message.sarc` 4.0.1 de 15 idiomas, com tags MSBT preservadas e os prefixos de plataforma (Wii, GBA…) vazios;
  - as músicas de título/menu, os sons de menu e o Lakitu.
  - Os `common.sarc`/`menu.sarc` do CTGP derrubam o menu do 4.0.1 e nunca são usados.
- **Runtime:** `runtime/mk8/support/romfs_overlay.c` implementa o gancho fraco `game_romfs_overlay()` com `MK8_ROMFS_OVERLAY`. É uma segunda sobreposição depois de `RECOMP_MOD_DIR`, que segue para a cabeça Mii. Uma pasta inválida encerra o jogo em vez de abrir o jogo base calado.
- **Launcher:**
  - cartão **Configurações → CTGP Deluxe** com importar, liga/desliga, estado e log, em pt/en/es;
  - `crates/installer/src/ctgp.rs` verifica o pacote antes de cada abertura (tamanho de cada arquivo, nada a mais);
  - com o CTGP ligado o jogo usa `save-ctgp`, criado uma vez como cópia de `save`;
  - build sem `pack_supported` recusa o pacote ligado.
- **Steam:** a sala publica `mk8_ctgp` (identidade do pacote ou `0`) e quem entra precisa do mesmo estado (`check_pack`).

**Problema encontrado e corrigido no runtime:**
- **Sintoma:** VRAM a 7,3 GB e RAM do jogo a 7,8 GB no sobrevoo inicial do GP. Uma corrida terminou em `VK_ERROR_OUT_OF_DEVICE_MEMORY` (`race-grand-prix-oz51459d`).
- **Bisseção** (GP, primeira corrida, até o sobrevoo):
  - não é o Lakitu, a música nem um arquivo de modelo;
  - é a lista de objetos `course_muunt.byaml` de `CT_00`, e nela os 7 `FireworksFc` (o Stadium original tem 2);
  - sem os fogos o pico cai a 1.648 MB, igual ao jogo base.
- **Mecanismo:** os fogos pesam a GPU, a resolução dinâmica do jogo refaz os alvos a cada poucos quadros em tamanhos novos (1499x843, 1546x870, 720x405…), e o runtime criava uma imagem por tamanho. `texture_retire` só liberava alvos substituídos depois de 60 s sem uso: ~700 imagens/s acumulavam.
- **Correção** (`toolchain/texture_retire.py`): alvo substituído sai após `MK8_TARGET_GRACE_FRAMES` (30, meio segundo), com a varredura na mesma cadência. O conteúdo dele já não existe nem no console. Texturas com conteúdo continuam com os 60 s.

| GP, 1ª corrida (quadros 3600–14000) | FPS médio | p99 | pior quadro | VRAM reservada / usada (pico) |
|---|---|---|---|---|
| jogo base (`race-grand-prix-x2kyzd8r`) | 59,89 | 17,7 ms | 72 ms | 1.648 / 1.459 MB |
| CTGP antes (`-70aahhh4`) | 56,22 | 35,1 ms | 138 ms | 7.373 / 7.314 MB |
| CTGP, carência 2 s (`-7dpwm7t1`) | 58,24 | 21,1 ms | 64 ms | 5.616 / 2.107 MB |
| CTGP, carência 0,5 s (`-cgxrwi9u`) | 57,84 | 22,6 ms | 80 ms | 2.928 / 1.732 MB |

Nessa corrida 6.043 imagens (17 GB) foram criadas e descartadas: a rotatividade da resolução dinâmica continua, só não acumula. Reaproveitar imagens entre tamanhos seria a próxima melhoria.

**Grande Prêmio completo com o CTGP** (`race-grand-prix-tovnrm2s`, 64.000 quadros, 18 min, cache frio):
- as quatro pistas do CTGP no lugar da copa Cogumelo:
  - Omatsuri Circuit;
  - Vanilla Lake 1;
  - Broken Pier;
  - Mystic Tangle.
- 12 pilotos, CPUs no traçado, itens e minimapa próprios; saída 0, sem `FATAL`;
- FPS médio por corrida: 58,2 / 59,2 / 59,8 / 59,8; p99 20,7 / 19,8 / 17,6 / 17,6 ms;
- picos isolados de 230–600 ms nas trocas de pista: compilação de primeiro uso com cache frio, como no jogo base;
- VRAM no pico: 3.696 MB reservados, 2.361 MB usados.

**Jogo base com a mesma correção** (`race-grand-prix-bm9gt9k4`, 1ª corrida): 59,91 FPS e p99 17,6 ms, como antes. A VRAM reservada no pico caiu de 1.648 para 1.136 MB.

**Validação:**
- Rust: fmt, Clippy e testes, incluindo 3 do `ctgp.rs` e a regra Steam.
- Python: 6 em `test_ctgp_pack.py`, só com dados sintéticos — SARC/MSBT ida e volta, mapeamento das vagas, nomes, montagem/verificação/importação e teste nativo de `romfs_overlay.c`.
- `test_texture_retire` com os casos novos.
- Navegador: `test_launcher_ctgp` (estados, importar, liga/desliga, bloqueio com o jogo aberto, en/es, 390 px), DLC e idiomas: 10 testes.

**Ainda não testado:**
- as copas do CTGP nas vagas do BCP (falta DLC importado aqui);
- outras pistas além das 4 da primeira copa;
- multijogador com o pacote em duas máquinas;
- os fantasmas de staff do contra-relógio e o fundo da tela de prêmio, que continuam os da pista original;
- os 5 nomes que a leitura do plugin não resolveu (`CT_08`, `0A`, `0D`, `37`, `3B`), que aparecem como "CTGP NN".

`gameplay_verified=false`.

### DLC importado e Fase B — 15/11/2026

**Booster Course Pass importado:**
- origem: `Downloads/…[0100152000023001][v65536].nsp`, 121 KB, com o `prod.keys` do `config.json`, por `toolchain/import_dlc.py`;
- verificado e anexado ao candidato pela build `--runtime-only`.
- **Pacote na disposição base:** a primeira copa do DLC mostrou "Moo Moo Mountain" (`CT_30`), correu a 60 FPS e trouxe o Lakitu do CTGP (`race-time-trial-ujfapcmy`).
- **Tela de copas com DLC + CTGP:** "Boo Cup" no lugar da copa Cogumelo, com nomes e imagens das 4 pistas CTGP (`race-time-trial-f6_8i48_`).
- **Os ícones das copas nessa tela continuam os originais.** Na tela de seleção eles vêm de dentro do `menu.sarc` do 4.0.1; os arquivos soltos `UI/cmn/race/ym_CupIcon*` que o pacote troca são os da corrida.

**Segunda disposição: no lugar das copas do DLC** (`--layout dlc`, seletor no cartão do launcher):
- as 12 copas do Booster Course Pass recebem as copas 1–12 do CTGP;
- o jogo base fica 100% original, e com o DLC ativo há as 48 pistas originais e 48 do CTGP ao mesmo tempo;
- as copas 13–16 do CTGP ficam de fora nessa disposição; trocar a disposição reimporta em segundos (hardlinks).
- **Testado:**
  - página do jogo base: copa Cogumelo e pistas originais, e a corrida carregou `Gu_FirstCircuit` original (`race-time-trial-m8_mnqio`);
  - página do DLC (R): "Boo Cup" com as 4 pistas CTGP, e a corrida carregou `Cnsw_11` com os arquivos do CTGP (`race-time-trial-7b9v6o3a`).
- **Limite visual dessa disposição:** os nomes do CTGP herdam o rótulo de plataforma da vaga ("Tour", "3DS", "N64", "Wii"). Esses rótulos (1461–1468) são os mesmos das pistas originais do jogo base, que nessa disposição continuam valendo.

A ordem real das copas do DLC (imagem `DLCnn` ↔ pasta `Cnsw_xy`) não segue o número da pasta (Rock Cup = `Cnsw_31`, `33`, `34`, `62`). Ela vem da tabela do próprio jogo, repetida pelo plugin, e foi conferida com os nomes `117xx` do 4.0.1 (11717 London Loop … 11720 Maple Treeway). A disposição base, que só usa as 4 primeiras copas do DLC, não muda: lá a ordem coincide.

**Fase B, parte sem código — concluída:**
- **Ícones das copas na tela de seleção:**
  - **Onde estão:** no 4.0.1 são texturas `ym_CupIcon<Nome>^h` de 200×200 em RGB565 tiled. Ficam num BNTX dentro de `menu.sarc` → `mn_L_CupSlctBtn_00.szs` (Yaz0, SARC de arquivos só com hash).
  - **Conversão:** o pacote decodifica os ícones coloridos do CTGP (`ym_CupIcon*_130x130^h`, 210×210; 15 em BC3 e o Blooper em BC7), reduz por média de área e grava sobre os ícones das vagas numa cópia do `menu.sarc` 4.0.1. O resto do arquivo não muda e o Yaz0 é gravado sem compressão (válido).
  - **Decodificador BC7:** modos 1 e 3–7; os modos de 3 subconjuntos (0 e 2) não são usados pelos ícones e são recusados. Um ícone ilegível mantém o original e a importação avisa.
  - `*_Nml^l` são normal maps, não os ícones.
  - **Na tela:** copas do jogo base com os 12 ícones do CTGP e as 4 primeiras do DLC com Life Mushroom, Ice Flower, Prankster Comet e Coin (`race-time-trial-x0ys7od8`).
- **Terceira disposição `dlc-late`:** copas 5–16 do CTGP nas 12 copas do DLC. Com `dlc` e `dlc-late`, as 64 pistas do CTGP ficam acessíveis ao lado das originais.
- **Testes:** 10 em `test_ctgp_pack`:
  - ícone convertido conferido texel a texel;
  - blocos BC3 e BC7 (modos 6 e 1, recusa do modo 0);
  - disposição `dlc-late`;
  - SARC de arquivos sem nome e Yaz0 de ida e volta.

**Fase B, parte com código — não iniciada, por decisão de escopo:** copas além de 24, botão aleatório e menus próprios do CTGP. A estrutura de gancho entra junto com o primeiro limite real alterado, no padrão já usado (`yaz0_native.py`: patch da unidade gerada validado por hash), e não como infraestrutura sem uso.

**Fase B, passo 1 — rastreamento com o jogo rodando (branch `ctgp-phase-b`, 16/11/2026, somente leitura):**
- **Ferramentas:**
  - `scripts/fnprof-diff.py` compara dois perfis `LARECOMP_FNPROF` e aponta a unidade gerada de cada função;
  - `MK8_TRACE_ENTRY=<endereço>[:x<reg>:<bytes>]` (`runtime/mk8/support/entry_trace.c`, ligado em `fnprof_hit` por `toolchain/entry_trace.py`) registra x0–x3, o chamador e bytes apontados por um registrador na entrada de funções escolhidas. Não muda registradores, memória nem fluxo; sem a variável o gancho fica desligado. Teste nativo: `tests/test_entry_trace.py`.
- **Perfis da tela de copas** (jogo base + DLC, sem CTGP, quadros 1760–1880): parado 2.805 funções; movendo o cursor por 6 copas 3.233 (428 novas); trocando de página (R).
  - 15 funções rodam exatamente 24 vezes (4 pistas × 6 movimentos). Entre elas `f_700043c2e8` (`main_089.c`), que recebe em x1 o registro da pista e usa o campo `+8` mais 100 como mensagem (rótulos 14xx → 15xx).
- **Registro de copa observado** (`MK8_TRACE_ENTRY=700043c2e8:x1:64`): por copa, 4 registros de 16 bytes `{ID da pista u32, -1, rótulo do nome u32, 0}`, com as copas a 0x80 bytes umas das outras num objeto da heap.
  - Cogumelo = IDs 27, 28, 19, 17 (rótulos 1401–1404); Flor = 16, 18, 20, 21; Estrela = 26, 29, 25, 24.
  - Os IDs internos das pistas não seguem a ordem das copas.
- **Construtor:** `f_70003f96b0` (`main_081.c`) é a única função com os 96 rótulos de pista.
  - Grava as copas como `objeto + copa × 0x80`, com índice limitado a 25 (`< 0x19`) e contagem em `+0x1430`.
  - Grava também um mapa pista → (copa, posição) de 8 bytes por ID, limitado a 123 (`< 0x7b`).
- **Tamanho do trabalho para copas extras (contagem no código gerado):**
  - o acesso ao vetor de copas com o limite de 25 aparece em 131 pontos de 10 funções;
  - o limite de 123 IDs de pista aparece em 129 pontos de 17 funções (entre elas `f_700050ccc0`, a dos ícones);
  - ainda faltam o caminho ID → pasta (`f_700089ed04`, por objetos virtuais) e os saves.
  - 16 copas do CTGP exigiriam 40 copas (o objeto tem lugar para 25) e IDs de pista ≥ 123: o objeto teria de ser realocado ou ampliado e todos esses pontos corrigidos.
- **Próximo passo (2):** localizar a alocação do objeto de copas (tamanho fixo), o mapa ID → pasta e os vetores do save indexados por ID de pista ou copa. Só depois decidir entre ampliar o objeto existente ou criar um segundo conjunto de copas só para o CTGP, com recordes num arquivo próprio.

**Fase B, passo 2 — IDs de pista (16/11/2026, somente leitura):**
- **A lista de pistas é um enum do sead:** uma string de 124 nomes separados por vírgula em `main+0xed765d`: `Invalid, Gu_Menu, Test, Reserved01 … Reserved14, Gu_MarioCircuit … Cnsw_68`.
  - O ID da pista é a posição menos 1. Confere com o rastreamento: Gu_MarioCircuit = 16, Gu_FirstCircuit = 27, Gu_WaterPark = 28.
  - O último ID é Cnsw_68 = 122, daí o limite de 123 (`0x7b`).
  - O nome da pasta é o próprio nome no enum (`Course/%s/%s`).
  - `f_700085bcd8` (`main_181.c`) e `f_7000a5f6d4` (`main_228.c`) usam essa string.
- **IDs livres:** a Nintendo deixou 14 reservados (`Reserved01`–`Reserved14`, IDs 2–15) e `Test` (ID 1). Uma pista com esses IDs seria lida de `Course/Reserved01/…`, pasta que a sobreposição pode criar.
- **Formato exato do objeto de copas** (construtor `f_70003f96b0`, 1.700 linhas, um único `return`):
  - copa = 0x80 bytes em `objeto + índice × 0x80`, com até 8 registros de 16 bytes `{ID u32, -1, rótulo u32, flag u8}` (vagas sem uso valem -1);
  - mapa reverso em `objeto + 0xC80`: 123 × `{copa u32, posição u32}`;
  - contagem em `+0x1430`: zerada e incrementada 24 vezes (12 copas base + 12 do DLC), e fica em 24. A 25ª entrada (índice 24 = a contagem) é a copa de batalha, com as 8 arenas até `Bu_DekaLine` (ID 70), escrita sem incrementar;
  - segundo mapa em `objeto + 0x1058` (123 × 8 bytes): o mapa reverso das arenas. O construtor do objeto (`f_70003f952c`) inicia os dois mapas com -1;
  - nenhuma das funções que leem copas lê a contagem: elas usam posições fixas.
- **Não há vaga livre:** as 25 entradas estão ocupadas e terminam exatamente em `+0xC80`, onde começa o mapa reverso. Uma 25ª copa de corrida, mesmo como prova de conceito, exige realocar o vetor de copas para uma área maior e corrigir todos os acessos:
  - o limite de 25 aparece em 163 pontos de 20 funções: 107 escritas do próprio construtor e 56 leituras em 19 funções (contagem completa da etapa 1 abaixo);
  - a base do mapa reverso (`+0xC80`) e a contagem (`+0x1430`);
  - a tela de copas (terceira página);
  - os IDs reservados 2–15 **não** servem para pistas novas (ver "IDs reservados" abaixo).
- **Etapa 1 feita (branch `ctgp-recomp`): copas numa área com lugar para 41.**
  - `toolchain/cup_relocation.py` altera a cópia de build de 11 unidades geradas, cada uma conferida por SHA-256. `main_082.c` e `main_115.c` são aceitas como ficam depois dos ganchos do diálogo Mii e da votação LAN (`toolchain/experimental.py`), que continuam aplicados.
  - O construtor fica igual e, no seu único `return`, entrega o objeto a `mk8_cups_built` (`runtime/mk8/support/cups.c`), que copia as 25 entradas para a área nova; as 16 entradas a mais começam vazias (registros -1).
  - As leituras passam a aceitar até 41 (`0x29`) e calculam a entrada por `mk8_cup_entry`: a área nova para o objeto do construtor, a conta original (com limite 25) para qualquer outro.
  - Cada limite é seguido pelo registrador do índice até a conta `base + (índice << 7)`: rótulos são ignorados, `goto` dentro da função é seguido e chamadas só são aceitas com o índice em registrador preservado (`x19`–`x28`). São 58 limites e 56 contas em 19 funções (dois limites de `f_700048e190` chegam à mesma conta); cada função é conferida por esses dois números e um limite que não chega à conta recusa a build.
  - A primeira versão (commit `65d0bc4`) só pegava limites com a conta logo em seguida: 42 contas em 9 funções. As outras 14 (por exemplo `f_70004a3940`, que copia o registro escolhido para a seleção) continuavam lendo o objeto original; uma copa acrescentada seria mostrada na tela e ignorada ao escolher a pista.
  - O mapa reverso e a contagem continuam no objeto original. `MK8_CUPS_RELOCATE=0` desliga tudo.
  - Testes: `tests/test_cup_relocation.py` (5: alteração sintética, incluindo rótulos, `goto` e chamadas; as 11 unidades verificadas como a build as recebe; recusa de contagem diferente; harness nativo da cópia/entradas vazias/desligamento).
  - No jogo (15/11/2026, com DLC): contra o relógio com a mudança ligada e desligada mostra a mesma tela de copas; o rastreador do painel de pistas (`MK8_TRACE_ENTRY=700043c2e8:x1:64`) leu os 24 registros da área nova com a mudança ligada e os 24 do objeto original com ela desligada. Grand Prix completo a 60 FPS, sem erros no log. Repetido com as 56 contas: mesma tela, contra o relógio a 60 FPS, Grand Prix até a volta 3/3 a 59–60 FPS, sem exceções.
  - Ainda não mostra copa nova: é só a base.
- **IDs reservados não servem para pistas novas** (experimento de 15/11/2026, só local):
  - O jogo converte o ID numa posição das tabelas por pista (imagem, música, recordes, fantasmas…): `ID − 16` para `ID < 75` e `ID − 27` acima. Para `Reserved01` (ID 2) a posição é -14.
  - Experimento: uma 26ª copa com os IDs 2–5 e pastas `Course/Reserved01–04` (cópias de uma pista do jogo, com `.barslist` reescrito), mostrada no lugar da Mushroom Cup. Com as 42 contas, a tela mostrou os nomes escolhidos e as imagens das pistas DLC01–04, e a corrida carregou a pista original. Com as 56, o jogo parou na tela de copas lendo `0x18` (`f_70004a5c48`, chamada por `f_70004a3940`), com `x9 = -14`.
  - Usá-los exigiria mudar essa conversão nos 129 pontos limitados a 123 e ampliar todas as tabelas por pista.
  - Também ficou registrado: a imagem do menu é `UI/cmn/menu/ym_CoursePict_<sufixo>^u.bntx`, com uma lista fixa de sufixos (48 pistas base, 8 arenas, DLC01–48, Random); o fundo do troféu é `UI/cmn/ending/ym_AwardBG_<nome>_00^o.bntx`; a música vem de nomes `pBGM_…` no binário, escolhidos por ID.
- **Caminho escolhido (15/11/2026): copas CTGP reusam os IDs das pistas originais e desviam os arquivos.**
  - O índice da RomFS é montado uma vez no início (`runtime/services/fs.c`, `upstream`); não há gancho por abertura de arquivo. Então o desvio é feito no jogo, trocando o **nome** da pasta.
  - `f_700089ec04` (`Course/%s/%s`) e `f_700089ed04` (`Course/%s`), em `main_186.c`, recebem o objeto de configuração da corrida: ID da pista em `+0x1a0`, e o nome vem de `f_700001b4ac(ID + 1)` (o enum). Para o ID 1 (`Test`) o jogo usa um nome próprio guardado em `+0x1a4`.
  - Rastreio (15/11/2026, `MK8_TRACE_ENTRY=700089ec04:x2+180:64`): `+0x1a0` = `0x1b` saindo da Mushroom Cup e `0x10` saindo da Flower Cup; nada perto indica a copa. A copa escolhida será anotada pelo runtime nos leitores que copiam o registro escolhido para a seleção (`+0x2084` copa, `+0x2088` pista: `f_70004980a0`, `f_7000498e90`, `f_70004a3940`).
  - Plano: (1) esses leitores avisam a copa escolhida; (2) depois de escolher o nome, os dois montadores de caminho perguntam ao runtime, que devolve `CTGPnn` quando a copa é acrescentada e a pista é dela; (3) a importação monta `Course/CTGPnn` com os arquivos da pista CT (`Course_CTGPnn.bars`/`.barslist` renomeados, formato já conferido byte a byte); (4) depois, nomes (rótulos novos), imagem do painel e música.
  - Limites conhecidos: recordes e fantasmas de uma pista CT ficam junto com os da pista original cujo ID ela usa (no save separado do CTGP); uma corrida aleatória logo depois de uma copa CTGP precisa zerar a anotação.
  - O rastreador aceita deslocamento no registrador (`x2+180:64`), com teste.
- **Copa CTGP acrescentada funcionando (15/11/2026, branch `ctgp-recomp`, teste local):**
  - `MK8_CUPS_EXTRA=<ID>:<rótulo>:<pasta>,…` (4 por copa, `;` entre copas): só aceita IDs que já estão numa copa de corrida (as tabelas do jogo existem para eles); o mapa reverso e a contagem ficam como estão. Inválido: o jogo para com o motivo.
  - Os leitores que copiam a pista escolhida (`f_70004980a0`, `f_7000498e90`, `f_70004a3940`, para a seleção em `+0x2088`) e o do Grand Prix (`f_70004fb290`, que copia as 4 pistas da copa para a lista do GP) chamam `mk8_cup_select`, que anota a copa. Os dois montadores de caminho (`main_186.c`, conferida por SHA-256) pedem o nome a `mk8_course_folder`, que devolve a pasta da pista quando a copa anotada é acrescentada e a tem.
  - `MK8_CUPS_PREVIEW=<copa>` (diagnóstico) mostra a primeira copa acrescentada no lugar dessa.
  - Pastas: `Course/CTGPnn` = arquivos da pista original do ID emprestado com os da pista CT por cima (a pasta nova não tem o recurso de cair na original). O jogo lê o banco de sons com os dois nomes (`Course_CTGPnn.bars`/`.barslist` e `Course_<original>.bars`/`.barslist`), então os dois ficam na pasta.
  - Teste com as 4 primeiras pistas do pacote (IDs 27, 28, 19, 17 → `CTGP01–04`) no lugar da Mushroom Cup: contra o relógio carregou `Course/CTGP01` a 60 FPS; Grand Prix com `Course/CTGP01` e depois `Course/CTGP02` (corrida 2), 8 corredores, 57–60 FPS, sem exceções. As pistas originais continuam nas copas delas.
- **Terceira página na tela de copas (15/11/2026, branch `ctgp-recomp`):**
  - O jogo tem duas páginas fixas: `f_7000494b44` monta os 12 botões com as copas 0–11 e `f_7000495564` com as 12–23 (cada botão em `tela + 0x4c0 + i×8`, a copa dele em `+0x3d0`); a navegação (`f_70004966c8`, máquina de estados) alterna o byte de página `tela + 0x5a3` entre 0 e 1 com L/R (bits 11/12 da entrada) ou passando da borda.
  - Com copas acrescentadas, o runtime guarda a página mostrada (base, DLC e uma por 12 copas acrescentadas): `mk8_cup_page_turn` anota o sentido logo depois de cada troca do byte, e as duas montagens pedem a copa de cada botão a `mk8_cup_page_id`. O byte do jogo continua alternando 0/1; só a lista de copas muda. Ao abrir a tela, a página é a da copa escolhida.
  - A checagem de DLC por copa (`f_7000863428`, `main_182.c`), que a consulta de estado (`f_7000494e44`) faz para as copas a partir da 13ª, responde pelo runtime para as acrescentadas: 0 (sem restrição, como uma copa DLC comprada) com pistas, 1 vazia. Rastreio: com 3 a copa aparecia com `???` e não podia ser escolhida.
  - `f_70004a3940` escolhe a pista pelo ID e acha a copa no mapa reverso (sempre uma copa do jogo): passou a chamar `mk8_cup_by_course`, que mantém a copa acrescentada quando as duas têm uma pista em comum (antes ela trocava a copa para 0 logo depois da escolha e a corrida carregava a pista original).
  - No jogo, sem modo de diagnóstico: R, R vai de base para DLC e para a página CTGP; L a partir da base vai direto para ela; escolher a copa e correr carregou `Course/CTGP01` a 60 FPS. Sem copas acrescentadas a tela fica igual.
  - Cada troca de página tem uma parada de ~380 ms (49 FPS no intervalo de 2 s) também sem copas acrescentadas (mesma entrada, 15/11/2026): já existe no jogo, não vem desta mudança.
  - Falta nesta tela: ícones das copas acrescentadas (a tabela de 24 ícones dá o da última, Spiny, para todas).
- **Importação "em copas novas" (16/11/2026, branch `ctgp-recomp`):**
  - `ctgp_pack` disposição `added` (`import_ctgp.py --layout added`, opção no launcher): cada pista CT `i` ganha `Course/CTGPnn` (arquivos da pista da vaga que a disposição `base` lhe dá, com os da CT por cima; o banco de sons da pista emprestada também como `Course_CTGPnn.bars`, e o `.barslist` dela renomeado — refeito igual ao do jogo nas 108 pistas), `CTGPnn_N/F.bfstm`, `ym_CoursePict_CTGPnn_00^u.bntx`, nomes em rótulos novos (`30000 + copa`, `31000 + i`, `31100 + i` na abertura) em todos os idiomas, ícones (`cup_select_add_icons`) e o `MK8_CUPS_EXTRA` no manifesto (`cups_extra`). Com hardlinks: 1.309 arquivos em segundos. Os IDs vêm do enum de pistas (`COURSE_IDS`); o runtime só aceita IDs de copas de corrida (a vaga de Animal Crossing é `Du_Animal_Spring`, ID 64, aceito).
  - Launcher: `cups_supported` no manifesto da build (só a build completa o marca; `--runtime-only` mantém o da biblioteca do jogo); `ctgp::launch_cups` dá ao jogo o `MK8_CUPS_EXTRA` do pacote `added` e recusa uma biblioteca sem suporte; `/api/ctgp/import` aceita `added`; textos em pt/en/es.
  - Área de copas: 49 lugares (25 do jogo e duas páginas de 12). Com 41, a quarta página apontava as 8 vagas vazias para copas inexistentes (o jogo cairia na Mushroom Cup); agora são copas vazias, travadas, com o ícone vazio.
  - Imagens: o gerenciador de texturas do menu (`f_7000405f74`) é criado com lugar para 150 (rastreio: contagem 150 de 150); as 105 imagens do jogo mais 64 não cabiam e as 16 últimas não entravam (o painel mantinha a anterior). Ganhou um lugar por pista acrescentada (`mk8_course_picture_places`, contado do `MK8_CUPS_EXTRA`, porque é criado antes das copas). Tentativas descartadas: o pool `UIHeap_CourseTex` (124) e o tamanho da sua heap não eram o limite; imagens reduzidas a RGB565 não resolviam e desbotavam (as originais são sRGB).
  - No jogo (pacote real, 16 copas): páginas 3 e 4 com ícones, nomes e imagens próprios; vaga vazia da página 4 não abre corrida; contra o relógio da copa 13 (`CTGP49`, ID 75 de DLC) com música própria a 60 FPS; Grand Prix da copa 1 (`CTGP01` → `CTGP02`) a 57–60 FPS; sem copas acrescentadas, contra o relógio e Grand Prix iguais ao jogo a 60 FPS.
- **Ícones das copas acrescentadas (16/11/2026, branch `ctgp-recomp`):**
  - O ícone de cada botão é um quadro da animação `P_CupIcon_00` (padrão de textura `FLTP`, curva em degraus) de `mn_L_CupSlctBtn_00.szs`: o quadro = o número da copa; com 24 quadros, as copas a partir da 25ª ficavam no último (Spiny).
  - `ctgp_pack.bntx_add_textures` acrescenta texturas ao BNTX (cópias do `BRTI` de uma textura-modelo com nome e imagem novos) e refaz toda a disposição pelo `_RLT`: vetor de ponteiros, `_STR` com o `_DIC` (árvore Patricia, refeita igual à do jogo), blocos `BRTI`, dados `BRTD` alinhados e o `_RLT` novo. Sem texturas a acrescentar, o arquivo sai igual até o `_RLT` (que só muda a forma de listar os mesmos ponteiros).
  - `flan_add_pattern` acrescenta chaves ao `FLTP` (quadro 25 + k = ícone k) e os nomes que faltam à lista de texturas; sem chaves novas, o arquivo sai igual. `cup_select_add_icons` junta os dois: ícones CT (130×130, BC3/BC7 decodificados e reduzidos a RGB565 como na fase A) nas copas acrescentadas e o ícone vazio do jogo (`ym_CupIconBack^h`) nas vagas sem copa até a 16ª.
  - No jogo (teste local, ícone Boo do CTGP): a terceira página mostra o Boo na copa acrescentada e o ícone vazio nas outras 11; as páginas base e DLC continuam com os ícones do jogo (o Spiny na última copa DLC).
- **Imagens das pistas CT no painel (16/11/2026, branch `ctgp-recomp`):**
  - `f_7000418ed0(nomes, &id, carregar)` monta `UI/cmn/menu/ym_CoursePict_<sufixo>^u.bntx` (sufixo = `nomes + 0x80`, índice ID + 1): com 1 carrega a imagem, com 0 acha a já carregada. O menu carrega todas de uma vez (`f_7000418e3c`: IDs 16–63, 67–122 e -1). As telas passam uma cópia do ID na pilha.
  - O runtime acrescenta as imagens `<pasta>_00` que a sobreposição tem: o menu as carrega depois das dele, com IDs a partir de 123, e o nome sai de `mk8_course_picture`. O painel de copa (`f_700043c2e8`) anota na entrada o registro que mostra e, onde lê o ID (`+0x3cc`) para a imagem, recebe o da imagem acrescentada; a tela do contra o relógio (`f_7000446068`, `+0xb4`) recebe a da pista da copa escolhida. O nome interno da textura no BNTX não importa (o CTGP traz `ym_CoursePict_Gu_City_00^u` dentro dos seus).
  - No jogo (imagens `CT_00`–`CT_03` do CTGP como `CTGP01–04_00`): o menu carregou as 4; a terceira página e a tela do contra o relógio mostram as imagens CT; a página da Mushroom Cup continua com as do jogo.
- **Música das pistas CT (16/11/2026, branch `ctgp-recomp`):**
  - O jogo lê a RomFS como imagem (`IStorage`) e o driver de RomFS do próprio SDK resolve os caminhos; o sistema de som abre a música por `nn::fs::OpenFile` (`sdk+0x50080`, achado pelo nome exportado no NSO do SDK) com `content://Audio/Resource/Stream/pBGM_<pista>_N.bfstm` (e `_F` na última volta), conferido com o rastreador.
  - Um gancho na entrada de `OpenFile` (`sdk_008.c`, SHA-256) pede o caminho a `mk8_stream_path`: numa corrida de pista de copa acrescentada (a troca de pasta aconteceu), `pBGM_*_N|F.bfstm` vira `<pasta>_N|F.bfstm` quando esse arquivo existe em `Audio/Resource/Stream` da sobreposição; senão, a música da pista emprestada. O arquivo novo está no índice da RomFS desde o início e é lido com o tamanho dele.
  - No jogo (teste local, músicas CT do pacote da Fase A renomeadas): Grand Prix da terceira página leu `CTGP01_N.bfstm`, `CTGP01_F.bfstm` na última volta e `CTGP02_N.bfstm` na corrida 2, a 59–60 FPS; menu, abertura e chegada com as músicas do jogo. O som em si não foi ouvido (teste sem áudio).
  - Ícones: o ícone do botão é um quadro de uma animação de troca de textura (`f_700043bb68` chama `f_70008ad3c4` com o quadro = copa); com 24 quadros, as copas acrescentadas mostram o último (Spiny). Acrescentar ícones exige texturas novas no BNTX de `mn_L_CupSlctBtn_00.szs` e quadros novos na animação.
- **Nomes próprios (15/11/2026, branch `ctgp-recomp`):**
  - Rótulos novos, sem tocar nos do jogo: `ctgp_pack.msbt_add` acrescenta rótulos a um MSBT só com `LBL1`/`TXT2` (o `Common.msbt` de cada idioma), refazendo a tabela de 101 baldes pelo hash do MSBT (`h = h × 0x492 + caractere`); refeito sem acréscimos, o `Common.msbt` de USen, EUpt e JPja sai byte a byte igual. O jogo lê só `UI/USen/message.sarc` nesta configuração.
  - Nome da pista: o rótulo do registro (`n`) e, na abertura da corrida, `n + 100` com as marcas de controle do jogo. Nome da copa: `f_700041fd6c` lê uma tabela de 24 rótulos (`main+0xf26c44`) sem limite; o runtime responde pelas copas acrescentadas (`MK8_CUPS_EXTRA` aceita `<rótulo da copa>=` no começo de cada copa). `main_084.c` é aceita como fica depois do gancho Mii.
  - Telas que acham a pista pelo ID: 8 leitores usam o mapa reverso (`+0xC80/+0xC84`) para achar a copa e a posição de uma pista e mostrá-la (o nome na tela do contra o relógio, por exemplo). Com uma copa acrescentada escolhida, `mk8_cup_by_slot` lhes dá a copa do jogo com os registros da copa acrescentada no lugar das pistas emprestadas; a tela de copas continua lendo a copa do jogo como é. `f_70004a3940` recebe também o ID da pista (`x25`) e devolve o registro da copa acrescentada; com outra pista, a corrida volta a ser de copa do jogo.
  - No jogo (teste local com as 4 primeiras pistas do pacote): a terceira página mostra "Boo Cup" e "GPDX Omatsuri Circuit", "Vanilla Lake 1", "Broken Pier", "Mystic Tangle"; a tela do contra o relógio e a confirmação mostram "GPDX Omatsuri Circuit"; Grand Prix a partir da terceira página com `CTGP01` e `CTGP02` a 60 FPS. O texto da abertura da corrida (`n + 100`) não foi capturado nas imagens.
  - Corridas que não saem de copa (16/11/2026): no VS com pistas aleatórias o jogo não chama nenhum dos leitores de copa (rastreio) e a configuração da corrida não guarda o modo (só o ID em `+0x1a0`), então uma copa acrescentada escolhida antes continuava valendo. Regra nova: só corridas "armadas" usam as pastas — a escolha de uma copa acrescentada (`mk8_cup_select`) arma a corrida da pista escolhida e as repetições dela; o Grand Prix (`mk8_cup_select_gp`, `f_70004fb290`) arma as 4 em ordem; qualquer corrida de outra pista desarma (IDs abaixo de 16, o curso do menu, não contam). Limite: um VS aleatório logo depois, cuja primeira pista sorteada seja a mesma ID da última pista CTGP corrida, ainda usaria a pasta CTGP nessa corrida.
  - Online: o launcher não passa `MK8_CUPS_EXTRA` em sessões de rede (cada máquina decidiria sozinha; o jogo troca IDs de pista, que as copas acrescentadas emprestam). Online valem as copas e pistas do jogo, com os demais arquivos do pacote.
  - Teste nativo da regra (`armed`) e no jogo: contra o relógio da copa 13 (`CTGP49`) e Grand Prix da copa 1 (`CTGP01` → `CTGP02`) a 60 FPS; contra o relógio sem copas acrescentadas igual ao jogo. O Grand Prix sem copas acrescentadas não foi repetido nesta versão (parado por falta de memória do sistema); o VS aleatório depois de uma copa CTGP foi coberto só pelo teste nativo. Recordes e fantasmas das pistas CT ficam com os da pista emprestada (save separado do CTGP).
- **Consequência para um conjunto próprio do CTGP:**
  - cabem até 15 pistas novas sem ampliar o espaço de IDs, mas o vetor original de copas não tem vaga livre (25 lugares: 24 copas de corrida e a de batalha; a etapa 1 acima abre 16) e a tela de copas só tem duas páginas de 12;
  - as 64 pistas do CTGP como pistas novas exigiriam ampliar o enum e os 129 pontos limitados a 123, o vetor de copas (131 pontos) e a tela de copas (terceira página), e separar os recordes do save.

**Fase B (código) — mapa da engenharia reversa (16/11/2026), sem alteração no jogo:**
- **Não há tabela estática copa → pista:** nenhuma permutação de 0–95 (nem de 0–47) em u8/u16/u32 nos segmentos de dados do `main`. O mapeamento é montado em tempo de execução.
- **Caminho das pistas:** `Course/%s/%s` (`main+0xf0d209`) e `Course/%s` (`+0xf0d223`) são usados por `f_700089ed04` (`main_186.c`). Ela pega o objeto da corrida via `0x70012f9e98`, lê um índice com limite em `+0x58` (u16), lê um byte em `+0x6a` e chama um método virtual (`rt_dispatch`). O nome da pasta vem de um objeto, não de uma tabela.
- **Inicialização da UI de menus:** `f_700040c5f4` (`main_083.c`) cria os pools de recursos do menu (tamanhos 0x35, 0x2a, 0x17, 0x10…) e usa as strings `*_00` de imagens de pista.
- **Rótulo de plataforma:** 1461 (`0x5b5`) só aparece no código como aritmética de datas (dias em 4 anos, em `f_70005645f8`…); o rótulo vem de dados, ainda não localizados.
- **Próximo passo para copas extras:** rastrear dinamicamente, com o jogo rodando, os objetos de pista e copa nos dois pontos acima (`FN_ENTER` com filtro de endereço, sem mudar comportamento). Só então decidir onde aumentar os limites, com testes de save.

**Fase B (código) — o que a leitura do 4.0.1 mostrou:**
- **Tabela de copas:** `main+0x12840b8` é a tabela estática de 24 ponteiros para os ícones (Mushroom…Thunder, DLC02–05, Charger00–11), com os ponteiros em relocações `R_AARCH64_RELATIVE` e não no dado.
- **Limite fixo no código:** `f_700050ccc0` faz `min(copa, 0x17)` antes de ler essa tabela, ou seja, 24 copas fixas.
- **Nomes de pistas:** `Gu_FirstCircuit_00`… em `main+0xee7de9` são montados por código (ADRP/ADD), sem tabela de ponteiros.
- **O que acrescentar copas exige** (em vez de substituir vagas): ganchos em várias rotinas com limites fixos (24 copas, 96 pistas), nas tabelas de pista/copa, nos saves dimensionados para 96 pistas, nas páginas da tela de copas e no online. É engenharia reversa de semanas, com risco a saves.
- **Plano:** antes, um teste nativo da tabela de ganchos em `FN_ENTER`; depois, cada limite com teste próprio.
- **Ainda dependem desse trabalho:** botão aleatório, menus e copas extras do CTGP.

## CTGP Deluxe — investigação, sem implementação — 14/11/2026

**O que o projeto declara** (ctgpdx.com: download, FAQ e aviso): a versão 1.1.1 "only works on Mario Kart 8 Deluxe version 3.0.3", e o site não explica por quê. A instalação é um ZIP de 3,86 GB (4,52 GB extraído) colocado na raiz do SD de um Switch desbloqueado. O Online Play foi removido, e local/LAN continuam. Os termos dizem "fan creations intended for personal use only and are not for sale or distribution". O pacote não foi baixado: os termos proíbem redistribuição e a extração deixaria o C: abaixo da reserva de 4 GiB.

**O que esta recompilação aceita hoje:**
- **Arquivos:** `RECOMP_MOD_DIR` sobrepõe `romfs/` e dá saves próprios ao mod (`runtime/mods.c`). O próprio `mods.h` diz "no game code".
- **Código:** só `main`, `sdk` e `subsdk0` do perfil 4.0.1 são recompilados, com hashes fixos.
- **Ganchos:** `rt_dispatch` aceita substituições por endereço só em chamadas indiretas. As chamadas diretas entre funções recompiladas não passam por ele. Toda função guest começa com `FN_ENTER(endereço)`, hoje usado só pelo perfilador.

**Por que um mod de código não funciona como no console.** Mods de código do Switch (exlaunch em `exefs/subsdk9` + `main.npdm`, ou patches IPS) fazem duas coisas, e as duas quebram aqui:
- **Ganchos por endereço:** eles escrevem instruções de desvio no `.text` do `main` usando endereços fixos da versão que conhecem (daí a trava na 3.0.3).
- **Código recompilado:** numa recompilação estática o código já virou C. Escrever instruções no `.text` em tempo de execução não muda nada, em qualquer versão.

**Caminhos, do menor ao maior:**
1. **Só arquivos sobre o 4.0.1** (já possível com `RECOMP_MOD_DIR`): substituições de pistas, música e interface podem funcionar. Copas e pistas extras, menus e opções novas dependem do código e não apareceriam. Formatos que mudaram entre 3.0.3 e 4.0.1 podem quebrar.
2. **Portar o código do mod para o 4.0.1:**
   - recompilar o módulo do mod como mais uma unidade do pipeline;
   - capturar os ganchos que ele instala no boot (escritas no `.text` do `main`) e aplicá-los como desvio na entrada das funções, com um `FN_ENTER` que consulte uma tabela de ganchos (exige recompilar o jogo inteiro uma vez);
   - mapear cada endereço 3.0.3 → 4.0.1 por comparação binária dos dois `main`, o que exige o `main` 3.0.3 do próprio usuário.
   Ganchos no meio de funções precisariam de pontos inseridos na recompilação.
3. **Recompilar o 3.0.3 como segundo perfil:** dispensa o mapeamento de endereços, mas todas as adaptações guest com endereço fixo (Mii, LAN, setjmp, Yaz0 e outras) teriam de ser refeitas para o 3.0.3. As do host em geral sobrevivem. A qualificação também teria de ser refeita.

**Varredura do pacote extraído pelo usuário (15/11/2026, só leitura, fora do repositório em `Documents/CTGPDX`; nada foi executado nem copiado):**
- **Tamanho e título:** 1.120 arquivos, 4,52 GB, tudo em `atmosphere/contents/0100152000022000/` (MK8 Deluxe).
- **Código:** `exefs/subsdk1` (Skyline, 42.869 B, build id `782d51e7…`), `exefs/main.npdm` e o plugin `romfs/skyline/plugins/CTGPDX.nro` (548.864 B). Não há `exefs_patches`.
- **Arquitetura — o fato decisivo:** o `main.npdm` do mod declara processo **32 bits** (flags `0x4`, espaço de endereço de 32 bits). O `main.npdm` do 4.0.1 deste projeto declara **64 bits** (`0x7`). `subsdk1` e o plugin são **ARM de 32 bits**: entrada `b` ARM `0xEA00001E`, `.dynamic` e símbolos ELF32 e importações `A32HookFunction`, `A32InlineHook` e `__aeabi_*`. O MK8 3.0.3 que o mod atende é, portanto, um executável de 32 bits, e o 4.0.1 é AArch64. Isso explica a trava na 3.0.3: a passagem para 64 bits mudou todo o código de máquina.
- **O que o plugin faz:** não importa nenhum símbolo do jogo; instala ganchos por endereço no `main` de 32 bits. Pelas strings:
  - 64 pistas `CT_00`–`CT_3F`, com música e arquivos lidos de `content://Course/` e `content://Audio/Resource/Stream/`;
  - ícones de copa extras (`L_CupIcon_18`–`25`) e um botão aleatório (`Random_00`, `RandomBtnOnOff`);
  - troca de fundo do menu;
  - versão `CTGPDelux010101`.
- **Arquivos (romfs):**
  - **novos, sem efeito sem o plugin** (1.020 arquivos): 64 pistas novas em `Course/CT_xx` (816 arquivos, 3,4 GB), 128 músicas `pBGM_CT_xx`, 64 imagens `ym_CoursePict_CT_xx`. Nenhuma substitui pista original; o jogo só as carrega se o código registrar os nomes `CT_xx`.
  - **substituem arquivos originais** (99): as músicas `BGM_MENU_*` e `BGM_TITLE*`, `StaticSysMenu.bars`, `JugemObj.bfres`, `message.sarc` de 15 idiomas, `common.sarc`, `menu.sarc`, `race.sarc`, telas de título e de carregamento, e 80 ícones de copa `ym_CupIcon*`.

**Consequência para os três caminhos:**
1. **Só arquivos sobre o 4.0.1:** aplicaria apenas os 99 arquivos que substituem originais. Não aparece nenhuma pista nova. As SARCs de mensagens e menus vêm da 3.0.3 e podem não corresponder às do 4.0.1, com textos ou layouts errados ou crash. Serve só como teste de compatibilidade de formato, não como "CTGP jogável".
2. **Portar o código:** não há comparação binária possível entre um `main` de 32 bits e um de 64 bits. Seria reescrever a lógica do plugin (registro de 64 pistas, copas, menus, música) como ganchos próprios nas funções AArch64 do 4.0.1, com engenharia reversa do 4.0.1. É um projeto novo de mod, não uma conversão; os 1.020 arquivos de pista seriam reaproveitados se os formatos forem os mesmos.
3. **Perfil 3.0.3:** o recompilador (LASRecomp/SMM2Recomp) traduz só AArch64; não há suporte a A32/Thumb. Exigiria um front-end ARM de 32 bits inteiro, mais a refação de todas as adaptações guest. É o caminho mais caro.

Nenhum dos três dá o CTGP completo no 4.0.1 sem um projeto grande. O passo 4 do roteiro abaixo (caminho 1) só deve ser tentado como teste de formato, depois que o jogo for reimportado.

**Roteiro para testar depois (caminho 1, só arquivos; nenhum passo altera código):**
1. Baixar o ZIP pela página oficial, para uso pessoal. Nunca colocá-lo em `upstream`, testes, releases ou no setup.
2. Listar sem extrair e guardar a lista fora do repositório:
   `local/python/Scripts/python.exe -I -c "import zipfile,sys; z=zipfile.ZipFile(sys.argv[1]); [print(i.file_size, i.filename) for i in z.infolist()]" "<CTGP.zip>" > local/ctgp-lista.txt`
   Na lista, procurar o título (`atmosphere/contents/0100152000022000/`, MK8 Deluxe), a pasta `romfs/` e a parte de código (`exefs/subsdk*`, `main.npdm`, `exefs_patches/`). Isso decide entre os caminhos 1, 2 e 3.
3. Extrair só a pasta do título num diretório novo e vazio, em disco com espaço para os ~4,5 GB **além** da reserva de 4 GiB, por exemplo `<disco>/ctgp/0100152000022000/`. Não executar nada de dentro dela.
4. Rodar pelo roteiro de teste, que já força `LARECOMP_SAVE_DIR` para uma cópia isolada. Isso importa porque `RECOMP_MOD_DIR` sozinho redireciona os saves para a pasta do mod. Começar por menu e contra-relógio:
   `local/python/Scripts/python.exe scripts/test-race.py time-trial --last-frame 4800 --shots 3 --shot-step 1400 --timeout 300 --env RECOMP_MOD_DIR=<disco>/ctgp/0100152000022000 --env LARECOMP_LOG_FILES=1`
   O `RECOMP_MOD_DIR` deve apontar para a pasta que **contém** `romfs/`. O launcher recusa `RECOMP_MOD_DIR` quando um Mii do perfil está ativo (`mii_native.rs`), então nesse teste o perfil Mii fica desativado.
5. Registrar como evidência:
   - os arquivos lidos do mod (`LARECOMP_LOG_FILES`);
   - erros de formato e crashes;
   - imagens do menu, da seleção de copa e da corrida;
   - quais pistas e menus mudaram e quais continuaram originais (o esperado sem o código do mod).
   `gameplay_verified` continua falso.
6. Depois da varredura de 15/11, o caminho 2 é uma reimplementação do plugin em AArch64, e o caminho 3 exige um front-end A32 (veja acima). Qualquer um deles precisa de decisão do usuário antes de começar. O caminho 2 começa com um teste nativo da tabela de ganchos em `FN_ENTER` com funções sintéticas; só depois vem a recompilação do jogo inteiro.

**Caminho 1 testado — 15/11/2026.**

*Condições:* jogo reimportado do NSZ (`Importação 4.0.1 verificada`), build completa, PRs #18/#19 incluídas. Contra-relógio de 4.800 quadros com `LARECOMP_LOG_FILES=1`. As sobreposições parciais são hardlinks em `local/ctgp-overlay-*`; a pasta `Documents/CTGPDX` não foi alterada e o plugin `skyline/` nunca foi usado.

| Sessão | Sobreposição | Resultado |
|---|---|---|
| `race-time-trial-o33nrkca` | nenhuma (base) | 60 FPS até o fim |
| `race-time-trial-m5um97f1` | romfs completo do CTGP | **crash** aos 16 s ao entrar no menu: `c0000005` em `0x90`, `f_70008bab3c`, logo após ler `UI/cmn/menu.sarc` |
| `race-time-trial-n4fawqy7` | sem `UI/cmn/menu.sarc` | **crash** no mesmo ponto: `c0000005` em `0`, `f_70005908a8` (resta `common.sarc`) |
| `race-time-trial-ssw00gbi` | sem `UI/` | 60 FPS até o fim; músicas de título/menu, `StaticSysMenu`, Lakitu do CTGP carregados |
| `race-time-trial-lzz6p1u3` | sem `UI/`, só `message.sarc` | 60 FPS até o fim |
| `race-time-trial-79ux_uty` | `CT_00` no lugar de `Gu_FirstCircuit`: arquivos da pista, `MapObj`, música e imagem; banco de sons da pista original | **pista CTGP correu**: modelo, cenário, objetos, minimapa e música próprios; 60 FPS, máximo 18 ms, sem erro |

**Conclusões:**
- O romfs do CTGP **como está** não funciona no 4.0.1: as SARCs de interface `common.sarc` e `menu.sarc`, montadas para 3.0.3 e para o plugin, derrubam o menu.
- Os formatos de pista são os mesmos do 4.0.1: modelo `FRES 0.5.0.3`, KCL, AAMP, `VFXB`, BNTX e BYAML. A música `FSTM` v3 e o Lakitu `FRES 0.5.0.2` também carregaram.
- **Uma pista CTGP corre no 4.0.1 sem nenhum código**, ocupando a vaga de uma pista existente.
- `gameplay_verified=false`: o teste cobriu uma pista, uma volta parcial e só o contra-relógio.

**Caminho 2 — plano, a decidir com o usuário (nada foi implementado):**
- **Fase A — pistas em vagas, sem código.**
  - Uma ferramenta local (`toolchain/ctgp_slots.py`) monta uma sobreposição privada em `local/`, com hardlinks quando o disco permite, a partir da cópia do usuário. Ela nunca vai para repositório, release ou setup.
  - Mapeia `CT_xx` para vagas existentes: arquivos de `Course/<vaga>`, `pBGM_<vaga>_N/F`, `ym_CoursePict_<vaga>_00^u` e o nome da pista no `message.sarc` (MSBT dentro da SARC).
  - Bancos de som da pista: cada `Course_CT_xx.barslist` (ARSL) aponta para o banco de uma pista original (`CT_00` → `Gu_DossunIseki`). A primeira versão usa o banco da vaga.
  - Limite: 48 vagas originais, ou mais 48 do Booster Course Pass (`Cnsw_*`, mesma estrutura) com o DLC importado. As 64 pistas não cabem todas ao mesmo tempo; o usuário escolhe ou alterna conjuntos.
  - Launcher: opção "Pacote de pistas" com ativação, como o DLC, e `RECOMP_MOD_DIR` só no processo filho. Nas salas da Steam, exigir o mesmo pacote (hash), como a regra de DLC.
  - Testes: unidade da montagem com arquivos sintéticos, e GP completo com quatro pistas CTGP (imagem, áudio, voltas, reinício e save).
- **Fase B — comportamento do CTGP, com código.** Copas e pistas extras além das vagas, botão aleatório e menus do CTGP.
  - Engenharia reversa, no AArch64 do 4.0.1, da tabela de pistas e copas e das telas de seleção.
  - Ganchos próprios aplicados pelo pipeline: por unidade gerada, como `yaz0_native.py`, ou por uma tabela de ganchos em `FN_ENTER`, com validação de hash.
  - Layouts de menu refeitos sobre as SARCs do 4.0.1, porque as do CTGP derrubam o menu.
  - Cada gancho com teste nativo e prova em corrida.
- **Fase C — multijogador** com o pacote: validação em duas máquinas, como exige o projeto.
- **Limites legais:** o CTGP é de uso pessoal e não pode ser redistribuído. A ferramenta só lê a cópia do próprio usuário; nada do pacote entra em fontes, testes públicos ou releases. Créditos e termos ficam em `docs/THIRD_PARTY.md` quando houver código.

## PR #12 (DLC e Configurações) e PR #9 integradas — 13/11/2026

Merge de `pr-12` (PlugNChug: Booster Course Pass verificado, segunda variante verificada do NSP base, aba **Configurações** com 58 opções do F1, avisos fixos dispensáveis); a PR #9 (recompilação acessível após a build) vem incluída no primeiro commit dela. Os 17 blocos de conflito eram aditivos e foram resolvidos mantendo os dois lados: verificações Mii + raiz do DLC e `SDL_VULKAN_LIBRARY` + `MK8_DLC_ROOT` na abertura do jogo; estados Mii/salas + `dlc`/`dlc_import` no status; `steps(root)?` com `start_steps_guarded` genérico; a importação de DLC passa a bloquear **Jogar** dentro de `start_game`; traduções mescladas como JSON (116 chaves novas; mantido "Automatic"); seção Mii redesenhada da fix-chore + nova seção Configurações. O teste de nome de usuário da PR não foi trazido, porque o campo foi removido na fix-chore, onde o multiplayer usa o Mii.

**Conflito semântico corrigido:** `toolchain/dlc.py` e o hook MiiModel de `toolchain/host.py` adaptam o mesmo caminho de arquivo de sistema em `services/fs.c`. O hook Mii roda antes, e o DLC recusava a forma já adaptada (`test_mode_patches`/`test_host_layout` falhavam). Agora `dlc.py` aceita exatamente uma das duas formas conhecidas e, na adaptada, compõe DLC → `MK8_MII_RESOURCE_DIR` → `sysdata`, mantendo a recusa de truncamento. Novo `test_adaptation_composes_with_mii_resource_mount` usa os arquivos reais do upstream. O roteador simulado do teste de recompilação (#9) passou a servir `.cjs`/`.wasm` e a reescrever o import `three` como o servidor real.

Validação: formatação, Clippy do workspace com `-D warnings` e 65 testes Rust; Python: DLC (11), configurações, importação, pipeline, conversor MiiModel, adaptações experimentais, `test_mode_patches`, `test_host_layout` e `test_prebuilt_host`; navegador: e2e (21), idiomas (3), DLC no launcher (4), editor Mii e configurações. Runtime recompilado com `--runtime-only`. `local/race-time-trial-q45u1qzo`: saída 0, serviço `aoc:u` novo (`installed=0` nesta máquina sem DLC), montagem `system data 0100000000000802` pelos recursos Mii gerados, retratos na grade Mii e o Mii com rosto e capacete. DLC em multiplayer continua não qualificado: anfitrião e convidado com estados de DLC diferentes ainda não são bloqueados nem avisados.

## Importação NSP/NSZ — segunda variante verificada do jogo base

O erro “Conteúdo do MK8 base ou da atualização 4.0.1 ausente no pacote” foi reproduzido com um NSP cujo programa base é `6d1fd4733f0e29b754081d2bf0c56d702ad89a86ea30e67b69bb84a0a66c8b0d`. O importador aceitava apenas `e1ace5248218fa6393ec1637ce8c2c7943fb1d126c2dea93135d641cfd692183`. Os dois têm o mesmo tamanho, mas conteúdo binário diferente; renomear o arquivo não satisfaz a verificação de integridade.

A seleção agora aceita essas duas identidades explícitas do programa base. Mantém o SHA-256 completo de cada NCA reconstruído, os conteúdos fixados da atualização, a integridade do LibHac e os hashes exatos do ExeFS 4.0.1 AArch64. Bases desconhecidas ou pacotes com ambas as variantes são recusados. A reserva de disco considera uma base por importação, sem somar o espaço das duas alternativas.

Validação: 5 testes do importador, 5 de acesso a arquivos e 3 de armazenamento passaram. Fixtures autorais cobrem as duas identidades, NCA e NCZ, importação de contêineres PFS0, tamanho/hash divergentes, seleção ambígua e orçamento de disco. Nos arquivos privados, a nova variante foi importada com a atualização em 47,3 s; passaram a integridade do LibHac, os hashes do ExeFS e a versão NACP. Todos os 6.597 arquivos do RomFS (12.131.696.580 bytes) são idênticos à extração que já funcionava. A build completa usando a configuração NSP concluiu em 41,2 s com os caches existentes, e os hashes do candidato resultante foram conferidos. Essas durações não representam uma build sem cache. Configurações salvas, arquivos de usuário e a instalação original foram preservados; as saídas novas ficaram nos dados de desenvolvimento isolados. Não houve nova execução do jogo nem qualificação de gameplay; `gameplay_verified` permanece falso.

## DLC — importação e controle no launcher

Prontidão dos campos de DLC: as mesmas tiles da etapa 2 recebem contorno verde e confirmação quando o NSP/chaves são encontrados. A API de consulta é somente leitura e reutiliza a seleção de chaves do importador (campo explícito, configuração salva ou detecção automática). O cliente verifica novamente ao digitar, escolher arquivos e atualizar o estado, descartando respostas de caminhos antigos. A cor não substitui validação de conteúdo, hashes ou descriptografia. Testes autorais cobrem arquivos presentes/ausentes, seleção explícita/automática de chaves e a rejeição de uma resposta atrasada para um caminho anterior. Contornos e marcas foram conferidos em desktop e celular; launcher release recompilado.

Integração da interface: o script privado continuava abrindo `target/release/Launcher.exe`, mas a branch de DLC havia partido do master sem o commit de recompilação ainda separado. A branch agora incorpora o commit original `d09565e`, preservando o layout com **Recompilar jogo** e **Jogar agora** juntos abaixo do banner verde. Não houve alteração no script privado ou no diretório de dados.

Reinstalação explícita: o cartão de DLC instalado oferece **Reinstalar ou selecionar outro NSP**. A API encaminha a opção booleana até `--reinstall` no importador; esse caminho valida/extrai o pacote selecionado mesmo quando os arquivos atuais estão íntegros. A publicação preserva a instalação anterior em backup, sem mudar a preferência de ativação, configuração ou saves. Falhas de validação deixam a instalação atual e o candidato intactos. O perfil segue limitado ao mesmo Booster Course Pass verificado; isso não amplia versões ou títulos aceitos. Validação: 10 testes de importação/IPC de DLC, 43 testes Rust, Clippy e formatação aprovados; 24 testes de navegador passaram e 1 de relay foi pulado, incluindo recompilação visível e reinstalação de DLC instalado. A reinstalação do NSP privado pela API e pelo supervisor reais terminou com backup criado, candidato e configurações idênticos, preservando a preferência de DLC. Launcher release recompilado; runtime/guest não foram alterados.

O NSP adicional fornecido pelo usuário (121.056 bytes) foi inspecionado em uma pasta privada de desenvolvimento. O CNMT identifica `AddOnContent` com título `0100152000023001`, associado ao aplicativo `0100152000022000`, e seu registro de conteúdo corresponde ao tamanho e SHA-256 do NCA de dados. A descriptografia com as chaves do usuário e a extração pelo LibHac com `ErrorOnInvalid` passaram. O RomFS adicional contém somente `contents.dat` (4.096 bytes) e `version.txt` (5 bytes). O RomFS combinado da versão 4.0.1 tem 48 pastas de curso `Cnsw_*`; isso é compatível com os recursos das pistas já estarem na atualização, mas não comprova sua execução nesta build.

O bloqueio confirmado era `aoc:u`: contagem e listagem devolviam zero. `toolchain/import_dlc.py` agora importa somente esse pacote verificado para um candidato experimental 4.0.1 já preparado, com bloqueio de preparação, reserva de disco, rejeição de links e publicação após validar hashes dos NCAs, associação do CNMT ao jogo e os dois arquivos extraídos. O ticket e as chaves do usuário são usados em memória pelo LibHac; nenhuma chave é gravada no manifesto. Um manifesto binário versionado e três registros no candidato permitem conferir a instalação e seus hashes antes de jogar. Uma instalação incompleta ou divergente é recusada, sem substituição automática.

`runtime/mk8/services/dlc.c` implementa descoberta, preparação e evento do conteúdo instalado; `toolchain/dlc.py` adapta somente a cópia privada verificada de `aoc:u` e `OpenDataStorageByDataId`. Sem manifesto, a contagem permanece zero e a preparação/montagem do DLC falha. Com manifesto e arquivos íntegros, o índice 1 é listado e o armazenamento monta os dois arquivos reais. Não há variável de ambiente que simule a instalação. Na execução real, o MK8 usou os comandos 8, 3, 7 e 5, depois abriu o armazenamento do título `0100152000023001`. Referências de protocolo e acesso em `THIRD_PARTY.md`.

Validação local isolada: importação do NSP real concluída e runtime recompilado com `--runtime-only` (213 unidades, ligação e 13 dependências verificadas), preservando a biblioteca recompilada do jogo. Passaram 7 testes de DLC, 5 do importador base e 5 de serviços/runtime. Os testes nativos executam o serviço com arquivos autorais: ausência, índice instalado, limites de entrada e saída, evento, título incorreto e arquivos/manifestos alterados. A importação cobre publicação, reutilização, falha do extrator, NCA alterado, caminho fora da pasta selecionada e preservação da configuração/candidato quando falha.

O jogo exibiu a Golden Dash Cup e iniciou Paris Promenade. Uma corrida Grand Prix de três voltas nessa pista terminou na tela de resultados (1º lugar), com saída normal após 16.500 quadros, em 284,2 s de sessão. As capturas registraram largada, corrida e resultados; a direção assistida e entradas programadas foram usadas. O save copiado foi atualizado após a chegada e carregado em uma nova execução, que voltou à seleção das pistas adicionais sem erro. Sem DLC, a mesma sequência de seleção permaneceu no menu da copa e o serviço registrou zero conteúdos instalados. Esses testes usaram saves copiados e pastas de sessão privadas; a instalação original e os saves pessoais não foram usados como destino.

O controle sem DLC também iniciou uma corrida base no Mario Kart Stadium, com captura aos 3.400 quadros e saída normal em 66,0 s. Isso verifica a entrada na pista base, não uma corrida base completa nesta rodada.

O usuário confirmou manualmente o funcionamento do protótipo. A interface agora oferece o cartão opcional **Conteúdo adicional**, após as três etapas de instalação: seleção de NSP/chaves, importação supervisionada com estado/log e reparo, em português e inglês. O controle **Ativar Booster Course Pass** fica na aba **Configurações**. O importador reaproveita a infraestrutura existente de processos e bloqueios; operações concorrentes, inclusive preparação direta pela CLI, recusam alterações. Durante o jogo, os controles ficam desabilitados e a API também rejeita a operação.

Os arquivos verificados ficam em `dlc/` na pasta de dados, independentes da identidade da build. A preferência fica em `dlc.json` e é lida ao iniciar o jogo; desativar não remove os arquivos nem modifica saves. O launcher fornece ao runtime a pasta verificada ou uma opção explícita de desativação, que também impede descoberta, preparação e montagem. O pipeline preserva o DLC em builds novas e migra a cópia antiga após conferir seus arquivos, sem excluir a origem. Um reparo só publica arquivos novos após validá-los, preservando o destino anterior em `dlc-backup-*`. Erros de acesso não são tratados como corrupção a contornar.

O candidato registra `dlc_supported`; a ausência desse campo em manifestos antigos equivale a falso. Essas builds continuam disponíveis para o jogo base, mas o cartão pede **Atualizar a build** antes de importar/alternar DLC, com um botão de atualização acessível na etapa 3. Não basta exibir uma preferência de ativação se o executável ainda não implementa o serviço.

Validação da interface: testes escritos primeiro reproduziram a ausência de persistência, bloqueio do runtime e controles; depois passaram 10 testes de DLC, 9 do pipeline, 5 de importação base e 5 de serviços/runtime. Os testes autorais também cobrem reparo com backup, preservação após falha, migração ao mudar a pasta de build e preferência desativada. Passaram 41 testes Rust do workspace, formatação e Clippy sem avisos. No navegador passaram 19 testes (16 gerais, com 1 pulado por falta de relay configurado, mais 2 de idiomas e 2 de DLC), incluindo importação, erros, alternância, tradução, atualização de runtime antigo e layout móvel sem rolagem horizontal. Launcher release e runtime de 213 unidades foram compilados, mantendo a biblioteca do jogo.

Duas execuções pela interface/API reais, com saves copiados, chegaram às capturas aos 2.000 quadros e saíram com código zero. Desativado: `aoc:u` retornou zero e a seleção mostrou o aviso de compra do DLC. Ativado após reiniciar o launcher: a preferência desativada foi recuperada, o controle pôde ser ligado e Paris Promenade ficou selecionável; houve preparação e montagem reais do conteúdo. Tentativas de alternar durante as sessões foram recusadas. Os hashes dos arquivos de usuário originais de desenvolvimento permaneceram idênticos. A importação idempotente do NSP real pela API/supervisor concluiu, com candidato e configurações idênticos. Uma tentativa anterior perdeu a captura por erros de swapchain Vulkan (`VK_ERROR_OUT_OF_DATE_KHR`); o processo privado foi encerrado, sem contar essa tentativa como aprovação ou alterar código gráfico.

Limites: não há alternância durante uma sessão do jogo, atualização/remoção automática, suporte genérico a outros NSPs/NSZs ou qualificação das 48 pistas. Áudio, controle físico, velocidade do relógio e multiplayer com DLC não foram qualificados nesta rodada; o indicador de 60 FPS nas capturas não substitui essas medições. Defeitos visuais preexistentes continuam visíveis. `gameplay_verified` permanece falso. As mudanças de DLC ficam registradas na branch `explore/dlc-support` para revisão; não houve PR ou release. Instruções em `README.pt-BR.md`.

