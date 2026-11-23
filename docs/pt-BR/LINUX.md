# Linux amd64 · 0.5.0 (experimental)

[English](../en/LINUX.md) · **Português** · [AppImage](https://github.com/Vnnc1/MK8-online/releases/download/v0.5.0/MK8-online-0.5.0-linux-x86_64.AppImage) · [SHA-256](https://github.com/Vnnc1/MK8-online/releases/download/v0.5.0/MK8-online-0.5.0-linux-x86_64.AppImage.sha256)

O AppImage inclui o launcher nativo para Linux, o importador e os fontes usados para compilar o jogo no seu PC. A edição Linux 0.5.0 vem do porte de mitch030504 na PR #83.

| | |
| --- | --- |
| CPU / sistema | Intel ou AMD de 64 bits, Linux amd64 |
| GPU | Driver Vulkan 1.3 |
| Distribuição de referência | Ubuntu 24.04 LTS amd64 (ambiente de compilação) |
| Hardware testado | Intel Iris Xe (registro de pista renderizada pelo porte) |
| Steam Deck | Modo desktop, experimental, aguardando uma corrida no aparelho físico |

[Porte e testes de hardware](https://github.com/Vnnc1/MK8-online/pull/83)

## Instalação

1. **Ubuntu 24.04** — instale os [pacotes](#pacotes-do-ubuntu-2404) abaixo. Eles fornecem as bibliotecas da interface, o Python 3.12 e o compilador usado para preparar seu jogo. Mantenha instalado o driver Vulkan da sua GPU.
2. **Steam Deck** — entre no modo desktop. Se tiver Distrobox e Podman em modo usuário, [crie um ambiente Ubuntu 24.04](#steam-deck-ubuntu-via-distrobox) e instale as dependências nele. Abra o AppImage nesse mesmo ambiente, agora e nas próximas vezes. Assim, as ferramentas ficam fora da partição de sistema do SteamOS. Use uma pasta gravável na sua área de usuário, com espaço para compilar o jogo. [Configuração do Distrobox](https://distrobox.it/compatibility/) · [Criação do ambiente](https://distrobox.it/usage/distrobox-create/)
3. Baixe o AppImage **e** seu arquivo SHA-256 na mesma pasta e [inicie-o](#iniciar). O tar.gz da release também abre por `MK8-online/AppRun`.
4. Siga **Instalação → Ferramentas → Arquivos do jogo → Build do jogo**. Selecione seus arquivos de jogo base, atualização 4.0.1 e chaves do console. O DLC é opcional.

Os dados ficam em `$XDG_DATA_HOME/MK8-online` ou `~/.local/share/MK8-online`. Mantenha o launcher e os dados em pastas graváveis. Para relatar um problema, envie a distribuição, a GPU, o driver, o modo de jogo e o relatório da tela **Suporte**.

### Pacotes do Ubuntu 24.04

```sh
sudo apt update
sudo apt install build-essential clang lld llvm cmake pkg-config python3.12-venv curl ca-certificates \
  libarchive-tools zstd file libwebkit2gtk-4.1-dev libgtk-3-dev libayatana-appindicator3-dev \
  librsvg2-dev libdbus-1-dev libvulkan-dev libshaderc-dev libavcodec-dev libavformat-dev \
  libavutil-dev libswscale-dev libx11-dev libxext-dev libxfixes-dev libxrandr-dev libxcursor-dev \
  libxi-dev libxss-dev libxtst-dev libwayland-dev libxkbcommon-dev libasound2-dev libpulse-dev \
  libpipewire-0.3-dev libdecor-0-dev libudev-dev
```

### Outras distribuições (Arch, CachyOS, Fedora)

A etapa de ferramentas precisa de um **Python 3.12** de 64 bits. Distribuições com `python3` mais novo costumam não ter `python3.12`: instale pelo AUR (`python312`), com `pyenv install 3.12` ou com `uv python install 3.12`. O launcher procura `MK8_PYTHON312`, `python3.12`, um `python3` que seja 3.12 e as instalações do pyenv, uv e asdf; para indicar um específico, abra o launcher com `MK8_PYTHON312=/caminho/para/python3.12`.

### Steam Deck: Ubuntu via Distrobox

```sh
distrobox create --name mk8-ubuntu --image docker.io/library/ubuntu:24.04
distrobox enter mk8-ubuntu
```

### Iniciar

```sh
sha256sum -c MK8-online-0.5.0-linux-x86_64.AppImage.sha256 &&
chmod +x MK8-online-0.5.0-linux-x86_64.AppImage &&
./MK8-online-0.5.0-linux-x86_64.AppImage
```

Sem FUSE:

```sh
./MK8-online-0.5.0-linux-x86_64.AppImage --appimage-extract-and-run
```

[Documentação do AppImage](https://docs.appimage.org/user-guide/run-appimages.html) · [Fonte Linux 0.5.0](https://github.com/Vnnc1/MK8-online/tree/edfc423697977594b49db64b67c433c73086664f)
