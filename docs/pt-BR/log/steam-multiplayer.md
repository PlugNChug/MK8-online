# Registro técnico — Steam e multijogador

[Índice](README.md) · [English](../../en/log/steam-multiplayer.md)

## SteamFriends e sobreposição no modo solo — issue #71

O executável público da 0.3.0, conferido pelo SHA-256 da release, reproduziu a falha da [issue #71](https://github.com/Vnnc1/MK8-online/issues/71) com a DLL x64 do Counter-Strike 2 na pasta de dados. Ela oferece `SteamAPI_SteamFriends_v018`; o launcher procura `SteamAPI_SteamFriends_v017` ou o export legado `SteamFriends`. A DLL testada de Brawlhalla oferece `v017` e a 0.3.0 conseguiu listar amigos com ela.

A [PR #33](https://github.com/Vnnc1/MK8-online/pull/33) entrou na 0.3.0. A tentativa de validar várias candidatas só chegou depois, no [commit 26a6dce](https://github.com/Vnnc1/MK8-online/commit/26a6dce). Na `fix-chore` em `a9aa55b`, a DLL `v018` na pasta de dados ainda impediu a busca nas bibliotecas de jogos, mesmo com Brawlhalla instalado. Em processos novos, a lista de amigos abriu tanto na busca automática sem DLL local quanto com a DLL incompatível nos dados e a compatível junto ao executável. O ZIP do PC do relator não acompanhou a issue.

Em duas execuções solo com o mesmo runtime, o usuário confirmou que Shift+Tab falhou após o erro de DLL e abriu a sobreposição quando o launcher listou os amigos antes de iniciar o jogo. O carregamento de `gameoverlayrenderer64.dll` no processo do MK8 acompanhou esse resultado. Na 0.3.0 pública, `MK8_STEAM_DLL` apontando para a cópia compatível também permitiu listar amigos mesmo com a DLL incompatível presente na pasta de dados.

As cópias testadas de Counter-Strike 2 e Deadlock têm o mesmo SHA-256. Uma prova isolada inicializou essa DLL, obteve a interface `v018` e leu a identidade e a contagem de amigos usando funções exportadas pela própria DLL. O launcher continua sem suporte explícito a essa versão. Salas, convites e transporte com `v018` permanecem sem validação.

## CI da PR #59 — liberação de sockets no Windows

A [execução em `fe69983`](https://github.com/Vnnc1/MK8-online/actions/runs/44372501134) voltou a falhar somente no teste Rust de liberação da porta Steam, com WinSock 10048; a etapa Python foi pulada. A correção de sockets já reproduzida e revisada em `4e34d8a` agora compõe esta PR como mudança separada. O arquivo de sessão anterior à integração era idêntico ao da reprodução original, e o arquivo integrado é idêntico ao fix revisado.

O gateway compartilha seus dois sockets originais com `Arc` entre os workers. Isso evita os handles herdáveis criados por `UdpSocket::try_clone`, que um processo filho podia manter vivos após o encerramento do gateway. A regressão usa o gateway real e confirma que ambas as portas são liberadas enquanto o filho permanece vivo, com limpeza do filho antes das asserções. Mantidos endpoints, timeouts, joins, autenticação e proteção contra replay; revisão independente da integração aprovada. O processo exato envolvido na falha hospedada ainda não foi observado.

Na fonte integrada passaram 96 testes Rust com o toolchain 1.100.0, incluindo os 45 do launcher e a nova regressão; um teste Steam ao vivo já ignorado permaneceu ignorado. Formatação, Clippy com `-D warnings`, build debug do launcher e nove testes Python do pipeline com TEMP em alias 8.3 passaram. Recibo privado: `local/uw-pr18/pr59-socket-integrated-checks.json`. Os 489 arquivos protegidos e o launcher instalado conservaram os hashes; nenhum processo de teste/build permaneceu ativo. Nova CI pendente; multiplayer em duas máquinas e `gameplay_verified` continuam sem qualificação.

A [PR #59](https://github.com/Vnnc1/MK8-online/pull/59) está aberta para `fix-chore`. A [repetição de CI em `aa472b1`](https://github.com/Vnnc1/MK8-online/actions/runs/44358328861) passou formatação, Clippy e testes Rust, inclusive a liberação de porta Steam. O pipeline Python falhou depois: o fixture sintético de atualização do runtime não fornecia os arquivos de menu exigidos pela preparação de Settings. Os registros abaixo que mencionam ausência de publicação descrevem etapas anteriores.

Reproduzida a mesma ausência de `UI/cmn/menu.sarc` com Python 3.12.10, como na CI. Corrigido somente o fixture de `test_runtime_refresh_records_new_dll_without_changing_game_or_saves`: a fronteira de preparação fornece duas saídas autorais; a atualização real verifica seus registros no manifesto, os argumentos de preparação e a preservação da biblioteca guest, save e recibo nativo. A derivação real e a rejeição de hashes continuam cobertas por `test_settings_layout.py`; nenhuma validação de produção foi removida. Passaram os nove testes do pipeline e os sete de layouts, sem skips, com revisão independente aprovada.

A [CI seguinte em `0ee287a`](https://github.com/Vnnc1/MK8-online/actions/runs/44371038014) também passou os gates Rust e revelou uma segunda diferença no fixture: TEMP usava `RUNNER~1`, enquanto `file_record` resolve o executável para `runneradmin`. Reproduzido o mesmo erro com um alias 8.3 local real. A raiz temporária agora é resolvida antes de construir os caminhos, como no chamador de produção; todas as asserções permanecem. O teste focado e os nove testes do pipeline passaram usando esse mesmo alias, sem skips, com revisão independente aprovada. A execução completa precisou de permissão para criar a junction temporária do teste já existente. Nova CI pendente; nenhum código de runtime/launcher, dado privado ou qualificação de gameplay mudou.

## Continuação das correções — Steam, texturas e apresentação

Na `fix-chore` após `07126fd`, seleção Steam agora tenta carregar/validar exports de cada DLL candidata até encontrar uma compatível, em vez de falhar na primeira biblioteca descoberta. Prioridade e escopo de caminhos explícitos/locais preservados; versões ABI exigidas não foram relaxadas. Falhas incluem caminho e motivo (até quatro detalhes). Teste comportamental cobre candidata incompatível seguida de compatível, ausência e erro de todas. 16 testes de sessão passaram e o teste Steam real optativo permaneceu ignorado. Não houve conexão Steam entre duas máquinas; #45 ainda precisa de reteste com as DLLs do relator.

Código das PRs #43/#44 aplicado localmente após revisão: histórico de cubemap requer imagem GPU existente, evitando reupload de memória guest reutilizada; cabeçalho CmdRec reduzido de 88 a 72 bytes. Todos os consumidores host recompilados juntos. Passaram 14 testes de texturas, 19 de submit/descritores/compute/oclusão e 1 exercendo os 62 comandos compactados. Sem merge remoto das PRs nesta etapa. Não atribuir a essas mudanças a solução de Miis brancos, faróis #49 ou falta de memória #52 sem reprodução.

Investigação #46 confirmou que o hook MK8 não habilita câmera ampla e a apresentação mantinha barras independentemente de `aspect=unlocked`. Adaptação própria `output_aspect.py` agora usa essa escolha para esticar apenas a apresentação final; resolução interna e shape da câmera permanecem iguais. Menu nativo esclarece esticamento, sem promessa de campo de visão maior. Teste C executa cálculo preparado em 3440×1440, 1280×1024 e 1920×1080 para preencher/preservar proporção, aprovado. Smoke nativo `local/race-time-trial-7gpln4nc` abriu swapchain 1280×540 e terminou com código 0; captura do título foi inspecionada, mas o hook captura a imagem do jogo em 1920×1080, portanto não comprova visualmente o esticamento final da janela. Falta conferir corrida/split-screen e apresentação final no monitor.

Launcher debug compilado, 36 testes aprovados; Clippy de launcher/sessão com `-D warnings` e formatação aprovados. Runtime final SHA-256 `ce7e59c1dd07c72205fcd0a0e1b22465bb5af98ac176596f6d1b192c57ecf65c`. Sem novo setup/publicação, nenhuma alteração de saves pessoais. Pendências prioritárias mantidas: erro 193/SDL #47, memória GPU #52, controle #50, iluminação split-screen #49; DLC #36 aguarda metadados do relator. Importação de saves #41/#48, início direto #37 e portabilidade #35 ainda não implementados. #51 requer comprovar espaço físico, pois o pipeline já usa hardlinks.

## Contribuição: Steam, navegação e Mii sobre fix-chore — 16/11/2026

Base de integração: `d443092`. A contribuição preserva DLC verificada, configurações, espanhol, CTGP e correções de runtime já existentes nessa branch. A correção equivalente do congelamento Mii já incorporada pelo mantenedor foi mantida.

- **Steam:** resolve acessores C versionados com fallback legado; busca bibliotecas registradas/adicionais e layouts internos sem corte em 40 jogos; mantém todos os amigos pesquisáveis, ordenados por disponibilidade/nome, inclusive offline. Não identifica amigos invisíveis. Nenhuma DLL Valve é distribuída; a dependência autossuficiente permanece pendente.
- **Transporte:** datagramas criptografados acima de 1200 bytes usam Reliable; menores continuam UnreliableNoDelay. O backend simulado impõe o limite real e testa as fronteiras nos dois sentidos. Retransmissão/ordenação podem aumentar a latência sob perda; o efeito em uma corrida real ainda precisa ser medido.
- **Sessão:** sair do jogo encerra workers e libera lobby/gateway; um convidado ocioso pode aceitar outra sala sem reiniciar o launcher, mas um jogo vivo continua protegido. Entrada valida LobbyEnter, identidade, DLC e CTGP; falhas deixam o lobby. Convites pelo cliente e pelo launcher usam a mesma rotina.
- **Navegação:** anfitrião escolhe automático ou manual; convidado escolhe a navegação antes de aceitar o convite. Ambos os botões de anfitrião criam uma sala Steam; apenas abrir manualmente não conecta a uma sala de outro anfitrião. Preferência do convidado persiste por computador.
- **DLC opcional:** o campo em Arquivos do jogo acompanha a preparação. Como o importador atual exige um candidate pronto, a importação ocorre após a build bem-sucedida, na mesma operação guiada. Campo vazio preserva a preferência; falha/cancelamento não dispara a importação pendente. O importador e os manifestos upstream são reutilizados; não há segundo formato de receipt. Importação posterior permanece em Instalar → Conteúdo adicional, acessível por atalho em Jogar, com ativação em Configurações.
- **Mii:** automação confirma o perfil ativo sem DOWN para um preset; texturas de óculos AFL RG8 trocam cobertura/luminância para a ordem NX, com revisão do conversor. Exige regenerar recursos Mii e recompilar o runtime para aplicar ambos os ajustes; atualizar apenas o launcher não repara recursos derivados antigos.

Validação local da integração: `cargo test --workspace --locked` (77 aprovados, um teste Steam real optativo ignorado), formatação, Clippy com `-D warnings` e build release do launcher. Conversor Mii: oito testes aprovados; automação LAN: um teste C compilado/executado em host e guest; supervisão/navegação: quatro testes com processos reais e executável autoral aprovados. Navegador: 24 testes aprovados e um relay externo optativo ignorado; idiomas: quatro aprovados. Capturas desktop/mobile inspecionadas. Nenhum arquivo comercial foi necessário para esses testes. Os artefatos de diagnóstico permanecem privados e não integram a contribuição.

Limites: sem corrida completa entre máquinas, reconexão real entre dois clientes, teste intercontinental ou nova importação comercial ponta a ponta nesta integração. Testes anteriores chegaram à seleção da pista, mas o anfitrião encerrou com Vulkan device loss também reproduzido em solo; esse computador foi retirado da próxima rodada de testes, sem atribuir causa física. `gameplay_verified` e `multiplayer_verified` permanecem falsos. Instalador, tag e versão de lançamento ficam para decisão do mantenedor após revisão.

## Salas da Steam exigem o mesmo estado de DLC — 13/11/2026

O anfitrião publica `mk8_dlc` (`1` com o Booster Course Pass instalado e ativado; `0` sem ele) nos dados do lobby, ao lado de `mk8_code`. O estado é o mesmo que o jogo recebe na abertura (`dlc::launch_root`). Ao entrar, `SteamRoom::join_live` lê a chave **antes de abrir o transporte** e recusa estados diferentes com uma mensagem acionável ("Importe e ative o DLC…" / "Desative o DLC…"), saindo do lobby da Steam. Lobby sem a chave, de launcher sem suporte a DLC, conta como sem DLC; valor desconhecido é recusado. A decisão fica em `steam::check_dlc` (teste `dlc_state_must_match_the_host_lobby`: quatro combinações, anfitrião antigo e valor inválido). A entrada automática por `+connect_lobby` agora mostra o erro em vez de ignorá-lo silenciosamente. O cartão de DLC em **Configurações** informa a regra; textos em PT/EN. Validação: formatação, Clippy, 66 testes Rust, e2e (21), idiomas (3) e DLC no launcher (4). **Sem prova com duas contas Steam reais.** Salas P2P (`MK8P-…`), convites e relay continuam sem essa verificação; hoje são usados apenas por scripts/API, pois a interface oferece só salas da Steam.

## Multijogador só pela Steam, com o Mii e entrada automática na sala — 12/11/2026

**Aba Multijogador:** só oferece salas pela Steam. Saíram da tela:

- criar sala na rede local, pela internet ou com código P2P;
- convites `mk8://`, campo de código, busca de salas e endereço alternativo;
- o campo "Nome de usuário".

Essas APIs continuam no servidor (`scripts/test-multiplayer.py` e os testes usam). A tela mostra:

- o Mii ativo ("Você entra nas salas como …", com **Trocar Mii**/**Criar Mii agora**, que leva a Perfil Mii);
- **Criar sala**;
- convites recebidos (**Aceitar**/**Recusar**) e salas abertas de amigos (**Entrar**);
- a lista de amigos online da Steam, com **Convidar** quando a sala é sua;
- **Convidar pela Steam** (overlay) e **Encerrar sala**.

**Mii e nome:** criar ou entrar em sala (`/api/steam/host`, `/api/steam/join`, `/api/session/host-steam`) exige um Mii salvo. O nome do Mii passa a ser o nome da conta no jogo (`RECOMP_USER_NAME`, `username.json`), o que aparece na lista de salas e no lobby.

**Steam (`crates/session/src/steam.rs`):**

- Novas funções:
  - lista de amigos (`GetFriendCount/ByIndex/PersonaName/PersonaState/GamePlayed/RichPresence`);
  - `InviteUserToLobby`.
- A sala do anfitrião publica a rich presence `mk8_room=1`. Um amigo aparece em "salas de amigos" quando está no mesmo AppID, em um lobby e com essa marca.
- **Callbacks** registrados com `SteamAPI_RegisterCallback`, com objeto `CCallbackBase` montado em Rust (vtable na ordem do MSVC):
  - `GameLobbyJoinRequested_t` (333): convite aceito no chat ou no overlay da Steam;
  - `GameRichPresenceJoinRequested_t` (337): "Entrar no jogo" pela lista de amigos;
  - `LobbyInvite_t` (503): convite recebido, mostrado no launcher;
  - `P2PSessionRequest_t` (1202): aceita a sessão P2P do convidado. O código antigo só aceitava depois de ler um pacote, e um pacote de sessão não aceita não chega a ser lido;
  - `LobbyEnter_t` (504): diagnóstico de que os callbacks chegam.
- `SteamAPI_RunCallbacks` passa a ser chamado sob uma trava (salas e uma thread de despacho a cada 100 ms).
- Depois do primeiro uso da Steam no PC (`local/steam-enabled`), o launcher reconecta sozinho ao abrir, para receber convites.

**Fluxo:**

1. Ao criar ou entrar na sala, o launcher abre o jogo sozinho quando o transporte confirma a conexão.
2. Quem aceita um convite pela Steam com o launcher aberto entra direto: o launcher vem à frente, entra na sala e abre o jogo.
3. Erros desse caminho aparecem como aviso (`steam_error`).

**Entrada automática dentro do jogo (experimental):** `runtime/mk8/net/auto_lan.inc`, aplicado por `toolchain/auto_lan.py`.

- Liga com `MK8_AUTO_LAN=host|guest`, que o launcher define só para salas Steam (`game_session::Request.auto_lan`), ou com `1` (o slot 0 hospeda).
- Aperta os botões pela entrada do próprio jogo (`g_test_buttons`, somada à do jogador). Espera **marcos observados**, e não quadros fixos:
  - o vídeo do menu (`MovieMenu.mp4`) abrir;
  - a rede virtual (`vlan`) subir, o que mostra que o jogo entrou no Lobby LAN.
- Sequência: título → menu → L+R+analógico → Lobby.
- **Anfitrião:** aperta **Create Room** e transmite pela `vlan`, uma vez por segundo, um aviso numa porta que o jogo não usa (47999). O `vlan.c` consome esse aviso e nunca o entrega ao jogo.
- **Convidado:** só desce até a sala e aperta A **depois de receber o aviso**. Sem ele, nunca aperta A (numa lista vazia, A criaria uma sala).
- Qualquer espera esgotada encerra a sequência e o jogador continua à mão. Personagem e regras ficam com o jogador.
- Uma primeira tentativa usou "primeiro pacote unicast" como sinal de sala. O pacote chegou antes de a sala existir, então ela foi descartada.

**Evidência:**

- **Steam ao vivo, neste PC (conta do usuário):**
  - a Steam conectou pelo `steam_api64.dll` de outro jogo instalado e listou 89 amigos;
  - sala só para amigos criada e transporte do anfitrião conectado;
  - `LobbyEnter_t` entregue (`callbacks=true`), o que prova o mecanismo de callbacks;
  - sem Mii, a sala é recusada; com Mii, o nome vira o do Mii;
  - nenhum convite foi enviado.
- **Duas instâncias reais do jogo no mesmo PC** (transporte local; `scripts/test-multiplayer.py --env MK8_AUTO_LAN=1`):
  - `local/multiplayer-rf8o8bsu` (primeiro sinal) e `local/multiplayer-perc57wm` (aviso da sala);
  - os dois chegaram sozinhos ao Lobby LAN, e o anfitrião criou a sala e foi para a garagem;
  - o convidado recebeu o aviso, viu "Host 1/12", entrou e ficou na escolha de personagem dentro da sala (capturas 1800 e 2400 inspecionadas);
  - o "communication error" final é o anfitrião encerrando no fim do roteiro.

**Não validado:**

- o lado do convidado pela Steam (outra conta: `JoinLobby`, troca `MK8J`/`MK8I`, túnel P2P e o aceite pelo chat);
- o overlay da Steam sobre a janela do launcher;
- PCs mais lentos (os intervalos entre marcos são fixos);
- saves com outro estado de menu;
- corrida inteira entre duas máquinas.

**Limites:**

- Não há instalador público com `steam_api64.dll`: o launcher procura uma cópia no PC (variável, pasta de dados, pasta do launcher ou `steamapps/common`).
- O launcher aparece como "Spacewar" (AppID 480).
- Quem não estiver com o launcher aberto, ao aceitar um convite, vê a Steam tentar abrir o Spacewar.
- `gameplay_verified=false` e `multiplayer_verified=false`.

**Validação:**

- Formatação, Clippy `-D warnings` e todos os testes Rust, incluindo `steam_callbacks_become_join_and_invite_events` (eventos e ABI de `FriendGameInfo_t`/`CCallbackBase`).
- `tests/test_launcher_e2e.py`, `tests/test_launcher_languages.py` e `tests/test_mii_launcher.py`: 18 passaram e 1 pulou (relay não configurado). Os testes de transporte usam a API; a tela nova é verificada (sem Mii, recusa; com Mii, nome; só Steam).
- `tests/test_runtime_stalls.py` confere o patch da entrada automática na cópia preparada.
- `scripts/test-mii-composer.py`.
- Correção encontrada pelos testes: o criador não lia o corpo das respostas de erro, e a requisição ficava aberta no navegador.

## `fix-multiplayer` integrada à `fix-chore` — 12/11/2026

Merge de `fix-multiplayer` (salas P2P com código único `MK8P-…` + STUN, integração Steam `MK8S-…`, separação de pin TLS por host no Nextendo, abertura automática do OAuth). Único conflito: traduções em `apps/launcher/web/en.json`, mantidas as dos dois lados. Formatação, Clippy do workspace com `-D warnings` e todos os testes Rust passaram. `scripts/test-multiplayer.py` reúne `--p2p`/`--mii-profile` (multiplayer) e `--mii-resources`/`--host-profile`/`--guest-profile` (fix-chore). Evidência combinada (`local/multiplayer-5u3b89qo`, `--p2p --mii-resources`, 200,4 s): sala P2P por código único com **os dois Miis de corpo 3D animados na garagem**, retratos nos cartões de votação e corrida do anfitrião com o ícone Mii no minimapa. A tela "A communication error has occurred." do convidado em `1/frame-10000.png` foi registrada 7,6 s depois de o anfitrião encerrar após sua última captura (`0/frame-10000.png` às 19:28:28, `1/frame-10000.png` às 19:28:35): é efeito do encerramento do roteiro, como na execução P2P anterior, e não uma falha da sala. A cabeça estática (fases 4–6) continua bloqueada em salas; o caminho para Miis online é o renderer original com recursos gerados. Corrida completa entre dois PCs continua pendente.

## Salas com código único P2P + STUN e integração Steam (`fix-multiplayer`) — 10/11/2026

Implementado também em `crates/session/src/p2p.rs` e integrado a `crates/session/src/lib.rs`, `apps/launcher/src/server.rs` e interface web a **Opção 1 (Código único reutilizável `MK8P-…` + NAT Traversal via STUN)** ao lado da opção Steam (`MK8S-…`), mantendo ambas disponíveis para testes comparativos:

- **STUN RFC 5389 no socket do anfitrião (`p2p::stun_discover_on_socket`):** ao iniciar uma sala, o `Host` consulta servidores STUN públicos (`stun.cloudflare.com:3478`, `stun.l.google.com:19302`) pelo próprio socket UDP da sessão para descobrir seu `IP:porta` externo (`XOR-MAPPED-ADDRESS`) e manter o mapeamento NAT com keepalives a cada 20 s (desativável em testes herméticos com `MK8_DISABLE_STUN=1`).
- **Ticket único reutilizável (`MK8P-…`) e auto-alocação de slots (`MK8-JOIN-1`):** codifica os endpoints candidatos (endpoint explícito/alternativo, endpoint público STUN e IP LAN detectado) + `room_token` de 16 bytes. Até 11 convidados podem colar o **mesmo código `MK8P-…`** em **Entrar** (ou clicar diretamente na sala encontrada em **Buscar salas** na rede local); `p2p::join_with_ticket` dispara probes em paralelo, recebe seu `Invite` individual autenticado (`XChaCha20-Poly1305`) e inicia o `Gateway` no mesmo socket UDP perfurado.

## Integração experimental com Steam Lobbies e P2P Relay (`fix-multiplayer`) — 10/11/2026


Implementado em `crates/session/src/steam.rs` e integrado ao launcher (`apps/launcher/src/server.rs`, `apps/launcher/src/main.rs` e interface web) um adaptador dinâmico opcional para Steamworks (`ISteamMatchmaking` + `ISteamNetworking` P2P Relay), sem distribuir binários proprietários da Valve no repositório:

- **Carregamento dinâmico (`LoadLibraryExW`):** procura `steam_api64.dll` em `MK8_STEAM_DLL`, na pasta de dados (`local/steam_api64.dll`), ao lado do executável ou em instalações locais da Steam (`steamapps/common`), configurando `SteamAppId=480` (ou `MK8_STEAM_APPID`) para testes.
- **Código único `MK8S-…` e negociação automática de slots:** ao clicar em **Criar sala via Steam**, o anfitrião cria um lobby de até 12 jogadores na Steam e pré-aloca os 11 slots autenticados (`XChaCha20-Poly1305`) no `Host` local em `127.0.0.1:0`. Convidados que entram com o mesmo código `MK8S-…` ou via convite da Steam (`+connect_lobby <id>`) negociam automaticamente seu convite individual pelo canal de controle P2P (`P2P_CHANNEL_CONTROL`) e tunelam os pacotes `MMKDVL1` pelo canal de dados (`P2P_CHANNEL_DATA`) via Steam Datagram Relay, sem precisar de VPN, sem abrir a porta UDP `47820` no firewall e sem gerar convites individuais manualmente.
- **Convite via Steam Overlay:** o botão **Convidar amigos na Steam** aciona `SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog` e publica Rich Presence (`+connect_lobby`).
- **Validação executada:** formatação (`cargo fmt --all -- --check`), Clippy (`--workspace --all-targets -- -D warnings`), os 50 testes unitários Rust (incluindo roundtrip STUN RFC 5389, roundtrip `RoomTicket` `MK8P-…`, múltiplos convidados simultâneos com um único `RoomTicket`, roundtrip de `SteamCode` `MK8S-…` e teste determinístico de troca bidirecional de pacotes `MMKDVL1` sobre barramento P2P simulado) e os 18 testes de ponta a ponta em navegador (`test_launcher_e2e.py` e `test_launcher_languages.py`, com `mk8-relay` configurado, 0 skips) passaram.
- **Evidência real dentro do jogo (`local/multiplayer-p2r5om9x`):** executado `scripts/test-multiplayer.py --p2p` com duas instâncias reais do launcher e do MK8 conectadas exclusivamente pelo código único `MK8P-…` (312,8 s, saída 0, dados protegidos intactos em `protected-data.json`). As capturas confirmam: seleção de regras/kart no quadro 4000 (`0/frame-04000.png`, `1/frame-04000.png`), ambos os jogadores e votos de pista na garagem LAN no quadro 6600 (`0/frame-06600.png`, `1/frame-06600.png`) e corrida ativa em Mario Kart Stadium no quadro 9200 (`0/frame-09200.png` mostra Mario em 2º lugar vendo o kart e a tag `Guest` à frente na pista; `1/frame-09200.png` mostra o convidado em 1º lugar no momento em que o anfitrião encerrou a captura em 9200). Corrida completa entre dois PCs separados continua pendente de qualificação; `gameplay_verified` permanece `false`.

## Log do convidado confirma crash na votação LAN — 09/11/2026

`mk8drecomp.previous.log` enviado pelo convidado registra a ponte LAN no slot 1 (`10.13.37.2`) e, por volta de 30 s, `exception c0000005 accessing 0` em `f_700080f4a4+34c`, guest LR `700080f564`; quadros `70000d51fc → 700016e13c → 700016ee54 → 700016f100`. Registradores x8=0 e x22/x25=0xffffffff. A inspeção do consumidor confirma leitura da lista em +0x548 antes de desreferenciar o ponteiro selecionado. A assinatura coincide com o problema pendente de recursos/retratos no teste Nextendo; agora há evidência também no LAN, na votação informada pelo usuário. Isso confirma um crash do guest recompilado, mas não qualifica a estabilidade da VPN nem prova a ordem exata das mensagens de desconexão nos dois PCs.

`previous2.log` e `previous3.log` terminam com `window closed`, sem a mesma exceção; o terceiro inclui tentativa de Online Play sem serviço. O aviso repetido `0 member(s)` não comprova sala vazia: o gateway atual responde `MMKW\x00` ao handshake local, separadamente da contagem real de participantes do transporte. Correção da inicialização/consumo dos recursos visuais ainda pendente; nenhum ponteiro fictício, retorno de sucesso ou alteração de saves foi aplicado.

## Relatórios da desconexão na votação — 09/11/2026

Os relatórios locais 1794260543 (anfitrião) e 1794260549 (convidado), separados por seis segundos, registram respectivamente transporte conectado com dois participantes/jogo ativo e convidado desconectado com saída `0xc0000409`. Isso não determina a ordem entre a queda do transporte e o encerramento do jogo. Os hashes do runtime, entradas main/sdk/subsdk0 e identidade de conteúdo coincidem; os binários recompilados individualmente diferem, o que sozinho não comprova incompatibilidade. O único log histórico incluído pelo anfitrião termina com fechamento normal de janela e não demonstra a causa desta votação.

Falha confirmada na coleta: `game.log` está vazio nos dois ZIPs; o runtime Windows redireciona a saída para `mk8drecomp.log` ao lado do executável, mas o relatório só coletava stdout e históricos da pasta de dados. Corrigido em `apps/launcher/src/report.rs` e nos chamadores de relatório manual/automático: inclui somente os quatro nomes fixos do log do runtime e suas rotações, mantendo limite de tamanho e redação, sem copiar saves ou demais arquivos da build. Testes verificam o ZIP real, conteúdo do log com stdout vazio e exclusão de arquivo privado adjacente; dois testes passaram, Clippy sem avisos. Esta alteração ainda não está no setup VPN entregue anteriormente. Para resolver o encerramento durante a votação, falta o `mk8drecomp.log` do convidado (e rotações caso tenha reiniciado o jogo); não foi atribuído a firewall, Mii ou VPN sem evidência.

## Teste local de salas LAN/VPN — 09/11/2026

Nextendo pausado a pedido do mantenedor. A interface diferencia a conexão do anfitrião dos convidados, confirma a aceitação pelo transporte sem afirmar entrada no lobby e orienta após 20 segundos sem confirmação. O campo de endereço alternativo pertence ao anfitrião: é mantido ao recolher o painel e sua edição invalida o convite exibido. Novos convites devem conter o IP Radmin do anfitrião, com UDP 47820 permitido. Wireless Play/LDN segue não implementado; o caminho de teste é Jogar na sala → LAN Play (L + R + clique no analógico esquerdo no menu principal).

A preparação do runtime substitui a orientação herdada de Course World/TGRCode por instruções LAN/VPN, preservando o código de erro e a verificação de limites da adaptação. Nenhum retorno de rede foi convertido em sucesso. Teste de navegador com dois launchers e UDP real confirmou aceitação, contagem de um convidado, convite com endereço explícito mesmo com painel recolhido e orientação de destino sem resposta. Idiomas e limites das adaptações passaram. Setup local solicitado para teste em duas máquinas; corrida entre PCs continua não qualificada e `gameplay_verified` permanece falso. Sem publicação no GitHub. Instalador local concluído em `target/dist/vpn-test-20261109/MK8-online-0.1.8-vpn-test-setup.exe` (55.697.920 bytes; SHA-256 `7b884652fcff06b15c398d6cd6aea2e9483b42c4007ff32ab6b3d70332c67a79`), com pacote de compartilhamento `target/dist/MK8-online-0.1.8-vpn-test.zip` incluindo fontes/avisos FFmpeg e instruções. A versão continua 0.1.8; a mensagem interna nova exige preparar a build novamente, mas os ajustes do launcher não. Build release e runtime pré-compilado concluídos; 15 testes E2E passaram (relay não configurado, 1 skip), 2 de idiomas, 2 de adaptação, 3 de transporte nativo e 4 do instalador passaram. A conferência dos 12 arquivos privados protegidos não encontrou alterações; o payload de 916 arquivos não inclui caminhos privados de entrada.

