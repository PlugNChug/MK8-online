<div align="center">

<img src="docs/tag-ratone-tend.png" alt="RatOne — criador e mantenedor do MK8-online" width="560">

# MK8-online

### Prepare uma vez. Corra nativamente. Conecte-se com amigos.

**Criado e mantido por [RatOne](https://github.com/Vnnc1)**

[English](README.md) · **Português (Brasil)**

![Plataforma](https://img.shields.io/badge/Windows-x64-0078D6)
![Gráficos](https://img.shields.io/badge/Gráficos-Vulkan-AC162C)
![Linguagens](https://img.shields.io/badge/Rust-C%2FC%2B%2B-555555)
![Release](https://img.shields.io/badge/Release-0.2.0-ef795d)
![Estado](https://img.shields.io/badge/Estado-Teste_público-e6a23c)
[![Licença](https://img.shields.io/badge/Licença-GPL--3.0--or--later-blue)](LICENSE)

Projeto experimental de recompilação estática de **Mario Kart 8 Deluxe 4.0.1** para Windows,
com launcher nativo, runtime Vulkan, criador de Mii e salas com amigos pela Steam.

[Download / Releases](https://github.com/Vnnc1/MK8-online/releases) · [Relatar um problema](https://github.com/Vnnc1/MK8-online/issues) · [Roteiro](docs/ROADMAP.md)

</div>

> [!IMPORTANT]
> Use seus próprios arquivos do jogo e as chaves do seu console. O repositório e as releases não incluem ROMs, recursos do jogo, chaves, saves ou código recompilado do jogo. A compilação acontece localmente, a partir dos seus arquivos.

> [!NOTE]
> **A 0.2.0 é uma release de teste público, ainda experimental.** Corridas solo são concluídas nos testes locais. Persistem defeitos visuais, engasgos no primeiro uso de shaders e crescimento de VRAM. O multijogador entre dois PCs diferentes ainda não foi qualificado: é justamente isso que esta release pede para os testadores experimentarem.

## Novidades da 0.2.0

- **Criador de Mii.** A aba **Perfil Mii** virou um criador. Dê um nome ao Mii e escolha cada parte (corpo, rosto, pele, cabelo, olhos, sobrancelhas, nariz, boca, barba, óculos, pinta) em botões que mostram o *seu* Mii com aquela opção. A prévia põe a cabeça no próprio corpo do MK8 usado nas salas, lido do seu jogo preparado: tipo, altura, porte e cor favorita ficam visíveis.
- **Seu Mii no jogo.** Ao salvar um Mii, o launcher prepara sozinho os modelos Mii do jogo. O jogo então desenha seu Mii com o renderer original: retratos na seleção, Mii Suits, o piloto com capacete e o ícone no minimapa.
- **Só salas pela Steam.** O **Multijogador** agora usa a Steam: crie uma sala e convide amigos da sua lista da Steam. Eles aceitam no launcher ou no chat da Steam, sem VPN e sem abrir portas. Você entra nas salas como seu Mii, e o nome dele é o nome que os outros veem.
- **O jogo abre na sala.** Depois de criar ou entrar numa sala, o launcher abre o jogo sozinho. Uma entrada automática experimental leva cada jogador à sala LAN: o anfitrião cria a sala, e os convidados entram assim que ela existe. Personagem e regras continuam por sua conta.
- **Correções:**
  - os logs do jogo não ficam mais vazios quando o launcher abre o jogo, então os relatórios de suporte voltam a contê-los;
  - a queda na votação de pista LAN (pedido de retrato Mii ausente) foi corrigida;
  - os caminhos dos recursos Mii funcionam no Windows;
  - pools de descritores e o histórico de cubemaps são liberados quando o jogo os finaliza.

Detalhes, evidências e limites: [estado técnico](docs/STATUS.md).

## Onde estamos

| Área | Progresso atual | Próxima etapa |
| --- | --- | --- |
| Launcher | Janela nativa, preparação guiada, instalador, atualizações e relatórios; inglês e PT-BR | Atualização 0.1.8 → 0.2.0 por testadores reais |
| Corrida solo | Mushroom Cup concluída; batalha, MKTV e contrarrelógio alcançados | Qualificação conjunta de imagem, áudio, controle e reinício/save |
| Mii | Criador, perfis e o renderer Mii do próprio jogo (retratos, trajes, piloto, minimapa) | Mii nas salas LAN, comparação com recursos do console |
| Multijogador | Salas Steam com convites; duas instâncias do jogo no mesmo PC entraram sozinhas na mesma sala LAN | Convidado entrando pela Steam a partir de outro PC, corridas completas entre dois PCs |
| Desempenho | Cerca de 60 FPS nos trechos medidos no PC de teste | Copas repetidas, estabilidade de VRAM e mais hardware |

São marcos observados, não um percentual de conclusão. `gameplay_verified` e `multiplayer_verified` continuam `false` até a qualificação completa.

## O que este projeto faz

O pipeline traduz o executável AArch64 do jogo para C e o compila para Windows x64. Um runtime C/C++ fornece gráficos, áudio, entrada e serviços de sistema; o launcher em Rust/Tauri cuida da preparação, builds, atualizações, Miis e salas.

RatOne desenvolve e mantém a adaptação do MK8, o launcher e a integração. A base de tradução/runtime vem do **SMM2Recomp e do LASRecomp**, com outros trabalhos creditados. Veja [fontes de terceiros e licenças](docs/THIRD_PARTY.md).

## Entre na pista

### Requisitos

- **Windows x64** e WebView2. O launcher instala sozinho um **Python 3.12 x64** privado.
- GPU e driver com **Vulkan 1.3 ou mais novo**. Não há especificação mínima geral de CPU/GPU qualificada.
- Seu próprio jogo base, a **atualização 4.0.1** e o `prod.keys` do seu console, ou uma pasta do jogo já extraída e suportada.
- Cerca de **28 GiB livres** para extração e preparação (incluindo 4 GiB de reserva de segurança), além de espaço para ferramentas e caches. SSD recomendado.
- Internet para o primeiro download das ferramentas (cerca de 0,4 GiB) e para atualizações.
- **Para Miis:** a biblioteca de peças `AFLResHigh_2_3.dat`, do seu próprio software. Ela não é distribuída, e o launcher confere o hash. Sem ela, as opções do Mii funcionam como botões numerados, mas os rostos não são desenhados.
- **Para multijogador:** o cliente da **Steam** aberto e logado, e qualquer jogo da Steam instalado que traga `steam_api64.dll` (ou uma cópia desse arquivo na pasta de dados do launcher). Nenhum arquivo da Valve é distribuído.

### Instalar e jogar

1. Baixe `MK8-online-0.2.0-setup.exe` em [Releases](https://github.com/Vnnc1/MK8-online/releases) e escolha as pastas do aplicativo e dos dados. A instalação é por usuário, sem administrador. Já tem a 0.1.8? Use **Suporte → Atualizar agora**: a pasta de dados e os saves são mantidos.
2. Abra **Instalar**. Conclua **Ferramentas** e selecione o jogo base e a atualização em **Arquivos do jogo** (o launcher encontra as chaves junto do jogo, ou selecione as suas).
3. Rode a etapa **Build do jogo**. Depois de atualizar de uma versão anterior, rode de novo: o jogo é recompilado com o runtime novo.
4. Escolha **Jogar solo**.

Uma pasta extraída precisa conter `exefs`, `romfs` e `control.nacp` (ou `control/control.nacp`): use **Já tenho o jogo extraído numa pasta**. O perfil ativo é o 4.0.1 AArch64; outras versões, DLC e importação de XCI/XCZ não foram validadas.

Os dados ficam por padrão em `%LOCALAPPDATA%\MK8-online` e sobrevivem a atualizações e à desinstalação. Para usar outro disco com o launcher portátil: `.\Launcher.exe --data-dir E:\MK8-Local`, e continue usando a mesma pasta.

**Idioma:** o launcher e o instalador oferecem **English** e **Português (Brasil)** (**Language / Idioma**). O idioma do jogo é independente.

### Crie seu Mii

1. Abra **Perfil Mii**. Na primeira vez, escolha o `AFLResHigh_2_3.dat` (**Procurar** e depois **Usar**).
2. Digite um nome e toque em **Criar Mii**. Passe pelas abas e toque na opção que quiser: cada botão mostra seu Mii com aquela opção. Em **Corpo** ficam tipo, altura, porte e cor favorita, e a prévia mostra o corpo inteiro (ou só o rosto).
3. Toque em **Salvar Mii**. Na primeira vez o launcher prepara os modelos Mii do jogo; o jogo usa seu Mii na próxima vez que abrir.

Você pode ter vários Miis e trocar entre eles no topo. **Perfil Mii** no menu **F1** do jogo traz esta tela à frente.

## Corra com amigos (Steam)

Cada jogador precisa da própria cópia do jogo preparada (mesma versão 4.0.1), de um Mii e da Steam aberta. Deixe o launcher aberto em **Multijogador**: a Steam o mostra como **Spacewar** (AppID 480, o aplicativo de testes da Steam), e é assim que os convites chegam até ele.

1. **Anfitrião:** **Multijogador → Criar sala**, depois **Convidar** ao lado do amigo (ou **Convidar pela Steam**). O jogo do anfitrião abre e cria a sala LAN sozinho.
2. **Amigo:** aceite o convite no launcher (**Aceitar**), no chat da Steam ou com **Entrar** na sala aberta do amigo. O jogo abre e entra na sala sozinho.
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

Em **Suporte → Gerar relatório**, crie o ZIP de diagnóstico e anexe a uma [issue](https://github.com/Vnnc1/MK8-online/issues), com CPU/GPU, versão do launcher, o que você fez e o que aconteceu. Para multijogador, anexe relatórios dos **dois** PCs. Os relatórios incluem logs do jogo, da build e das ferramentas. Caminhos pessoais e nomes de conta são ocultados, e saves e chaves ficam de fora. Revise os anexos antes de compartilhar. Nunca publique ROMs, chaves do console ou arquivos privados.

## Compilar e contribuir

O desenvolvimento acontece na `fix-chore`. São necessários Rust, Python 3.12 x64, PowerShell 7 e .NET 10 x64 (para o importador). Na raiz do repositório:

```powershell
.\scripts\start-launcher.ps1
```

O script baixa as fontes upstream fixadas, confere os hashes, compila o launcher e o abre. Para empacotar o launcher portátil: `python scripts/package-launcher.py` (saída: `target/portable/Launcher.exe`). Os passos de release estão em [RELEASING.md](docs/RELEASING.md).

Mantenha as adaptações em `runtime/mk8` e `toolchain`; a preparação as aplica a uma cópia verificada. Não edite os snapshots upstream nem o código gerado do jogo. Siga o [AGENTS.md](AGENTS.md) e rode os testes da área alterada. Caminhos só de desenvolvimento estão documentados no [estado técnico](docs/STATUS.md):

- transportes LAN/relay/P2P pela API;
- o conversor estático de cabeça Mii;
- o protótipo Nextendo.

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

## Créditos e licença

**O MK8-online é criado e mantido por RatOne.** Os autores upstream mantêm o crédito e a autoria das suas contribuições. Atribuições e avisos de dependências estão em [THIRD_PARTY.md](docs/THIRD_PARTY.md) e [docs/licenses](docs/licenses). O criador de Mii desenha os rostos com o [FFL.js](https://github.com/ariankordi/FFL.js) (AGPL-3.0).

O código do projeto é licenciado sob **[GPL-3.0-or-later](LICENSE)**. As dependências mantêm suas licenças:

- o snapshot do SMM2Recomp inclui GPLv3;
- o LASRecomp inclui avisos MIT;
- o setup leva o FFmpeg (LGPL 2.1), com o código-fonte anexado a cada release.

A licença não concede direitos sobre o código, os recursos ou as marcas da Nintendo.

Projeto comunitário independente, sem afiliação nem endosso da Nintendo ou da Valve. Mario Kart e nomes relacionados pertencem aos respectivos donos.
