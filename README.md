<div align="center">

<img src="docs/tag-ratone-tend.png" alt="RatOne — creator and maintainer of MK8-online" width="560">

# MK8-online

### Prepare once. Race natively. Connect with friends.

**Created and maintained by [RatOne](https://github.com/Vnnc1)**

**English** · [Português (Brasil)](README.pt-BR.md)

[![Release](https://img.shields.io/github/v/release/Vnnc1/MK8-online?include_prereleases&label=Release&color=ef795d)](https://github.com/Vnnc1/MK8-online/releases/latest)
![Status](https://img.shields.io/badge/Status-Public_test-e6a23c)
![Platforms](https://img.shields.io/badge/Windows_x64-Linux_amd64_%28experimental%29-0078D6)
![Graphics](https://img.shields.io/badge/Graphics-Vulkan-AC162C)
[![License](https://img.shields.io/badge/License-GPL--3.0--or--later-blue)](LICENSE)
[![Discord](https://img.shields.io/badge/Discord-Join-5865F2?logo=discord&logoColor=white)](https://discord.gg/4Yg4Axpkb6)

[Download](https://github.com/Vnnc1/MK8-online/releases/latest) · [User guide](docs/GUIDE.md) · [Linux guide](docs/LINUX.md) · [Roadmap](docs/ROADMAP.md) · [Report an issue](https://github.com/Vnnc1/MK8-online/issues) · [Discord](https://discord.gg/4Yg4Axpkb6)

</div>

MK8-online is an experimental static recompilation of **Mario Kart 8 Deluxe 4.0.1** for Windows x64 and Linux amd64. The game's AArch64 executable is translated to C and compiled on your PC. A Vulkan runtime supplies graphics, audio, input and system services. A Rust/Tauri launcher handles preparation, builds, updates, Miis and online play.

> [!IMPORTANT]
> Bring your own game files and console keys. This repository and its releases contain no ROMs, game assets, keys, saves or recompiled game code. The game is compiled locally from your own files.

## How it works

1. **Your files.** You select your own base game, the 4.0.1 update and the keys from your console. The launcher verifies them by hash and extracts them into your data folder.
2. **Recompilation on your PC.** The pipeline translates the game's AArch64 code into C, applies the project's verified adaptations and compiles it with a pinned toolchain into native Windows or Linux libraries. Nothing recompiled is downloaded; the first build takes around 30 minutes and later builds reuse finished work.
3. **Native runtime.** The game runs as a native program. The runtime implements the console services the game expects and draws through Vulkan, with its own settings, Menu and diagnostics.
4. **Launcher.** One window prepares, builds, updates and starts the game, manages your Miis, settings and optional content, and connects you to friends through Nextendo or Steam rooms.

## Features

- **Native build:** the game runs as a native Windows or Linux executable, with no emulator.
- **Guided launcher:** installation, game build, updates, uninstall and diagnostic reports, in English, Brazilian Portuguese and Spanish.
- **Mii creator:** your Mii appears in game portraits, suits, the driver and the minimap.
- **Online play:** native matchmaking through [Nextendo](https://nextendo.network/), plus Steam rooms with friend invites.
- **Graphics:** internal resolution scaling with FSR 1/3/4 and DLSS, bloom and camera options, experimental ultrawide and frame generation.
- **In-game Menu:** five themes that share settings with the launcher, including a native game-style Menu.
- **Controllers:** SDL gamepads, keyboard, Switch Pro Controller and Joy-Cons, with player assignment, HD Rumble, motion steering and local split-screen.
- **Extra content:** optional Booster Course Pass DLC and an experimental CTGP Deluxe course pack, both from your own files.

## Download

| Platform | Package |
| --- | --- |
| Windows x64 | Installer (`MK8-online-<version>-setup.exe`) or portable ZIP |
| Linux amd64 (experimental) | AppImage or tarball · [Linux guide](docs/LINUX.md) |

All packages are on the [latest release](https://github.com/Vnnc1/MK8-online/releases/latest), each with a SHA-256 file.

## Requirements

- Windows x64 with WebView2, or Linux amd64 ([details](docs/LINUX.md))
- A GPU and driver with **Vulkan 1.3** or newer
- Your own base game, **4.0.1 update** and `prod.keys`
- About **28 GiB** of free disk space (SSD recommended) and an internet connection for the first setup

## Quick start

1. Run the installer and choose the application and data folders. No administrator rights are needed.
2. In **Install**, complete **Tools**, then select your base game and update in **Game files**.
3. Run **Build the game**.
4. Create a Mii in **Mii profile**, then choose **Play solo**, **Play on Nextendo** or **Multiplayer**.

When a new version arrives, use **Support → Update now**, keep your data folder and run **Update the build** if the launcher asks. The [user guide](docs/GUIDE.md) covers graphics, controllers, online play, DLC, CTGP, portable mode and troubleshooting.

## Project status

MK8-online is a **public test**. It reaches the menu, completes full cups and has been raced online, but it has known defects and is **not qualified**: `gameplay_verified` remains `false`. Every change, test and open problem is recorded in the [technical status](docs/STATUS.md) (in Portuguese).

## Contributing

Bug reports with a diagnostic ZIP from **Support → Create report** are the most useful help ([how to report](docs/GUIDE.md#reporting-a-problem)). To build from source on Windows, run `.\scripts\start-launcher.ps1`; it fetches the pinned upstream sources, verifies their hashes, builds the launcher and opens it. Release steps are in [RELEASING.md](docs/RELEASING.md). Contributors and AI agents follow [AGENTS.md](AGENTS.md): adaptations live in `runtime/mk8` and `toolchain`, never in `upstream` or generated code.

## Credits and license

**MK8-online is created and maintained by RatOne.** The translation and runtime foundation comes from **SMM2Recomp** and **LASRecomp**, with additional credited work from other projects and contributors. The native Linux port and the FSR/DLSS integration were contributed by mitch030504. Upstream authors keep credit and ownership of their contributions. The Mii creator draws faces with [FFL.js](https://github.com/ariankordi/FFL.js) (AGPL-3.0). See [third-party sources and licenses](docs/THIRD_PARTY.md) and [docs/licenses](docs/licenses).

Project code is licensed under **[GPL-3.0-or-later](LICENSE)**. Dependencies keep their own licenses: the SMM2Recomp snapshot includes GPLv3, LASRecomp includes MIT notices, and the setup carries FFmpeg (LGPL 2.1), with its source attached to each release. The license grants no rights to Nintendo's game code, assets or trademarks.

MK8-online is an independent community project. It is not affiliated with or endorsed by Nintendo or Valve. Mario Kart and related names belong to their respective owners.
