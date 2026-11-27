# Registro técnico — Mii

[Índice](README.md) · [English](../../en/log/mii.md)

## Criação do primeiro Mii bloqueada quando a prévia 3D falha (issue #149) — 26/11/2026

O relato na versão 0.6.3 mostra o botão **Criar e personalizar** desabilitado no launcher em espanhol após digitar um nome. O relatório não inclui log do navegador. Bloquear o WebGL no Chromium reproduziu o botão desabilitado em um perfil novo na `fix-chore` em `5603bfb`. O editor inicializava o renderer WebGL antes de carregar o perfil, e uma falha do renderer deixava o formulário desabilitado.

O editor agora carrega os perfis antes de iniciar a prévia. Se o renderer falhar, os controles numerados existentes ainda permitem criar, editar e salvar. A regressão no navegador falha na base e passa com o launcher recompilado. Um ensaio separado com WebGL e os recursos preparados renderizou a cabeça, o corpo e uma miniatura de rosto. A máquina do relator não estava disponível para reteste. O ensaio não executou o jogo.

## Launcher congelado no criador Mii (issues #15 e #16) — 14/11/2026

O servidor HTTP do launcher atende uma requisição por vez. O criador pedia a biblioteca AFL (4,5 MB) e entregava a resposta ainda não lida a `FFL.initWithResource`, que primeiro espera o módulo baixar `ffl-emscripten.wasm` desse mesmo servidor. Quando o corpo passava dos buffers do WebView/TCP, o laço ficava preso escrevendo a biblioteca e nunca atendia o `.wasm`: a tela ficava em "Carregando peças do Mii…" e **todas** as outras chamadas paravam (Procurar, Jogar, status). Isso explica os dois relatos, inclusive "Jogar solo não faz nada" depois de abrir o criador. Dependia do tamanho dos buffers de cada máquina; aqui os testes do criador passavam.

Correção: respostas acima de 32 KiB são escritas em thread própria (`binary_response` em `apps/launcher/src/server.rs`), e o criador lê a AFL inteira antes de iniciar o módulo FFL. Reprodução com o `Launcher.exe` real e a AFL verificada: com a resposta da biblioteca sem leitura, o pedido do `.wasm` esperou 5 s sem resposta no binário anterior; no corrigido saiu na hora. O novo `tests/test_launcher_unread_response.py` faz o mesmo com um recurso público grande (`three.core.js`), sem arquivos privados: falha no launcher anterior e passa no novo.

Validação: formatação, Clippy com `-D warnings` e 66 testes Rust; navegador com o launcher release: e2e (21, 1 pulado por relay), idiomas (3), Mii (1) e o teste novo; `scripts/test-mii-composer.py` com a AFL privada passou (biblioteca, miniaturas, corpo, salvamento e recursos do jogo). Sem novo setup nem release.

## Espera Mii e controle pelo receptor USB — 13/11/2026

Na instalação reportada, a geração Mii terminou em aproximadamente 4,3 s e os recursos estavam ativados. O aviso global de aguardar conversão permanecia depois de uma tentativa antecipada de jogar. O status geral agora acompanha/finaliza os jobs Mii; a tela Jogar informa preparação/conclusão/falha, desabilita os botões durante o trabalho e substitui somente o aviso antigo correspondente. O criador também marca preparação antes do primeiro await para evitar duas solicitações simultâneas. Recursos existentes continuam reutilizados; não foi aplicada nem alegada aceleração do conversor.

O GameSir T4n Lite com receptor USB era reconhecido pela SDL3 e enviava botões/gatilho em prova isolada, mas não aparecia no log do jogo iniciado pelo launcher. O launcher esvazia `SDL_GAMECONTROLLER_IGNORE_DEVICES` e `SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT`, preservando mapeamento e salas Steam. A confirmação anterior do usuário no menu não validava a inicialização da API Steam: com o SDK conectado, a falha voltou mesmo com os filtros vazios. A instalação reportada tinha o hash do setup esperado, descartando pacote antigo. Uma prova C nativa iniciada pelo launcher após conectar a Steam retornou XInput 1167 e zero gamepads SDL; fora desse contexto, o mesmo controle era detectado.

A interface Steam confirmou **Ativar a Entrada Steam** forçado para Spacewar (AppID 480). Depois de o usuário selecionar **Desativar a Entrada Steam** somente para esse jogo, a mesma prova com SDK conectado retornou XInput 0 em duas DLLs e abriu `XInput Controller #1` (VID 3537, PID 1040) via SDL3. O resultado se repetiu com inicialização adicional do SDK no filho. Reabrir somente o jogo na sessão antiga do launcher não bastou. Após fechar ambos e abrir uma sessão nova, conectando a API Steam antes de iniciar o MK8, o log registrou o controle e o usuário confirmou L + R/analógico funcionando com a Steam aberta. Trata-se de configuração por jogo e reinício do launcher, não de correção automática; a interface explica esses passos em Multiplayer → Como funciona, nos dois idiomas. Não houve mudança de runtime/ABI, driver ou preferências globais da Steam. Os testes de remoção de variáveis Steam do filho foram descartados por não resolverem a falha. Salas/corrida multiplayer em duas máquinas continuam sem qualificação.

Validação: 38 testes Rust, formatação e Clippy aprovados; navegador com 16 testes aprovados e um pulado, incluindo preparação/conclusão/falha e tradução do aviso; criador com AFL real, miniaturas, corpo, salvamento, conversão e conclusão acompanhada pelo status geral passou. Três testes de processos também passaram com o launcher empacotado: o processo filho recebe os filtros vazios e mantém o mapeamento personalizado, preserva saves e mantém os bloqueios de uso enquanto necessário. A confirmação do controle é de navegação no menu, não uma corrida completa. `gameplay_verified` e `multiplayer_verified` continuam falsos.

**Setup 0.2.0 aprovado e publicado:** 57.975.296 bytes, SHA-256 `356381687927b3c93a83ee6b91bb4a6c5e22876cb23d48bb5d39f3c9efa314d3`, em `target/dist/MK8-online-0.2.0-setup.exe` do Cargo configurado. Substitui o pacote `f792f663…`, incluindo a orientação de Entrada Steam em português e inglês. Build Rust de distribuição concluída; runtime e pipeline inalterados foram reutilizados com hashes conferidos. Payload com 935 arquivos, sem caminhos pessoais/arquivos privados detectados; 23 arquivos pessoais acompanhados preservados. Título e descrição anteriores mantidos, mudando apenas o SHA-256 gerado do setup. No pacote final, os três testes de processos e 16 testes de navegador passaram (um pulado); orientação de controle visível nos dois idiomas, sem erros JavaScript e com captura revisada. A configuração de Entrada Steam permanece uma escolha por jogo na Steam, não é alterada pelo instalador.

## Biblioteca Mii automática — 13/11/2026, fix-chore

Ao abrir **Perfil Mii** sem uma biblioteca válida, o launcher baixa a cópia do antigo CDN Miitomo no Internet Archive (origem explicitada na interface). Usa HTTPS, limites de tamanho/tempo e SHA-256 fixo do ZIP antes de extrair só o AFL esperado; verifica novamente o tamanho e hash do AFL antes de publicar o arquivo e a configuração. Não executa conteúdo baixado. `mii-library/AFLResHigh_2_3.dat` fica na pasta privada de dados, com reutilização offline. Biblioteca já configurada é preservada; uma seleção manual feita durante o download tem precedência. Há nova tentativa e seleção local quando a rede falha. Nenhum recurso foi incorporado ao GitHub ou ao instalador.

Validação: build Rust debug concluída, `cargo fmt`, Clippy com `-D warnings` e 37 testes Rust aprovados. Suítes de navegador: 18 aprovados e 1 pulado (relay não configurado). `scripts/test-mii-library-download.py` exercitou o download real, tamanho/hash, autenticação, ausência de download antes de abrir o criador, falha real de conexão/retry, limpeza de temporários, tradução e cache após reinício sem acesso HTTPS, inclusive recriação da configuração a partir do cache. `scripts/test-mii-composer.py` passou com seleção manual, miniaturas reais, corpo do jogo por gênero/altura/cor, salvamento, geração dos recursos do jogo e recarga. Capturas privadas em `local/mii-composer-check/`.

Alteração local posterior à release pública 0.2.0. A pedido do usuário, foi gerado **novo setup de distribuição 0.2.0 para teste antes de commit/merge/republicação**, usando `scripts/release.py --tag v0.2.0`, runtime recompilado (213 unidades) e launcher Rust otimizado. `MK8-online-0.2.0-setup.exe`: 57.948.672 bytes, SHA-256 `1d6c33d85734f27aa97d3845364b84cb0b03a44dae291648102635efb3e85ce1`. O pacote tem 935 arquivos; fontes atuais e hashes do host/FFmpeg conferidos, sem recursos privados ou caminhos pessoais detectados, e 23 arquivos pessoais acompanhados preservados. Download real/cache offline e os três testes de criação/edição e idiomas passaram novamente usando o launcher empacotado. Título e descrição anteriores preservados byte a byte, exceto o SHA-256 gerado do setup. Evidências em `local/rebuild-020-build.log` e `local/release-artifacts-0.2.0.json`; artefatos em `target/dist/` do Cargo configurado. Nenhum commit, merge, push ou publicação nesta etapa; aguarda teste do usuário.

Disponibilidade futura do Internet Archive não é garantida. Fontes de runtime/guest não alteradas e nenhuma nova corrida executada; `gameplay_verified` e `multiplayer_verified` permanecem falsos.

## Criador de Mii no launcher (tela Perfil Mii) — 12/11/2026

A tela **Perfil Mii** foi refeita como **criador**. Saíram:

- a prévia procedural aproximada;
- os formulários antigos;
- importar/exportar `.charinfo`;
- o inspetor de GLB;
- o modal 3D;
- os cartões "Modelos dentro do jogo" e "Cabeça no jogo".

As APIs desses recursos continuam no servidor. Ficaram na tela:

- a lista de Miis salvos e **+ Novo Mii**;
- o palco 3D (**Corpo inteiro**/**Rosto**), com nome e **Salvar Mii**;
- as abas Corpo, Rosto, Cabelo, Olhos, Sobrancelhas, Nariz, Boca, Barba, Óculos e Pinta.

**Como as opções aparecem:**

- Cada estilo e cada cor é um **botão com a miniatura do próprio Mii com aquela opção**, desenhada pelo FFL a partir da biblioteca AFL local. Há enquadramentos próprios para cabeça, rosto, olhos, nariz e boca, e a opção atual fica marcada.
- Tom de pele e cor favorita usam as cores do próprio FFL (`FFLGetFacelineColor`/`FFLGetFavoriteColor`).
- Tipo e inverter usam botões de texto.
- Tamanhos e posições usam controle deslizante com −/+.
- Os limites vêm de `/api/mii/fields`. Campos administrativos (região da fonte, tipo, região de origem) são preservados sem aparecer.

**Fluxo:**

1. O usuário nomeia o Mii e usa **Criar Mii**. O perfil é criado em `local/mii-profile.json` e `local/profiles/<id>/`.
2. A tela abre a aba Rosto. As edições ficam pendentes até **Salvar Mii** (`/api/mii/data`, que também grava o nome).
3. Trocar de Mii com alterações pendentes pede confirmação.
4. A biblioteca AFL é pedida uma única vez, com seletor nativo de arquivos `.dat`. Sem ela, as opções aparecem numeradas e continuam funcionando.

**Corpo:** é o do próprio MK8, `romfs/UI3D/MiiBodyMiddle/MiiBodyMiddle.bfres` (o corpo das salas e do MKTV, com MaleBody e FemaleBody). `crates/installer/src/mii_body.rs` o lê **no lugar**, a partir do jogo preparado, por `GET /api/mii/body`, e devolve um GLB em memória; nada derivado é gravado.

- Só o arquivo verificado é aceito (SHA-256 `d7bc2730…7d93208`, BFRES 0.5.0.3).
- São conferidos deslocamentos, quantidades, formatos de atributo (posição float, normal 10:10:10:2, UV 16:16), índices, o esqueleto e a textura `Mii_Alb` (BC3 sRGB 384², com deswizzle Tegra e decodificação BC3 próprios).
- O esqueleto precisa ter escala 1 e rotação nula em todos os ossos; o cachecol rígido é posicionado pelo osso `chest`.
- Na prévia, o tipo escolhe o corpo, e altura/porte aplicam a escala do SDK (`getBodyScale` do FFL, a mesma fórmula de `nn::mii`).
- O traje é tingido com a cor favorita, como no jogo, e a cabeça FFL é presa ao osso `head` (escala 0,1).
- O corpo aparece em pose de referência (braços abertos), porque a prévia não aplica animação.

**Ida para o jogo:**

- Ao criar/salvar um Mii, ou ao escolher a biblioteca, o launcher gera e ativa sozinho os recursos MiiModel (`mii_resources`), se ainda não estiverem ativos.
- Antes disso, desativa a cabeça estática convertida (fase 4–6), que conflita com o renderer original.
- O perfil ativo continua indo ao jogo na abertura por `MK8_MII_CHARINFO`.
- **Perfil Mii** no menu F1 do runtime (e o applet de editor Mii) traz esta tela à frente.

**Lobby com Mii padrão — hipótese descartada:** com teste nativo e duas instâncias LAN (`local/multiplayer-hfnd4lil`), um patch que gravava o CreateId do perfil em `UserData.miiID` (`IProfile::Get`) não teve efeito: o MK8 não chama `IProfile::Get` com buffer nesse fluxo. O patch foi removido e o runtime recompilado sem ele.

Com `MK8_TRACE_MII=1` (`local/multiplayer-r4oj5h08`) aparecem:

- `Get` com fontes 1 e 2 na inicialização;
- **13 `BuildRandom`** (cmd 6);
- `IsFullDatabase` repetidas.

O `BuildRandom` do upstream devolve sempre o mesmo Mii padrão, e as 13 chamadas coincidem com as 12+1 instâncias Mii da interface de salas. O próximo passo é localizar no código guest o consumidor desses resultados. Devolver o perfil em `BuildRandom` seria um resultado fictício e não foi feito.

**Validação:**

- Formatação e Clippy do workspace com `-D warnings`.
- Todos os testes Rust, com três novos em `mii_body`: decodificação BC3/normal/deswizzle/PNG, recusa de arquivo diferente, e leitura do arquivo verificado quando presente (contagens 808/678/175, osso `head` e textura).
- `tests/test_mii_launcher.py`, reescrito: sem biblioteca e sem jogo, cria, recusa nome vazio, edita rosto/corpo/boca pelos botões, salva, cria segundo Mii, alterna, confirma descarte, recarrega, largura de 780 px e inglês.
- `tests/test_launcher_languages.py`.
- `scripts/test-mii-composer.py`, reescrito: com biblioteca e jogo preparado, desenha as 132 miniaturas de penteado, troca olhos, corpo feminino, altura 127 e cor 5; salva, gera recursos, confere o GLB do corpo e a recusa sem token, recarrega e não faz requisições externas. Capturas em `local/mii-composer-check/creator*.png`, inspecionadas.
- `scripts/test-mii-native-launcher.py` e `scripts/test-mii-resources-launcher.py` passaram a usar a API ou o novo fluxo no lugar dos botões removidos.

`gameplay_verified=false` e `multiplayer_verified=false`.

## Renderer Mii original com recursos derivados do AFL — 11/11/2026

`toolchain/mii_resource_nx.py` gera, a partir do AFL local verificado (`AFLResHigh_2_3.dat`, SHA-256 `4a4be71d…7722`), os seis arquivos no layout MiiModel lido pelo MK8 4.0.1: `ShapeMid/High.dat` e `NXTexture{Low,Mid}{Linear,SRGB}.dat`. Os arquivos derivados ficam privados (`local/mii-resource-nx-v2`), como o AFL. Com eles, `scripts/test-race.py --mii-resources <pasta>` define `MK8_EXPERIMENTAL_NO_MII=0` e `MK8_MII_RESOURCE_DIR`, ativando o **renderer Mii original do jogo**, sem objeto fictício e sem mudança no código guest.

**Conversão:**

- **Formas:** cabeçalho NX de 0x6c (`maxSize[12]` em +0x0c, `maxAlignment[12]` em +0x3c) e entradas de 0x58, já descritas abaixo. Posição float4 BE → 4× meia precisão; UV float2 → 2× meia precisão; normal 10:10:10:2, tangente e cor copiadas na ordem little-endian; índices u16 (no AFL o elemento 5 guarda a quantidade, não bytes). Transformações em tabelas separadas: cabelo normal 0x48 em 0x12704 e rosto 0x24 em 0x14c24. O dado começa em 0x14dd4.
- **Texturas:** o AFL guarda nível 0 **linear** (conferido visualmente) com rodapé BE. O nível 0 é convertido para block-linear Tegra com a mesma altura de bloco padrão e o mesmo endereçamento de GOB de `upstream/.../nvn/tegra.c`. Por enquanto há um único nível de mipmap, porque o layout de mipmaps do AFL ainda não foi decodificado.
- **Mapeamento:** tipos de textura NX `[chapéu, olho 62, sobrancelha 24, barba 2 (AFL 1–2), rosto, maquiagem, óculos, pinta, boca 37, bigode, linha do nariz]`. Os tipos de forma 0–2 e 9 foram confirmados pelo código (`GetBeardType`, `GetFacelineType`, `GetHairType`). O agrupamento chapéu/testa/cabelo de 3–8 e nariz/linha do nariz de 10–11 foi inferido; o resultado visual abaixo é compatível.
- O conversor valida hash, limites, quantidades, índices e cabeçalhos antes de escrever, recusa saída existente e mantém a reserva de 4 GiB. São seis testes com dados próprios (`tests/test_mii_resource_nx.py`): swizzle conferido contra o endereçamento GOB do runtime, ida e volta, meia precisão, deslocamentos das tabelas, recusas e hash.

**Evidência no jogo (inspecionada):**

- `local/race-time-trial-xfzz9flt` (perfil feminino C): saída 0. `frame-02100.png` mostra o Mii no menu de kart com rosto completo (olhos, sobrancelhas, boca e nariz) e o **capacete original**; `frame-03300.png` mostra o mesmo na apresentação da pista. `game.log`: `system data 0100000000000802: …mii-resource-nx-v2`, sem exceções.
- `local/race-time-trial-1rjufkt1` (perfil A): saída 0. `frame-01300.png` mostra a **grade de seleção Mii com retratos** dos 7 Miis (o perfil A com cabelo longo e pele mais escura, e os seis padrões); `frame-01550.png` mostra o menu "Mii Suits" com Miis 3D em cada traje; `frame-01800/02050.png`, o piloto com expressão animada.
- Correção de uma afirmação anterior: no MK8 original, o Mii com Normal Suit **usa capacete**. A bola azul no chão era esse modelo desenhado fora do lugar com o renderer desativado. Com os recursos gerados, ele aparece na cabeça.

**Geração pelo launcher (11/11/2026):** o cartão **Modelos dentro do jogo** ganhou o botão **Gerar a partir da biblioteca local**. `crates/installer/src/mii_resources.rs` (`generate`/`finish`) confere de novo o AFL configurado no editor (tamanho e SHA-256), executa `toolchain/mii_resource_nx.py` sob o supervisor existente (bloqueios de ferramentas; cancelamento ao fechar o launcher) em uma pasta nova `<dados>/mii-resources-generated/<trabalho>/files`, com `generation.log`, e só ativa os recursos depois de validar o manifesto (esquema, hash do AFL, `rendering_verified=false`, hash de cada um dos seis arquivos) e de passar pela mesma validação de `configure`. Rotas: `POST /api/mii/resources/generate` e `GET /api/mii/resources/job`; **Jogar** espera a geração terminar. Interface em PT/EN. Novo teste Rust: manifesto de outro AFL, manifesto incompleto e arquivo alterado são recusados sem ativar nada. `scripts/test-mii-resources-launcher.py` passou (`local/mii-resources-launcher-o6x17ct3`): sem AFL, o botão informa o requisito e nada é ativado; com AFL, gera e ativa pela interface; o jogo aberto por **Jogar solo** mostra os retratos na grade Mii e o Mii com rosto e capacete no menu de kart (`frame-01300/02100.png`); uma segunda geração cria outra pasta e mantém a anterior. O `game.log` do launcher fica vazio para o runtime reexecutado, por isso as capturas são a evidência. Validação: formatação, Clippy, 33 testes Rust, `test_launcher_languages.py` e `test_mii_glb.py`. `tests/test_mii_launcher.py`, que procurava "Detalhes completos do Switch" (texto removido no commit `9fd7b3a`), agora abre o editor 3D, edita/salva os campos Switch sem AFL e fecha o modal antes de exportar; voltou a passar.

**Sala LAN com duas instâncias (11/11/2026):** `scripts/test-multiplayer.py --mii-resources local/mii-resource-nx-v2 --host-profile … --guest-profile …` configura a opção **Modelos dentro do jogo** de cada launcher pela própria API (`POST /api/mii/resources`) e copia um perfil diferente para cada jogador. A primeira tentativa parou com `svcBreak` ("Mii data … missing"): o launcher passava a pasta canonizada com prefixo verbatim do Windows (`\\?\`), que rejeita o separador `/` acrescentado pelo runtime. Isso foi corrigido em `crates/installer/src/mii_resources.rs` com `runtime_path` (compartilhado com `mii_native.rs`), e o teste agora confere o caminho com barra. Nova execução (`local/multiplayer-ve35ansp`): as duas capturas finais foram alcançadas em 197,7 s, sem exceções nos logs. Inspecionado: **garagem da sala com os dois Miis de corpo 3D** (Normal Suit) e rostos; retratos Mii na lista do lobby e nos cartões de votação; votação de pista sem o crash anterior em `f_700080f4a4`; o convidado corre como Mii com capacete original (introdução "1st Race" e pista). **Pendente:** os Miis da garagem parecem padrão (cabelo curto) e não os perfis A/C; o lobby provavelmente usa o Mii da conta, e não `mii-profile.json`. Também falta repetir em dois PCs separados.

**Mapa das áreas Mii do jogo (11/11/2026)**, a partir das strings e dos consumidores no executável 4.0.1:

- **Validadas com o renderer original e os recursos gerados:**
  - seleção de Mii com retratos (`MiiSele`);
  - Mii Suits (`MiiSuit`);
  - piloto no menu, na apresentação e na corrida (`f_70000d4ffc` em `main_026.c`: `Driver`/`Head`/`Helmet`/`FaceAnim`);
  - garagem da sala LAN com corpos 3D, mais retratos na lista e nos cartões de votação (`MiiWiFiMatchingHeap`, fila `main_084.c`);
  - **ícone com rosto do Mii no minimapa** da corrida (`MiiMapHeap`; `local/multiplayer-ve35ansp/1/frame-09150.png` e `frame-10000.png`).
  - **Batalha com Mii** (Balloon Battle, Battle Stadium/Wuhu, rodadas de 1 minuto, 4 rodadas, 11 CPUs): o roteiro escolhe Batalha, o Mii e Normal Suit e reduz o tempo da rodada (duas vezes `LEFT`: 3 → 2 → 1 minuto; as opções circulam). `local/race-time-trial-_x9uojkx` (475 s) e `local/race-time-trial-gjavz_tz` (529 s) terminaram com saída 0 e sem exceções. Inspecionado: **placar de cada rodada** com retrato do Mii na linha "Player" e o Mii 3D no kart; ícone com rosto no minimapa da arena; **resultado final** com câmera orbitando o Mii 3D, expressão de derrota (12º lugar) e classificação acumulada com retrato. O resultado final espera `A`. Em seguida, o menu oferece "View Results" / "View Highlight Reel" (`local/race-time-trial-3x8qk979`).
  - **MKTV (Highlight Reel)**: com `A` no resultado final e depois `DOWN` + `A` (`local/race-time-trial-dzqhx8k1`, 546 s, saída 0, sem exceções), o replay da batalha mostra **closes do Mii 3D** (rosto, capacete, Normal Suit) em vários ângulos, com a legenda "9th / Player" e os controles do MKTV (Slo-mo, Menu).
- **Ainda não alcançadas pelos roteiros:**
  - lista de fantasmas com ícone do autor (`GhostScrollList`, `P_GhostMiiDummy_00`, `UIHeap_MiiIconAuthor`): o `[input]` do `game.log` confirma que `DOWN`, `STICK_DOWN` e `A` chegam ao jogo. Com a sequência completa de copa/pista, o `A` abre "Start Time Trial?" (`local/race-time-trial-rdrlazjk`), mas o cursor nunca sai de "Solo Race". O save de teste não tem fantasma do curso, e as opções online exigem servidor; para ter um fantasma é preciso concluir um contrarrelógio. Nessa mesma execução, o ícone com rosto do Mii também aparece no minimapa do contrarrelógio. Observação para roteiros: a confirmação de copa (2740) e de pista (2920) precisa permanecer antes de qualquer navegação no menu;
  - telas MKTV de edição/salvamento (`Theater_Remake`, `P_MKTV_Mii_00`) e resultados de corrida (e não de batalha): ainda não percorridas. A lista de fantasmas exige um contrarrelógio concluído;
  - telas online (`WiFi_Mii`, `WiFi_MiiComment`, `MiiFriendCloseupHeap`): exigem servidor.
- `main_108.c` (`f_70004c5664`, `mn_Background_00`) e `main_113.c` (confirmação de Mii) pedem retratos pelo mesmo `f_70008104d4` já exercitado pela fila do lobby.

**Limites:** um único nível de mipmap; texturas Low/Mid e Linear/SRGB idênticas; nenhuma comparação com recursos MiiModel reais; a escolha de olhos/sobrancelhas/bocas entre os primeiros N do AFL não foi comparada peça a peça. Ainda falta testar salas LAN/online, MKTV, corrida completa, reinício/save e o fluxo do launcher. A opção existente **Modelos dentro do jogo** aceita a pasta, mas o seu teste de ponta a ponta com esta pasta não foi executado. A sobreposição estática de cabeça (fases 4–6) é desnecessária com o renderer ativo e entra em conflito com ele. `gameplay_verified=false` e `multiplayer_verified=false`.


## Mii feminino nativo — fase 6, 11/11/2026

A conversão nativa agora aceita Miis **femininos**. O gênero do CharInfo (byte `0x28`) escolhe o corpo verificado: `MiiM.bfres` (SHA-256 `d2d4f099…568a`) para masculino e `MiiF.bfres` (SHA-256 `8fa6b46908450bb05d24a2a4607fdd205e51ee00f956abd33ea80368b5a4dfc8`) para feminino. Uma sonda BfresLibrary confirmou que os dois têm os mesmos modelos (`Helmet`, `Mii`, `MiiFace`), 29 ossos, `Head` no índice 10 e transformações idênticas na cadeia `Skl_Root → Spine1 → Spine2 → Head`. Por isso a transformação calibrada `(x,y,z) → (y,z,x) * 0,1` vale para os dois.

**Achado em jogo:** com `MK8_EXPERIMENTAL_NO_MII=1`, o jogo carrega `Driver/MiiM.szs` para todo Mii, mesmo com um perfil feminino. Prova: uma sobreposição só com `MiiF.szs` (`local/race-time-trial-qz0_nyvm`) não mostrou cabeça; uma sobreposição com `MiiM.szs` da cabeça A e `MiiF.szs` da cabeça C, usando o perfil feminino (`local/race-time-trial-neb2uo1t`), mostrou a cabeça **A**. Assim, o corpo feminino com a cabeça convertida é servido com o nome `romfs/Driver/MiiM.szs`. O manifesto registra `body` (`MiiM`/`MiiF`) e `overlay_file="MiiM.szs"`. Um arquivo `MiiF.szs` ativo é recusado pelo launcher, pois o jogo nunca o lê.

- `toolchain/mii_head_builder.cs` aceita os dois originais verificados por hash e informa o corpo usado. O executável local foi recompilado; o anterior foi conservado como `local/mii-native-probe/MiiHeadBuilder-phase5.exe`.
- `toolchain/mii_native_head.py` escolhe o corpo pelo gênero e recusa, antes de criar qualquer saída, um original que não corresponda ao gênero. Gênero diferente de 0/1 é recusado.
- `crates/installer/src/mii_native.rs`: novo campo opcional `original_female` em `mii-native-tools.json` (padrão `local/mii-native-probe/MiiF.bfres`). Configurações antigas continuam válidas, apenas sem suporte feminino. `status` informa `body` e considera as ferramentas prontas conforme o corpo do perfil selecionado. `publish` confere `body`, `overlay_file` e o hash do original correspondente.
- O texto do editor (PT/EN) agora informa suporte a masculino e feminino.

**Evidência nativa inspecionada:**

- `local/race-time-trial-_kwzk43q`: perfil feminino C (cabelo 12, pele 1), corpo MiiF servido como `MiiM.szs`, saída 0 em 96,4 s, aproximadamente 60 FPS. `frame-02000.png` mostra a cabeça no menu de kart; `frame-04800.png` mostra a cabeça na pista.
- `local/mii-native-launcher-cretj_th`: `scripts/test-mii-native-launcher.py --glb local/mii-composer-check/profile-C.glb`. Conversão pelo botão do editor, abertura real por **Jogar solo**, saída 0, cabeça visível em `frame-03400.png`, além das recusas de modelo desatualizado, troca de perfil e peça não suportada.
- `scripts/test-mii-composer.py` agora cria também o perfil feminino `Editor C` e exporta `profile-C.glb`.

**Capacete/rosto no chão (corrigido):** a bola azul ao lado da roda traseira, vista na apresentação antes da largada, era o modelo `Helmet` do BFRES. Atrás dela havia uma forma preta, que era o modelo `MiiFace` (texturas `Mii_Dummy`). Os dois são definidos no espaço do osso `Head`, mas, com o renderer Mii desativado, o jogo os desenha na origem do kart, meio enterrados no asfalto. Isso ocorre também **sem sobreposição** (`local/race-time-trial-whybwj8w`). Diagnóstico: recolher somente `Helmet` deixou a forma preta (`local/race-time-trial-79ql9gdk`); recolher `Helmet` e `MiiFace` mostrou a lateral real do Standard Kart (`local/race-time-trial-1g0gt0_2`). O construtor agora recolhe para um ponto os vértices desses dois modelos, mantendo modelos e materiais para as consultas do jogo, e confere o resultado salvo. Evidência com o construtor `231a5bb5…`: feminino `local/race-time-trial-gfr9k06t` e masculino `local/race-time-trial-rf5baylc` (`frame-03300.png` lateral, `frame-04800.png` pista), além de `scripts/test-mii-native-launcher.py --glb …/profile-C.glb` aprovado novamente. Pelo que se conhece do jogo original, o Mii com Normal Suit corre sem capacete; capacetes aparecem nos trajes especiais, que não são tratados aqui.

**Validação:** 13 testes Python do conversor/composição, testes Rust de installer/launcher (32), Clippy `-D warnings`, formatação, `test_mii_glb.py`, `test_launcher_languages.py` e os roteiros de navegador acima passaram.

**Limites:** continua sendo uma substituição estática do único recurso que o jogo carrega. Todos os pilotos Mii da sessão recebem o mesmo corpo e a mesma cabeça; isso também vale para o corpo feminino de um perfil feminino. Carecas, barba/óculos, expressões, retratos, cabeças por jogador, multiplayer com cabeça ativa e corrida completa continuam pendentes. `gameplay_verified=false` e `multiplayer_verified=false`. A forma do corpo feminino é a do recurso original MiiF; não houve comparação lado a lado com o renderer Mii original.


## Provedor substituto de recursos Mii — mapeamento, 11/11/2026

**Estratégia escolhida:** não fabricar o objeto de modelo do SDK. Em vez disso, gerar **arquivos de recurso reais** no formato MiiModel do Switch (`ShapeMid/High.dat`, `NXTexture{Low,Mid}{Linear,SRGB}.dat`) a partir do AFL local já verificado (`AFLResHigh_2_3.dat`), e servi-los pelo caminho existente `MK8_MII_RESOURCE_DIR` com `MK8_EXPERIMENTAL_NO_MII=0`. Assim, o renderer Mii original do jogo, com código e ABI inalterados, montaria rostos, corpos, retratos, salas e MKTV para cada jogador. Os arquivos derivados continuam privados, como o AFL.

**Evidência no executável 4.0.1** (NSO descomprimido localmente; endereços de referência, não ABI qualificada):

- A biblioteca Mii está ligada estaticamente ao `main`. Montagem `0x0100000000000802` (`f_70008e1e8c`); tabela de nomes em `0x70012d6038` (`MiiResource:/NXTexture…`, `ShapeMid/High.dat`); leitura integral por `f_70008e1fb0`/`f_70008e201c`; inicialização `f_70008e1e08` → `f_70008ea6e0`, que só guarda os buffers. Nenhuma verificação de assinatura foi vista no carregamento.
- **Formas** (`f_70008ea9d4`): 12 tipos, entradas de **0x58** bytes, na ordem FFL e com contagens `[4 barba, 12 rosto, 12 máscara, 132 chapéu normal, 132 chapéu cap, 132 testa normal, 132 testa cap, 132 cabelo normal, 132 cabelo cap, 1 óculos, 18 nariz, 18 linha do nariz]`. Tabelas começam em 0x6c, 0x1cc, 0x5ec, 0xa0c, 0x376c, 0x64cc, 0x922c, 0xbf8c, 0xecec, 0x11a4c, 0x11aa4 e 0x120d4. Há dois vetores `u32[12]` no cabeçalho, em +0x0c e +0x3c.
- **Texturas** (`f_70008eabc8`): 11 tipos, entradas de **0x1c** bytes, contagens `[132, 62 olhos, 24 sobrancelhas, 2, 12 rosto, 12 maquiagem, 20 óculos, 2 pintas, 37 bocas, 6 bigodes, 18 linhas do nariz]`. Tabelas em 0x64, 0xed4, 0x159c, 0x183c, 0x1874, 0x19c4, 0x1b14, 0x1d44, 0x1d7c, 0x2188 e 0x2230. O cabeçalho tem vetores `u32[11]` em +0x0c e +0x38. `f_70008eab08` lê, de cada entrada, largura u16 (+4), altura u16 (+6), formato u8 (+8, tabela `nn::gfx` em `0x7000f7668c`: 0x201, 0x901, 0xb01, 0x1d01, 0x1e01, 0x2001, 0x2d01) e mipmaps u8 (+9).
- **AFL local confirmado:** cabeçalho de 0x14 bytes (`FFRA`, big-endian); entradas de 0x10 bytes (posição, tamanho, tamanho comprimido, nível, bits de janela, memória, estratégia); dados zlib. Texturas com 11 tipos `[3, 132, 80, 28, 12, 12, 20, 2, 37, 6, 18]`: as **232 partes não vazias descomprimem com o tamanho declarado**. Formas na mesma ordem das 12 do NX: 585 partes válidas. As entradas de rosto 8–10 apontam para antes do início dos dados (0x4d00), ou seja, são inválidas no próprio AFL; precisam de tratamento explícito.

**Formato decodificado — continuação (11/11/2026):**

- Correção: na tabela de contagens, formas e texturas estavam trocadas. Formas: `f_70008ea9d4`, 12 tipos, entradas de 0x58. Texturas: `f_70008eabc8`, 11 tipos, entradas de 0x1c, ordem NX `[chapéu 132, olho 62, sobrancelha 24, barba 2, rosto 12, maquiagem 12, óculos 20, pinta 2, boca 37, bigode 6, linha do nariz 18]`.
- Entradas NX: os primeiros 0x10 bytes são posição, tamanho e tamanho comprimido. O dado é zlib, descomprimido por `nn::util::DecompressZlib`, com área de trabalho 0x1bf0, a partir da base do arquivo. **Forma (0x58):** cinco deslocamentos de atributo em +0x10 e tamanhos em +0x24; índice u16 em +0x38/+0x3c (invertido ao espelhar); caixa delimitadora em +0x40 (mín.) e +0x4c (máx.). Atributo 0 = posição em 4× meia precisão (8 bytes); 1 = normal em 4 bytes; 2 = UV; 3 = 4 bytes (o byte 0 é remapeado ao espelhar); 4 = sem tratamento no construtor. As transformações de rosto (0x48 por rosto) ficam em +0x12704 e as de cabelo (0x24 por cabelo) em +0x14c24 do arquivo de formas. **Textura (0x1c):** em +0x10, largura u16 (+4), altura u16 (+6), formato u8 (+8; índices 0/1/2 → `nn::gfx` 0x201 R8, 0x901 R8G8, 0xb01 RGBA8; 3–6 parecem formatos BC), mipmaps u8 (+9) e modo de ladrilho u8 (+0xa → tabela `0x7000f20b00`: 0 ótimo, 1 linear).
- AFL local, com o layout confirmado por descompressão: cabeçalho de 0x14 bytes; `maxSize[11]` de texturas; **365 entradas** de textura na ordem FFL `[barba 3, chapéu 132, olho 80, sobrancelha 28, rosto 12, maquiagem 12, óculos 20, pinta 2, boca 52, bigode 6, linha do nariz 18]`, todas com rodapé de 12 bytes (mip, largura, altura, mipmaps, formato 0 R8 / 1 RG8 / 2 RGBA8). Em 0x1710 vem `maxSize[12]` das formas; as **formas começam em 0x1740**. Cada forma AFL começa com um cabeçalho de 0x48 bytes (seis deslocamentos, seis tamanhos e caixa delimitadora), seguido de posição float4, normal 10:10:10:2, UV float2, cor, parâmetro e índices u16.
- Diferenças a converter: posição float4 → meia precisão; reorganizar o cabeçalho de 0x48 do AFL na entrada de 0x58; transformações de rosto/cabelo em tabelas separadas; para as texturas, preferir modo linear, evitando swizzle, se o runtime aceitar. O AFL tem mais olhos, sobrancelhas e bocas (expressões extras); o mapeamento por índice para os primeiros N ainda precisa de verificação visual.

**Próximas etapas:** (1) decodificar os campos restantes das entradas NX (0x58 forma: deslocamentos/tamanhos de atributos, caixa e transformações; 0x1c textura: deslocamento e mipmaps) a partir dos consumidores no `main`; (2) converter atributos/índices FFL para o layout NX e texturas para os formatos `nn::gfx`, incluindo o swizzle de bloco do Switch, se exigido; (3) gerar os seis arquivos com validação estrutural e testes; (4) testar no jogo com `MK8_EXPERIMENTAL_NO_MII=0`, primeiro no menu e na corrida solo, depois em salas LAN com duas instâncias. Até a etapa 4 passar, nenhum consumidor gráfico é reativado.

## Mii com corpo nas telas do jogo — investigação, 11/11/2026

As telas com Mii de corpo inteiro usam `UI3D/MiiBodyMiddle/MiiBodyMiddle.bfres` (modelos `MaleBody`, `FemaleBody` e `MiiFace`; 16 ossos, `head` no índice 8, eixo Y para cima, mesma escala do `MiiFace` do piloto). O MKTV usa `UI3D/MiiBodyMiddleTheater/MiiBodyMiddleTheater.bfres`. No executável 4.0.1 descomprimido, os nomes estão em `0x7000ee6751`/`0x7000ee6786` (corpo/teatro) e `0x7000ee67db`/`0x7000ee6802` (`FemaleBody`/`MaleBody`). Quem carrega o recurso é `f_70003e0d84` (`main_078.c`), chamado por `0x70003e08b0`. As 12+1 instâncias de Mii da interface (salas/votação) são contadas pelos métodos virtuais +0x08/+0x10 em `main_085.c`. No modo sem Mii, `toolchain/experimental.py` força essas contagens a zero e pula `f_70003e0ea0`, o que evita os crashes conhecidos em `f_700080f4a4`. Assim, essas telas não criam o corpo; não é só a cabeça que falta.

**Conclusão da análise guest (11/11/2026):** cada Mii com corpo é montado por `f_70003e1310`, que usa um dos heaps `MiiWiFiMatchingHeap` (salas), `MiiTheaterHeap` (MKTV) ou `MiiFriendCloseupHeap`. Essa função obtém do gerenciador Mii o **objeto de modelo do SDK** por `f_700080f4a4` (salvo em +0x2b0; é a mesma função do crash da votação). Depois, `f_700081149c(modelo+0x10)` lê o gênero pelos acessores CharInfo do SDK (`0x7000b53100`/`0x7000b54400`) para escolher `FemaleBody`/`MaleBody` do recurso em +0x11a0. Por fim, `f_7000810c90(modelo, "head")` prende o rosto desse mesmo modelo ao osso `head`, e a coroa usa `/crown/Crown.bfres`. Portanto não existe caminho "somente corpo": criar essas instâncias sem o modelo do SDK exigiria objeto fictício ou ponteiros vazios, o que este projeto proíbe. Caminhos legítimos: (1) recursos MiiModel `0100000000000802` extraídos do próprio console, com `MK8_EXPERIMENTAL_NO_MII=0` (caminho já implementado, não qualificado); (2) um provedor substituto real do objeto de modelo Mii (CharInfo + malhas/texturas renderizáveis com ABI verificada), que é trabalho grande de engenharia reversa do SDK. Nenhum código guest foi alterado.

## Modelos Mii 3D convertidos no Multiplayer e análise do Nextendo (`fix-multiplayer`) — 10/11/2026

- **Auditoria da falha do Nextendo (`local/race-time-trial-cmcpfy8d`) e proteção de retratos (`f_700080f4a4`):** no último teste ao vivo do Nextendo com login real (`local/race-time-trial-cmcpfy8d`), o jogo autenticou OAuth + BAAS + NEX, passou por `Worldwide` → `Race` → seleção de personagem/kart e entrou na sala em `"You're spectating. Get ready!"` (`frame-04500.png`), encerrando no quadro ~4650 com `exception c0000005 accessing 0` em `f_700080f4a4+34c` chamado por `70000d51fc` (`main_026.c:316`). A auditoria completa de `main_*.c` confirmou que existem apenas três pontos de chamada para `f_700080f4a4` no jogo inteiro (`main_026.c:316`, `main_078.c:24271/25140` via `main_084.c:f_700041cb5c`, e `main_102.c:10515` no estado 2 de `f_70004af884`), e todos os três estão protegidos quando `MK8_EXPERIMENTAL_NO_MII=1` no candidato atual (`1979f48120ee9d7035562438a4c2916901f57996275e47867b6187af8af8957b`, que inclui `PORTRAIT_SCREEN_PATCHES` em `main_026.c`).
- **Interação entre Cabeças Mii 3D convertidas (`MiiM.szs`) e Multiplayer/Nextendo:** a conversão nativa das fases 4 e 5 substitui o osso `Head` (índice 10) em `romfs/Driver/MiiM.szs` mantendo `MK8_EXPERIMENTAL_NO_MII=1`. Assim, todas as proteções de `toolchain/experimental.py` contra falhas de firmware `MiiModel` nos retratos, globo e menus online continuam ativas, enquanto o modelo 3D do piloto Mii na pista exibe a cabeça convertida do perfil.
- **Separação de pin TLS por host no Nextendo (`net_tls_pin_for_host` / `RECOMP_NEXTENDO_ZNC_TLS_PIN`):** a inspeção TLS de `51.178.29.194:443` confirmou dois certificados distintos servidos por SNI: `g2b309e01-lp1.s.n.srv.nintendo.net` (`CN=nintendo-private-server`, RSA-2048 SPKI SHA-256 `770551f13152f3d55370de6c039d3859cff0d39882f27c6c9fc6cfdb327d74b8`) e `api-lp1.znc.srv.nintendo.net` (`Issuer=Nextendo Network CA1`, `CN=accounts.nintendo.com`, RSA-2048 SPKI SHA-256 `eb22631902f276218368e59dfac73f656bf8391c356e6c37f079049db40d596f`). Implementado `net_tls_pin_for_host` em `toolchain/nextendo.py` (`net/netcfg.h`, `net/netcfg.c` e `services/ssl.c`) para exigir igualdade estrita de 32 bytes contra `RECOMP_NEXTENDO_ZNC_TLS_PIN` quando configurado para `api-lp1.znc.srv.nintendo.net`, preservando o pin padrão quando omitido e sem aceitar certificados genéricos. Adicionada também abertura direta do navegador (`ShellExecuteA`) em `runtime/mk8/net/nextendo_probe.c` e `scripts/test-race.py`, recompilado o runtime (`--runtime-only`, candidato `74c68d423612d80b29de8d562d21a1a7b2ecfccb922c810f2479673b7bd2b047`) e validados os 9 testes em `tests/test_nextendo.py` (incluindo o novo teste nativo `test_per_host_tls_pin_separation`).
- **Evidência real em jogo (`local/multiplayer-uze8q270`):** adicionado `--mii-profile` a `scripts/test-multiplayer.py` e executado teste real com duas instâncias do launcher e do MK8 conectadas pelo código único `MK8P-…` usando o perfil e a cabeça 3D convertida de `local/mii-native-composer-a` (`330,7 s`, saída 0, `protected-data.json` com `unchanged: true`). As capturas confirmam: entrada na garagem LAN sem crash de retrato (`1/frame-05600.png`), votação sincronizada de pista (`0/frame-07800.png`, `1/frame-07800.png`) e corrida multiplayer ativa em Mario Kart Stadium com o convidado pilotando o **Mii 3D convertido (cabelo castanho + Normal Suit)** logo atrás do anfitrião (`Host`, Mario) na pista (`1/frame-10000.png`). `gameplay_verified` e `multiplayer_verified` permanecem `false`.

## Conversão e aplicação pelo launcher — fase 5, 10/11/2026

O editor agora oferece **Salvar perfil e modelo → Preparar e usar no jogo**. Após a conversão terminar, **Jogar solo** aplica a cabeça do perfil selecionado na próxima abertura. **Desativar no jogo** conserva os arquivos; **Ativar no jogo** reutiliza uma conversão válida. Alterações feitas com o jogo aberto só entram na próxima abertura. A interface funciona em português e inglês.

`crates/installer/src/mii_native.rs` valida o GLB salvo, CharInfo completo, BFRES original, hashes das entradas e manifesto da saída. Cada trabalho usa uma pasta privada nova em `<dados>/profiles/<id>/native/<trabalho>/`, com `conversion.log` e saída `model/romfs/Driver/MiiM.szs`. O recibo `native-head.json` do perfil só é publicado após sucesso real do processo e validação do resultado. Trocar/editar o perfil durante a conversão, partes não suportadas ou falha do conversor não ativam uma saída incorreta. A supervisão existente mantém os bloqueios de ferramentas e cancela o trabalho se o launcher encerrar; arquivos incompletos são conservados para diagnóstico.

Na abertura do jogo, o installer verifica novamente identidade, integridade, localização e conteúdo exclusivo da sobreposição. Modelos ativos desatualizados/corrompidos impedem a abertura até serem preparados novamente ou desativados. Um perfil sem conversão ativa não herda a cabeça de outro. `RECOMP_MOD_DIR` é configurado somente no processo filho; uma sobreposição já definida no ambiente ou recursos MiiModel ativos causam conflito explícito. `LARECOMP_SAVE_DIR` aponta para `<dados>/user-no-mii/save`, mantendo os saves fora da pasta da sobreposição. O renderer Mii SDK continua desabilitado neste caminho.

**Ferramentas locais:** nesta árvore, os padrões são `local/python/Scripts/python.exe`, Pillow em `local/mii-library-research/python-libs`, `local/mii-native-probe/MiiM.bfres` e `local/mii-native-probe/MiiHeadBuilder.exe`. O construtor precisa ter sido compilado da versão atual de `toolchain/mii_head_builder.cs`, com as quatro DLLs net48 do BfresLibrary junto ao executável, conforme fase 4. Para usar outros caminhos, criar `<dados>/mii-native-tools.json` com caminhos absolutos:

```json
{
  "python": "D:/Ferramentas/python/python.exe",
  "libraries": "D:/Ferramentas/python-libs",
  "original": "D:/DadosPrivados/MiiM.bfres",
  "original_female": "D:/DadosPrivados/MiiF.bfres",
  "builder": "D:/Ferramentas/mii/MiiHeadBuilder.exe"
}
```

`libraries` deve conter o pacote `PIL` do Pillow. O AFL do editor continua configurado separadamente. O launcher não baixa/extrai esses recursos nem distribui corpo, GLB ou ferramentas privadas. A CLI aceita `--python-libs` para carregar o Pillow explicitamente, pois o supervisor remove variáveis Python herdadas. O BFRES original continua restrito ao SHA-256 `d2d4f0992ab12c0773e0a89add358576c2ceccff0450deb5053740f73c57568a`.

**Validação:** build debug, 31 testes Rust de installer/launcher, Clippy com `-D warnings`, formatação e 18 testes Python de conversão/composição/roteiro passaram. `python scripts/test-mii-composer.py` passou novamente. `python scripts/test-mii-native-launcher.py` exercitou o launcher compilado com dados e saves isolados: botão de conversão, abertura real pelo botão Jogar solo, saída 0, recusa de modelo desatualizado, desativação, troca de perfil durante conversão e erro real de peça não suportada. A reativação do cache após falha e os controles PT/EN passaram em uma verificação complementar, sem criar outro trabalho de conversão. O roteiro público também inclui a reativação para as próximas execuções.

Evidências privadas em `local/mii-native-launcher-bs25s2jb`: `prepared.png`, `frame-02000.png`, `frame-03400.png`, `frame-04800.png`, `cached-english.png`, logs e `evidence.json`. As imagens inspecionadas mostram a cabeça no menu e acompanhando o piloto na pista. Para repetir, gerar primeiro os GLBs com `scripts/test-mii-composer.py`, manter um candidato local já preparado e as dependências privadas acima; o teste nativo copia os saves para a própria pasta de teste. Nenhuma alteração guest/runtime foi necessária nesta fase.

**Limites atuais:** substituição estática compartilhada de `MiiM.szs`, somente corpo masculino Normal Suit e partes aceitas pelo conversor. Outros pilotos que usem o mesmo recurso recebem a mesma cabeça. O lançamento em salas multiplayer do launcher é bloqueado enquanto a cabeça está ativa; isso não intercepta a entrada manual no Online Play dentro do jogo. Não há suporte qualificado a feminino, carecas sem malha de cabelo, peças adicionais de barba/óculos, expressões, retratos, roupas alternativas, cabeças independentes por jogador ou troca durante partida. O roteiro chega à barreira na primeira volta: não qualifica corrida completa, áudio, controle físico, reinício/save ou velocidade global do relógio. `gameplay_verified=false` e `multiplayer_verified=false` permanecem. A próxima etapa de modelos exige ampliar as peças/corpos suportados e validar cada variante antes da integração dinâmica por jogador; o modal dentro do jogo continua pendente.

## Cabeças do editor no MK8 — fase 4, 10/11/2026

Dois modelos criados/salvos pelo editor do launcher foram convertidos e desenhados no **menu de kart e na pista do MK8 nativo**. O perfil A usa rosto 0, pele 2, cabelo 24; o perfil B usa rosto 4, pele 0, cabelo 33. São cabeças distintas, com quatro e três partes opacas respectivamente (o segundo cabelo não tem peça de testa). Os corpos e animações continuam vindo do recurso masculino Normal Suit. Não foi habilitado o renderer Mii original, nem alterado código guest/upstream, o RomFS original ou os perfis/saves pessoais.

O novo `toolchain/mii_native_head.py` lê o GLB gerado pelo launcher e identifica as peças pelos metadados FFL `modulateType`. Valida CharInfo, envelope, buffers, índices, normais, limites, PNGs incorporados e transformações; recusa arquivos externos, extensões, animações e peças não suportadas. Aplica escala/translação dos nós de nariz e linha do nariz antes da conversão `(x,y,z) → (y,z,x) * 0,1` para o osso `Head`. Expande a UV espelhada da face e compõe máscara/linha do nariz em mapas opacos, multiplicando cores e misturando transparência em espaço linear. Mantém mapas sRGB, normal SNORM e brilho neutro do caminho anterior.

O construtor C# foi recompilado e agora aceita a testa opcional, exigindo face, nariz e cabelo sem enfraquecer as validações existentes. Continua exigindo o hash do BFRES original MK8 4.0.1 e `Head` no índice 10. O empacotamento conserva o limite de 4 MiB, reserva de disco de 4 GiB e recusa de saídas existentes. A saída privada contém intermediários, perfil associado, BFRES, `romfs/Driver/MiiM.szs` e manifesto com hashes/escopo; o conversor também conserva o GLB de entrada nas novas saídas. Não é um importador de GLB arbitrário.

**Evidência nativa inspecionada:**

- A: sobreposição `local/mii-native-composer-a`, capturas em `local/race-time-trial-8w4elf4s`, saída 0 em 97,7 s.
- B: sobreposição `local/mii-native-composer-b-short`, capturas em `local/race-time-trial-g9zz2cok`, saída 0 em 97,3 s.
- Em ambos, `frame-02000.png` mostra rosto/cabelo no menu; `frame-03400.png` e `frame-04800.png` mostram a cabeça acompanhando o piloto na pista. Os trechos finais mediram aproximadamente 58–60 FPS, com pausas anteriores de compilação de shaders. O cabelo longo ainda apresenta sombreamento irregular nas pontas; não há equivalência visual geral com o renderer FFL.

O roteiro acelera até a barreira na primeira volta: **não é corrida completa**. Não foram qualificados áudio, controle físico, save/reinício de corrida, velocidade global do relógio ou multiplayer. `gameplay_verified=false` e `multiplayer_verified=false` permanecem nos manifestos.

Validação adicional: 18 testes Python passaram (geometria, transformações, cor linear/sRGB, composição, rejeições, arquivos sem sobrescrita e roteiro). O executável C# rejeitou um índice fora do buffer antes de escrever BFRES. O teste Chromium do launcher passou novamente e agora exporta os dois GLBs. `scripts/test-race.py --mii-profile` copia o perfil indicado para a sessão isolada, permitindo testar cada cabeça sem trocar o perfil pessoal ativo.

Reproduzir com as dependências privadas já disponíveis nesta máquina (a pasta de saída precisa ser nova):

```powershell
$env:PYTHONPATH=(Resolve-Path local/mii-library-research/python-libs).Path
local/python/Scripts/python.exe toolchain/mii_native_head.py local/mii-composer-check/profile-B.glb local/nova-cabeca-nativa --original local/mii-native-probe/MiiM.bfres --builder local/mii-native-probe/MiiHeadBuilder.exe
local/python/Scripts/python.exe scripts/test-race.py time-trial --character mii --mii-profile local/nova-cabeca-nativa/mii-profile.json --last-frame 4800 --shots 3 --shot-step 1400 --timeout 180 --env "RECOMP_MOD_DIR=$((Resolve-Path local/nova-cabeca-nativa).Path)"
```

Antes de usar o conversor com uma versão alterada do construtor, recompilar `toolchain/mii_head_builder.cs` com o `csc.exe` do .NET Framework, referências `System.Web.Extensions`, `System.Core` e as quatro DLLs net48 do BfresLibrary privado; manter as DLLs junto ao executável. O hash do construtor executado é registrado no manifesto.

**Integração posterior:** a conversão e seleção da sobreposição pelo launcher foram implementadas na fase 5 acima. A substituição continua sendo de `MiiM.szs` inteiro: todos os pilotos que compartilhem esse recurso podem receber a mesma cabeça. Feminino, carecas sem malha de cabelo, barbas/óculos com peças adicionais, expressões animadas, retratos, roupas alternativas e vários Miis simultâneos exigem trabalho separado. Não há troca de modelo durante partida nem modal dentro do renderizador.

## Editor e perfis Mii — fase 3, 10/11/2026

O launcher agora oferece **Perfil Mii → Editar Mii 3D**. O usuário cria/seleciona um perfil existente, informa o caminho do AFL local verificado, edita os 49 campos Switch com prévia das peças reais e salva perfil + GLB texturizado. A cabeça pode ser girada, ampliada, reaberta e exportada. Criação/nome continuam no formulário principal; o modal faz a edição completa da cabeça. Altura, porte e campos administrativos são preservados no CharInfo, mas não mudam a prévia de cabeça isolada. Interface em português/inglês. Não há modal dentro do renderizador do jogo.

`crates/installer/src/mii_composer.rs` mantém a configuração separada em `local/mii-composer.json`; lê o AFL por tamanho e SHA-256 conhecido a cada carregamento/salvamento. `local/mii-resources.json` e os consumidores originais de MiiModel não são alterados. O recurso gráfico não é incorporado ao executável ou ao Git. Somente FFL.js/WASM, seus auxiliares/licença e o GLTFExporter são dependências públicas incorporadas ao launcher; procedência e hashes em `web/vendor/*/manifest.json` e `THIRD_PARTY.md`.

Os modelos ficam em `local/profiles/<id>/assembled-<sha256>.glb`, com recibo `assembled-head.json` contendo CharInfo completo, hashes e `native_integration_verified=false`. O `head.glb` procedural anterior é preservado. A gravação confere o perfil esperado, valida o envelope GLB, proíbe recursos externos e exige vínculo CharInfo, normais e imagens incorporadas. Trocar/editar o perfil rejeita um salvamento atrasado e invalida o recibo antigo; arquivos anteriores são conservados. O arquivo derivado é escrito antes do perfil/recibo: uma falha posterior é reportada, podendo deixar um GLB órfão, sem substituir o modelo anterior por conteúdo parcial.

Validação: 30 testes Rust do installer/launcher passaram; build debug e Clippy com `-D warnings` concluídos. `scripts/test-mii-composer.py` inicia o executável real com `--data-dir` temporário, exercita criação de dois perfis, cabelo/pele/rosto, intervalo inválido, salvamento, reabertura, exportação, recarga da página, isolamento por identidade, rejeição de GLB/recurso inválidos e invalidação após edição. Verifica idiomas, ausência de requisições externas e erros de navegador. Evidências privadas em `local/mii-composer-check` (imagens, GLB e manifesto). Os perfis/saves pessoais são preservados.

**Próxima fase:** converter as cabeças geradas por perfil para os materiais, coordenadas e attachment do pipeline BFRES nativo e testá-las no MK8. O teste nativo anterior cobre apenas a cabeça pessoal estática; não foi repetido nesta mudança de launcher. Não há cobertura de todos os estilos/expressões, corpo completo, troca durante partida ou multiplayer. `gameplay_verified` permanece falso.

## Montagem de cabeças Mii — fase 2, 10/11/2026

Implementado um protótipo local em `toolchain/mii_assembly.html`, separado do launcher/runtime, que usa FFL.js/WASM e o AFL verificado na fase 1. Dois CharInfo sintéticos de 88 bytes produzem cabeças distintas, com seleção de formato do rosto, pele, cabelo, olhos e boca. Há edição do ID de cabelo (0–131), composição das texturas, exportação GLB e reabertura do arquivo exportado. Nenhum perfil pessoal foi lido ou alterado.

A montagem usa a seleção de peças e as transformações da FFL, em vez de tratar a sequência `shape_N.glb` como IDs de estilos do editor. Campos exercitados: formato em `0x2d`, pele `0x2e`, cabelo `0x31`, cor do cabelo `0x32`, olhos `0x34` e boca `0x45`. Perfil A: rosto 0, cabelo 33, olhos 2, boca 23; perfil B: rosto 4, cabelo 24, olhos 16, boca 7. A edição adicional de cabelo 12 passou; 132 foi rejeitado. A API recebe CharInfo diretamente, conservando o contrato de dados do launcher. Isso ainda não qualifica todos os estilos/campos.

As cabeças montadas incluem normais e materiais, ausentes nos GLBs brutos da fase 1. As máscaras são compostas pelo shader FFL e convertidas para texturas RGBA para exportação. O shader auxiliar `TextureShaderMaterial` da revisão consultada falhou ao compilar; foi usado `FFLShaderMaterial` para a composição, sem modificar fontes externas. Corrigida na adaptação própria a conversão linear→sRGB dos pixels lidos dos render targets, pois o GLB reaberto inicialmente escurecia os detalhes do rosto.

`scripts/test-mii-assembly.py` executa Chromium/Playwright com servidor temporário limitado aos arquivos de teste em loopback, valida recurso e revisão do FFL.js e preserva a licença junto à cópia temporária. Exercita os dois perfis, normais, texturas, downloads distintos, reabertura pelo GLTFLoader, edição/rejeição de cabelo, erros de console e ausência de requisições externas. Compara os pixels da prévia com os do GLB reaberto, exigindo diferença média RGB inferior a 0,1/255. O teste completo passou; também passaram os quatro testes da biblioteca.

Evidências privadas em `local/mii-assembly-check`: `profile-A.png`, `profile-B.png`, `export-A.png`, `export-B.png`, os dois GLBs e `manifest.json` com hashes e diferenças de pixels. Imagens originais/reabertas inspecionadas. O manifesto mantém `native_integration_verified=false`: **estes dois modelos ainda não foram testados no MK8**. O teste anterior em jogo continua limitado ao GLB pessoal, pela sobreposição estática.

Reprodução nesta máquina: `python scripts/test-mii-assembly.py` (Python com Playwright/Chromium). Pré-requisitos privados: o AFL da fase 1, checkout limpo `local/mii-library-research/FFL.js` na revisão `4b62d7e6bb7b5ba3932b5236a2c05a2ef1b02fec` e `local/mii-library-research/GLTFExporter.js` do Three.js r183, hash registrado em `THIRD_PARTY.md` e verificado pelo teste. O teste não baixa dependências nem publica um serviço externo.

**Próxima fase:** conectar a composição ao editor/perfil do launcher, com importação local do recurso e geração de modelo por identidade; depois adaptar e testar as cabeças geradas no pipeline nativo. O protótipo não é um modal integrado ao jogo, não habilita carregamento durante partida nem inclui a biblioteca no instalador. Expressões animadas, cobertura completa de estilos e multiplayer continuam pendentes. `gameplay_verified` permanece falso.

## Biblioteca de peças Mii — fase 1, 10/11/2026

Encontrada e extraída uma fonte de peças, sem automatizar exportações no Mii Creator: `AFLResHigh_2_3.dat`, do Miitomo, obtido pelo endereço público arquivado do antigo CDN indicado por jaames/mii-assets e FFL-Testing. O arquivo tem 4.579.008 bytes e SHA-256 `4a4be71d75162c20b48720ef89cd3d4e6cd4e8e21bcbc2aed63ee08d96de7722`. Não é o recurso NXTexture/Shape do SDK Switch e não deve ser passado ao caminho original de firmware como se fosse equivalente.

Resultado em `local/mii-parts-library`: **585 malhas GLB com geometria, uma entrada vazia (`shape_359.glb`) e 247 PNGs**. O arquivo contém 859 entradas de malha e 365 de textura; entradas sem dados não produzem arquivos. `manifest.json` registra hashes por arquivo, contagens, atributos, limites espaciais, dimensões e limitações. Verificados cabeçalhos/chunks, buffers, índices, posições finitas e contagens dos atributos; todas as imagens foram decodificadas. Amostra de 48 texturas inspecionada em `texture-contact-sheet.png`, com olhos, sobrancelhas, óculos, bocas e outros detalhes. Isso valida a extração/inventário, não a montagem nem o desenho dessas peças no MK8.

Adicionado `toolchain/mii_library.py`: recebe recurso e checkout externo já obtidos, verifica hashes, reserva de 4 GiB e saída nova; adapta em memória duas incompatibilidades do extrator antigo com NumPy/Pillow atuais, sem editar o código de terceiros. O arquivo de recursos e as seis fontes Python do extrator estão fixados por hash. O teste completo com o script terminou com saída 0; quatro testes sintéticos passaram, cobrindo geometria/limites, truncamento, índice inválido e rejeição de recurso desconhecido antes de criar a saída. Dependências usadas apenas na pasta privada de pesquisa: NumPy 2.5.4 e Pillow 13.0.0.

Reprodução local (escolher uma pasta de saída que ainda não exista):

```powershell
$env:PYTHONPATH=(Resolve-Path local/mii-library-research/python-libs).Path
local/python/Scripts/python.exe toolchain/mii_library.py local/mii-library-research/AFLResHigh_2_3.dat local/mii-library-research/mii-assets local/nova-biblioteca-mii
```

**Decisão da fase 1:** há uma fonte de peças e um caminho de extração offline; não é necessário exportar combinações em massa do site. O acesso público a `mii.nxw.pw/app` redireciona para login, e o repositório público do Creator se identifica como versão antiga. Não foram usados endpoints privados nem alterada a conta do usuário. O FFL.js é candidato para compor as peças a partir dos dados do perfil; ele continua exigindo o recurso AFL/FFL.

**Próxima fase:** mapear IDs/variantes, validar transformação e materiais, compor duas cabeças diferentes e testar o resultado antes de conectar a seleção de peças ao launcher. Os GLBs extraídos não incluem normais nem materiais, e sua convenção UV precisa ser tratada. Biblioteca atual permanece privada, com `editor_ready=false`, `native_integration_verified=false` e `distribution_license_verified=false`; não foi incorporada ao instalador. Não foi confirmada licença de distribuição dos recursos nem licença explícita do extrator arquivado. Referências em `THIRD_PARTY.md`. A pesquisa encontrou também um visualizador Fusion, mas a página o identifica como demonstração, não biblioteca; seu código não foi incorporado. Nenhuma mudança no runtime, nos saves ou nos perfis; `gameplay_verified` continua falso.


## Cabeça Mii no motor nativo: encaixe e textura corrigidos — 10/11/2026

A exporta??o pessoal agora aparece no **menu e na pista do MK8**, pela sobreposi??o privada `local/mii-head-mod/romfs/Driver/MiiM.szs`. A cabe?a foi incorporada ao modelo do corpo Mii masculino, rigidamente ligada ao osso `Head` (?ndice 10), mantendo o esqueleto/anima??es originais. N?o foram alterados RomFS original, perfis, saves, upstream ou c?digo guest; a pol?tica sem renderer Mii original permanece ativa. Isso ? uma integra??o est?tica de um modelo, n?o qualifica??o de uma biblioteca de Miis.

Corre??es preservadas em `toolchain/mii_head_bake.py` e `toolchain/mii_head_builder.cs`: transforma??o local `(x,y,z) -> (x,z,-y)` com escala calibrada 7,14; expans?o das UVs espelhadas da pele de 0?2 para 0?1; proje??o/composi??o est?tica de olhos, boca, sobrancelhas e linha do nariz nas texturas opacas. O material comum do jogo substitui os shaders especiais Mii que continuavam pretos. Mapas de cor usam sRGB, normal neutra usa SNORM e uma m?scara neutra de brilho evita o aspecto met?lico herdado do capacete. A transpar?ncia original n?o foi qualificada; a composi??o evita depender dela para esta express?o fixa.

O construtor C# valida o SHA-256 do BFRES original descomprimido (`d2d4f0992ab12c0773e0a89add358576c2ceccff0450deb5053740f73c57568a`), osso/modelo esperado, nomes, dimens?es, ?ndices, normais e limites. O empacotador recusa BFRES acima de 4 MiB e n?o sobrescreve sa?da existente. Texturas limitadas a 256 pixels; a tentativa privada com texturas maiores e BFRES de aproximadamente 9 MB falhou ao selecionar o Mii, sem causa interna completamente estabelecida. N?o tratar o limite conservador como or?amento oficial do motor. Depend?ncia de convers?o/avisos registrados em `THIRD_PARTY.md`.

**Evid?ncia final:** `local/race-time-trial-4yvvjdnd`, sa?da 0 ap?s 97,2 s; capturas `frame-02000.png` (rosto no menu), `frame-03400.png` e `frame-04800.png` (cabe?a na pista). Capturas inspecionadas: pele/olhos/boca presentes e cabe?a acompanhando o corpo; brilho met?lico removido. O roteiro acelera at? a barreira na primeira volta, portanto **n?o ? uma corrida completa**. Trechos finais medidos entre 59,4?60 FPS, com pico de 40 ms em um intervalo; isso n?o qualifica desempenho em outras m?quinas. `local/mii-head-mod/manifest.json` registra hashes, escopo e evid?ncia, mantendo `gameplay_verified=false` e `multiplayer_verified=false`.

Valida??o: construtor C# compilado e seu artefato executado no jogo; seis testes de geometria/composi??o/Yaz0 com dados sint?ticos e os sete testes do roteiro passaram. Reprodu??o da composi??o gera JSON e pixels id?nticos; o execut?vel do construtor rejeitou um ?ndice fora do buffer antes de criar a sa?da. O roteiro agora for?a `LARECOMP_SAVE_DIR` para a c?pia isolada, porque um `RECOMP_MOD_DIR` poderia redirecionar saves para dentro da sobreposi??o.

Repetir o teste local j? preparado:

```powershell
local/python/Scripts/python.exe scripts/test-race.py time-trial --character mii --last-frame 4800 --shots 3 --shot-step 1400 --timeout 180 --env "RECOMP_MOD_DIR=$((Resolve-Path local/mii-head-mod).Path)"
```

Reprodu??o da convers?o em desenvolvimento: o est?gio Python recebe o intermedi?rio privado `head-small.json` (seis partes, posi??es j? escaladas, eixos locais do GLB e RGBA redimensionados), n?o um GLB arbitr?rio. `python toolchain/mii_head_bake.py <intermediario.json> <nova-pasta>` produz `head.json` e mapas compostos. Compilar `toolchain/mii_head_builder.cs` com o `csc.exe` do .NET Framework, refer?ncias `System.Web.Extensions`, `System.Core` e as quatro DLLs net48 do pacote BfresLibrary verificado; manter as DLLs junto ao execut?vel. Executar `MiiHeadBuilder.exe <MiiM-original.bfres> <nova-pasta/head.json> <novo-output.bfres>`; depois `python toolchain/mii_head_bake.py --pack <novo-output.bfres> <overlay/romfs/Driver/MiiM.szs>`. Pastas de sa?da precisam existir para o empacotamento; sa?das existentes s?o recusadas. Os arquivos de entrada, intermedi?rios, DLLs e recursos derivados permanecem privados.

**Limites e pr?ximo trabalho:** substitui o corpo masculino Normal Suit inteiro; outros Miis que usem esse recurso podem receber a mesma cabe?a. N?o h? escolha de cabe?a por identidade/perfil, importador geral no launcher, express?es din?micas, retratos integrados ou suporte feminino qualificado. O GLB cont?m um Mii pronto, n?o todos os cabelos/olhos/formatos. O launcher j? salva v?rios perfis e o menu do runtime pode traz?-lo ? frente; ainda falta associar um recurso nativo a cada perfil e implementar o editor modal solicitado. Um editor completo precisa de biblioteca de pe?as pr?pria ou importada localmente, composi??o de malhas/materiais e a mesma convers?o usada pelo jogo. A troca segura inicial deve ocorrer na pr?xima abertura do jogo; carregamento durante partida e multiplayer exigem trabalho e testes separados. N?o confundir o editor procedural aproximado existente com essa biblioteca.

## Integração Mii: corrida de referência e consumidores nativos — 10/11/2026

Executado o caminho real de seleção do primeiro Mii, Normal Suit e Time Trial em Mario Kart Stadium, com cópias isoladas dos saves/perfil. `local/race-time-trial-ps9fgizm/frame-04200.png` mostra o corpo Mii azul e kart na pista, **sem cabeça**. Isso confirma que o corpo/animação do piloto pode ser mantido; não confirma carregamento do GLB. O teste acelera sem dirigir e termina junto à barreira, na primeira volta; não é uma corrida completa.

Corrigido `scripts/test-race.py`: copiar `mii-profile.json` sozinho não fornecia os dados ao serviço Mii. Agora passa identidade/aparência e, quando presente, CharInfo com os mesmos overlays de nome/cor/tipo do launcher; preserva campos importados e não modifica o perfil original. `--character mii` reproduz os diálogos extras dessa seleção, limitado por enquanto a Time Trial/Stadium. O recibo registra personagem, hash do perfil e `model_rendering_verified=false`. O modo gráfico sem Mii continua explícito; este roteiro ainda não aceita/instala GLB no jogo.

Adicionado `MK8_TRACE_MII_CONSUMERS=1` no runtime: registra no máximo 64 locais de chamada distintos da política sem modelos, sem acessar estruturas guest, alterar retornos ou bloquear outra thread de desenho. A execução identificou preparação `0x700080fa44`, atualização `0x7000810278`, consulta `0x700041a834`, worker `0x700041a6b0`, retratos `0x700041b2f0` e os consumidores de seleção/confirmacão já protegidos. A leitura do código gerado verificado localizou também `0x7000815724`, que prepara texturas com o nome `MiiCharModelTexture`, chamado por `0x7000814a04`. Esses endereços são referências de investigação do perfil 4.0.1, **não uma ABI de renderização substituta validada**. Não foram alterados upstream ou arquivos gerados.

Validação: runtime recompilado com `--runtime-only`, link e dependências concluídos; 11 testes de layout/host (incluindo deduplicação/limite do diagnóstico e preservação da política) e sete do roteiro passaram. Repetição com o código atualizado: `local/race-time-trial-_ap1xdzt`, saída 0 após 98,2 s, captura `frame-04800.png` inspecionada, corpo presente e cabeça ausente. Não houve crash nessa execução. Logs de build/testes em `local/mii-integration-build.log` e `local/mii-integration-host-tests.log`.

Reprodução: `local/python/Scripts/python.exe scripts/test-race.py time-trial --character mii --last-frame 4800 --shots 3 --shot-step 1200 --timeout 180 --env MK8_TRACE_MII_CONSUMERS=1`.

**Objetivo pendente:** conectar a cabeça/texturas do GLB à transformação animada do pescoço e ao passe de desenho nativo. Ainda falta estabelecer esse contrato e a vida útil dos recursos gráficos; não existe carregador GLB conectado ao renderer do jogo nesta etapa. Não reativar a preparação original sem os recursos exigidos, nem retornar um objeto vazio como substituto. Retratos, expressões, corrida completa e multiplayer continuam pendentes; `gameplay_verified` permanece falso.

## Inspeção da exportação pessoal do Mii Creator — 10/11/2026

Os dois arquivos mais recentes fornecidos em Downloads foram `Test_all_body_2026-11-10T21_43_21.682Z.glb` (281.044 bytes) e `Test.miic` (140 bytes). O GLB 2.0 contém oito malhas, 3.423 triângulos, três texturas PNG embutidas e um esqueleto com 31 juntas. As partes da cabeça são separadas (`Faceline`, `Nose`, `Forehead`, `Hair`, `Mask`, `Noseline`); o corpo usa skinning. Não há clipes de animação. `asset.extras.miiData` corresponde byte a byte ao `.miic` fornecido. Trata-se de geometria real de um personagem, não de uma biblioteca de todas as peças de Mii.

Implementado no launcher: **Perfil Mii → Inspecionar GLB exportado**, com corpo/cabeça, rotação/zoom, malha, esqueleto, inventário e comparação do `.miic`. Three.js é empacotado localmente com licença MIT; não há CDN nem envio de dados. O visualizador aceita GLB 2.0 autocontido de até 8 MiB, limita JSON, coleções, hierarquia, accessors e dimensões PNG, e recusa URIs externas e extensões não suportadas. Erro de textura impede anunciar carregamento concluído. Não grava perfil/save, não altera os arquivos de Downloads e não habilita consumidores gráficos guest.

A inspeção inicial detectou texturas bloqueadas pela política de conteúdo, apesar de a geometria carregar. A política foi corrigida para permitir blobs locais; o teste sintético agora inclui textura PNG e verifica erros de console. Capturas finais `local/mii-preview-ui/export-full.png`, `export-head.png` e `export-wire.png` foram geradas; corpo e cabeça foram inspecionados. Rosto, olhos, cabelo e roupa aparecem com as texturas do arquivo. A sessão visível usa o launcher debug e mantém os arquivos apenas na memória do navegador.

Validação: builds debug e release concluídas; formatação e Clippy com avisos tratados como erro aprovados. Teste novo `tests/test_mii_glb.py` renderiza geometria/PNG autorais, alterna cabeça/malha/esqueleto, compara dados iguais/diferentes, recusa URI externa, ciclo, limites de accessor/buffer e arquivo truncado, e recarrega modelo válido após erros. Passaram também o teste do editor Mii e os dois de idiomas. A verificação privada do GLB do mantenedor confirmou renderização e correspondência de dados, sem requisições externas. Nenhum modelo pessoal foi adicionado às fixtures ou ao pacote.

Os 44 testes Rust do workspace também passaram após a última mudança. Ao solicitar teste no MK8, a auditoria confirmou que `runtime/mk8` não possui consumidor GLB: `toolchain/host.py` apenas monta o título MiiModel por `MK8_MII_RESOURCE_DIR`, e a preparação gráfica guest continua condicionada a `mk8_no_mii_renderer()`. O GLB do navegador não é passado ao processo do jogo. Assim, não foi anunciada nem executada uma corrida como teste desse modelo. A próxima implementação necessária é uma ponte nativa de cabeça/materiais para um único perfil em solo, com pontos de chamada e ABI verificados antes da recompilação guest; só depois cabe avançar para retratos, expressões e multiplayer.

Limite confirmado pelo código público: o decodificador antigo usa outro layout; o `.miic` novo não é CharInfo de 88 bytes e não foi convertido por suposição. A visualização não passa a gerar novos estilos a partir dessa exportação. Integração no jogo ainda requer carregar buffers/materiais, calibrar matriz e escala do pescoço, adaptar retratos/expressões e vida útil dos recursos. O corpo genérico exportado não substitui automaticamente o corpo/animações do piloto MK8. Nenhuma corrida com esse modelo foi executada; `gameplay_verified` permanece falso. Referências e licenças em [THIRD_PARTY.md](../THIRD_PARTY.md).

## Perfis Mii e caminho experimental de modelos reais — 10/11/2026

Pesquisa em Tomodachi LTD Recomp, Mush8, FFL.js/FFL-Testing e documentação de Nextendo/NX Native registrada com revisões e limitações em [THIRD_PARTY.md](../THIRD_PARTY.md). O Tomodachi utiliza outro runtime e o editor do seu próprio jogo. O Mush8 demonstra a separação mais útil para esta base: dados CharInfo no serviço Mii, recursos do firmware para a cabeça desenhada pelo SDK, e login/token/rede para Nextendo. Não foi localizado um componente que, apenas adicionando um GLB, habilite o online do MK8.

**Implementado nas fontes locais:** a aba **Perfil Mii** recupera criação, edição, seleção e identidade persistente. O editor simples oferece nome, cor, tipo, rosto, pele, cabelo, olhos, óculos, altura e porte. “Detalhes completos do Switch” expõe os 49 campos de CharInfo, incluindo nariz, boca, sobrancelhas, barba, bigode e posições/escalas. Importação/exportação de `.charinfo`/`.ufsd` de 88 bytes valida tamanho, nome, identidade e domínios antes de salvar. Campos importados fora do editor simples permanecem intactos ao renomear ou alterar outros controles; salvar detalhes mantém o ID e recusa um perfil ativo diferente. `.storedata`, bancos NFDB e QR codes não são aceitos nesta etapa. O nome de conta/sala continua separado do nome Mii; não há nova conta Nintendo ou migração de saves.

A prévia WebGL utiliza a geometria procedural original do projeto, com rotação, e está identificada como **aproximada**. Ela não é o renderer Nintendo e não representa todos os estilos importados. O arquivo GLB derivado continua sendo uma prévia, não um recurso de sistema. A passagem para o jogo usa os 88 bytes completos em `MK8_MII_CHARINFO`, revalidados pelo runtime antes de responder à consulta de dados Mii. O banco apresenta o perfil ativo e os seis presets existentes. Perfis antigos sem `charinfo` continuam legíveis. Saves permanecem em `user-no-mii`; o nome histórico da pasta não decide o modo gráfico.

O menu do runtime ganhou **Perfil Mii**, que traz o launcher à frente. O applet Mii também pode solicitar essa janela e retorna cancelamento, sem inventar um resultado de edição concluída ou sobrescrever os campos importados pelo editor legado. Alterações valem na **próxima abertura do jogo**: não há atualização de modelos/conta em uma partida ativa. A janela do launcher precisa continuar aberta para receber o pedido do runtime.

**Modelos reais:** a opção experimental recebe a pasta extraída do título de sistema `0100000000000802`, com `ShapeMid.dat`, `ShapeHigh.dat`, `NXTextureLowLinear.dat`, `NXTextureLowSRGB.dat`, `NXTextureMidLinear.dat` e `NXTextureMidSRGB.dat`. Registra tamanho e SHA-256 de cada arquivo e os confere antes de iniciar o jogo. Recusa ausência, truncamento básico e cabeçalhos sem partes; essa triagem não é uma validação completa do formato pelo SDK. O runtime monta essa pasta somente para o título MiiModel, pelo serviço de armazenamento/RomFS existente. Quando a configuração está ativada e íntegra, o launcher passa `MK8_EXPERIMENTAL_NO_MII=0`, permitindo aos hooks condicionais existentes executar a preparação, retratos e modelos originais. Desativar a opção restaura o modo sem modelos. Nenhum arquivo vazio, ponteiro fictício, firmware ou recurso de outro jogo foi fornecido.

### Validação executada nesta implementação

Compilação do launcher de desenvolvimento (`target/debug/Launcher.exe`) e do host Windows concluídas; o host compilou 212 unidades e fez o link. Passaram os 44 testes Rust do workspace, `cargo fmt --all -- --check` e Clippy com avisos tratados como erro. Passaram também os três testes nativos de dados Mii/montagem do armazenamento, os 12 testes dos hooks experimentais e os 46 testes de regressão de `scripts/verify-graphics.py`, sem skips nessa suíte.

O teste de ponta a ponta `tests/test_mii_launcher.py` passou com launcher real e Chromium: criação, persistência do ID, importação preservando estilos, edição detalhada, exportação byte a byte, rejeição de dados inválidos, recusa de recursos ausentes e edição sem WebGL. A captura `local/performance-validation/mii-editor.png` foi inspecionada. Os dois testes de idiomas passaram. Na suíte geral `tests/test_launcher_e2e.py`, 15 testes passaram e um foi ignorado porque o executável do relay não estava configurado. Esses testes não executam partidas nem validam o renderer Mii do SDK comercial.

SHA-256 do launcher de desenvolvimento: `8aad4a7705126c4dfd4c2ff561c71b7b4b1cc8919210884549a73501c3219c11`. SHA-256 do host `local/performance-validation/host/mk8drecomp.exe`: `4b25901c779d350d31a8695387e5ba2688ba767a4d7219c2cc71b5ec2c014a23`. O host é uma compilação de desenvolvimento sem biblioteca recompilada do jogo; não constitui um pacote jogável independente. Nenhum instalador foi gerado nesta etapa.

### Testar Mii em outra máquina

1. Recompilar o launcher a partir destas fontes e o host com `--runtime-only` na instalação MK8 4.0.1 já preparada, conforme o procedimento abaixo. Não trocar apenas o EXE de uma instalação com runtime antigo.
2. Abrir **Perfil Mii**, criar um perfil, salvar, fechar/reabrir o launcher e conferir o ID. Exportar `.charinfo`; importar um Mii diferente; verificar que renomear e editar um campo não muda os demais. A importação de ID já existente é recusada para preservar o perfil anterior.
3. Preparar a extração do título MiiModel do próprio firmware. Esta tela recebe a pasta já extraída, não uma coleção de NCAs nem um GLB. Informar essa pasta em **Modelos dentro do jogo — experimental**, marcar a opção e salvar. Os arquivos são lidos no lugar, sem substituir dados privados.
4. Iniciar uma nova sessão, conferir os Miis no menu, retratos, corpo, cabeça, encaixe no pescoço e animações, depois jogar com o Mii em corrida solo, voltar ao título e reiniciar. Verificar iluminação, expressões, nomes, save e ausência de acessos nulos. Testar alteração de perfil em uma sessão seguinte e o botão **Perfil Mii** do menu do runtime. Se houver falha gráfica, desativar a opção no launcher, preservar `game.log` e comparar com o modo sem modelos.
5. Repetir em LAN/relay entre duas máquinas, com perfis distintos, observando a cabeça e o retrato do outro jogador, entrada/saída de sala e troca de pista. Registrar uso de VRAM e tempo de quadro em sessões longas: a renderização de Miis adiciona trabalho e recursos GPU ainda não medidos aqui.

**Limites atuais:** o usuário confirmou que não há firmware/recursos Mii nem instalação preparada disponível para este teste. Portanto cabeças/retratos/animações no MK8 e a aceitação pelo SDK do jogo real permanecem sem qualificação, apesar do caminho de integração estar implementado. Não há autenticação Nextendo, token NEX, amigos ou matchmaking integrados. Exibir/validar um Mii não prova aceitação pelo servidor; o cliente precisa de integração própria e identidade OAuth autorizada, não a identidade de outro projeto. `gameplay_verified` continua falso. Nenhuma release/instalador foi publicado.

## Correção local da solicitação de retratos — 0.1.9-test.1

A tela em `main_026.c` solicitava `f_700080f4a4` mesmo com o gerenciador de retratos sem recursos. A adaptação em `toolchain/experimental.py` valida SHA-256 da unidade MK8 4.0.1 e duas fronteiras: no modo sem renderizador, com gerenciador ausente ou contagem/ponteiro da lista ambos zero, omite o bloco de reserva/solicitação e mantém o campo do retrato nulo. Os dois ramos de apresentação que consumiam esse retrato também são omitidos para os tipos 0x1d/0x34, seguindo para o caminho comum da tela. A liberação existente já aceita retrato nulo. Nenhum objeto visual ou sucesso de rede é fabricado; recursos presentes e outros tipos mantêm o comportamento original.

Teste nativo sintético executa ambos os tipos, outro tipo, modo normal/experimental, gerenciador ausente/presente e combinações de contagem/ponteiro, verificando solicitação, consumo e continuação da tela. Dois testes de votação/retratos e 12 testes das demais adaptações passaram; unidade real recompilada e bibliotecas consumidoras religadas. Compilação completa concluída em 97,7 s após usar o Python privado com suas dependências (a tentativa inicial com Python do sistema parou por falta de unicorn). Relatórios agora incluem os logs do runtime; dois testes Rust, formatação e Clippy passaram. A versão privada 0.1.9-test.1 distingue esta build e solicita nova preparação nos PCs com 0.1.8. Sem tag/release no GitHub. Qualificação em duas máquinas e `gameplay_verified` continuam pendentes.

Validação real em `local/multiplayer-j2sdcvxw`: dois launchers e dois jogos com cópias isoladas dos saves. Capturas 6600 mostram a votação dos dois participantes; capturas 9200 mostram Mario e Luigi na mesma pista, com o anfitrião visível para o convidado, posições 1º/2º e volta 1/3. Teste encerrou após 211,1 s sem o crash informado. Não completou a corrida nem reproduziu duas máquinas/VPN. `protected-data.json` confirmou preservação dos saves e a conferência externa dos 12 arquivos protegidos manteve todos os hashes.

Setup concluído: `target/dist/MK8-online-0.1.9-test.1-setup.exe` (55.750.656 bytes), SHA-256 `4b5f94c7b82c4f63c87e316a1a761012401ee8adf3739ac7d85e42c877112181`. Pacote compartilhável `MK8-online-0.1.9-test.1-test-package.zip` inclui instruções, checksum e fontes/avisos FFmpeg. Payload verificado com 916 arquivos, adaptação de retratos presente e sem entradas privadas. Dois testes de navegador contra o executável empacotado passaram (convites/conexão e relatório/atualizações). Instalar e preparar novamente a build nos dois PCs é obrigatório; não apagar dados nem saves.


