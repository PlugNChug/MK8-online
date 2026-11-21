# Linux amd64 · 0.5.0 (experimental)

[English](#english) · [Português](#português) · [AppImage](https://github.com/Vnnc1/MK8-online/releases/download/v0.5.0/MK8-online-0.5.0-linux-x86_64.AppImage) · [SHA-256](https://github.com/Vnnc1/MK8-online/releases/download/v0.5.0/MK8-online-0.5.0-linux-x86_64.AppImage.sha256)

## English

The AppImage contains the native Linux launcher, the importer and the sources used to build the game on your PC. The 0.5.0 Linux edition is based on mitch030504’s port in PR #83. Use a 64-bit Intel or AMD system with a Vulkan 1.3 driver. Ubuntu 24.04 LTS amd64 is the build environment. The port also has gameplay evidence on Linux with Intel Iris Xe. Steam Deck Desktop Mode is an experimental target awaiting a race on physical hardware. [Port and hardware results](https://github.com/Vnnc1/MK8-online/pull/83)

1. **Ubuntu 24.04** · Install the packages in the first command block below. These supply the desktop libraries, Python 3.12 and the compiler that builds your own game. Keep the Vulkan driver for your GPU installed.
2. **Steam Deck** · Switch to Desktop Mode. If Distrobox and rootless Podman are available, the second command block opens an Ubuntu 24.04 environment where you can install the dependencies. Run the AppImage and subsequent launches inside that same environment. This keeps development packages off the SteamOS system partition. Use a writable folder in your home directory with ample space for a local build. [Distrobox setup](https://distrobox.it/compatibility/) · [Creating a container](https://distrobox.it/usage/distrobox-create/)
3. Download **both** the AppImage and its checksum to the same folder. Open a terminal there and run the launch commands below. If FUSE is unavailable, use the extraction option shown there. The tar.gz in Releases also opens via `MK8-online/AppRun`.
4. Follow **Install → Tools → Game files → Build the game**. Select your own base game, 4.0.1 update and console keys. DLC is optional. Data defaults to `$XDG_DATA_HOME/MK8-online` or `~/.local/share/MK8-online`. Keep the launcher and its data in writable locations. For an issue, include the distro, GPU/driver, mode and the report from **Support**.

## Português

O AppImage inclui o launcher nativo para Linux, o importador e os fontes usados para compilar o jogo no seu PC. A edição Linux 0.5.0 vem do porte de mitch030504 na PR #83. Use um sistema Intel ou AMD de 64 bits com driver Vulkan 1.3. O pacote é compilado no Ubuntu 24.04 LTS amd64. O porte também tem registro de pista renderizada em uma Intel Iris Xe no Linux. O modo desktop do Steam Deck continua experimental e aguarda uma corrida no aparelho físico. [Porte e testes de hardware](https://github.com/Vnnc1/MK8-online/pull/83)

1. **Ubuntu 24.04** · Instale os pacotes do primeiro bloco abaixo. Eles fornecem as bibliotecas da interface, o Python 3.12 e o compilador usado para preparar seu jogo. Mantenha instalado o driver Vulkan da sua GPU.
2. **Steam Deck** · Entre no modo desktop. Se tiver Distrobox e Podman em modo usuário, o segundo bloco abre um ambiente Ubuntu 24.04 para instalar as dependências. Abra o AppImage nesse ambiente também nas próximas vezes. Assim, as ferramentas ficam fora da partição de sistema do SteamOS. Use uma pasta gravável na sua área de usuário, com espaço para compilar o jogo. [Configuração do Distrobox](https://distrobox.it/compatibility/) · [Criação do ambiente](https://distrobox.it/usage/distrobox-create/)
3. Baixe o AppImage e seu arquivo SHA-256 na mesma pasta. Abra um terminal nela e execute os comandos de inicialização abaixo. Se faltar FUSE, use a opção de extração indicada. O tar.gz da release também abre por `MK8-online/AppRun`.
4. Siga **Instalação → Ferramentas → Arquivos do jogo → Build do jogo**. Selecione seus arquivos de jogo base, atualização 4.0.1 e chaves do console. O DLC é opcional. Os dados ficam em `$XDG_DATA_HOME/MK8-online` ou `~/.local/share/MK8-online`. Mantenha o launcher e os dados em pastas graváveis. Para relatar um problema, envie a distribuição, a GPU, o driver, o modo de jogo e o relatório da tela **Suporte**.

### Ubuntu 24.04 · Packages / Pacotes

```sh
sudo apt update
sudo apt install build-essential clang lld llvm cmake pkg-config python3.12-venv curl ca-certificates \
  libarchive-tools zstd file libwebkit2gtk-4.1-dev libgtk-3-dev libayatana-appindicator3-dev \
  librsvg2-dev libdbus-1-dev libvulkan-dev libshaderc-dev libavcodec-dev libavformat-dev \
  libavutil-dev libswscale-dev libx11-dev libxext-dev libxfixes-dev libxrandr-dev libxcursor-dev \
  libxi-dev libxss-dev libxtst-dev libwayland-dev libxkbcommon-dev libasound2-dev libpulse-dev \
  libpipewire-0.3-dev libdecor-0-dev libudev-dev
```

### Steam Deck · Ubuntu via Distrobox

```sh
distrobox create --name mk8-ubuntu --image docker.io/library/ubuntu:24.04
distrobox enter mk8-ubuntu
```

### AppImage · Launch / Iniciar

```sh
sha256sum -c MK8-online-0.5.0-linux-x86_64.AppImage.sha256 &&
chmod +x MK8-online-0.5.0-linux-x86_64.AppImage &&
./MK8-online-0.5.0-linux-x86_64.AppImage
```

**Without FUSE / Sem FUSE**

```sh
./MK8-online-0.5.0-linux-x86_64.AppImage --appimage-extract-and-run
```

[AppImage documentation / Documentação do AppImage](https://docs.appimage.org/user-guide/run-appimages.html) · [Linux 0.5.0 source / Fonte Linux 0.5.0](https://github.com/Vnnc1/MK8-online/tree/edfc423697977594b49db64b67c433c73086664f)
