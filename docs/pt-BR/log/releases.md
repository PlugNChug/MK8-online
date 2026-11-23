# Registro técnico — Versões e distribuição

[Índice](README.md) · [English](../../en/log/releases.md)

## Notas que estavam no README (até a 0.5.0)

O README passou a ser só a apresentação do projeto. As notas de versão, correções e instruções que ele acumulava foram movidas para cá sem alteração; o uso atual está em `docs/GUIDE.pt-BR.md`.

> [!IMPORTANT]
> Use seus próprios arquivos do jogo e as chaves do seu console. O repositório e as releases não incluem ROMs, recursos do jogo, chaves, saves ou código recompilado do jogo. A compilação acontece localmente, a partir dos seus arquivos.

> [!NOTE]
> **Baixe a versão 0.5.0** · [Instalador Windows](https://github.com/Vnnc1/MK8-online/releases/download/v0.5.0/MK8-online-0.5.0-setup.exe) · [ZIP portátil Windows](https://github.com/Vnnc1/MK8-online/releases/download/v0.5.0/MK8-online-0.5.0-portable.zip) · [AppImage Linux amd64](https://github.com/Vnnc1/MK8-online/releases/download/v0.5.0/MK8-online-0.5.0-linux-x86_64.AppImage)
>
> No Windows, mantenha a pasta de dados e conclua **Instalação → Ferramentas → Build do jogo** se solicitado. No Linux, o launcher e o jogo recompilado executam nativamente. O guia explica como instalar as dependências no Ubuntu 24.04 e como experimentar no modo desktop do Steam Deck. [Guia Linux](../LINUX.md) · [Notas da versão](../../releases/0.5.0.md)

O instalador Windows 0.5.0 permite usar arquivos do RomFS cujo caminho no Windows chega a 260 caracteres ou mais, inclusive com acentos e emoji. Se a preparação anterior parou com a mensagem `MAX_PATH` do RomFS, mantenha a mesma pasta de dados, instale o launcher atualizado e tente novamente em **Instalação → Build do jogo**. A compilação aproveita as etapas já concluídas.

### Windows 0.5.0 · Jogue com seu Mii

Na 0.5.0, o solo, as salas Steam e o Nextendo usam o Mii ativo. Na primeira partida, dê um nome ao Mii, personalize e escolha **Salvar e continuar** para seguir no modo escolhido. Para o Nextendo, confirme o e-mail e vincule o Discord à conta. Autorize uma vez no navegador enquanto o launcher estiver aberto. Ao abrir, o jogo mostra a conta Nextendo e o Mii local. Fechar o launcher ou revogar o acesso exige um novo login. [Gerenciar conta Nextendo](https://nextendo.network/account)

### Windows 0.5.0 · Opções gráficas

No launcher 0.5.0, abra **Jogar → Gráficos** ou **Configurações → Vídeo e controles → Imagem**. Se Instalação oferecer **Atualizar a build**, conclua essa etapa primeiro, inclusive ao atualizar uma instalação de teste da 0.4.1. Escolha uma resolução interna menor, como **75%**, e selecione **FSR 3.1.6**, **FSR 4.1.2** (RDNA 4 compatível) ou **DLSS Super Resolution** (RTX compatível). Leia e aceite os termos do fabricante no launcher para instalar o componente opcional. Salve e inicie o jogo com o runtime atualizado. O painel mostra o método e as dimensões informados pelo jogo. A resolução interna pode ser arredondada para se ajustar aos alvos de renderização.

No jogo, abra **F1 → Ajustes → Vídeo** para escolher a ampliação integrada ou um provedor instalado e disponível neste PC. Todos os temas do Menu usam a mesma lista. Para carregar outro provedor temporal, reabra o jogo pelo launcher. Com o launcher aberto, as alterações feitas no jogo também aparecem em Configurações e no seletor de idioma da tela Jogar.

FSR 1 e bilinear já vêm disponíveis. Se o provedor recusar o recurso, o jogo usa a ampliação integrada. A opção separada de geração de quadros usa a interpolação própria do runtime. Ative o contador para comparar os FPS do jogo, os FPS de saída e os milissegundos por quadro do jogo.

### Upscaling e frame generation (opcional)

Nada de propriedade de fornecedor vai nos pacotes. O launcher baixa cada componente da fonte/release oficial fixada
pelo próprio fornecedor depois que você aceita os termos, confere tamanho e SHA-256 e, quando não existe binário
para Linux, compila localmente a partir das fontes do fornecedor ou deste projeto:

- **AMD FSR** — no Linux, upscaling FSR 3.1.6 e 4.1.2 e frame generation FSR 3 / FSR 4 (MLFI) são compilados
  localmente a partir do SDK FidelityFX MIT da AMD e do port GPL-3.0 de FSR 4.1.2 deste projeto. No Windows, as DLLs
  Vulkan assinadas da AMD são baixadas e verificadas. Nenhum binário, shader ou modelo da AMD é redistribuído.
- **NVIDIA DLSS** — Super Resolution e Frame Generation carregam os plugins NGX oficiais para Linux em tempo de
  execução (exigem GPU e driver RTX). DLSS 5 Neural Rendering não tem plugin público e não está disponível.
- **Intel XeSS** — somente Windows, porque a Intel não publica biblioteca para Linux. Super Resolution está
  implementado e verificado sob Wine; frame generation XeSS e XeLL são exclusivos de D3D12.

Uma guarda de release falha o build se qualquer biblioteca de provider, banco de modelos ou blob de shader gerado
entrar nos pacotes.

### Novidades da 0.4.0

**Multijogador online nativo com o Nextendo.** Selecione **Jogar → Jogar no Nextendo** no launcher, entre na sua conta pelo navegador e abra **Online Play → Mundial → Corrida** no Mario Kart para jogar com outras pessoas. Pressione **F1** dentro do jogo para consultar salas e amigos. As sessões Nextendo usam as pistas originais.

A edição portátil pode ser movida entre unidades do Windows. Com o jogo preparado, `Launcher.exe --play` abre o modo solo diretamente. Os controles ganharam direção por movimento, vibração HD e a opção de usar cada Joy-Con na horizontal no multijogador local. Saves e replays temporários do Grand Prix ficam separados, e o troféu da Copa Casco permaneceu após reiniciar nos testes. As correções gráficas abrangem as cores das copas, a contagem da largada e a iluminação em tela dividida. Ao atualizar, mantenha sua pasta de dados e siga **Instalação → Ferramentas → Build do jogo**.


Vozes de áudio novas começam com volume anterior zero e usam a rampa existente no primeiro bloco do mixer. O patch aborda estalos em efeitos repetidos da pista e exige reconstruir o jogo/runtime; outros sons e a qualificação completa de áudio permanecem em aberto.

**Atribuição de controles nas builds de desenvolvimento.** Na tela de título, pressione um botão no controle que deseja usar como jogador 1. Uma tecla, um clique ou um toque na tela seleciona o teclado. Pressione **SL** ou **SR** para usar um Joy-Con na horizontal. Com apenas um Joy-Con esquerdo e um direito, um botão frontal seleciona o par. Com várias metades conectadas, segure **L** no esquerdo e **R** no direito que deseja combinar e solte os dois. Volte ao título para escolher de novo.

Para jogar com teclado, Pro Controller e dois Joy-Con na horizontal, pressione **K** na tela de título para usar o teclado como jogador 1 ou **A** no Pro Controller para assumir essa posição. Na seleção de personagens, cada Joy-Con na horizontal entra com **SL + SR**. Um teclado disponível entra com **Enter**.

Em corridas locais com tela dividida, o jogador que desconecta fica sem comandos e os demais podem continuar correndo. Ao reconectar, um controle identificável volta ao mesmo jogador, mesmo que o Windows liste os dispositivos em outra ordem. Um par de Joy-Con volta a responder quando as duas metades estão conectadas. Se não for possível distinguir dois controles iguais, registre-os de novo na seleção de personagens.

Se faltar um controle ao entrar na seleção de personagens, a tela **Controller assignment** destaca a primeira vaga. Se apenas o jogador 1 já estiver registrado, a seleção começa no jogador 2 e avança conforme cada participante entra. O rodapé mostra os ícones do jogo para **L + R** no controle e **SL + SR** em um Joy-Con na horizontal. Pressione os dois botões ao mesmo tempo para entrar ou use **Enter** se o teclado estiver disponível. Quem já entrou mantém a posição. Enquanto houver uma vaga, o botão central **Cancel**, a tecla **Esc** ou o botão Voltar do jogador 1 cancela o registro. Quando todos entrarem, o botão muda para **OK** e o jogador 1 confirma para continuar. Builds antigas e mods com um layout de menu compartilhado próprio podem exibir o aviso de texto com os mesmos comandos.

O menu de controles funciona na seleção de personagens com qualquer tema do Menu. Com dois a quatro jogadores locais, use **+** no controle do jogador 1 ou a tecla atribuída a Start. No multijogador local também é possível clicar ou tocar em **−/+**. Na LAN com dois jogadores, esse botão fica oculto. Selecione uma linha para atribuir um controle livre ou trocar dois jogadores de posição, ou limpe as linhas e registre os jogadores por ordem de entrada. Se outro jogador ativo já usar o controle, um aviso identifica quem o está usando. **Apply** aplica as atribuições e **Cancel** mantém as anteriores. Durante corridas e batalhas em tela dividida, o atalho das configurações exibe um aviso breve para que todos possam continuar jogando. As configurações continuam disponíveis nos menus e durante a corrida com um único jogador local.

Para separar um par em **Controller assignment**, selecione o jogador que vai receber um Joy-Con, escolha **Assign controller** e segure **SL + SR** nessa metade. O jogador que usava o par fica com a outra metade na horizontal. Escolha **Apply** para aplicar a troca ou **Cancel** para manter o par. Para combinar duas metades disponíveis, segure **L** no Joy-Con esquerdo e **R** no direito e solte os dois.

O Pro Controller original do Switch, os Joy-Con individuais e um único par identificável de Joy-Con oficiais via Bluetooth têm vibração HD. No modo combinado, o jogo controla a frequência e a intensidade de cada lado separadamente. No modo horizontal, cada Joy-Con recebe os dois canais de vibração daquele jogador. Com o giroscópio ativado em um controle Nintendo, o runtime aguarda uma nova leitura desse controle antes de enviar o próximo efeito HD ativo. Isso evita que envios repetidos de vibração interrompam a leitura dos controles. Os demais controles compatíveis usam a vibração da SDL. Abrir as configurações ou trocar de janela interrompe a vibração.


No Windows, a opção **60 FPS em tela dividida**, em Vídeo, permite atualizar todas as vistas a cada quadro nas corridas locais com três ou quatro jogadores. Ela vem desligada. Salve a escolha antes de iniciar a corrida. Resoluções internas mais altas podem reduzir a taxa de quadros. Quem já preparou o jogo deve recompilá-lo por completo para atualizar o código da corrida.

No Windows, **Vídeo → Bloom** controla o halo em torno das superfícies luminosas. A opção vem ligada. Ao desligá-la, as próprias luzes continuam visíveis. Uma instalação preparada anteriormente precisa atualizar o runtime para aplicar a preferência.

A versão 0.4.0 acrescenta **Ultrawide (experimental)** às configurações de vídeo do launcher e do menu dentro do jogo. Clique em **Salvar configurações** no launcher antes de iniciar. A opção amplia câmeras solo suportadas de Contra o Relógio, Grand Prix e VS sem equipes entre 16:9 e 32:9, preserva o enquadramento vertical e mantém o HUD proporcional nas bordas. As vistas suportadas incluem direção frontal/traseira nesses modos solo, sobrevoos GP/VS e close-ups iniciais GP/VS/Contra o Relógio. Original 16:9 e Esticar continuam disponíveis. Cenas/modos sem suporte, tela dividida e geração de quadros usam 16:9; câmeras online e de replay não recebem suporte ultrawide. Reconstrua o jogo, incluindo as unidades guest afetadas, para aplicar.

Após recompilar o launcher e o jogo para Windows a partir desta branch, perfis novos começam com o tema **Nativo**. Em **Vídeo → Tema do menu**, também é possível escolher **Simples escuro**, **Simples claro**, **NES escuro** ou **NES claro**. O launcher mostra uma captura dos ajustes rápidos para cada opção. Clique em **Salvar configurações** para usar o tema na próxima vez que iniciar o jogo. No jogo, Tema do menu é a primeira opção em Vídeo. Escolha um tema, feche o Menu e abra-o de novo para ver a mudança. Perfis existentes mantêm a aparência escolhida. O antigo tema Padrão passou a se chamar Simples escuro, e o NES passou a se chamar NES escuro. Perfis que ativaram a opção antiga do menu nativo continuam usando o Nativo.

A chave inglesa do menu principal, F1, Home/Guide e Menos + Mais abrem o Menu. Shift+F1 abre o painel convencional.

Pressione F1 durante o carregamento de uma pista no modo solo para abrir o Menu nativo. Você pode continuar ajustando as opções quando a apresentação da pista começar. A linha selecionada e o valor que estiver digitando são preservados. Se você pressionar F1 na inicialização, o Menu abrirá assim que a interface do jogo estiver pronta.

No Windows, a opção **Distância da câmera de pilotagem**, em Vídeo, ajusta a vista frontal e a traseira no Contra o Relógio, Grand Prix e VS solo sem equipes. A distância original é 100%. Use as setas para mudar de dez em dez pontos percentuais ou digite um número inteiro de 75 a 150. Valores maiores afastam a câmera do kart. O close-up inicial mantém a distância original e, quando começa a pilotagem, a câmera passa gradualmente para a distância escolhida. O jogo continua verificando a colisão com paredes. Salve a configuração no launcher ou ajuste-a no jogo com F1. Recompile o jogo e o runtime juntos para usar a opção.

**CTGP Deluxe e o menu de configurações.** O relançamento corrige o crash ao abrir o menu principal com as duas opções ligadas. Atualize a build para o launcher regenerar o layout do menu a partir dos arquivos do jogo.

#### Instalação e runtime

A versão 0.4.0 restaura a sombra ausente do banner de largada de Rock Rock Mountain, respeitando o estado de depth clamp solicitado pelo jogo. Reconstrua o runtime para aplicar o fix. Condições verificadas no [estado técnico](README.md).

O instalador pode abrir o launcher na mesma janela após a instalação. A opção vem marcada, e o launcher usa a pasta de dados escolhida durante a instalação, inclusive em outro disco.

Se uma atualização exigir a reinstalação das ferramentas, a tela Instalação abre em **Ferramentas**. Após a verificação, ela segue para **Build do jogo** quando os arquivos estão prontos, ou para **Arquivos do jogo** se faltar alguma entrada. A instalação bem-sucedida remove as cópias obsoletas de ferramentas reconhecidas nas versões anteriores.

Abra **Uninstall.exe**, ao lado de **Launcher.exe**, para desinstalar. A opção padrão guarda saves, Miis, configurações e o jogo preparado. Para reutilizá-los, escolha a mesma pasta de dados na reinstalação. A remoção completa mostra a pasta de dados e exige confirmação.

O runtime da 0.4.0 inclui um ajuste Vulkan para a GTX 1070 desktop (dispositivo PCI `10de:1b81`). Consultas elegíveis de tamanho de textura constante são movidas para fora de ramificações de shaders de fragmento. As demais GPUs seguem pelo caminho atual. Recompile o runtime para aplicar o ajuste. A validação está no [estado técnico](README.md).

**Inicialização direta:** `Launcher.exe --play` abre o jogo solo já preparado sem a janela do launcher. Para selecionar seus dados: `Launcher.exe --play --data-dir "D:\\MK8\\data"`. Mantém validações, bloqueios e logs; não instala ferramentas nem recompila. Salas Steam continuam pelo launcher.

**Modo portátil Windows:** extraia `MK8-online-0.5.0-portable.zip` e mantenha `portable.txt` ao lado do executável. Fontes, ferramentas e dados ficam em `portable/`, sem executar setup. Para atualizar, substitua o executável e preserve essa pasta; atualização por instalador fica desativada. `--data-dir` tem prioridade para a pasta de dados. Não migra instalações existentes automaticamente.

**Mudança de pasta/unidade:** abra esta versão uma vez antes de mover, feche o jogo e o launcher e copie/mova a pasta inteira, incluindo `portable/`. Na próxima abertura, caminhos internos do jogo, builds, Miis, fontes e Python são ajustados automaticamente. Os metadados anteriores ficam em `portable/.relocation-backups/`; saves e arquivos binários do jogo não são reescritos. Recursos selecionados fora da pasta portátil continuam externos: mantenha dentro dela o que precisar levar. O ZIP portátil requer Windows x64 e drivers Vulkan compatíveis. No Linux, use o AppImage indicado acima.


Em **Jogar**, volte à partida ou abra o Perfil Mii. Em **Nextendo**, consulte as salas e a lista de amigos com a presença de cada um. Selecione uma sala para ver os detalhes e use Voltar para retornar à lista. Para disputar uma corrida, use **Online Play** no Mario Kart.

#### Recursos disponíveis desde a 0.3.0

- **CTGP Deluxe no MK8 4.0.1:** importe os recursos das pistas do CTGP Deluxe 1.1.1 (originalmente para 3.0.3), incluindo 64 pistas em 16 copas adicionais com nomes, imagens, ícones e músicas. A adaptação não executa o plugin original; a compatibilidade é incompleta e ainda há bugs.
- **Launcher redesenhado:** instalação horizontal em etapas, configurações em módulos, idioma do jogo acessível, criação guiada de Mii, amigos/salas Steam e relatórios de suporte mais claros.
- **DLC opcional:** importe seu Booster Course Pass na preparação ou depois e ative/desative sem excluí-lo. Jogo, DLC e chaves nunca são distribuídos.
- **Correções e melhorias:** congelamento ao carregar recursos Mii, óculos, verificação Vulkan, memória/texturas, miniaturas MKTV, manual local, fontes, shaders de partículas, áudio, seleção de controles e esperas de shaders na tela dividida em dois jogadores.
- **Configurações e apresentação:** leitura CRLF do INI corrigida. A geração experimental de quadros pode apresentar cerca de 120 FPS nos trechos medidos, mantendo a simulação em 60 Hz; continua desligada por padrão.

O histórico do CTGP e do redesenho do launcher está nas [notas da 0.3.0](../../releases/0.3.0.md). Para as limitações da versão atual, consulte o [estado técnico](README.md).

### Onde estamos

**Correções no Windows na 0.5.0.** Recompilar o runtime do Windows habilita caminhos com caracteres não ASCII para o jogo, os saves e as configurações. Se o jogo fechar com erro, o launcher mostra a última mensagem fatal disponível e guarda um ZIP de diagnóstico em **Suporte**. Isso também funciona com `Launcher.exe --play`. A integração Steam tenta a próxima DLL instalada quando uma candidata carrega, mas falha ao inicializar as interfaces necessárias. O launcher também aceita o Booster Course Pass v65536 verificado em NSP ou NSZ.

Recompile o jogo depois de instalar o instalador 0.5.0 para aplicar a correção da votação de pistas quando faltam modelos Mii. Durante a preparação do jogo, o gerador agora tenta novamente se o Windows bloquear temporariamente a renomeação de um módulo concluído.

**Apresentação a 120 FPS:** a opção experimental de geração de quadros já existente interpola uma imagem entre quadros reais, mantendo o jogo a 60 Hz. Exige monitor de alta frequência e folga na GPU; não desbloqueia a simulação nativa nem elimina engasgos de shaders. A investigação local mediu cerca de 120 quadros apresentados por segundo em um trecho solo estável com RX 580 2048SP e monitor a 180 Hz. Isso não qualifica uma corrida completa ou multiplayer. O runtime da 0.3.0 também corrige a leitura de INI com CRLF, que podia desativar silenciosamente `frame_gen = 120` e interpretar outras opções textuais incorretamente. Detalhes e limites em [STATUS.md](README.md).

As correções de desempenho da 0.3.0 usam o temporizador de alta resolução do Windows nas esperas limitadas de shaders, aplicam esse orçamento à cena de tela dividida de dois jogadores e cancelam a configuração de controles quando faltam jogadores. Recompile o jogo/runtime para aplicá-las. Na versão 0.3.0, para teclado e um controle no multijogador local, ative **Controles começam no jogador 2** em Vídeo e controles. A primeira compilação de shaders ainda pode causar pausas; essas correções não qualificam todas as pistas CTGP nem o multiplayer entre PCs.

A versão 0.3.0 integra as PRs #27–31 e #33: guia local, leitura de miniaturas dos replays, lifetime de texturas, suporte limitado de shader, fontes privadas e melhorias Steam/Mii/DLC. O commit `8202c32` também integrou a fase B do CTGP (copas adicionais e seus hooks de guest). Verificações da integração e limites estão em [STATUS.md](README.md).

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

### O que este projeto faz

O pipeline traduz o executável AArch64 do jogo para C e o compila para Windows x64 ou Linux amd64 nativo. Um runtime C/C++ fornece gráficos, áudio, entrada e serviços de sistema; o launcher em Rust/Tauri cuida da preparação, builds, atualizações, Miis e salas.

RatOne desenvolve e mantém a adaptação do MK8, o launcher e a integração. A base de tradução/runtime vem do **SMM2Recomp e do LASRecomp**, com outros trabalhos creditados. Veja [fontes de terceiros e licenças](../THIRD_PARTY.md).

### Entre na pista

#### Requisitos no Windows

- **Windows x64** e WebView2. O launcher instala sozinho um **Python 3.12 x64** privado.
- GPU e driver com **Vulkan 1.3 ou mais novo**. Não há especificação mínima geral de CPU/GPU qualificada.
- Seu próprio jogo base, a **atualização 4.0.1** e o `prod.keys` do seu console, ou uma pasta do jogo já extraída e suportada.
- Cerca de **28 GiB livres** para extração e preparação (incluindo 4 GiB de reserva de segurança), além de espaço para ferramentas e caches. SSD recomendado.
- Internet para o primeiro download das ferramentas (cerca de 0,4 GiB) e para atualizações.
- **Para Miis:** ao abrir o criador, o launcher baixa `AFLResHigh_2_3.dat` de uma cópia do antigo CDN Miitomo no Internet Archive. Confere tamanho e SHA-256 do ZIP e da biblioteca, depois guarda o arquivo na pasta de dados para uso offline. O arquivo não acompanha o GitHub nem o instalador. Se o arquivo online estiver indisponível, tente novamente ou selecione uma cópia local compatível; sem a biblioteca, as opções ficam numeradas e os rostos não são desenhados.
- **Para multijogador:** entre na Steam e use uma DLL Steamworks compatível de 64 bits de um jogo já instalado. Nesta branch, o launcher tenta primeiro as DLLs configuradas e locais e procura nas bibliotecas Steam se nenhuma fornecer as funções necessárias. Se o launcher informar que falta `SteamAPI_SteamFriends_v017`, aponte `MK8_STEAM_DLL` para o caminho completo de uma `steam_api64.dll` compatível, reabra o launcher e entre em **Multijogador** para conferir a lista de amigos. A cópia testada de Brawlhalla funcionou na 0.3.0. As cópias testadas de Counter-Strike 2 e Deadlock exportam `v018` e são recusadas pela 0.3.0. Nenhum arquivo da Valve é distribuído.

#### Instalar e jogar

1. Baixe `MK8-online-0.5.0-setup.exe` em [Releases](https://github.com/Vnnc1/MK8-online/releases) e escolha as pastas do aplicativo e dos dados. A instalação é por usuário, sem administrador. Já tem uma versão anterior? Use **Suporte → Atualizar agora**: a pasta de dados e os saves são mantidos.
2. Abra **Instalar**. Conclua **Ferramentas** e selecione o jogo base e a atualização em **Arquivos do jogo** (o launcher encontra as chaves junto do jogo, ou selecione as suas).
3. Rode a etapa **Build do jogo**. Depois de atualizar de uma versão anterior, rode de novo: o jogo é recompilado com o runtime novo.
4. Escolha **Jogar solo**.

A etapa **Ferramentas** do launcher prepara o Python privado diretamente, sem PowerShell ou carregamento de ZIP pelo .NET. O CPython vem do pacote oficial NuGet fixado; o arquivo e as dependências são conferidos por hash. Ambientes interrompidos são preservados antes do reparo. Mantenha a proteção do antivírus ativa.

Uma pasta extraída precisa conter `exefs`, `romfs` e `control.nacp` (ou `control/control.nacp`): use **Já tenho o jogo extraído numa pasta**. O perfil ativo é o 4.0.1 AArch64; outras versões do jogo e importação de XCI/XCZ não foram validadas. O DLC suportado é importado separadamente.

A importação direta de NSP/NSZ aceita duas variantes verificadas do programa base, mantendo a conferência integral dos hashes e da atualização 4.0.1. Outros conteúdos base continuam sem suporte; nesse caso, use uma pasta extraída compatível. Os detalhes da validação estão no [estado técnico](README.md).

Os dados ficam por padrão em `%LOCALAPPDATA%\MK8-online` e sobrevivem a atualizações e à desinstalação. Para usar outro disco com o launcher portátil: `.\Launcher.exe --data-dir E:\MK8-Local`, e continue usando a mesma pasta.

**Idioma:** o launcher e o instalador oferecem **English**, **Português (Brasil)** e **Español** (**Language / Idioma**). O idioma do jogo é independente.

#### Crie seu Mii

1. Abra **Perfil Mii** e aguarde o primeiro download da biblioteca. A origem Internet Archive aparece no criador. Para escolher um arquivo existente, abra **Usar um arquivo local**, depois **Procurar** e **Usar**. Uma biblioteca válida já configurada é reutilizada automaticamente.
2. Digite um nome e toque em **Criar Mii**. Passe pelas abas e toque na opção que quiser: cada botão mostra seu Mii com aquela opção. Em **Corpo** ficam tipo, altura, porte e cor favorita, e a prévia mostra o corpo inteiro (ou só o rosto).
3. Toque em **Salvar Mii**. Na primeira vez o launcher prepara os modelos Mii do jogo; o jogo usa seu Mii na próxima vez que abrir.

**Jogar** informa quando a preparação Mii está em andamento e libera o botão ao concluir. Os recursos já preparados são reutilizados. Você pode manter a Entrada Steam ativada para o Spacewar ao jogar solo ou em uma sala Steam. Na `fix-chore`, um controle virtual inativo da Steam deixa a vaga do jogador disponível para o controle físico detectado pelo GameInput do Windows. Se o seu controle não responder, informe o modelo e o tipo de conexão junto com o relatório de **Suporte → Gerar relatório**.

Você pode ter vários Miis e trocar entre eles no topo. **Perfil Mii** no menu **F1** do jogo traz esta tela à frente.

### Corra com amigos (Steam)

Cada jogador precisa da própria cópia do jogo preparada (mesma versão 4.0.1), de um Mii e da Steam aberta. Deixe o launcher aberto em **Multijogador**: a Steam o mostra como **Spacewar** (AppID 480, o aplicativo de testes da Steam), e é assim que os convites chegam até ele.

1. **Anfitrião:** **Multijogador → Criar sala**, depois **Convidar** ao lado do amigo (ou **Convidar pela Steam**). O jogo do anfitrião abre e cria a sala LAN sozinho.
2. **Convidado:** escolha um ou dois jogadores locais ao entrar na sala de um amigo pelo launcher. Aceitar um convite diretamente pela Steam inicia a sessão com um jogador local. Com a navegação automática ativada e uma compilação compatível, o jogo abre a lista de salas LAN. Para dois jogadores, conclua primeiro as etapas de controle e Mii. Selecione a sala do anfitrião na lista.
3. Todos escolhem personagem e kart; o anfitrião define as regras.

Se a entrada automática parar (ela é experimental e prefere desistir a apertar a coisa errada), continue à mão: no menu principal do jogo segure **L + R** e clique o **analógico esquerdo** para mostrar **LAN Play**. O anfitrião usa **Create Room**; os convidados escolhem a sala do anfitrião em **Lobby**.

Limites conhecidos:

- Um amigo com o launcher fechado vai ver a Steam tentar abrir o Spacewar ao aceitar: abra o launcher e use **Entrar**.
- Um convidado entrando pela Steam a partir de outro PC é o que esta release precisa confirmar.
- O jogo suporta até 12 jogadores em LAN, mas só dois foram testados.

### Desempenho, com contexto

Num **Xeon E5-2666 v3 / 16 GB RAM / RX 580 2048SP**, trechos medidos de uma Mushroom Cup completa:

| Pista | FPS médio | Tempo de quadro p95 |
| --- | ---: | ---: |
| Mario Kart Stadium | 59,99 | 17,46 ms |
| Water Park | 59,98 | 17,45 ms |
| Sweet Sweet Canyon | 59,97 | 17,46 ms |
| Thwomp Ruins | 59,99 | 17,33 ms |

As medições excluem as janelas de captura e não garantem 60 FPS em toda cena nem em outros PCs. Shaders usados pela primeira vez ainda podem engasgar, principalmente quando o conteúdo de outros jogadores aparece pela primeira vez, e **a VRAM ainda cresce entre pistas**. Veja [medições e investigações em aberto](README.md).

### Ajude a melhorar a próxima volta

Em **Suporte → Gerar relatório**, crie o ZIP de diagnóstico e anexe a uma [issue](https://github.com/Vnnc1/MK8-online/issues), com CPU/GPU, versão do launcher, o que você fez e o que aconteceu. Para multijogador, anexe relatórios dos **dois** PCs. Os relatórios incluem logs do jogo, da build e das ferramentas. Na `fix-chore`, incluem também `dlc.log` após uma tentativa de importação do DLC. Caminhos pessoais e nomes de conta são ocultados, e saves e chaves ficam de fora. Revise os anexos antes de compartilhar. Nunca publique ROMs, chaves do console ou arquivos privados.

### Pacote de pistas CTGP Deluxe (opcional, experimental)

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
[STATUS.md](README.md).

### Compilar e contribuir

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

O script baixa as fontes upstream fixadas, confere os hashes, compila o launcher e o abre. Para empacotar o launcher portátil: `python scripts/package-launcher.py` (saída: `target/portable/Launcher.exe`). Os passos de release estão em [RELEASING.md](../RELEASING.md).

Para gerar um instalador local de teste sem publicar uma release, execute `python scripts/release.py`. O setup fica em `<diretório target do Cargo>/dist/MK8-online-<versão>-setup.exe`. Instale manualmente builds de teste com a mesma versão; o atualizador só oferece versões mais novas. O pacote inclui o novo design padrão e as operações reais de instalação/build, sem o storyboard debug.

Mantenha as adaptações em `runtime/mk8` e `toolchain`; a preparação as aplica a uma cópia verificada. Não edite os snapshots upstream nem o código gerado do jogo. Siga o [AGENTS.md](../../../AGENTS.md) e rode os testes da área alterada. Caminhos só de desenvolvimento estão documentados no [estado técnico](README.md):

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

#### Configurações do jogo

Os avisos do launcher aparecem em um painel fixo no topo da janela, visível durante a rolagem e ao trocar de aba. Use **Fechar aviso** (×) para dispensá-los. A entrada tem uma animação curta, desativada quando o sistema solicita redução de movimento.

As opções gerais ficam em **Configurações**: vídeo, áudio, idioma do jogo, ícones dos botões, posição dos controles e mapeamento de teclado/controle. O launcher usa o mesmo `user-no-mii/mk8d.ini` do menu F1. Clique em **Salvar configurações** para aplicar no próximo início do jogo; **Recarregar do jogo** recupera os valores gravados pelo menu F1. Alterações não salvas permanecem na tela durante as atualizações de estado. Durante o salvamento e a leitura de confirmação, os campos e os botões Salvar/Recarregar ficam bloqueados; uma falha libera a edição sem descartar os valores pendentes. Comentários e opções não editadas são preservados.

As configurações não podem ser salvas enquanto o jogo ou uma preparação usa os dados. Os mapeamentos são selecionados em listas; atribuir uma tecla ou botão já usado troca os dois destinos, como no menu F1. Ações de conta, instalação de mods e saída continuam no menu do jogo. Salas continuam em **Multijogador**.

#### Conteúdo adicional no launcher

Após preparar o jogo, abra **Instalar → Conteúdo adicional**. Selecione o NSP ou NSZ do Booster Course Pass v65536 e, quando necessário, as chaves do seu console; clique em **Importar DLC NSP/NSZ…**. O launcher mostra o estado da operação e seu log. Uma build antiga precisa ser atualizada antes de habilitar esses controles.

O controle **Ativar Booster Course Pass** fica em **Configurações → Conteúdo adicional** e vale ao iniciar o jogo. Importação e reparo permanecem em **Instalar**. Desativar mantém os arquivos importados e os saves. A preferência é salva por pasta de dados e permanece após reiniciar o launcher ou reconstruir o jogo. Importação e alterações ficam bloqueadas enquanto o jogo ou outra preparação usa os dados. Uma instalação inválida oferece **Reparar importação…**; a substituição só acontece após validar os novos arquivos, preservando a pasta anterior em um backup local.

Mesmo com o DLC instalado, **Instalar → Conteúdo adicional → Reinstalar ou selecionar outro NSP/NSZ** permite escolher novamente o pacote e as chaves. **Reinstalar DLC…** valida e extrai o pacote selecionado, sem reutilizar automaticamente a instalação existente. Os arquivos atuais só são substituídos após a validação; ficam preservados em um backup local. A preferência de ativação e os saves são mantidos. Apenas o pacote do perfil verificado continua aceito.

Os campos de NSP e chaves mostram contorno verde e marca de confirmação quando os arquivos são encontrados, como na etapa 2. O campo de chaves também considera a configuração já salva e a detecção automática. Essa indicação confirma a presença dos arquivos; o conteúdo do pacote e as chaves continuam sendo validados durante a importação.

O perfil aceito continua restrito ao Booster Course Pass verificado (`0100152000023001`, versão de conteúdo 65536) com o MK8 4.0.1 AArch64. Para testes por linha de comando, use uma pasta de dados isolada já preparada, seu NSP adicional e as chaves do seu console:

```powershell
.\local\python\Scripts\python.exe toolchain/import_dlc.py --package 'D:\Switch\BoosterCoursePass.nsp' --keys 'D:\Switch\prod.keys' --local 'E:\MK8-TESTE'
.\local\python\Scripts\python.exe toolchain/build.py --config 'E:\MK8-TESTE\config.json' --local 'E:\MK8-TESTE' --runtime-only
```

A importação confere os hashes dos NCAs e a associação do CNMT ao jogo, usa o ticket e as chaves para descriptografar o conteúdo e verifica os arquivos extraídos. O runtime oferece o índice instalado e monta esses arquivos reais; arquivos ausentes, vazios ou alterados são recusados. O comando de runtime reaproveita a biblioteca recompilada do jogo. Ainda não há alternância durante uma sessão do jogo, remoção automática nem suporte genérico a outros pacotes ou versões. Evidências e limites ficam em [STATUS.md](README.md).

### Créditos e licença

**O MK8-online é criado e mantido por RatOne.** Os autores upstream mantêm o crédito e a autoria das suas contribuições. Atribuições e avisos de dependências estão em [THIRD_PARTY.md](../THIRD_PARTY.md) e [docs/licenses](../../licenses). O criador de Mii desenha os rostos com o [FFL.js](https://github.com/ariankordi/FFL.js) (AGPL-3.0).

O código do projeto é licenciado sob **[GPL-3.0-or-later](../../../LICENSE)**. As dependências mantêm suas licenças:

- o snapshot do SMM2Recomp inclui GPLv3;
- o LASRecomp inclui avisos MIT;
- o setup leva o FFmpeg (LGPL 2.1), com o código-fonte anexado a cada release.

A licença não concede direitos sobre o código, os recursos ou as marcas da Nintendo.

Projeto comunitário independente, sem afiliação nem endosso da Nintendo ou da Valve. Mario Kart e nomes relacionados pertencem aos respectivos donos.

A renderização condicional aguarda reports de oclusão pendentes nos dois operandos antes de comparar as contagens reais. Corrige desaparecimentos transitórios nos previews, preservando resultados legítimos de objetos ocultos. A preparação aplica o patch; é necessário reconstruir o jogo/runtime.

O tradutor de shaders Maxwell passou a respeitar a interpolação declarada pelo jogo para entradas de fragmento. Isso reduziu a cintilação no trecho testado de Boo Lake em tela dividida. Recompile o runtime para aplicar a correção.

Na `fix-chore`, recompilar o runtime do Windows também mostra o jogo em chinês simplificado ou tradicional quando selecionado (#99). O ajuste de shader da GTX 1070 passa a valer para todas as GPUs NVIDIA GTX 10 (Pascal), para tratar a perda do dispositivo da GTX 1060 na primeira corrida (#114). O log do jogo registra o driver Vulkan e as camadas disponíveis antes de criar o dispositivo. Anexe esse log se o jogo fechar ao iniciar.

Se os emblemas das copas e os números da largada aparecem azuis na 0.3.0, o runtime da 0.4.0 corrige as cores dessas texturas RGB565. Recompile o jogo com o runtime atualizado para aplicar a correção.

## Distribuição da 0.5.0

O instalador e o ZIP portátil do Windows são os pacotes da revisão validada pelo mantenedor. O ZIP abriu em uma pasta temporária, respondeu pela interface local e reutilizou o runtime pré-compilado após a verificação da chave e dos hashes. Os testes de pistas, Miis, áudio, controles e gráficos abaixo descrevem o Windows.

O AppImage e o tar.gz amd64 usam o porte Linux de mitch030504, com a versão do workspace, launcher e manifesto ajustada para 0.5.0. No CI da 0.5.0 em Ubuntu 24.04, passaram os testes Rust e de ELF, a geração dos dois pacotes, a inicialização do launcher e do importador empacotados e a atualização e reversão headless do tar.gz. Na PR, a preparação do jogo e os testes de renderização cobriram uma Intel Iris Xe. A distribuição Linux tem fontes e resultados próprios. SteamOS em Steam Deck físico e partidas entre duas máquinas permanecem em validação. [Fonte exata](https://github.com/Vnnc1/MK8-online/tree/edfc423697977594b49db64b67c433c73086664f) · [PR #83](https://github.com/Vnnc1/MK8-online/pull/83) · [Empacotamento 0.5.0](https://github.com/Vnnc1/MK8-online/actions/runs/44856523053) · [Notas bilíngues](../../releases/0.5.0.md)

## Reteste de dois crashes da 0.4.0 na candidata 0.5.0

Os relatórios `1795197270` (automático) e `1795197286` (manual) contêm os mesmos arquivos de diagnóstico, exceto pelos metadados do relatório. No log atual e em dois anteriores, o Nextendo autorizou a conta na RTX 5070 Ti. A votação, com a preparação dos modelos desabilitada, tentou escrever em `0x4ac` na rotina `f_7000428838`. Na recompilação 0.5.0, executei as duas rotinas de notificação preparadas com 72 combinações de estado, contagem, armazenamento e índice. Listas vazias foram ignoradas e listas válidas atualizaram o modelo esperado. O teste local cobre a assinatura da falha. A votação pública na máquina do jogador ainda precisa de reteste.

O relatório `1795218451` descreve outra falha, ao abrir o solo sem DLC na RTX 4090. A 0.4.0 concluiu a build, indexou os 6.597 arquivos de RomFS e selecionou a GPU. O último registro foi `occlusion counts: precise`; a saída `0xc0000005` chegou antes da mensagem posterior à criação do dispositivo Vulkan. O ZIP não identifica o módulo ou a instrução que falhou. Usando o executável empacotado 0.5.0 e uma pasta de dados sem DLC instalado ou ativado, duas aberturas por **Jogar solo** na RX 580 2048SP chegaram à tela de título, capturaram o quadro 240 e encerraram com código zero. Fica pendente a confirmação na RTX 4090 com o driver 621.24 informado pelo jogador.

## Setup 0.4.0 em SSD exFAT — “Can't restore time: Invalid argument”

Um relato (Windows 11 25H2, instalação nova, pasta de dados em `G:\...` num SSD externo exFAT) parou em 81%, em **Preparar arquivos do launcher**, com `Can't restore time: Invalid argument` para cada arquivo do payload. O payload é gravado com `ZipInfo(nome)`, que usa a data padrão do ZIP: 1980-01-01 00:00 em horário local. Extraído sem `-m`, o `tar.exe` do Windows restaura essa data; aqui (UTC−3) o arquivo ficou com 1980-01-01 00:00 local. A leste de UTC, isso corresponde a 31/12/1979 em UTC, abaixo do mínimo do exFAT, e o `SetFileTime` falha. Em NTFS, a mesma data é aceita. O setup agora extrai com `tar -xmf` (`extract_payload`), mantendo a data da extração.

Um disco exFAT teria falhado em seguida: a build expõe o RomFS por hard links (`mirror`), e FAT32/exFAT não os suportam. A mensagem era “precisa estar no mesmo disco”, embora fosse o mesmo disco. O setup agora consulta `FILE_SUPPORTS_HARD_LINKS` do volume (`GetVolumeInformationW`), mostra o aviso ao escolher a pasta de dados e recusa a instalação nesse disco. A build informa que o disco não aceita hard links e precisa ser NTFS. Não foi feita uma cópia completa como alternativa, porque isso duplicaria cerca de 11 GB.

Validação: o teste Rust `payload_extraction_does_not_restore_the_1980_zip_date` extrai pelo helper real um ZIP com a data de 1980; sem `-m`, o `tar` restaura 1980, conforme conferido manualmente. Passaram também o teste da recusa por sistema de arquivos (NTFS real; exFAT pela regra), o teste de navegador do aviso em inglês e espanhol, o teste do `mirror` com WinError 1, 104 testes Rust do workspace, Clippy e formatação. Esta máquina não tem volume exFAT, e criá-lo exige administrador, por isso o setup ainda não foi executado num disco exFAT real.

## Validação dos pacotes Windows x64 da 0.4.0

O instalador da 0.4.0 atualizou uma instalação isolada da 0.3.0, abriu a janela do launcher na versão correta e preservou o save de teste. O desinstalador passou pelos fluxos de cancelar, manter os dados e remover os dados gerenciados. A remoção completa preservou o arquivo de controle externo. O ZIP portátil abriu em duas unidades do Windows e reconheceu a nova pasta de dados após a cópia. O save de teste manteve o mesmo conteúdo.

Passaram a formatação, o Clippy, 99 testes Rust, 28 testes de navegador com relay, 6 de idioma e 9 do pipeline. Um teste Rust opcional de Steam ao vivo ficou fora da execução. A conferência do pacote abrangeu os 1.006 arquivos internos, as fontes de runtime e os manifestos do host e do FFmpeg. Os hashes dos dois downloads estão nas [notas públicas da 0.4.0](https://github.com/Vnnc1/MK8-online/releases/tag/v0.4.0). A corrida Steam completa entre dois PCs continua pendente.

## Pré-release 0.3.0 — pacote validado em 16/11/2026

Publicação solicitada pelo mantenedor e concluída, reunindo a `fix-chore` e suas PRs aceitas na branch principal **master** (o repositório não usa `main`). Versão de workspace/launcher `0.3.0`, tag `v0.3.0` no commit `47b05c5`, marcada como pré-release mesmo sem sufixo. Merge `c2b4ee6`; branches sincronizadas. Notas em inglês e português, em seções expansíveis, em [releases/0.3.0.md](../../releases/0.3.0.md); README, README.pt-BR, ROADMAP e instruções de publicação atualizados.

Escopo: CTGP Deluxe 1.1.1 adaptado do alvo 3.0.3 para os recursos da recompilação 4.0.1, incluindo 64 pistas/16 copas adicionais, novo launcher padrão, DLC opcional, Mii, contribuições #9/#12/#18/#19/#25/#27–31/#33, desempenho/controles e correção CRLF. O plugin original CTGP não é executado. A publicação assume explicitamente as limitações abaixo, inclusive reconexão Steam, falhas Vulkan/memória e gráficos ainda sem solução confirmada. Não se afirma compatibilidade 100%, corrida Steam qualificada nem simulação a 120 Hz. Dados privados e saves preservados; nenhuma alteração de gameplay nesta preparação.

**Pacote validado:** `MK8-online-0.3.0-setup.exe`, 59.518.464 bytes. SHA-256 `7d7b650137a6ec445c44ad351be7b59e178506cc9a3f1cad2505746f9945d984`. Host recompilado (220 unidades) com chave `7e38c0a393effad58b4da64ceef79f7fbfe975bbb16df7937d41f03f0afb8ea3`, incluindo CRLF; launcher release compilado e empacotado com atualizador configurado para o repositório. Os 966 arquivos do payload foram auditados: hashes/manifestos conferidos, fontes iguais às atuais, sem jogos/chaves/saves/caminhos pessoais. Arquivos privados do checkout permaneceram iguais. A instalação externa foi aberta durante a preparação e dois arquivos de configuração/perfil mudaram; essas alterações da sessão ativa foram preservadas, sem restaurar o snapshot antigo. Fontes e script FFmpeg acompanham o setup.

**Verificação:** formatação, Clippy com `-D warnings` e 85 testes Rust aprovados (um Steam real optativo ignorado); 34 testes CTGP/DLC/INI/pré-compilados aprovados (um que exige RomFS local ignorado); 51 testes de interface embarcada, idiomas, servidor e janela nativa aprovados (um relay optativo ignorado). Total de 170 casos distintos aprovados. Instalação, abertura e desinstalação reais em pasta isolada concluídas; dados mantidos, registro e atalhos pessoais restaurados. Tela final do instalador 0.3.0 inspecionada. Versões, notas bilíngues, hash e 38 links locais conferidos. Dois caminhos pessoais em exemplos antigos desta documentação foram neutralizados; histórico Git anterior não foi reescrito.

Após o merge local, o checkout normalizou fins de linha em oito fontes. O pacote foi reconstruído dessa árvore final: as 133 fontes embarcadas de runtime/pipeline permaneceram equivalentes além dos fins de linha. A auditoria passou com igualdade exata dos arquivos finais; dez testes INI/pré-compilados e a instalação/abertura/desinstalação isolada passaram novamente. O hash acima identifica esse pacote final, não a compilação anterior ao merge.

**Publicação verificada:** [release v0.3.0](https://github.com/Vnnc1/MK8-online/releases/tag/v0.3.0), publicada em 16/11 às 22:16 UTC, como pré-release, com o binário local validado. Título e notas conferidos; download público sem autenticação dos quatro anexos concluído com tamanhos e SHA-256 idênticos aos locais. Mais 15 testes essenciais de interface/janela nativa passaram no pacote final após o merge. A execução duplicada do workflow de release `43968119785` foi cancelada antes da publicação, preservando os anexos testados; o CI normal da master não foi cancelado. As PRs #9/#12/#25 antes abertas foram reconhecidas como integradas pelo GitHub, sem PR aberta ao fim desta publicação. Evidências de publicação permanecem privadas em `local/release-0.3.0-public-verification.json`.

**CI remoto:** [execução 43967935989](https://github.com/Vnnc1/MK8-online/actions/runs/43967935989), no commit da tag, aprovada integralmente na tentativa 2, sem mudança de código ou relaxamento das verificações. A primeira tentativa falhou ao tentar reutilizar em até dois segundos a porta UDP no teste `closed_game_and_idle_guest_release_steam_workers_before_next_join` (Windows 10048). O caso isolado e dez repetições locais dos 36 testes do launcher passaram, assim como a reexecução completa no GitHub. A causa dessa falha intermitente não foi isolada; acompanhar, sem confundi-la com resolução do bug de reconexão #32. Log local privado: `local/release-0.3.0-ci-recheck.log`.

Os ensaios de UI usam operações longas/rede controladas; não houve nova corrida completa, teste em outra GPU ou multiplayer entre PCs nesta preparação. O histórico abaixo descreve o estado e os artefatos anteriores. `gameplay_verified=false`, `multiplayer_verified=false`.

## 0.2.0 republicada — 13/11/2026

O usuário testou e aprovou o setup de SHA-256 `356381687927b3c93a83ee6b91bb4a6c5e22876cb23d48bb5d39f3c9efa314d3`. Commit `dee0c2c` na `fix-chore`, merge `630538d` na `master` e tag `v0.2.0` atualizada para esse merge. A [release pública](https://github.com/Vnnc1/MK8-online/releases/tag/v0.2.0) contém exatamente o binário aprovado, com o título e a descrição anteriores (somente o hash gerado atualizado), incluindo SHA-256 e fontes/scripts FFmpeg. Os quatro anexos tiveram seus hashes verificados; o download público sem autenticação do setup retornou os mesmos 57.975.296 bytes e SHA-256. A build automática duplicada da tag foi cancelada para preservar esse pacote. Os 59 testes Rust do workspace, formatação e Clippy passaram antes do envio. A confirmação do usuário inclui o controle com a Steam aberta após a configuração por jogo descrita abaixo; não qualifica multiplayer em duas máquinas.

## Fechamento da release pública 0.2.0 — 13/11/2026

**Histórico da publicação anterior:** o usuário informou ter retirado a postagem da release enquanto testa o novo setup descrito acima. O registro abaixo se refere ao pacote anterior, não à republicação pendente.

**Publicada:** [v0.2.0 no GitHub](https://github.com/Vnnc1/MK8-online/releases/tag/v0.2.0), marcada como Latest, com setup, SHA-256 e os dois anexos FFmpeg. A `fix-chore` foi integrada à `master` por fast-forward e ambas foram enviadas. O download público completo do setup foi conferido contra o SHA-256 local e o digest do GitHub. A consulta anônima à API de releases retornou limite de requisições neste IP durante a verificação final; a página Latest e os downloads públicos funcionaram. Isso não foi contado como validação do botão de atualização contra a release publicada.

A 0.2.0 reúne as alterações da `fix-chore` para a `master`: criador Mii com miniaturas e corpo do jogo, preparação dos recursos para o renderer Mii original, salas/convites Steam e entrada automática experimental no lobby, além das correções de logs, caminhos Windows e gerenciamento de recursos gráficos documentadas abaixo. Notas bilíngues e título ficam em `docs/releases/0.2.0.md`; os dois READMEs descrevem os mesmos requisitos e limites. **Multiplayer Steam continua experimental, sem corrida entre dois PCs qualificada.**

O pacote foi recompilado pelos scripts de distribuição, incluindo o host nativo. Os scripts agora resolvem a pasta de build por `cargo metadata`, respeitando o disco configurado; o workflow usa o título das notas e o CI também cobre a branch `master`. O link gerado para o status aponta para a tag correta. Um ajuste posterior somente nos workflows instala as dependências Python fixadas e verificadas por hash antes dos testes/empacotamento em runners novos; não modifica o setup validado da tag.

**Validação desta release:**

- `cargo fmt --all -- --check`, Clippy de todo o workspace com `-D warnings` e 57 testes Rust passaram.
- 23 testes de pipeline, runtime e reutilização de host/FFmpeg passaram com o Python privado e suas dependências. A primeira execução com o Python do sistema não tinha `lz4`/`zstandard`; isso não foi tratado como teste aprovado.
- 19 testes de navegador passaram contra o executável **empacotado**, incluindo relay habilitado, tráfego UDP bidirecional, idiomas, interface Steam e criação/edição/persistência de Miis. Foi corrigida uma expectativa antiga de texto do teste de relay (`Na sala`); o teste do criador aguarda sua inicialização antes de verificar os controles.
- O roteiro de composição também passou com o launcher empacotado, AFL/jogo privados e Python local configurado na sessão isolada: 132 miniaturas de cabelo, olhos, corpos masculino/feminino, altura/cor, salvamento, geração real dos seis recursos Mii e recarga, sem requisições externas. A captura `local/mii-composer-check/creator.png` foi inspecionada. Sem o Python de ferramentas preparado, a geração é recusada; esse pré-requisito foi fornecido para o teste, sem incluir recursos privados na distribuição.
- O setup real percorreu suas telas, instalou, abriu o launcher 0.2.0 e desinstalou em uma pasta isolada. A atualização silenciosa **0.1.8 → 0.2.0** preservou pastas, idioma, configuração e save de teste. O registro HKCU e os atalhos da instalação anterior foram restaurados e conferidos.
- A auditoria conferiu 934 arquivos do payload, os fontes atuais e hashes do host/FFmpeg. Não encontrou recursos de jogo, chaves ou caminhos pessoais no pacote verificado. Os 23 arquivos pessoais de configuração/perfis/saves acompanhados permaneceram iguais.

Setup: `MK8-online-0.2.0-setup.exe`, 57.900.544 bytes, SHA-256 `52ee5d1b34c80a0e2908ba815707d8130092ed186a4fa830f6053eed293e154f`. A distribuição acompanha `.sha256`, `ffmpeg-9.0.2.tar.xz` e `build-ffmpeg.sh`; nenhuma biblioteca AFL ou DLL da Steam é distribuída. Evidências privadas em `local/release-0.2.0-*.log`, `local/release-artifacts-0.2.0.json`, `local/release-validation-0.2.0-final` e `local/release-upgrade-0.2.0`.

Esta validação de distribuição não repetiu a qualificação completa de gameplay nem comprovou o convidado Steam em outra máquina. As evidências de jogo abaixo mantêm seu escopo original; `gameplay_verified=false` e `multiplayer_verified=false` permanecem. Após atualizar, preparar o jogo novamente com o runtime novo.

## Release 0.1.8 — versão de testes

Versões Cargo/Tauri/lock sincronizadas em 0.1.8. Esta release reúne as correções de preparação, Python/pip, integridade de caches, coordenação entre instâncias, supervisão de processos, tratamento de arquivos bloqueados e carregador Vulkan descritas abaixo. As referências a “correção local” nas seções seguintes registram o estado de cada investigação antes desta publicação.

A validação inicial do instalador reproduziu acesso negado em 77%: a extração estava completa, mas a renomeação da pasta temporária falhou. A publicação agora aguarda até 15 segundos para erros transitórios Windows 5/32/33, preserva os arquivos e informa os caminhos se falhar. Nova tentativa usa outra pasta temporária, sem excluir a anterior. Um teste Rust mantém um handle real no diretório, confirma preservação após falha e sucesso após desbloqueio. Isso não identifica o processo/antivírus responsável nem contorna permissões persistentes.

Validação: formatação, Clippy e 38 testes Rust aprovados; 49 testes Python de acesso, downloads, armazenamento, pipeline, cache nativo, reparo, Vulkan, host pré-compilado e ferramentas aprovados. Passaram 16 testes de navegador com relay real, 2 de idiomas e 3 de supervisão com processos reais no pacote final. O teste de supervisão aguarda o JSON de prontidão terminar de ser escrito antes de lê-lo. Instalação real, abertura e desinstalação em pasta isolada passaram; atualização silenciosa 0.1.7 → 0.1.8 preservou pastas, idioma, configuração e save sintético. Registro e atalhos pessoais restaurados. READMEs EN/PT renderizados e conferidos. Executável e 912 arquivos do payload revisados, sem identificadores pessoais conhecidos; 13 arquivos privados monitorados permaneceram intactos.

Instalador final: `MK8-online-0.1.8-setup.exe`, SHA-256 `bd2574ec3d430b16c07e7841b107036632213febc1112c9c4a352379324099fb`. Publicada inicialmente como pré-release v0.1.8, com setup, SHA-256 e fontes/script FFmpeg. A pedido do mantenedor, passou a release Latest para aparecer na página inicial; título bilíngue e notas em inglês seguidas de português. O caráter experimental permanece explícito. A API `/releases/latest` confirmou v0.1.8, sem alteração dos anexos. Publicada em https://github.com/Vnnc1/MK8-online/releases/tag/v0.1.8. O download público foi conferido pelo SHA-256; o launcher 0.1.7 real em pasta isolada retornou `available=true`, `installable=true` e `latest.tag=v0.1.8`. A instalação de atualização foi validada separadamente, sem atualizar a instalação pessoal. Gameplay e multiplayer entre máquinas continuam não qualificados.

## Estabilização em andamento — fontes locais, sem nova release

Investigado o [issue #4](https://github.com/Vnnc1/MK8-online/issues/4), relatado na 0.1.6: o SHA-256 recebido para `lz4-4.4.5.tar.gz` coincide com a distribuição de fontes oficial do PyPI. O requisito fixado espera o wheel CPython 3.12 x64. Reproduzido o mesmo erro com `PIP_NO_BINARY=:all:`; isso demonstra uma causa possível, mas o relato não informa a configuração completa nem confirma essa origem na máquina afetada. O issue #3 registrava seleção de wheel cp313 e é coberto pelo bootstrap 3.12; o #1 registrava Python ausente. Não foram publicados comentários ou encerrados issues.

Correções locais:

- `setup-tools.ps1` isola Python/pip de configurações externas, usa o índice oficial e exige wheels; mantém versões e SHA-256 fixados. Nenhum hash de fonte foi acrescentado para mascarar o erro.
- A prontidão do launcher executa o Python privado 3.12 x64 e importa as dependências necessárias. A 0.1.7 ainda consultava Python global e podia marcar Ferramentas como incompleto apesar do bootstrap bem-sucedido; um executável apenas existente também não prova prontidão.
- Downloads do compilador, bibliotecas e DLLs verificam arquivos em cache, reaproveitam os válidos e baixam novamente os inválidos. A substituição só ocorre depois do SHA-256 correto; a falha preserva o arquivo anterior.
- Recibos JSON truncados ou com formato incorreto deixam de abortar a retomada da compilação nativa. Objetos só são reutilizados após validação de entrada e SHA-256; cache inválido leva à recompilação necessária.

Validação: build Rust release de desenvolvimento concluída; formatação e Clippy do workspace sem avisos; 33 testes Rust aprovados. Passaram 5 testes de download por HTTP local, 6 de cache com compilação nativa real de fixtures, 4 de ferramentas, 7 de pipeline e o bootstrap PowerShell com pacote CPython oficial, configuração pip/Python externa incompatível, wheel sintético verificado, reparo e reaproveitamento. Reproduzido o hash do issue #4 e confirmado download do wheel correto com as novas opções.

Navegador: 16 testes passaram no launcher de desenvolvimento, incluindo relay real e prontidão do Python privado com PATH vazio e variáveis Python incompatíveis; 2 testes de idiomas também passaram. O teste de isolamento compara ainda o estado com e sem essas variáveis, permitindo execução em checkouts sem ferramentas preparadas; nesta máquina o ambiente privado estava funcional.

Limites: ainda falta confirmação na máquina do issue #4. Esta rodada cobre instalação, prontidão, downloads e retomada do compilador; não constitui auditoria exaustiva do tradutor ou qualificação do jogo. Integridade de árvores nativas já extraídas, concorrência/cancelamento e segurança das rotinas de limpeza seguem como pontos para investigação. Runtime/guest e dados privados não foram alterados; `gameplay_verified` permanece falso. Nenhum novo instalador, tag ou release foi publicado nesta rodada.

## Release 0.1.7 (09/11/2026)

Instalador 0.1.7 gerado com Python automático e reparo de ambientes. Versões Cargo/Tauri/lock sincronizadas, atualizador configurado para Vnnc1/MK8-online. SHA-256: `972862012185c82b6af03a0950e2d10091b679c2eb17054df11f8648aabdfe38`. 32 testes Rust, formatação, Clippy e bootstrap passaram. Instalação real, abertura nos dois idiomas, desinstalação e preservação de dados passaram em pasta isolada; registro e atalhos anteriores restaurados. Executável/payload revisados; 910 arquivos, scripts conferidos, sem identificadores pessoais conhecidos. README EN/PT renderizados e conferidos. 15 testes de navegador (incluindo relay real) e 2 de idiomas passaram no pacote. Publicada em https://github.com/Vnnc1/MK8-online/releases/tag/v0.1.7 com setup, SHA-256 e fontes/script FFmpeg. Download público do setup conferido pelo SHA-256. O launcher 0.1.6 real, em pasta isolada, retornou `available=true`, `installable=true` e `latest.tag=v0.1.7` ao consultar o atualizador. Instalação/desinstalação do novo setup foi testada separadamente; não foi acionada atualização sobre a instalação pessoal.

## Teste de disco no CI (09/11/2026)

O log `logs_115262453029.zip` mostra o workflow de release parando em `disk_kind_of_the_system_drive_is_reported`: `hard_disk` retornou `None`, permitido pelo contrato quando a consulta Windows não informa o tipo de disco. O teste pressupunha incorretamente que qualquer disco local responderia. Substituído por uma verificação com chamadas reais à API: uma pasta inexistente deve retornar o mesmo resultado do diretório pai existente, inclusive `None`. Nenhuma detecção foi forçada para HD/SSD e nenhum teste foi ignorado. Runtime, guest e interface permanecem sem alterações. A execução remota do workflow ainda precisa ser repetida após publicar a correção.

Validação local da correção: 32 testes Rust passaram, incluindo a consulta real para pasta inexistente; `cargo fmt --all -- --check` e Clippy do workspace com `-D warnings` passaram. Build de desenvolvimento do launcher concluída com `cargo build -p mk8-online --bin Launcher --locked`.

## Release 0.1.6 (08/11/2026)

Setup recompilado a partir do estado atual, com inglês/PT-BR e `MK8_UPDATE_REPO=Vnnc1/MK8-online`, preservando a nomenclatura `MK8-online-0.1.6-setup.exe` e `.exe.sha256`. SHA-256: `0ac78e37a7cbbb081f6a6b9cb7946a455fbe698208e4dd8dd65c9882f3b796d4` (55.499.776 bytes). A publicação também inclui os fontes `ffmpeg-9.0.2.tar.xz` e `build-ffmpeg.sh`; o workflow agora os anexa nas próximas tags. O launcher consulta releases publicadas, incluindo pré-releases, sem autenticação; requer repositório público. A visibilidade pública foi observada no GitHub durante esta preparação, sem alteração de visibilidade pelo agente. Publicação e consulta final ainda em andamento.

Validação do pacote: runtime pré-compilado concluído, build Rust release concluída, formatação e Clippy sem avisos; 32 testes Rust aprovados. Dos 17 testes de launcher/navegador, 16 passaram de primeira; o teste de suporte pressupunha que ausência de variável de ambiente significava repositório não configurado e falhou porque a configuração agora está embutida. Corrigido para consultar o estado real da API, passou na repetição. Inclui janela nativa, idiomas e relay real em loopback. Os dois READMEs foram renderizados e seus previews conferidos.

Instalação real em pasta isolada, abertura da versão 0.1.6 com idioma persistente e desinstalação passaram. O registro de desinstalação e os atalhos anteriores foram restaurados e conferidos. Dados de teste foram preservados. Uma primeira asserção supunha incorretamente que abrir o launcher criaria `config.json`; o teste passou após criar explicitamente o arquivo antes de verificar sua preservação. A verificação do binário final e dos 909 arquivos do payload não encontrou os identificadores pessoais conhecidos. Os 13 arquivos protegidos de configuração/perfis/saves mantêm seus hashes. Nenhuma corrida ou qualificação multiplayer adicional foi feita nesta etapa; `gameplay_verified` permanece falso.

## Apresentação pública e idiomas (08/11/2026)

README principal refeito em inglês, com versão `README.pt-BR.md`, assinatura PNG e autoria RatOne em destaque, quadro de progresso, instalação, LAN/relay e medições contextualizadas. Nextendo aparece como investigação futura, sem integração ou compatibilidade presumida. GPL-3.0-or-later preservada: o snapshot SMM2Recomp contém GPLv3, embora parte da origem LASRecomp seja MIT; ver `THIRD_PARTY.md`.

Launcher e instalador agora oferecem inglês/PT-BR. A interface escolhe inicialmente o idioma do WebView/navegador (inglês como alternativa) e permite trocar pelo seletor sem recarregar os campos. A preferência do launcher fica em `language.json` na pasta de dados, separada de `config.json`, perfis e saves; no instalador ela acompanha a instalação para `<pasta escolhida>/data/language.json`. Só `en` e `pt-BR` são aceitos. Estados de protocolo, nomes dos jogadores, caminhos e convites permanecem intactos. Logs de ferramentas e detalhes externos de diagnóstico mantêm o texto original; isso não traduz o jogo.

Validação: build Rust release concluída, formatação e Clippy sem avisos, 32 testes Rust aprovados. A suíte de navegador teve 14 testes aprovados e um de relay inicialmente ignorado; esse teste foi executado separadamente com relay real em loopback e passou, incluindo troca de idioma com sala ativa e tráfego UDP bidirecional. Os dois testes novos verificaram persistência ao reiniciar o processo/servidor, rejeição de idioma inválido, mensagens de build/erro, campos preservados e a interface do instalador com API simulada. Capturas conferidas em desktop, largura de 390 px e instalador 880×620; previews e links dos dois READMEs verificados. Nenhuma corrida nova foi qualificada nesta etapa.

Revisão de publicação: 178 arquivos atuais, mensagens de commit e 324 blobs dos 21 commits alcançáveis nas referências Git locais na conferência final. A busca por padrões de tokens, chaves privadas, credenciais em URLs, chaves de console, e-mails, nomes/caminhos pessoais conhecidos não encontrou ocorrência no conteúdo. Créditos públicos de dependências foram preservados. Os PNGs foram inspecionados; a assinatura tem metadados de software/perfil de cor, sem identificação pessoal encontrada. Não havia arquivos privados adicionais nos diretórios de fontes incluídos pelo empacotador, além de caches Python excluídos do payload. `.gitignore` agora também cobre XCZ e formatos comuns de credenciais. Essa revisão por padrões não é garantia de ausência de segredos e não cobre configurações, anexos ou logs já publicados no GitHub.

**Pendência antes de abrir o histórico:** os 21 commits têm nome/e-mail pessoais nos campos de autor e committer. Alterar arquivos ou configurar `noreply` para novos commits não remove os registros anteriores. Não houve reescrita, force-push ou mudança de visibilidade do repositório.

**Binários:** a revisão encontrou o identificador da conta local nos caminhos de compilação Rust do executável de desenvolvimento e em 347 ocorrências no portátil anterior. O payload ZIP anterior não apresentou esse identificador. `scripts/package-launcher.py` agora acrescenta `--remap-path-prefix` para checkout, perfil do usuário e cache Cargo, preservando flags preexistentes por `CARGO_ENCODED_RUSTFLAGS`. A primeira recompilação deixou uma ocorrência: o `generate_context` do Tauri embutia o diretório de configuração no modo de desenvolvimento. O empacotamento agora habilita `tauri/custom-protocol`, removendo esse dado do contexto de produção; as janelas continuam usando a API HTTP em loopback existente.

Empacotamento concluído: runtime pré-compilado de 212 unidades e `target/portable/Launcher.exe`, SHA-256 `7827a7afc9904d1034f862400038ee98fbc5c8428c6a4378d4c2915479c77920`. A conferência do novo executável encontrou zero ocorrências do identificador da conta local, nome/e-mail de autoria e caminhos de perfil de usuário do Windows; os 909 arquivos do payload também não contêm o identificador da conta. Isso não substitui uma auditoria geral dos binários de terceiros. O executável de desenvolvimento depende do caminho do checkout e não deve ser distribuído como release.

No portátil final passaram os dois testes de idioma, a abertura da janela nativa WebView2 nos dois idiomas e o teste de relay com UDP bidirecional. O modo nativo `--install` também foi aberto: boas-vindas e escolha de pastas trocaram entre inglês/PT-BR, preservando os caminhos, inclusive no fluxo de atualização de uma instalação existente. A instalação/desinstalação real não foi iniciada nessa conferência. Os 13 arquivos protegidos de configuração/perfis/saves conferidos mantêm os hashes anteriores. Nenhuma release foi publicada; os instaladores antigos no GitHub não recebem essas mudanças automaticamente.

