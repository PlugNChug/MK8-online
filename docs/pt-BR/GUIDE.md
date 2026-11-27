# Guia do usuário

[English](../en/GUIDE.md) · **Português**

[English](../en/GUIDE.md) · **Português**

Este guia explica como instalar, configurar e jogar o MK8-online. No Linux, comece pelo [guia Linux](LINUX.md) e volte aqui para os recursos do launcher e do jogo. Tudo abaixo é experimental; o [estado técnico](STATUS.md) registra o que já foi verificado e cada mudança.

- [Requisitos](#requisitos)
- [Instalar e jogar](#instalar-e-jogar)
- [Atualização, modo portátil e desinstalação](#atualização-modo-portátil-e-desinstalação)
- [Mii](#mii)
- [Gráficos](#gráficos)
- [Controles](#controles)
- [Menu do jogo e configurações](#menu-do-jogo-e-configurações)
- [Online com o Nextendo](#online-com-o-nextendo)
- [Salas Steam com amigos](#salas-steam-com-amigos)
- [DLC Booster Course Pass](#dlc-booster-course-pass)
- [Pacote de pistas CTGP Deluxe](#pacote-de-pistas-ctgp-deluxe)
- [Mods de RomFS](#mods-de-romfs)
- [Fontes do console](#fontes-do-console)
- [Relatar um problema](#relatar-um-problema)
- [Solução de problemas](#solução-de-problemas)

## Requisitos

- **Windows x64** com WebView2. O launcher instala sozinho um **Python 3.12 x64** privado.
- **Linux amd64 (experimental):** o AppImage ou o tarball e os pacotes do [guia Linux](LINUX.md), incluindo o **Python 3.12**.
- GPU e driver com **Vulkan 1.3 ou mais novo**. Não há especificação mínima geral de CPU/GPU qualificada.
- Seu próprio jogo base, a **atualização 4.0.1** e o `prod.keys` do seu console, ou uma pasta do jogo já extraída e suportada.
- Cerca de **28 GiB livres** para extração e preparação (incluindo 4 GiB de reserva de segurança), além de espaço para ferramentas e caches. SSD recomendado.
- Internet para o primeiro download das ferramentas (cerca de 0,4 GiB) e para atualizações.
- **Para Miis:** o criador baixa `AFLResHigh_2_3.dat` no primeiro uso (veja [Mii](#mii)).
- **Para salas Steam:** Steam conectada e uma DLL Steamworks compatível de 64 bits de um jogo instalado (veja [Salas Steam](#salas-steam-com-amigos)). Nenhum arquivo da Valve é distribuído.

## Instalar e jogar

1. Baixe `MK8-online-<versão>-setup.exe` em [Releases](https://github.com/Vnnc1/MK8-online/releases) e escolha as pastas do aplicativo e dos dados. A instalação é por usuário, sem administrador. O instalador pode abrir o launcher na mesma janela ao terminar.
2. Abra **Instalar** e conclua **Ferramentas**.
3. Em **Arquivos do jogo**, selecione o jogo base e a atualização 4.0.1. O launcher encontra as chaves junto do jogo, ou selecione as suas. Também é possível incluir aqui um NSP/NSZ opcional do Booster Course Pass.
4. Rode **Build do jogo**. Uma build limpa leva cerca de 30 minutos; as seguintes reaproveitam o que já foi feito.
5. Escolha **Jogar solo**.

**Pasta extraída:** precisa conter `exefs`, `romfs` e `control.nacp` (ou `control/control.nacp`). Use **Já tenho o jogo extraído numa pasta**. A importação direta de NSP/NSZ aceita duas variantes verificadas do programa base. O perfil ativo é o 4.0.1 AArch64; outras versões do jogo e importação de XCI/XCZ não foram validadas.

**Pasta de dados:** por padrão `%LOCALAPPDATA%\MK8-online`; sobrevive a atualizações e à desinstalação.

**Idioma:** o launcher e o instalador oferecem English, Português (Brasil) e Español (**Language / Idioma**). O idioma do jogo é escolhido à parte, acima de **Jogar** ou em **Configurações**.

A etapa **Ferramentas** prepara o Python privado diretamente, sem PowerShell nem carregamento de ZIP pelo .NET. O CPython vem do pacote oficial NuGet fixado, e o arquivo e as dependências são conferidos por hash. Mantenha o antivírus ativo.

## Atualização, modo portátil e desinstalação

**Atualizar:** use **Suporte → Atualizar agora**. A pasta de dados e os saves são mantidos. Se **Instalar** oferecer **Atualizar a build** ou abrir **Ferramentas**, conclua essas etapas: o jogo é recompilado com o runtime novo.

**Inicialização direta:** `Launcher.exe --play` abre o jogo solo já preparado sem a janela do launcher. Acrescente `--data-dir "D:\MK8\data"` para selecionar dados existentes. As validações de arquivos, Vulkan e bloqueios continuam; não instala ferramentas nem recompila. Erros retornam código de saída diferente de zero. Salas Steam continuam pelo launcher.

**Pacote portátil:** extraia `MK8-online-<versão>-portable.zip` numa pasta gravável e mantenha `portable.txt` ao lado de `Launcher.exe`. Fontes, ferramentas e dados ficam na pasta `portable` ao lado. `--data-dir` muda só o local dos dados do jogo. Instalações existentes não são migradas automaticamente. Para atualizar, substitua o executável e mantenha `portable/`; o atualizador por instalador fica desativado nesse modo.

**Mover uma cópia portátil:** abra a nova versão uma vez, feche o jogo e o launcher e copie ou mova a **pasta inteira**, incluindo `portable/`. Na próxima abertura, os caminhos internos e o ambiente Python são ajustados automaticamente; os metadados anteriores ficam em `portable/.relocation-backups/`. Saves e arquivos binários do jogo não são reescritos. Arquivos selecionados fora da pasta portátil continuam externos.

**Desinstalar:** abra **Uninstall.exe** ao lado de **Launcher.exe**. Por padrão, saves, Miis, configurações e o jogo preparado são mantidos; escolha a mesma pasta de dados ao reinstalar para reutilizá-los. A remoção completa mostra a pasta de dados e pede confirmação.

## Mii

1. Abra **Perfil Mii** e aguarde o primeiro download da biblioteca. O criador baixa `AFLResHigh_2_3.dat` de uma cópia do antigo CDN Miitomo no Internet Archive, confere tamanho e SHA-256 do ZIP e da biblioteca e guarda o arquivo na pasta de dados para uso offline. Para usar um arquivo que você já tem, abra **Usar um arquivo local**, depois **Procurar** e **Usar**.
2. Digite um nome (1 a 10 caracteres) e escolha **Criar Mii**. Percorra as abas e escolha as opções; cada botão mostra seu Mii com aquela opção. **Corpo** tem tipo, altura, porte e cor favorita.
3. Escolha **Salvar Mii**. Na primeira vez, o launcher prepara os modelos Mii do jogo; **Jogar** mostra o andamento e libera o botão ao terminar.

O solo, as salas Steam e o Nextendo usam o Mii ativo. Você pode ter vários Miis e trocar entre eles no topo da tela. **Perfil Mii** no menu **F1** do jogo traz esta tela à frente. Sem a biblioteca, as opções ficam numeradas e os rostos não são desenhados.

## Gráficos

Abra **Jogar → Gráficos** ou **Configurações → Vídeo e controles → Imagem**.

**Ampliação:** escolha uma resolução interna menor, como **75%**, e um método:

| Método | Disponibilidade |
| --- | --- |
| Bilinear, FSR 1 | Integrados |
| FSR 3.1.6 | Componente opcional |
| FSR 4.1.2 | Componente opcional, GPUs RDNA 4 compatíveis |
| DLSS Super Resolution | Componente opcional, GPUs RTX compatíveis |

Os componentes opcionais são baixados do fabricante depois que você aceita os termos no launcher. O painel mostra o método e as dimensões informados pelo jogo; a resolução real pode ser arredondada para os alvos de renderização. Se um provedor for recusado, o jogo usa a ampliação integrada. No jogo, **F1 → Ajustes → Vídeo** mostra a mesma lista; reabra o jogo pelo launcher para carregar outro provedor temporal.

No Linux, os ampliadores temporais e a geração de quadros FSR 3 são compilados no seu PC a partir do SDK do fabricante, na aba **FSR** do launcher, depois que você aceita a licença; os componentes opcionais do Windows não são usados.

**Proporção:** Original 16:9, Esticar ou **Ultrawide (experimental)**. O ultrawide amplia as câmeras solo suportadas de Contra o Relógio, Grand Prix e VS sem equipes entre 16:9 e 32:9 e mantém o HUD proporcional. Cenas sem suporte, tela dividida, online, replays e geração de quadros ficam em 16:9. Clique em **Salvar configurações** antes de iniciar o jogo.

**Distância da câmera de pilotagem:** de 75% a 150% (100% é a original). Afasta ou aproxima as câmeras de pilotagem frontal e traseira no solo (Contra o Relógio, Grand Prix e VS sem equipes); o close-up inicial mantém o enquadramento.

**60 FPS em tela dividida:** com três ou quatro jogadores locais, tenta atualizar todas as vistas a cada quadro em vez de alterná-las. A taxa alcançada depende da GPU.

**Bloom:** ligado por padrão. Desligado, remove o halo em torno das superfícies luminosas; as próprias luzes continuam acesas.

**Geração de quadros (experimental, desligada por padrão):** interpola uma imagem entre quadros reais, mantendo o jogo a 60 Hz. Exige monitor de alta frequência e folga na GPU, e não elimina engasgos de shaders.

**Contador de FPS:** mostra os FPS do jogo, os FPS de saída e os milissegundos por quadro do jogo.

## Controles

Controles compatíveis com a SDL, teclado e controles do Switch são suportados. A Entrada Steam pode ficar ativada para o Spacewar no solo ou numa sala Steam; um controle virtual inativo da Steam deixa a vaga livre para o controle físico detectado pelo GameInput do Windows.

**Escolher o jogador 1:** na tela de título, pressione um botão no controle desejado. Uma tecla, um clique ou um toque seleciona o teclado (**K** também funciona). Pressione **SL** ou **SR** para um Joy-Con na horizontal. Com um Joy-Con esquerdo e um direito, um botão frontal seleciona o par; com várias metades conectadas, segure **L** no esquerdo e **R** no direito que deseja combinar e solte os dois. Volte ao título para escolher de novo.

**Adicionar jogadores:** ao entrar na seleção de personagens, **Controller assignment** destaca a primeira vaga. Pressione **L + R** no controle, **SL + SR** num Joy-Con na horizontal ou **Enter** num teclado disponível para entrar. **Cancel**, **Esc** ou o botão Voltar do jogador 1 cancelam; quando todos entrarem, o jogador 1 confirma com **OK**.

**Reatribuir:** na seleção de personagens, com dois a quatro jogadores locais, pressione **+** no controle do jogador 1 (ou a tecla de Start, ou clique em **−/+**) para abrir o menu de controles. Selecione uma linha para atribuir um controle livre ou trocar dois jogadores e use **Apply**.

**Keyboard Splitter no Windows:** o MK8-online tem uma vaga para o teclado nativo. Se uma tecla mover dois jogadores, abra o menu de controles na seleção de personagens e escolha **Clear all**. Pressione os botões mapeados como **L + R** em cada um dos dois controles virtuais, na ordem dos jogadores, e escolha **Apply**. Isso remove a atribuição do teclado nativo e deixa apenas os controles virtuais enviarem comandos para esses jogadores.

**Separar um par de Joy-Con:** no menu de controles, selecione o jogador que vai receber uma metade e segure **SL + SR** nesse Joy-Con. **Apply** separa o par e **Cancel** mantém. Cada metade passa a controlar o próprio jogador. Pro Controller e teclado podem ser combinados com Joy-Con em qualquer ordem.

**Desconexões:** numa corrida local em tela dividida, quem desconecta fica sem comandos e os demais continuam. Um controle identificável volta à mesma vaga ao reconectar.

**Vibração HD:** no Pro Controller original do Switch, nos Joy-Con individuais e num par identificável de Joy-Con oficiais via Bluetooth. Os demais controles usam a vibração da SDL.

**Intensidade da vibração** começa em 100% da saída calibrada de cada controle. Selecione 0% em **Controles** no menu F1 para parar a vibração de todos os jogadores. Também é possível usar o controle deslizante em **Configurações → Vídeo e controles → Controles** no launcher antes de iniciar o jogo. As opções anteriores ligado e desligado passam a 100% e 0%.

**Direção por movimento:** conecte um controle que forneça giroscópio e acelerômetro à SDL. No menu de pausa da corrida, pressione **Y** para ativar os **controles por movimento** e segure o controle como um volante.

No Windows, o jogo pode ler os sensores de movimento de um Switch Pro Controller via Bluetooth enquanto a Entrada Steam cuida dos botões e analógicos. O leitor de movimento inicia automaticamente. Se o controle desconectar, reconecte-o por Bluetooth para retomar a direção. Se os controles por movimento estiverem indisponíveis na sua configuração, selecione **Desativar Entrada Steam** em **Propriedades → Controle** do jogo na Steam e reinicie para usar o controle nativo. [Relato sobre a Entrada Steam #164](https://github.com/Vnnc1/MK8-online/issues/164)

## Menu do jogo e configurações

O Menu usa o tema escolhido no launcher (**Vídeo → Tema do menu**): Nativo, Simples escuro, Simples claro, NES escuro ou NES claro. **F1**, **Home/Guide**, **Menos + Mais** e a chave inglesa do menu principal o abrem; **Shift+F1** abre o painel convencional. O idioma acompanha o launcher.

Pressione **F1** ou **Menos + Mais** para abrir o menu do jogo. **Jogar** volta à partida ou abre o Perfil Mii; **Ajustes** tem vídeo, áudio, controles e mods. Após o login no Nextendo, ele também mostra as salas reais do MK8 e os amigos. O **menu nativo de configurações**, opcional, acrescenta uma chave inglesa ao menu principal do jogo.

O botão **Info** do menu principal abre a janela local **How to Play**, com controles, drift, itens, assistências e modos. Feche com **Back to game** ou Esc.

No launcher, **Configurações** reúne vídeo, áudio, idioma do jogo, ícones dos botões, ordem dos controles e mapeamento de teclado/controle. O arquivo `user-no-mii/mk8d.ini` é o mesmo do menu F1. **Salvar configurações** aplica no próximo início; **Recarregar do jogo** lê os valores gravados pelo menu F1. Não é possível salvar enquanto o jogo ou uma preparação usa os dados.

**Vídeo → Ocultar cursor automaticamente** vem ativado. Em tela cheia, o ponteiro some após dois segundos de inatividade. Usar o mouse, abrir o menu F1 ou sair da tela cheia faz o ponteiro voltar.

## Online com o Nextendo

1. Confirme o e-mail da sua conta Nextendo e vincule o Discord a ela. [Gerenciar conta Nextendo](https://nextendo.network/account)
2. Atualize a build se for solicitado e selecione **Jogar → Jogar no Nextendo**.
3. No primeiro acesso, o launcher mostra a impressão digital da chave pública autoassinada do servidor de corrida e pergunta se você confia nela neste PC. Os demais serviços verificam a CA do Nextendo e o nome do servidor.
4. Entre pelo navegador. Mantenha o launcher aberto: a autorização fica na memória enquanto a janela estiver aberta.
5. No Mario Kart, abra **Online Play → Mundial → Corrida**.

As sessões Nextendo usam as pistas originais; o CTGP fica desativado na sessão. A reconexão é experimental. Se o Nextendo retornar `discord_unlinked`, vincule o Discord à mesma conta e selecione **Jogar no Nextendo** novamente.

## Salas Steam com amigos

Cada jogador precisa do próprio jogo preparado (mesma versão 4.0.1, mesmo estado de DLC e CTGP), de um Mii e da Steam aberta. Deixe o launcher aberto em **Multijogador**: a Steam o mostra como **Spacewar** (AppID 480, o aplicativo de testes da Steam), e é assim que os convites chegam.

1. **Anfitrião:** **Multijogador → Criar sala**, depois **Convidar** ao lado do amigo (ou **Convidar pela Steam**). O jogo do anfitrião abre e cria a sala LAN.
2. **Amigo:** aceite o convite no launcher (**Aceitar**), no chat da Steam ou com **Entrar** na sala do amigo e escolha **um ou dois jogadores locais**. O jogo abre a lista de salas LAN e entra; com dois jogadores, o segundo se registra e escolhe o Mii antes.
3. Todos escolhem personagem e kart; o anfitrião define as regras.

Se a entrada automática parar, continue à mão: no menu principal, segure **L + R** e clique o **analógico esquerdo** para mostrar **LAN Play**. O anfitrião usa **Create Room**; os convidados escolhem a sala em **Lobby**.

**DLL Steam:** o launcher tenta a DLL configurada, a pasta de dados e a pasta do executável e depois procura nas bibliotecas Steam instaladas, passando à próxima candidata se uma não inicializar as interfaces necessárias. Se ainda faltar `SteamAPI_SteamFriends_v017`, aponte `MK8_STEAM_DLL` para o caminho completo de uma `steam_api64.dll` compatível (`libsteam_api.so` no Linux) e reabra o launcher.

**Limites conhecidos:** um amigo com o launcher fechado verá a Steam tentar abrir o Spacewar; abra o launcher e use **Entrar**. Uma corrida completa entre dois PCs ainda não foi qualificada, e só dois jogadores foram testados (o jogo aceita até 12 em LAN).

## DLC Booster Course Pass

Importe na preparação (**Arquivos do jogo**) ou depois em **Instalar → Conteúdo adicional**: selecione o NSP ou NSZ do Booster Course Pass v65536 (título `0100152000023001`) e, se necessário, suas chaves, e use **Importar DLC NSP/NSZ…**. O importador confere os hashes dos NCAs, o CNMT e os arquivos extraídos. Uma reinstalação com falha preserva a instalação anterior.

Ative ou desative em **Configurações → Conteúdo adicional → Ativar Booster Course Pass**. Desativar mantém os arquivos importados e os saves. **Reinstalar ou selecionar outro NSP/NSZ** permite escolher outro pacote; os arquivos só são substituídos após a validação.

## Pacote de pistas CTGP Deluxe

O CTGP Deluxe 1.1.1 foi feito para o MK8 3.0.3. O MK8-online adapta seus recursos de pistas ao 4.0.1 sem executar o plugin original. A compatibilidade é incompleta: persistem bugs por pista, defeitos gráficos e quedas de desempenho.

1. Baixe o CTGP Deluxe em [ctgpdx.com](https://www.ctgpdx.com/download) (uso pessoal) e extraia.
2. **Instalar → Conteúdo adicional → CTGP Deluxe**: escolha a pasta extraída (a que tem `atmosphere/`) e clique em **Importar CTGP…**. No mesmo disco da pasta de dados leva segundos e não ocupa espaço extra (hardlinks). Escolha onde ficam as pistas:
   - **No lugar das copas do jogo base** — 48 pistas, +16 com o DLC.
   - **No lugar das copas do DLC** — 48 pistas ao lado das 48 originais; requer o Booster Course Pass.
   - **Em 16 copas novas** — as 64 pistas em mais duas páginas de copas (L/R), com nomes, imagens, músicas e ícones próprios.
3. **Ativar CTGP Deluxe** liga o pacote; desligue para jogar o jogo original. Com o CTGP ligado, o jogo usa saves próprios, criados uma vez como cópia dos seus.

Nas salas Steam, todos precisam do mesmo estado de CTGP. Os menus próprios e o botão aleatório do CTGP não estão disponíveis; os fantasmas de staff são os da pista original; as sessões online usam as copas do jogo. O MK8-online não distribui o CTGP.

## Mods de RomFS

Mods que só substituem arquivos do jogo (texturas, modelos, músicas, layouts) para o MK8 Deluxe 4.0.1 podem ser instalados à mão. Coloque os arquivos extraídos do mod em `<pasta de dados>/user-no-mii/mods/<nome do mod>/romfs/`, com os mesmos caminhos do RomFS do jogo. No jogo, abra **F1 → Configurações → Mods**, mude **Show** para **Installed**, selecione o mod, ative-o, feche e abra o jogo de novo. Para voltar aos arquivos originais, desative o mod e reinicie. Quando vários mods substituem o mesmo arquivo, vale o que estiver mais abaixo na lista de Mods.

Só arquivos de RomFS são carregados: patches de ExeFS, patches IPS, cheats e plugins executáveis não funcionam. Um mod que troca o layout do menu (`UI/cmn/common.sarc` ou `menu.sarc`) desliga a entrada nativa do Menu do launcher enquanto estiver ativo. As sessões do Nextendo começam sem os mods instalados. As salas Steam comparam o DLC e o CTGP, mas não os mods instalados à mão, então combine com os amigos. Para o CTGP Deluxe, use o importador acima.

## Fontes do console

Por padrão, os diálogos usam fontes substitutas (Segoe UI antes de Arial no Windows). Para usar fontes extraídas do seu próprio Switch:

1. Com homebrew funcionando, coloque o `.nro` dos [releases do SharedFontDumper](https://github.com/simontime/SharedFontDumper/releases) em `switch/` no SD e abra pelo Homebrew Menu. Pressione **A** para extrair TTFs individuais e **+** para sair.
2. Copie a pasta `fonts` do SD para o PC.
3. Em **Configurações → Fontes do jogo**, use **Importar pasta de fontes**. É necessário `FontStandard.ttf` ou `nintendo_udsg-r_std_003.ttf/.bfttf`; fontes regionais/de botões são opcionais.

**Padrão do sistema** desativa as fontes importadas sem apagá-las; **Fontes do console importadas** reativa. As mudanças valem no próximo início. `shared_font.bin` e arquivos de firmware não são aceitos.

## Relatar um problema

Em **Suporte → Gerar relatório**, crie o ZIP de diagnóstico e anexe a uma [issue](https://github.com/Vnnc1/MK8-online/issues) com CPU/GPU, versão do launcher, o que você fez e o que aconteceu. Para multijogador, anexe relatórios dos **dois** PCs. Os relatórios incluem logs do jogo, da build, das ferramentas e do DLC; caminhos pessoais e nomes de conta são ocultados, e saves e chaves ficam de fora. Revise os anexos antes de compartilhar. Nunca publique ROMs, chaves do console ou arquivos privados.

A aba **Suporte** também prepara um rascunho de issue no GitHub para você revisar e publicar; nada é enviado automaticamente.

## Solução de problemas

| Sintoma | O que fazer |
| --- | --- |
| A build parou com a mensagem `MAX_PATH` do RomFS | Instale a 0.5.0 ou mais nova, mantenha a mesma pasta de dados e tente de novo em **Instalar → Build do jogo**. O trabalho concluído é reaproveitado. |
| O jogo fecha inesperadamente | O launcher mostra a última mensagem fatal do runtime e guarda um ZIP de diagnóstico em **Suporte**. Anexe-o a uma issue. |
| Uma correção da versão nova não aparece | Recompile: **Instalar → Atualizar a build** (ou **Ferramentas → Build do jogo**). A maioria das correções do runtime exige recompilação. |
| Engasgo na primeira vez que um efeito aparece | Esperado com cache de shaders frio; o cache em disco ajuda nas corridas seguintes. |
| Lista de amigos Steam vazia ou falta `SteamAPI_SteamFriends_v017` | Veja [DLL Steam](#salas-steam-com-amigos). |
| `discord_unlinked` no Nextendo | Vincule o Discord à mesma conta Nextendo e tente de novo. |
| O controle não responde | Informe o modelo e o tipo de conexão junto com o relatório de **Suporte**. |
| Linux: **Ferramentas** diz que o Python 3.12 não foi encontrado | Instale o Python 3.12 (Ubuntu: `python3.12 python3.12-venv`; Arch/CachyOS: `python312` do AUR, pyenv ou uv) ou defina `MK8_PYTHON312` com o caminho dele e reabra o launcher. |
