<div align="center">

<img src="docs/tag-ratone-tend.png" alt="RatOne — criador e mantenedor do MK8-online" width="560">

# MK8-online

### Prepare uma vez. Corra nativamente. Conecte-se com amigos.

**Criado e mantido por [RatOne](https://github.com/Vnnc1)**

[English](README.md) · **Português (Brasil)**

![Versão](https://img.shields.io/badge/Vers%C3%A3o-0.6.3-ef795d)
![Estado](https://img.shields.io/badge/Estado-Teste_p%C3%BAblico-e6a23c)
![Plataformas](https://img.shields.io/badge/Windows_x64-Linux_amd64_%28experimental%29-0078D6)
![Gráficos](https://img.shields.io/badge/Gr%C3%A1ficos-Vulkan-AC162C)
[![Licença](https://img.shields.io/badge/Licen%C3%A7a-GPL--3.0--or--later-blue)](LICENSE)
[![Discord](https://img.shields.io/badge/Discord-Entrar-5865F2?logo=discord&logoColor=white)](https://discord.gg/4Yg4Axpkb6)

[Download](https://github.com/Vnnc1/MK8-online/releases) · [Guia do usuário](docs/pt-BR/GUIDE.md) · [Estado](docs/pt-BR/STATUS.md) · [Roteiro](docs/pt-BR/ROADMAP.md) · [Relatar um problema](https://github.com/Vnnc1/MK8-online/issues) · [Discord](https://discord.gg/4Yg4Axpkb6)

</div>

O MK8-online é uma recompilação estática experimental do **Mario Kart 8 Deluxe 4.0.1** para Windows x64 e Linux amd64. O executável AArch64 do jogo é traduzido para C e compilado no seu PC. Um runtime Vulkan fornece gráficos, áudio, entrada e serviços de sistema. Um launcher em Rust/Tauri cuida da preparação, das builds, das atualizações, dos Miis e do jogo online.

> [!IMPORTANT]
> Use seus próprios arquivos do jogo e as chaves do seu console. O repositório e as releases não incluem ROMs, recursos do jogo, chaves, saves nem código recompilado do jogo. A compilação acontece localmente, a partir dos seus arquivos.

## Recursos

- **Build nativa:** o jogo roda como executável nativo de Windows ou Linux, sem emulador.
- **Launcher guiado:** instalação, build do jogo, atualizações, desinstalação e relatórios de diagnóstico, em inglês, português do Brasil e espanhol.
- **Criador de Mii:** seu Mii aparece nos retratos, nas roupas, no piloto e no minimapa do jogo.
- **Jogo online:** matchmaking nativo pelo [Nextendo](https://nextendo.network/), além de salas Steam com convites de amigos.
- **Gráficos:** escala da resolução interna com FSR 1/3/4 e DLSS, além de ultrawide e geração de quadros experimentais.
- **Controles:** controles SDL, teclado, Pro Controller e Joy-Con do Switch, com vibração HD, direção por movimento e tela dividida local.
- **Conteúdo extra:** DLC Booster Course Pass opcional e pacote de pistas CTGP Deluxe experimental, ambos a partir dos seus arquivos, além de mods de RomFS que você mesmo instala (texturas, modelos, músicas).

## Download

| Plataforma | Pacote |
| --- | --- |
| Windows x64 | [Instalador](https://github.com/Vnnc1/MK8-online/releases/download/v0.6.3/MK8-online-0.6.3-setup.exe) · [ZIP portátil](https://github.com/Vnnc1/MK8-online/releases/download/v0.6.3/MK8-online-0.6.3-portable.zip) |
| Linux amd64 (experimental) | [AppImage](https://github.com/Vnnc1/MK8-online/releases/download/v0.6.3/MK8-online-0.6.3-linux-x86_64.AppImage) · [tar.gz](https://github.com/Vnnc1/MK8-online/releases/download/v0.6.3/MK8-online-0.6.3-linux-x86_64.tar.gz) · [Guia Linux](docs/pt-BR/LINUX.md) |

[Notas da versão 0.6.3](docs/releases/0.6.3.md) · [Todas as versões](https://github.com/Vnnc1/MK8-online/releases)

## Requisitos

- Windows x64 com WebView2, ou Linux amd64 ([detalhes](docs/pt-BR/LINUX.md))
- GPU e driver com **Vulkan 1.3** ou mais novo
- Seu próprio jogo base, a **atualização 4.0.1** e o `prod.keys`
- Cerca de **28 GiB** livres em disco (SSD recomendado) e internet na primeira configuração

## Início rápido

1. Execute o instalador e escolha as pastas do aplicativo e dos dados. Não é preciso ser administrador.
2. Em **Instalar**, conclua **Ferramentas** e selecione o jogo base e a atualização em **Arquivos do jogo**.
3. Rode **Build do jogo**. A primeira build leva cerca de 30 minutos.
4. Crie um Mii ou importe um arquivo `.charinfo` em **Perfil Mii** e escolha **Jogar solo**, **Jogar no Nextendo** ou **Multijogador**.

**Atualizando?** Use **Suporte → Atualizar agora**, mantenha a pasta de dados e rode **Atualizar a build** se o launcher pedir. O [guia do usuário](docs/pt-BR/GUIDE.md) cobre gráficos, controles, jogo online, DLC, CTGP, modo portátil e solução de problemas.

## Estado do projeto

O MK8-online está em **teste público**. Ele chega ao menu e completa copas inteiras, e já houve corridas online no Nextendo, mas tem defeitos conhecidos e **não está qualificado**: `gameplay_verified` e `multiplayer_verified` continuam `false`. No PC de referência (RX 580), os trechos de corrida medidos ficam em torno de 60 FPS. Os engasgos de shaders no primeiro uso e o crescimento da VRAM entre pistas continuam em aberto.

[Estado atual](docs/pt-BR/STATUS.md) · [Roteiro](docs/pt-BR/ROADMAP.md)

## Contribuir

A ajuda mais útil são relatos de bugs com o ZIP de diagnóstico de **Suporte → Gerar relatório** ([como relatar](docs/pt-BR/GUIDE.md#relatar-um-problema)). Para compilar a partir do código-fonte ou enviar mudanças, leia o [CONTRIBUTING](docs/pt-BR/CONTRIBUTING.md). Agentes de IA também devem seguir o [AGENTS](AGENTS.md).

## Créditos e licença

**O MK8-online é criado e mantido por RatOne.** A base de tradução e de runtime vem do **SMM2Recomp** e do **LASRecomp**, com outros trabalhos creditados de outros projetos e colaboradores. Os autores upstream mantêm o crédito e a autoria das suas contribuições. O criador de Mii desenha os rostos com o [FFL.js](https://github.com/ariankordi/FFL.js) (AGPL-3.0). Veja [fontes de terceiros e licenças](docs/pt-BR/THIRD_PARTY.md) e [docs/licenses](docs/licenses).

O código do projeto é licenciado sob **[GPL-3.0-or-later](LICENSE)**. As dependências mantêm suas próprias licenças: o snapshot do SMM2Recomp inclui GPLv3, o LASRecomp inclui avisos MIT e o setup leva o FFmpeg (LGPL 2.1), com o código-fonte anexado a cada release. A licença não concede direitos sobre o código, os recursos ou as marcas da Nintendo.

O MK8-online é um projeto comunitário independente, sem afiliação nem endosso da Nintendo ou da Valve. Mario Kart e nomes relacionados pertencem aos respectivos donos.
