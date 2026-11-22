<div align="center">

<img src="docs/tag-ratone-tend.png" alt="RatOne — criador e mantenedor do MK8-online" width="560">

# MK8-online

### Prepare uma vez. Corra nativamente. Conecte-se com amigos.

**Criado e mantido por [RatOne](https://github.com/Vnnc1)**

[English](README.md) · **Português (Brasil)**

![Plataforma](https://img.shields.io/badge/Windows-x64-0078D6)
![Linux](https://img.shields.io/badge/Linux-amd64%20AppImage%20%7C%20experimental-5c6647)
![Gráficos](https://img.shields.io/badge/Gráficos-Vulkan-AC162C)
![Linguagens](https://img.shields.io/badge/Rust-C%2FC%2B%2B-555555)
![Release](https://img.shields.io/badge/Release-0.5.0-ef795d)
![Estado](https://img.shields.io/badge/Estado-Teste_público-e6a23c)
[![Licença](https://img.shields.io/badge/Licença-GPL--3.0--or--later-blue)](LICENSE)
[![Discord](https://img.shields.io/badge/Discord-Entrar-5865F2?logo=discord&logoColor=white)](https://discord.gg/4Yg4Axpkb6)

Projeto experimental de recompilação estática de **Mario Kart 8 Deluxe 4.0.1** para Windows x64 e Linux amd64 (experimental),
com runtime Vulkan, criador de Mii, Online Play nativo pelo Nextendo e salas com amigos pela Steam.

[Download / Releases](https://github.com/Vnnc1/MK8-online/releases) · [Relatar um problema](https://github.com/Vnnc1/MK8-online/issues) · [Discord](https://discord.gg/4Yg4Axpkb6) · [Roteiro](docs/ROADMAP.md)

</div>

> [!IMPORTANT]
> Use seus próprios arquivos do jogo e as chaves do seu console. O repositório e as releases não incluem ROMs, recursos do jogo, chaves, saves ou código recompilado do jogo. A compilação acontece localmente, a partir dos seus arquivos.

> [!NOTE]
> **Baixe a versão 0.5.0** · [Instalador Windows](https://github.com/Vnnc1/MK8-online/releases/download/v0.5.0/MK8-online-0.5.0-setup.exe) · [ZIP portátil Windows](https://github.com/Vnnc1/MK8-online/releases/download/v0.5.0/MK8-online-0.5.0-portable.zip) · [AppImage Linux amd64](https://github.com/Vnnc1/MK8-online/releases/download/v0.5.0/MK8-online-0.5.0-linux-x86_64.AppImage)
>
> No Windows, mantenha a pasta de dados e conclua **Instalação → Ferramentas → Build do jogo** se solicitado. No Linux, o launcher e o jogo recompilado executam nativamente. O guia explica como instalar as dependências no Ubuntu 24.04 e como experimentar no modo desktop do Steam Deck. [Guia Linux](docs/LINUX.md) · [Notas da versão](docs/releases/0.5.0.md)

O instalador Windows 0.5.0 permite usar arquivos do RomFS cujo caminho no Windows chega a 260 caracteres ou mais, inclusive com acentos e emoji. Se a preparação anterior parou com a mensagem `MAX_PATH` do RomFS, mantenha a mesma pasta de dados, instale o launcher atualizado e tente novamente em **Instalação → Build do jogo**. A compilação aproveita as etapas já concluídas.

## Windows 0.5.0 · Jogue com seu Mii

Na 0.5.0, o solo, as salas Steam e o Nextendo usam o Mii ativo. Na primeira partida, dê um nome ao Mii, personalize e escolha **Salvar e continuar** para seguir no modo escolhido. Para o Nextendo, confirme o e-mail e vincule o Discord à conta. Autorize uma vez no navegador enquanto o launcher estiver aberto. Ao abrir, o jogo mostra a conta Nextendo e o Mii local. Fechar o launcher ou revogar o acesso exige um novo login. [Gerenciar conta Nextendo](https://nextendo.network/account)

## Windows 0.5.0 · Opções gráficas

No launcher 0.5.0, abra **Jogar → Gráficos** ou **Configurações → Vídeo e controles → Imagem**. Se Instalação oferecer **Atualizar a build**, conclua essa etapa primeiro, inclusive ao atualizar uma instalação de teste da 0.4.1. Escolha uma resolução interna menor, como **75%**, e selecione **FSR 3.1.6**, **FSR 4.1.2** (RDNA 4 compatível) ou **DLSS Super Resolution** (RTX compatível). Leia e aceite os termos do fabricante no launcher para instalar o componente opcional. Salve e inicie o jogo com o runtime atualizado. O painel mostra o método e as dimensões informados pelo jogo. A resolução interna pode ser arredondada para se ajustar aos alvos de renderização.

No jogo, abra **F1 → Ajustes → Vídeo** para escolher a ampliação integrada ou um provedor instalado e disponível neste PC. O menu nativo opcional usa a mesma lista. Para carregar outro provedor temporal, reabra o jogo pelo launcher. Com o launcher aberto, as alterações feitas no jogo também aparecem em Configurações e no seletor de idioma da tela Jogar.

FSR 1 e bilinear já vêm disponíveis. Se o provedor recusar o recurso, o jogo usa a ampliação integrada. A opção separada de geração de quadros usa a interpolação própria do runtime. Ative o contador para comparar os FPS do jogo, os FPS de saída e os milissegundos por quadro do jogo.

## Novidades da 0.4.0

**Multijogador online nativo com o Nextendo.** Selecione **Jogar → Jogar no Nextendo** no launcher, entre na sua conta pelo navegador e abra **Online Play → Mundial → Corrida** no Mario Kart para jogar com outras pessoas. Pressione **F1** dentro do jogo para consultar salas e amigos. As sessões Nextendo usam as pistas originais.

A edição portátil pode ser movida entre unidades do Windows. Com o jogo preparado, `Launcher.exe --play` abre o modo solo diretamente. Os controles ganharam direção por movimento, vibração HD e a opção de usar cada Joy-Con na horizontal no multijogador local. Saves e replays temporários do Grand Prix ficam separados, e o troféu da Copa Casco permaneceu após reiniciar nos testes. As correções gráficas abrangem as cores das copas, a contagem da largada e a iluminação em tela dividida. Ao atualizar, mantenha sua pasta de dados e siga **Instalação → Ferramentas → Build do jogo**.


Vozes de áudio novas começam com volume anterior zero e usam a rampa existente no primeiro bloco do mixer. O patch aborda estalos em efeitos repetidos da pista e exige reconstruir o jogo/runtime; outros sons e a qualificação completa de áudio permanecem em aberto.

**Atribuição de controles nas builds de desenvolvimento.** Na tela de título, pressione um botão no controle que deseja usar como jogador 1. Uma tecla, um clique ou um toque na tela seleciona o teclado. Pressione **SL** ou **SR** para usar um Joy-Con na horizontal. Com apenas um Joy-Con esquerdo e um direito, um botão frontal seleciona o par. Com várias metades conectadas, segure **L** no esquerdo e **R** no direito que deseja combinar e solte os dois. Volte ao título para escolher de novo.

Para jogar com teclado, Pro Controller e dois Joy-Con deitados, selecione o modo individual (horizontal) dos Joy-Con. Pressione **K** no teclado na tela de título para usá-lo como jogador 1. Se preferir o Pro Controller como jogador 1, pressione **A** nele na tela de título e escolha quatro jogadores. Quando o jogo pedir o quarto participante, pressione **Enter** no teclado e confirme com **A** no Pro Controller.

Em corridas locais com tela dividida, o jogador que desconecta fica sem comandos e os demais podem continuar correndo. Ao reconectar, um controle identificável volta ao mesmo jogador, mesmo que o Windows liste os dispositivos em outra ordem. Um par de Joy-Con volta a responder quando as duas metades estão conectadas. Se não for possível distinguir dois controles iguais, registre-os de novo na seleção de personagens.

Se faltar um controle ao entrar na seleção de personagens, a tela **Controller assignment** destaca a primeira vaga. Se apenas o jogador 1 já estiver registrado, a seleção começa no jogador 2 e avança conforme cada participante entra. O rodapé mostra os ícones do jogo para **L + R** no controle e **SL + SR** em um Joy-Con na horizontal. Pressione os dois botões ao mesmo tempo para entrar ou use **Enter** se o teclado estiver disponível. Quem já entrou mantém a posição. Enquanto houver uma vaga, o botão central **Cancel**, a tecla **Esc** ou o botão Voltar do jogador 1 cancela o registro. Quando todos entrarem, o botão muda para **OK** e o jogador 1 confirma para continuar. Builds antigas e mods com um layout de menu compartilhado próprio podem exibir o aviso de texto com os mesmos comandos.

O menu de controles funciona na seleção de personagens mesmo com a tela experimental de configurações desativada. Com dois a quatro jogadores locais, use **+** no controle do jogador 1 ou a tecla atribuída a Start. No multijogador local também é possível clicar ou tocar em **−/+**. Na LAN com dois jogadores, esse botão fica oculto. Selecione uma linha para atribuir um controle livre ou trocar dois jogadores de posição, ou limpe as linhas e registre os jogadores por ordem de entrada. Se outro jogador ativo já usar o controle, um aviso identifica quem o está usando. **Apply** aplica as atribuições e **Cancel** mantém as anteriores. Durante corridas e batalhas em tela dividida, o atalho das configurações exibe um aviso breve para que todos possam continuar jogando. As configurações continuam disponíveis nos menus e durante a corrida com um único jogador local.

Abra **F1 → Ajustes → Controle → Joy-Con mode**. Escolha **Combined** para usar as duas metades como um controle ou **Individual (horizontal)** para cada jogador usar um Joy-Con de lado. A escolha vale para todos os Joy-Con conectados e fica salva para a próxima sessão. O Pro Controller pode ser usado junto com os Joy-Con nos dois modos.

O Pro Controller original do Switch, os Joy-Con individuais e um único par identificável de Joy-Con oficiais via Bluetooth têm vibração HD. No modo combinado, o jogo controla a frequência e a intensidade de cada lado separadamente. No modo horizontal, cada Joy-Con recebe os dois canais de vibração daquele jogador. Os demais controles compatíveis usam a vibração da SDL. Abrir as configurações ou trocar de janela interrompe a vibração.


A versão 0.4.0 acrescenta **Ultrawide (experimental)** às configurações de vídeo do launcher e do menu dentro do jogo. Clique em **Salvar configurações** no launcher antes de iniciar. A opção amplia câmeras solo suportadas de Contra o Relógio, Grand Prix e VS sem equipes entre 16:9 e 32:9, preserva o enquadramento vertical e mantém o HUD proporcional nas bordas. As vistas suportadas incluem direção frontal/traseira nesses modos solo, sobrevoos GP/VS e close-ups iniciais GP/VS/Contra o Relógio. Original 16:9 e Esticar continuam disponíveis. Cenas/modos sem suporte, tela dividida e geração de quadros usam 16:9; câmeras online e de replay não recebem suporte ultrawide. Reconstrua o jogo, incluindo as unidades guest afetadas, para aplicar.

A opção de **menu nativo de configurações** é salva automaticamente para o próximo início. Acrescenta a chave no menu principal do jogo e preserva o fallback F1/Menos + Mais e a prioridade do atalho do hub. Compartilha o arquivo de configurações do runtime, incluindo a proporção. Os layouts privados são derivados localmente de recursos verificados do jogo; nenhum é distribuído. A qualificação completa continua pendente; evidências manuais locais e limites estão em [STATUS.md](docs/STATUS.md).

**CTGP Deluxe e o menu de configurações.** O relançamento corrige o crash ao abrir o menu principal com as duas opções ligadas. Atualize a build para o launcher regenerar o layout do menu a partir dos arquivos do jogo.

### Instalação e runtime

A versão 0.4.0 restaura a sombra ausente do banner de largada de Rock Rock Mountain, respeitando o estado de depth clamp solicitado pelo jogo. Reconstrua o runtime para aplicar o fix. Condições verificadas no [estado técnico](docs/STATUS.md).

O instalador pode abrir o launcher na mesma janela após a instalação. A opção vem marcada, e o launcher usa a pasta de dados escolhida durante a instalação, inclusive em outro disco.

Se uma atualização exigir a reinstalação das ferramentas, a tela Instalação abre em **Ferramentas**. Após a verificação, ela segue para **Build do jogo** quando os arquivos estão prontos, ou para **Arquivos do jogo** se faltar alguma entrada. A instalação bem-sucedida remove as cópias obsoletas de ferramentas reconhecidas nas versões anteriores.

Abra **Uninstall.exe**, ao lado de **Launcher.exe**, para desinstalar. A opção padrão guarda saves, Miis, configurações e o jogo preparado. Para reutilizá-los, escolha a mesma pasta de dados na reinstalação. A remoção completa mostra a pasta de dados e exige confirmação.

O runtime da 0.4.0 inclui um ajuste Vulkan para a GTX 1070 desktop (dispositivo PCI `10de:1b81`). Consultas elegíveis de tamanho de textura constante são movidas para fora de ramificações de shaders de fragmento. As demais GPUs seguem pelo caminho atual. Recompile o runtime para aplicar o ajuste. A validação está no [estado técnico](docs/STATUS.md).

**Inicialização direta:** `Launcher.exe --play` abre o jogo solo já preparado sem a janela do launcher. Para selecionar seus dados: `Launcher.exe --play --data-dir "D:\\MK8\\data"`. Mantém validações, bloqueios e logs; não instala ferramentas nem recompila. Salas Steam continuam pelo launcher.

**Modo portátil Windows:** extraia `MK8-online-0.5.0-portable.zip` e mantenha `portable.txt` ao lado do executável. Fontes, ferramentas e dados ficam em `portable/`, sem executar setup. Para atualizar, substitua o executável e preserve essa pasta; atualização por instalador fica desativada. `--data-dir` tem prioridade para a pasta de dados. Não migra instalações existentes automaticamente.

**Mudança de pasta/unidade:** abra esta versão uma vez antes de mover, feche o jogo e o launcher e copie/mova a pasta inteira, incluindo `portable/`. Na próxima abertura, caminhos internos do jogo, builds, Miis, fontes e Python são ajustados automaticamente. Os metadados anteriores ficam em `portable/.relocation-backups/`; saves e arquivos binários do jogo não são reescritos. Recursos selecionados fora da pasta portátil continuam externos: mantenha dentro dela o que precisar levar. O ZIP portátil requer Windows x64 e drivers Vulkan compatíveis. No Linux, use o AppImage indicado acima.


Pressione **F1** ou **Menos + Mais** para abrir o menu do jogo. Em Jogar, volte à partida ou abra o Perfil Mii. Ajustes reúne vídeo, áudio, controles e mods. O painel usa o tema de menu escolhido no launcher. Nos detalhes de um mod, Voltar retorna à lista de mods. Após o login, Nextendo mostra as salas reais do MK8 e Social consulta os amigos e a presença com a permissão `friends`. Selecionar uma sala abre os detalhes. Para disputar uma corrida, use **Online Play** no Mario Kart.

### Recursos disponíveis desde a 0.3.0

- **CTGP Deluxe no MK8 4.0.1:** importe os recursos das pistas do CTGP Deluxe 1.1.1 (originalmente para 3.0.3), incluindo 64 pistas em 16 copas adicionais com nomes, imagens, ícones e músicas. A adaptação não executa o plugin original; a compatibilidade é incompleta e ainda há bugs.
- **Launcher redesenhado:** instalação horizontal em etapas, configurações em módulos, idioma do jogo acessível, criação guiada de Mii, amigos/salas Steam e relatórios de suporte mais claros.
- **DLC opcional:** importe seu Booster Course Pass na preparação ou depois e ative/desative sem excluí-lo. Jogo, DLC e chaves nunca são distribuídos.
- **Correções e melhorias:** congelamento ao carregar recursos Mii, óculos, verificação Vulkan, memória/texturas, miniaturas MKTV, manual local, fontes, shaders de partículas, áudio, seleção de controles e esperas de shaders na tela dividida em dois jogadores.
- **Configurações e apresentação:** leitura CRLF do INI corrigida. A geração experimental de quadros pode apresentar cerca de 120 FPS nos trechos medidos, mantendo a simulação em 60 Hz; continua desligada por padrão.

O histórico do CTGP e do redesenho do launcher está nas [notas da 0.3.0](docs/releases/0.3.0.md). Para as limitações da versão atual, consulte o [estado técnico](docs/STATUS.md).

## Onde estamos

**Correções no Windows na 0.5.0.** Recompilar o runtime do Windows habilita caminhos com caracteres não ASCII para o jogo, os saves e as configurações. Se o jogo fechar com erro, o launcher mostra a última mensagem fatal disponível e guarda um ZIP de diagnóstico em **Suporte**. Isso também funciona com `Launcher.exe --play`. A integração Steam tenta a próxima DLL instalada quando uma candidata carrega, mas falha ao inicializar as interfaces necessárias. O launcher também aceita o Booster Course Pass v65536 verificado em NSP ou NSZ.

Recompile o jogo depois de instalar o instalador 0.5.0 para aplicar a correção da votação de pistas quando faltam modelos Mii. Durante a preparação do jogo, o gerador agora tenta novamente se o Windows bloquear temporariamente a renomeação de um módulo concluído.

**Apresentação a 120 FPS:** a opção experimental de geração de quadros já existente interpola uma imagem entre quadros reais, mantendo o jogo a 60 Hz. Exige monitor de alta frequência e folga na GPU; não desbloqueia a simulação nativa nem elimina engasgos de shaders. A investigação local mediu cerca de 120 quadros apresentados por segundo em um trecho solo estável com RX 580 2048SP e monitor a 180 Hz. Isso não qualifica uma corrida completa ou multiplayer. O runtime da 0.3.0 também corrige a leitura de INI com CRLF, que podia desativar silenciosamente `frame_gen = 120` e interpretar outras opções textuais incorretamente. Detalhes e limites em [STATUS.md](docs/STATUS.md).

As correções de desempenho da 0.3.0 usam o temporizador de alta resolução do Windows nas esperas limitadas de shaders, aplicam esse orçamento à cena de tela dividida de dois jogadores e cancelam a configuração de controles quando faltam jogadores. Recompile o jogo/runtime para aplicá-las. Na versão 0.3.0, para teclado e um controle no multijogador local, ative **Controles começam no jogador 2** em Vídeo e controles. A primeira compilação de shaders ainda pode causar pausas; essas correções não qualificam todas as pistas CTGP nem o multiplayer entre PCs.

A versão 0.3.0 integra as PRs #27–31 e #33: guia local, leitura de miniaturas dos replays, lifetime de texturas, suporte limitado de shader, fontes privadas e melhorias Steam/Mii/DLC. O commit `8202c32` também integrou a fase B do CTGP (copas adicionais e seus hooks de guest). Verificações da integração e limites estão em [STATUS.md](docs/STATUS.md).

Melhorias Steam da 0.3.0: descoberta da DLL Steam em bibliotecas adicionais e pastas internas dos jogos, com acessores C versionados; busca de todos os amigos, inclusive offline; fragmentação confiável de datagramas LAN maiores; e liberação da sessão Steam ao fechar o jogo, permitindo aceitar outro convite sem reiniciar o launcher. Atualize os dois PCs; uma corrida completa entre máquinas ainda não foi qualificada.

O Multijogador oferece **Criar Sala (Automático)** e **Jogar (Sala Manual)**. Ambos criam a sala Steam do anfitrião; convidados aceitam um convite e escolhem navegação automática ou manual dentro do jogo. A automação mantém o Mii ativo selecionado. O conversor corrige os canais da textura dos óculos: regenere os recursos Mii derivados e recompile o runtime para aplicar essas correções Mii.

Na `fix-chore`, **Instalar → Arquivos do jogo** aceita um NSP ou NSZ opcional do Booster Course Pass, importado após a build bem-sucedida na mesma preparação pelo importador verificado existente. Campo vazio preserva a DLC atual. A importação posterior continua em **Instalar → Conteúdo adicional**, com atalho em **Jogar**; ativar/desativar continua em **Configurações**. A Steam continua exigindo DLC e CTGP equivalentes. Nenhuma DLL Valve ou arquivo do jogo é distribuído.

| Área | Progresso atual | Próxima etapa |
| --- | --- | --- |
| Launcher | Janela nativa, preparação guiada, instalador, atualizações e relatórios; espanhol, inglês e PT-BR | Atualização 0.3.0 → 0.4.0 em mais PCs |
| Corrida solo | Mushroom Cup concluída; batalha, MKTV e contrarrelógio alcançados | Qualificação conjunta de imagem, áudio, controle e reinício/save |
| Mii | Criador, perfis e o renderer Mii do próprio jogo (retratos, trajes, piloto, minimapa) | Mii nas salas LAN, comparação com recursos do console |
| Multijogador | Busca de partidas no Online Play nativo pelo Nextendo, convites Steam e testes de LAN local | Completar uma corrida Nextendo no relançamento e uma corrida Steam entre dois PCs |
| Desempenho | Cerca de 60 FPS nos trechos medidos no PC de teste | Copas repetidas, estabilidade de VRAM e mais hardware |

São marcos observados, não um percentual de conclusão. `gameplay_verified` e `multiplayer_verified` continuam `false` até a qualificação completa.

A leitura de miniaturas do MKTV agora copia a imagem renderizada pela GPU ao gravar um replay. Arquivos existentes já podem conter JPEGs corrompidos; a mudança preserva esses arquivos e vale para miniaturas gravadas depois dela.

## O que este projeto faz

O pipeline traduz o executável AArch64 do jogo para C e o compila para Windows x64 ou Linux amd64 nativo. Um runtime C/C++ fornece gráficos, áudio, entrada e serviços de sistema; o launcher em Rust/Tauri cuida da preparação, builds, atualizações, Miis e salas.

RatOne desenvolve e mantém a adaptação do MK8, o launcher e a integração. A base de tradução/runtime vem do **SMM2Recomp e do LASRecomp**, com outros trabalhos creditados. Veja [fontes de terceiros e licenças](docs/THIRD_PARTY.md).

## Entre na pista

### Requisitos no Windows

- **Windows x64** e WebView2. O launcher instala sozinho um **Python 3.12 x64** privado.
- GPU e driver com **Vulkan 1.3 ou mais novo**. Não há especificação mínima geral de CPU/GPU qualificada.
- Seu próprio jogo base, a **atualização 4.0.1** e o `prod.keys` do seu console, ou uma pasta do jogo já extraída e suportada.
- Cerca de **28 GiB livres** para extração e preparação (incluindo 4 GiB de reserva de segurança), além de espaço para ferramentas e caches. SSD recomendado.
- Internet para o primeiro download das ferramentas (cerca de 0,4 GiB) e para atualizações.
- **Para Miis:** ao abrir o criador, o launcher baixa `AFLResHigh_2_3.dat` de uma cópia do antigo CDN Miitomo no Internet Archive. Confere tamanho e SHA-256 do ZIP e da biblioteca, depois guarda o arquivo na pasta de dados para uso offline. O arquivo não acompanha o GitHub nem o instalador. Se o arquivo online estiver indisponível, tente novamente ou selecione uma cópia local compatível; sem a biblioteca, as opções ficam numeradas e os rostos não são desenhados.
- **Para multijogador:** entre na Steam e use uma DLL Steamworks compatível de 64 bits de um jogo já instalado. Nesta branch, o launcher tenta primeiro as DLLs configuradas e locais e procura nas bibliotecas Steam se nenhuma fornecer as funções necessárias. Se o launcher informar que falta `SteamAPI_SteamFriends_v017`, aponte `MK8_STEAM_DLL` para o caminho completo de uma `steam_api64.dll` compatível, reabra o launcher e entre em **Multijogador** para conferir a lista de amigos. A cópia testada de Brawlhalla funcionou na 0.3.0. As cópias testadas de Counter-Strike 2 e Deadlock exportam `v018` e são recusadas pela 0.3.0. Nenhum arquivo da Valve é distribuído.

### Instalar e jogar

1. Baixe `MK8-online-0.5.0-setup.exe` em [Releases](https://github.com/Vnnc1/MK8-online/releases) e escolha as pastas do aplicativo e dos dados. A instalação é por usuário, sem administrador. Já tem uma versão anterior? Use **Suporte → Atualizar agora**: a pasta de dados e os saves são mantidos.
2. Abra **Instalar**. Conclua **Ferramentas** e selecione o jogo base e a atualização em **Arquivos do jogo** (o launcher encontra as chaves junto do jogo, ou selecione as suas).
3. Rode a etapa **Build do jogo**. Depois de atualizar de uma versão anterior, rode de novo: o jogo é recompilado com o runtime novo.
4. Escolha **Jogar solo**.

A etapa **Ferramentas** do launcher prepara o Python privado diretamente, sem PowerShell ou carregamento de ZIP pelo .NET. O CPython vem do pacote oficial NuGet fixado; o arquivo e as dependências são conferidos por hash. Ambientes interrompidos são preservados antes do reparo. Mantenha a proteção do antivírus ativa.

Uma pasta extraída precisa conter `exefs`, `romfs` e `control.nacp` (ou `control/control.nacp`): use **Já tenho o jogo extraído numa pasta**. O perfil ativo é o 4.0.1 AArch64; outras versões do jogo e importação de XCI/XCZ não foram validadas. O DLC suportado é importado separadamente.

A importação direta de NSP/NSZ aceita duas variantes verificadas do programa base, mantendo a conferência integral dos hashes e da atualização 4.0.1. Outros conteúdos base continuam sem suporte; nesse caso, use uma pasta extraída compatível. Os detalhes da validação estão no [estado técnico](docs/STATUS.md).

Os dados ficam por padrão em `%LOCALAPPDATA%\MK8-online` e sobrevivem a atualizações e à desinstalação. Para usar outro disco com o launcher portátil: `.\Launcher.exe --data-dir E:\MK8-Local`, e continue usando a mesma pasta.

**Idioma:** o launcher e o instalador oferecem **English**, **Português (Brasil)** e **Español** (**Language / Idioma**). O idioma do jogo é independente.

### Crie seu Mii

1. Abra **Perfil Mii** e aguarde o primeiro download da biblioteca. A origem Internet Archive aparece no criador. Para escolher um arquivo existente, abra **Usar um arquivo local**, depois **Procurar** e **Usar**. Uma biblioteca válida já configurada é reutilizada automaticamente.
2. Digite um nome e toque em **Criar Mii**. Passe pelas abas e toque na opção que quiser: cada botão mostra seu Mii com aquela opção. Em **Corpo** ficam tipo, altura, porte e cor favorita, e a prévia mostra o corpo inteiro (ou só o rosto).
3. Toque em **Salvar Mii**. Na primeira vez o launcher prepara os modelos Mii do jogo; o jogo usa seu Mii na próxima vez que abrir.

**Jogar** informa quando a preparação Mii está em andamento e libera o botão ao concluir. Os recursos já preparados são reutilizados. Você pode manter a Entrada Steam ativada para o Spacewar ao jogar solo ou em uma sala Steam. Na `fix-chore`, um controle virtual inativo da Steam deixa a vaga do jogador disponível para o controle físico detectado pelo GameInput do Windows. Se o seu controle não responder, informe o modelo e o tipo de conexão junto com o relatório de **Suporte → Gerar relatório**.

Você pode ter vários Miis e trocar entre eles no topo. **Perfil Mii** no menu **F1** do jogo traz esta tela à frente.

## Corra com amigos (Steam)

Cada jogador precisa da própria cópia do jogo preparada (mesma versão 4.0.1), de um Mii e da Steam aberta. Deixe o launcher aberto em **Multijogador**: a Steam o mostra como **Spacewar** (AppID 480, o aplicativo de testes da Steam), e é assim que os convites chegam até ele.

1. **Anfitrião:** **Multijogador → Criar sala**, depois **Convidar** ao lado do amigo (ou **Convidar pela Steam**). O jogo do anfitrião abre e cria a sala LAN sozinho.
2. **Convidado:** escolha um ou dois jogadores locais ao entrar na sala de um amigo pelo launcher. Aceitar um convite diretamente pela Steam inicia a sessão com um jogador local. Com a navegação automática ativada e uma compilação compatível, o jogo abre a lista de salas LAN. Para dois jogadores, conclua primeiro as etapas de controle e Mii. Selecione a sala do anfitrião na lista.
3. Todos escolhem personagem e kart; o anfitrião define as regras.

Se a entrada automática parar (ela é experimental e prefere desistir a apertar a coisa errada), continue à mão: no menu principal do jogo segure **L + R** e clique o **analógico esquerdo** para mostrar **LAN Play**. O anfitrião usa **Create Room**; os convidados escolhem a sala do anfitrião em **Lobby**.

Limites conhecidos:

- Um amigo com o launcher fechado vai ver a Steam tentar abrir o Spacewar ao aceitar: abra o launcher e use **Entrar**.
- Um convidado entrando pela Steam a partir de outro PC é o que esta release precisa confirmar.
- O jogo suporta até 12 jogadores em LAN, mas só dois foram testados.

## Desempenho, com contexto

Num **Xeon E5-2666 v3 / 16 GB RAM / RX 580 2048SP**, trechos medidos de uma Mushroom Cup completa:

| Pista | FPS médio | Tempo de quadro p95 |
| --- | ---: | ---: |
| Mario Kart Stadium | 59,99 | 17,46 ms |
| Water Park | 59,98 | 17,45 ms |
| Sweet Sweet Canyon | 59,97 | 17,46 ms |
| Thwomp Ruins | 59,99 | 17,33 ms |

As medições excluem as janelas de captura e não garantem 60 FPS em toda cena nem em outros PCs. Shaders usados pela primeira vez ainda podem engasgar, principalmente quando o conteúdo de outros jogadores aparece pela primeira vez, e **a VRAM ainda cresce entre pistas**. Veja [medições e investigações em aberto](docs/STATUS.md).

## Ajude a melhorar a próxima volta

Em **Suporte → Gerar relatório**, crie o ZIP de diagnóstico e anexe a uma [issue](https://github.com/Vnnc1/MK8-online/issues), com CPU/GPU, versão do launcher, o que você fez e o que aconteceu. Para multijogador, anexe relatórios dos **dois** PCs. Os relatórios incluem logs do jogo, da build e das ferramentas. Na `fix-chore`, incluem também `dlc.log` após uma tentativa de importação do DLC. Caminhos pessoais e nomes de conta são ocultados, e saves e chaves ficam de fora. Revise os anexos antes de compartilhar. Nunca publique ROMs, chaves do console ou arquivos privados.

## Pacote de pistas CTGP Deluxe (opcional, experimental)

O CTGP Deluxe 1.1.1 foi feito para o MK8 3.0.3 (32 bits). A 0.3.0 adapta seus recursos de pistas ao MK8 4.0.1
(64 bits), sem executar o plugin original. Escolha 16 copas adicionais com as 64 pistas ou um layout de substituição,
descrito abaixo. A compatibilidade é incompleta: persistem bugs por pista, defeitos gráficos e quedas de desempenho.

1. Baixe o CTGP Deluxe em [ctgpdx.com](https://www.ctgpdx.com/download) (uso pessoal) e extraia o ZIP.
2. **Instalar → Conteúdo adicional → CTGP Deluxe**: escolha a pasta extraída (a que tem `atmosphere/`) e clique em **Importar CTGP…**.
   No mesmo disco da pasta de dados leva segundos e não ocupa espaço extra (hardlinks).
   Escolha onde ficam as pistas: **no lugar das copas do jogo base** (48 pistas, +16 com o DLC), **no lugar das
   copas do DLC** (48 pistas ao lado das 48 originais; requer o Booster Course Pass importado e ativado) ou **em 16
   copas novas** (as 64 pistas em mais duas páginas da tela de copas, L/R, com nomes, imagens, músicas e ícones
   próprios; nenhuma pista original sai do lugar).
3. **Ativar CTGP Deluxe** liga o pacote; desligue para jogar o jogo original. O jogo, o DLC e os seus saves nunca são
   alterados; com o CTGP ligado o jogo usa saves próprios, criados uma vez como cópia dos seus.

Nas salas da Steam todos precisam do mesmo pacote (o mesmo CTGP ativado ou todos sem ele). O MK8-online não distribui
o CTGP: ele é lido da sua cópia e fica no seu PC. Limites conhecidos: menus próprios e botão aleatório do CTGP
dependem do código dele (indisponível); os fantasmas de staff do contra-relógio são os da pista original; nas copas
novas, cada pista do CTGP guarda os recordes junto com os da pista original cuja vaga ela usa (nos saves do CTGP), e
as sessões online usam as copas do jogo (senão cada máquina decidiria sozinha quais corridas são de copa CTGP). Veja
[STATUS.md](docs/STATUS.md).

## Compilar e contribuir

A build aceita unidades C geradas do MK8 4.0.1 com quebras de linha do Windows (CRLF) ou do Unix (LF). Nos dois casos, os bytes precisam reproduzir o mesmo SHA-256 registrado antes de aplicar os hooks do jogo. O pacote Windows usa x64.

O botão **Info** abre a janela local **How to Play**. **Back to game**, Esc ou fechar a janela retorna ao jogo. O guia cobre botões padrão, drift, itens, assistências e modos; mapeamentos de PC ficam em F1 → Ajustes → Controle. Não exige extrair manual nem conexão à internet. Recompile launcher e runtime para este suporte. O guia inicial está em inglês.

Para experimentar a direção por movimento na 0.4.0, conecte um controle que forneça giroscópio e acelerômetro à SDL. No menu de pausa da corrida, pressione **Y** para ativar os **controles por movimento** (seta curva amarela). Segure o controle como um volante e incline-o para a esquerda ou para a direita. Recompile o runtime uma vez para obter esse recurso.

No Windows, diálogos que usam a fonte compartilhada preferem Segoe UI a Arial quando não há dump das fontes do console nem `LARECOMP_FONT` explícito. Isso evita a aparência alta e estreita do fallback Arial na confirmação de saída da corrida. Continuam sendo fontes substitutas; dumps do console têm prioridade.

Em **Configurações → Fontes do jogo**, **Importar pasta de fontes** copia fontes do seu próprio Switch. Requer `FontStandard.ttf` ou `nintendo_udsg-r_std_003.ttf/.bfttf`; fontes regionais/de botões são opcionais. Nomes do SharedFontDumper são normalizados automaticamente. Os controles de fontes seguem o idioma do launcher: português brasileiro, inglês ou espanhol. Limites TrueType e hashes são conferidos; isso não autentica a origem Nintendo. **Padrão do sistema** desativa sem apagar; **Fontes do console importadas** reativa. Vale no próximo início, sem nova build do jogo após instalar o launcher/runtime com este suporte. Pastas legadas e overrides `LARECOMP_FONT` continuam suportados.

Com homebrew já funcionando: baixe o `.nro` dos [releases do SharedFontDumper](https://github.com/simontime/SharedFontDumper/releases), coloque em `switch/` no SD e abra pelo Homebrew Menu. **A** extrai TTFs individuais; **+** sai após concluir. Copie a pasta `fonts` do SD para o PC e importe-a. Os controles/nomes estão no [fonte](https://github.com/simontime/SharedFontDumper/blob/master/source/main.c). A compatibilidade desta ferramenta antiga com firmware/homebrew atuais não foi testada aqui. `shared_font.bin` e arquivos de firmware não são aceitos.

O desenvolvimento acontece na `fix-chore`. São necessários Rust, Python 3.12 x64, PowerShell 7 e .NET 10 x64 (para o importador). Na raiz do repositório:

```powershell
.\scripts\start-launcher.ps1
```

O script baixa as fontes upstream fixadas, confere os hashes, compila o launcher e o abre. Para empacotar o launcher portátil: `python scripts/package-launcher.py` (saída: `target/portable/Launcher.exe`). Os passos de release estão em [RELEASING.md](docs/RELEASING.md).

Para gerar um instalador local de teste sem publicar uma release, execute `python scripts/release.py`. O setup fica em `<diretório target do Cargo>/dist/MK8-online-<versão>-setup.exe`. Instale manualmente builds de teste com a mesma versão; o atualizador só oferece versões mais novas. O pacote inclui o novo design padrão e as operações reais de instalação/build, sem o storyboard debug.

Mantenha as adaptações em `runtime/mk8` e `toolchain`; a preparação as aplica a uma cópia verificada. Não edite os snapshots upstream nem o código gerado do jogo. Siga o [AGENTS.md](AGENTS.md) e rode os testes da área alterada. Caminhos só de desenvolvimento estão documentados no [estado técnico](docs/STATUS.md):

- transportes LAN/relay/P2P pela API;
- o conversor estático de cabeça Mii.

Para usar o Nextendo, atualize a build do jogo e selecione **Jogar → Jogar no Nextendo**. No primeiro acesso, o launcher mostra a impressão digital da chave pública do servidor de corrida e pergunta se você confia nela neste PC. Os demais serviços de jogo usam a autoridade certificadora do Nextendo e a verificação do nome do servidor. Faça login pelo navegador e abra **Online Play** no MK8. Na versão de teste, a autorização fica na memória enquanto o launcher estiver aberto. O CTGP é desativado nessa sessão. A reconexão continua experimental.

Se o Nextendo retornar `discord_unlinked` após o login, vincule o Discord à mesma conta Nextendo e selecione **Jogar no Nextendo** novamente.

| Local | Responsabilidade |
| --- | --- |
| `apps/launcher` | Launcher nativo, interface web, instalador, atualizações e relatórios |
| `crates/installer`, `crates/manifest`, `crates/build-manager` | Instalação, validação, Miis e supervisão de builds |
| `crates/session`, `crates/transport` | Salas, Steam, relay e transporte autenticado |
| `toolchain`, `runtime/mk8` | Pipeline de recompilação e adaptações do runtime do MK8 |
| `games/mk8d` | Perfis de versão, símbolos e configuração do jogo |
| `scripts`, `tests` | Preparação, empacotamento e testes de regressão |
| `upstream/manifests` | Manifestos das fontes fixadas; os snapshots baixados ficam fora do Git |
| `local`, `target`, `prebuilt` | Dados privados e artefatos gerados, ignorados pelo Git |

### Configurações do jogo

Os avisos do launcher aparecem em um painel fixo no topo da janela, visível durante a rolagem e ao trocar de aba. Use **Fechar aviso** (×) para dispensá-los. A entrada tem uma animação curta, desativada quando o sistema solicita redução de movimento.

As opções gerais ficam em **Configurações**: vídeo, áudio, idioma do jogo, ícones dos botões, posição dos controles e mapeamento de teclado/controle. O launcher usa o mesmo `user-no-mii/mk8d.ini` do menu F1. Clique em **Salvar configurações** para aplicar no próximo início do jogo; **Recarregar do jogo** recupera os valores gravados pelo menu F1. Alterações não salvas permanecem na tela durante as atualizações de estado. Durante o salvamento e a leitura de confirmação, os campos e os botões Salvar/Recarregar ficam bloqueados; uma falha libera a edição sem descartar os valores pendentes. Comentários e opções não editadas são preservados.

As configurações não podem ser salvas enquanto o jogo ou uma preparação usa os dados. Os mapeamentos são selecionados em listas; atribuir uma tecla ou botão já usado troca os dois destinos, como no menu F1. Ações de conta, instalação de mods e saída continuam no menu do jogo. Salas continuam em **Multijogador**.

### Conteúdo adicional no launcher

Após preparar o jogo, abra **Instalar → Conteúdo adicional**. Selecione o NSP ou NSZ do Booster Course Pass v65536 e, quando necessário, as chaves do seu console; clique em **Importar DLC NSP/NSZ…**. O launcher mostra o estado da operação e seu log. Uma build antiga precisa ser atualizada antes de habilitar esses controles.

O controle **Ativar Booster Course Pass** fica em **Configurações → Conteúdo adicional** e vale ao iniciar o jogo. Importação e reparo permanecem em **Instalar**. Desativar mantém os arquivos importados e os saves. A preferência é salva por pasta de dados e permanece após reiniciar o launcher ou reconstruir o jogo. Importação e alterações ficam bloqueadas enquanto o jogo ou outra preparação usa os dados. Uma instalação inválida oferece **Reparar importação…**; a substituição só acontece após validar os novos arquivos, preservando a pasta anterior em um backup local.

Mesmo com o DLC instalado, **Instalar → Conteúdo adicional → Reinstalar ou selecionar outro NSP/NSZ** permite escolher novamente o pacote e as chaves. **Reinstalar DLC…** valida e extrai o pacote selecionado, sem reutilizar automaticamente a instalação existente. Os arquivos atuais só são substituídos após a validação; ficam preservados em um backup local. A preferência de ativação e os saves são mantidos. Apenas o pacote do perfil verificado continua aceito.

Os campos de NSP e chaves mostram contorno verde e marca de confirmação quando os arquivos são encontrados, como na etapa 2. O campo de chaves também considera a configuração já salva e a detecção automática. Essa indicação confirma a presença dos arquivos; o conteúdo do pacote e as chaves continuam sendo validados durante a importação.

O perfil aceito continua restrito ao Booster Course Pass verificado (`0100152000023001`, versão de conteúdo 65536) com o MK8 4.0.1 AArch64. Para testes por linha de comando, use uma pasta de dados isolada já preparada, seu NSP adicional e as chaves do seu console:

```powershell
.\local\python\Scripts\python.exe toolchain/import_dlc.py --package 'D:\Switch\BoosterCoursePass.nsp' --keys 'D:\Switch\prod.keys' --local 'E:\MK8-TESTE'
.\local\python\Scripts\python.exe toolchain/build.py --config 'E:\MK8-TESTE\config.json' --local 'E:\MK8-TESTE' --runtime-only
```

A importação confere os hashes dos NCAs e a associação do CNMT ao jogo, usa o ticket e as chaves para descriptografar o conteúdo e verifica os arquivos extraídos. O runtime oferece o índice instalado e monta esses arquivos reais; arquivos ausentes, vazios ou alterados são recusados. O comando de runtime reaproveita a biblioteca recompilada do jogo. Ainda não há alternância durante uma sessão do jogo, remoção automática nem suporte genérico a outros pacotes ou versões. Evidências e limites ficam em [STATUS.md](docs/STATUS.md).

## Créditos e licença

**O MK8-online é criado e mantido por RatOne.** Os autores upstream mantêm o crédito e a autoria das suas contribuições. Atribuições e avisos de dependências estão em [THIRD_PARTY.md](docs/THIRD_PARTY.md) e [docs/licenses](docs/licenses). O criador de Mii desenha os rostos com o [FFL.js](https://github.com/ariankordi/FFL.js) (AGPL-3.0).

O código do projeto é licenciado sob **[GPL-3.0-or-later](LICENSE)**. As dependências mantêm suas licenças:

- o snapshot do SMM2Recomp inclui GPLv3;
- o LASRecomp inclui avisos MIT;
- o setup leva o FFmpeg (LGPL 2.1), com o código-fonte anexado a cada release.

A licença não concede direitos sobre o código, os recursos ou as marcas da Nintendo.

Projeto comunitário independente, sem afiliação nem endosso da Nintendo ou da Valve. Mario Kart e nomes relacionados pertencem aos respectivos donos.

A renderização condicional aguarda reports de oclusão pendentes nos dois operandos antes de comparar as contagens reais. Corrige desaparecimentos transitórios nos previews, preservando resultados legítimos de objetos ocultos. A preparação aplica o patch; é necessário reconstruir o jogo/runtime.

O tradutor de shaders Maxwell passou a respeitar a interpolação declarada pelo jogo para entradas de fragmento. Isso reduziu a cintilação no trecho testado de Boo Lake em tela dividida. Recompile o runtime para aplicar a correção.

Se os emblemas das copas e os números da largada aparecem azuis na 0.3.0, o runtime da 0.4.0 corrige as cores dessas texturas RGB565. Recompile o jogo com o runtime atualizado para aplicar a correção.
