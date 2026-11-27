# Linux amd64 · 0.5.0 (experimental)

**English** · [Português](../pt-BR/LINUX.md) · [AppImage](https://github.com/Vnnc1/MK8-online/releases/download/v0.5.0/MK8-online-0.5.0-linux-x86_64.AppImage) · [SHA-256](https://github.com/Vnnc1/MK8-online/releases/download/v0.5.0/MK8-online-0.5.0-linux-x86_64.AppImage.sha256)

The AppImage contains the native Linux launcher, the importer and the sources used to build the game on your PC. The 0.5.0 Linux edition is based on mitch030504's port in PR #83.

| | |
| --- | --- |
| CPU / OS | 64-bit Intel or AMD, Linux amd64 |
| GPU | Vulkan 1.3 driver |
| Reference distro | Ubuntu 24.04 LTS amd64 (build environment) |
| Tested hardware | Intel Iris Xe (gameplay evidence from the port) |
| Steam Deck | Desktop Mode, experimental, awaiting a race on physical hardware |

[Port and hardware results](https://github.com/Vnnc1/MK8-online/pull/83)

## Install

1. **Ubuntu 24.04** — install the [packages](#ubuntu-2404-packages) below. They supply the desktop libraries, Python 3.12 and the compiler that builds your own game. Keep the Vulkan driver for your GPU installed.
2. **Steam Deck** — switch to Desktop Mode. If Distrobox and rootless Podman are available, [create an Ubuntu 24.04 container](#steam-deck-ubuntu-via-distrobox) and install the dependencies there. Run the AppImage, now and on later launches, inside that same container. This keeps development packages off the SteamOS system partition. Use a writable folder in your home directory with ample space. [Distrobox setup](https://distrobox.it/compatibility/) · [Creating a container](https://distrobox.it/usage/distrobox-create/)
3. Download **both** the AppImage and its checksum to the same folder, then [launch it](#launch). The tar.gz in Releases also opens via `MK8-online/AppRun`.
4. Follow **Install → Tools → Game files → Build the game**. Select your own base game, 4.0.1 update and console keys. DLC is optional.

Data defaults to `$XDG_DATA_HOME/MK8-online` or `~/.local/share/MK8-online`. Keep the launcher and its data in writable locations. When reporting an issue, include the distro, GPU/driver, mode and the report from **Support**.

### Ubuntu 24.04 packages

```sh
sudo apt update
sudo apt install build-essential clang lld llvm cmake pkg-config python3.12-venv curl ca-certificates \
  libarchive-tools zstd file libwebkit2gtk-4.1-dev libgtk-3-dev libayatana-appindicator3-dev \
  librsvg2-dev libdbus-1-dev libvulkan-dev libshaderc-dev libavcodec-dev libavformat-dev \
  libavutil-dev libswscale-dev libx11-dev libxext-dev libxfixes-dev libxrandr-dev libxcursor-dev \
  libxi-dev libxss-dev libxtst-dev libwayland-dev libxkbcommon-dev libasound2-dev libpulse-dev \
  libpipewire-0.3-dev libdecor-0-dev libudev-dev
```

If the tools step must build SDL, install CMake and the development packages for your display and audio backends in the build environment. The Ubuntu list above includes these packages. For other distributions, see [SDL's Linux build dependencies](https://wiki.libsdl.org/SDL3/README-linux#build-dependencies).

### Other distributions (Arch, CachyOS, Fedora)

The tools step needs a 64-bit **Python 3.12**. Distributions that ship a newer `python3` usually have no `python3.12`: install it from the AUR (`python312`), with `pyenv install 3.12` or with `uv python install 3.12`. The launcher looks for `MK8_PYTHON312`, `python3.12`, a `python3` that is 3.12, and the pyenv, uv and asdf installs; to point at a specific one, start the launcher with `MK8_PYTHON312=/path/to/python3.12`.

Keep that Python installation available because the tools environment links to it. If setup leaves an incomplete environment, run **Tools** again. Setup preserves the old environment under `local/python-incomplete-*/` inside the active version directory and creates a fresh one.

### Steam Deck: Ubuntu via Distrobox

```sh
distrobox create --name mk8-ubuntu --image docker.io/library/ubuntu:24.04
distrobox enter mk8-ubuntu
```

### Launch

```sh
sha256sum -c MK8-online-0.5.0-linux-x86_64.AppImage.sha256 &&
chmod +x MK8-online-0.5.0-linux-x86_64.AppImage &&
./MK8-online-0.5.0-linux-x86_64.AppImage
```

Without FUSE:

```sh
./MK8-online-0.5.0-linux-x86_64.AppImage --appimage-extract-and-run
```

[AppImage documentation](https://docs.appimage.org/user-guide/run-appimages.html) · [Linux 0.5.0 source](https://github.com/Vnnc1/MK8-online/tree/edfc423697977594b49db64b67c433c73086664f)

### Containers with a noexec /tmp

The Fedora 44 image used by the Unraid Steam Headless 2 template can extract the 0.6.3 AppImage and then exit with code 127 because `libwebkit2gtk-4.1.so.0` is missing. The launcher also uses `bsdtar` to unpack its files on first use. From a root shell **inside the Fedora container**, install both packages. Include this command in your derived image if you recreate the container regularly. [Steam Headless template](https://steamheadless.com/docs/installation/deployment/unraid-template/)

```sh
dnf --repo=fedora --repo=updates install -y webkit2gtk4.1 bsdtar
```

From the container desktop, as your usual user, run the following in the download directory. Use a home directory on a filesystem that allows execution.

```sh
sha256sum -c MK8-online-0.6.3-linux-x86_64.AppImage.sha256 &&
chmod +x MK8-online-0.6.3-linux-x86_64.AppImage &&
mkdir -p -m 700 "$HOME/tmp" &&
TMPDIR="$HOME/tmp" APPIMAGE_EXTRACT_AND_RUN=1 ./MK8-online-0.6.3-linux-x86_64.AppImage
```

With those packages installed, the released AppImage opened its setup window and served the launcher interface in Steam Headless 2.44.20261019.1 while `/tmp` remained `noexec`. The test used a virtual display. [Steam Headless verification](https://github.com/PlugNChug/MK8-online/actions/runs/46038695417)

You can also use the tarball on a writable filesystem that allows execution. It needs the same libraries and `bsdtar` (`libarchive-tools` on Ubuntu).

```sh
sha256sum -c MK8-online-0.6.3-linux-x86_64.tar.gz.sha256 &&
tar -xzf MK8-online-0.6.3-linux-x86_64.tar.gz &&
./MK8-online/AppRun
```

The tarball and the AppImage with TMPDIR on an executable filesystem both served the launcher and ran its importer in an Ubuntu 24.04 container with a noexec /tmp. The AppImage also opened a desktop window with APPIMAGE_EXTRACT_AND_RUN=1. If startup fails, run the entry point from a terminal and keep its first error line. [Container verification](https://github.com/PlugNChug/MK8-online/actions/runs/46033999648)
