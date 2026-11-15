<div align="center">

<img src="docs/tag-ratone-tend.png" alt="RatOne — creator and maintainer of MK8-online" width="560">

# MK8-online

### Prepare once. Race natively. Connect with friends.

**Created and maintained by [RatOne](https://github.com/Vnnc1)**

**English** · [Português (Brasil)](README.pt-BR.md)

![Platform](https://img.shields.io/badge/Windows-x64-0078D6)
![Graphics](https://img.shields.io/badge/Graphics-Vulkan-AC162C)
![Stack](https://img.shields.io/badge/Rust-C%2FC%2B%2B-555555)
![Release](https://img.shields.io/badge/Release-0.2.0-ef795d)
![Status](https://img.shields.io/badge/Status-Public_test-e6a23c)
[![License](https://img.shields.io/badge/License-GPL--3.0--or--later-blue)](LICENSE)

An experimental **Mario Kart 8 Deluxe 4.0.1** static recompilation project for Windows,
with a native launcher, a Vulkan runtime, a Mii creator and Steam rooms with friends.

[Download / Releases](https://github.com/Vnnc1/MK8-online/releases) · [Report an issue](https://github.com/Vnnc1/MK8-online/issues) · [Roadmap](docs/ROADMAP.md)

</div>

> [!IMPORTANT]
> Bring your own game files and console keys. This repository and its releases do not include ROMs, game assets, keys, saves or recompiled game code. The game is compiled locally from your own files.

> [!NOTE]
> **0.2.0 is a public test release, still experimental.** Solo races complete in local tests. Visual defects, first-use shader stutters and VRAM growth remain. Multiplayer between two separate PCs has not been qualified yet: that is exactly what this release asks testers to try.

## What's new in 0.2.0

- **Mii creator.** The **Mii profile** tab is a creator now. Name your Mii, then pick every part (body, face, skin, hair, eyes, eyebrows, nose, mouth, beard, glasses, mole) from buttons that show *your* Mii with that option. The preview puts the head on MK8's own lobby body, read from your prepared game, so type, height, build and favorite color are visible.
- **Your Mii in the game.** Saving a Mii prepares the in-game Mii models automatically. The game then draws your Mii with its original renderer: character select portraits, Mii Suits, the driver with its helmet and the minimap icon.
- **Steam rooms only.** **Multiplayer** now uses Steam: create a room and invite friends from your Steam friends list. They accept in the Launcher or in Steam chat, and nobody needs a VPN or open ports. You join rooms as your Mii, and its name is the name others see.
- **The game opens in the room.** After a room is created or joined, the Launcher opens the game by itself. An experimental automatic entry then takes each player into the LAN room: the host creates it, and guests join once the host's room exists. You still choose your character and the rules.
- **Fixes:** game logs are no longer empty when the game is started by the Launcher, so support reports contain them again. The crash on the LAN track vote (a missing Mii portrait request) is fixed. Mii resource paths work on Windows, and descriptor pools and cubemap history are released when the game finalizes them.

Details, evidence and remaining limits: [technical status](docs/STATUS.md) (Portuguese).

## Where it stands

| Area | Current progress | Next checkpoint |
| --- | --- | --- |
| Launcher | Native window, guided preparation, installer, updates, reports; English and Brazilian Portuguese | Update from 0.1.8 to 0.2.0 by real testers |
| Solo racing | Mushroom Cup completed; battles, MKTV highlight reel and time trials reached | Full image, audio, controller and restart/save qualification |
| Mii | Creator, profiles and the game's own Mii renderer (portraits, suits, driver, minimap) | Mii in LAN lobbies, comparison with console resources |
| Multiplayer | Steam rooms with invites; two game instances on one PC entered the same LAN room automatically | A guest joining over Steam from another PC, full races between two PCs |
| Performance | Around 60 FPS in measured race sections on the test PC | Repeated cups, VRAM stability and more hardware |

These are observed milestones, not a percentage-complete estimate. `gameplay_verified` and `multiplayer_verified` stay `false` until full qualification passes.

## What this project does

The pipeline translates the supported game's AArch64 executable to C and compiles it for Windows x64. A C/C++ runtime supplies graphics, audio, input and system services; the Rust/Tauri launcher manages preparation, builds, updates, Miis and rooms.

RatOne develops and maintains the MK8 adaptation, launcher and integration. The translation/runtime foundation comes from **SMM2Recomp and LASRecomp**, with additional credited work from other projects. See [third-party sources and licenses](docs/THIRD_PARTY.md).

## Get on track

### Requirements

- **Windows x64** and WebView2. The Launcher installs a private **Python 3.12 x64** by itself.
- A GPU and driver exposing **Vulkan 1.3 or newer**. A general minimum CPU/GPU specification has not been qualified.
- Your own base game, **4.0.1 update** and `prod.keys` from your console, or a supported extracted game folder.
- About **28 GiB free** for extraction and preparation (including a 4 GiB safety reserve), plus room for tools and caches. An SSD is recommended.
- Internet for the first tool download (about 0.4 GiB) and for updates.
- **For Miis:** opening the creator downloads `AFLResHigh_2_3.dat` from an Internet Archive copy of the former Miitomo CDN. The Launcher verifies the ZIP and library by size and SHA-256, then caches the file privately for offline use. It is not bundled in GitHub or the installer. If the archive is unavailable, retry or select a compatible local file; without the library, options remain numbered and faces are not drawn.
- **For multiplayer:** the **Steam** client, signed in, and any Steam game installed that ships `steam_api64.dll` (or a copy of that file in the Launcher's data folder). No Valve file is bundled.

### Install and play

1. Download `MK8-online-0.2.0-setup.exe` from [Releases](https://github.com/Vnnc1/MK8-online/releases) and choose the application and data folders. Installation is per user, without administrator rights. Already on 0.1.8? Use **Support → Update now**: your data folder and saves are kept.
2. Open **Install**. Complete **Tools**, then select the base game and the update in **Game files** (the Launcher finds keys next to the game, or select your own).
3. Run **Build the game**. After updating from an earlier version, build again: the game is recompiled with the new runtime.
4. Choose **Play solo**.

The Launcher's **Tools** step prepares private Python directly, without PowerShell or .NET ZIP loading. CPython comes from the pinned official NuGet package; archive and dependency hashes are checked. Interrupted environments are preserved before repair. Keep antivirus protection enabled.

An extracted folder must contain `exefs`, `romfs` and `control.nacp` (or `control/control.nacp`): choose **I already have an extracted game folder**. The active profile is 4.0.1 AArch64; other versions, DLC and XCI/XCZ import are not validated.

Data defaults to `%LOCALAPPDATA%\MK8-online` and survives updates and uninstalling. To use another drive with the portable launcher: `.\Launcher.exe --data-dir E:\MK8-Local`, and keep using the same folder.

**Language:** the Launcher and installer offer **English** and **Português (Brasil)** (**Language / Idioma**). The game's own language is separate.

### Create your Mii

1. Open **Mii profile** and wait for the first library download. Its Internet Archive origin is shown in the creator. To use an existing file, expand **Use a local file**, then **Browse** and **Use**. A valid configured library is reused automatically.
2. Type a name and choose **Create Mii**. Then go through the tabs and tap the option you like: each button shows your Mii with that option. **Body** has type, height, build and favorite color, and the preview shows the whole body (or just the face).
3. Choose **Save Mii**. The Launcher prepares the in-game Mii models the first time; the game uses your Mii the next time it opens.

**Play** shows when Mii preparation is running and enables the button when it finishes. Existing prepared resources are reused. If an XInput controller stops responding while Steam is connected, open **Steam → Library → Spacewar → Properties → Controller**, select **Disable Steam Input** for that game, then close and reopen both MK8 and the Launcher. Steam remains connected for friends and rooms. Clearing Steam's inherited SDL device filters alone does not resolve forced Steam Input remapping. A GameSir T4n Lite through its USB receiver was confirmed working in the menu after this adjustment, with the Steam API connected.

You can keep several Miis and switch between them at the top. **Mii profile** in the game's **F1** menu brings this screen forward.

## Race with friends (Steam)

Each player needs their own prepared copy of the game (same 4.0.1 version), a Mii, and Steam open. Keep the Launcher open on **Multiplayer**: Steam shows it as **Spacewar** (AppID 480, Steam's test app), which is how invites reach it.

1. **Host:** **Multiplayer → Create room**, then **Invite** next to a friend (or **Invite through Steam**). The host's game opens and creates the LAN room by itself.
2. **Friend:** accept the invite in the Launcher (**Accept**), in Steam chat, or with **Join** under the friend's open room. The game opens and joins the room by itself.
3. Everyone picks a character and kart; the host sets the rules.

If the automatic entry stops (it is experimental and gives up rather than pressing the wrong thing), continue by hand: at the game's main menu hold **L + R** and click the **left stick** to reveal **LAN Play**. The host uses **Create Room**; guests choose the host's room in **Lobby**.

Known limits: a friend whose Launcher is closed will see Steam try to open Spacewar when accepting; open the Launcher and use **Join** instead. A guest joining over Steam from another PC is what this release needs to confirm. The game supports up to 12 LAN players, but only two players have been tested.

## Performance, with context

On a **Xeon E5-2666 v3 / 16 GB RAM / RX 580 2048SP**, measured race sections of a full Mushroom Cup run:

| Track | Average FPS | p95 frame time |
| --- | ---: | ---: |
| Mario Kart Stadium | 59.99 | 17.46 ms |
| Water Park | 59.98 | 17.45 ms |
| Sweet Sweet Canyon | 59.97 | 17.46 ms |
| Thwomp Ruins | 59.99 | 17.33 ms |

These exclude capture windows and do not guarantee 60 FPS in every scene or on other PCs. First-use shaders can still stutter, especially when other players' content appears for the first time, and **VRAM still grows between tracks**. See [measurements and open investigations](docs/STATUS.md).

## Help improve the next lap

In **Support → Create report**, create a diagnostic ZIP and attach it to an [issue](https://github.com/Vnnc1/MK8-online/issues), with your CPU/GPU, launcher version, what you did and what happened. For multiplayer, attach reports from **both** PCs. Reports include game, build and tool logs; personal paths and account names are redacted, and saves and keys are excluded. Review attachments before sharing. Never post ROMs, console keys or private files.

## Build and contribute

Development happens on `fix-chore`. You need Rust, Python 3.12 x64, PowerShell 7 and .NET 10 x64 (for the importer). From the repository root:

```powershell
.\scripts\start-launcher.ps1
```

It downloads the pinned upstream sources, verifies their hashes, builds the launcher and opens it. To package the portable launcher: `python scripts/package-launcher.py` (output: `target/portable/Launcher.exe`). Release steps are in [RELEASING.md](docs/RELEASING.md).

Keep adaptations in `runtime/mk8` and `toolchain`; preparation applies them to a verified build copy. Do not edit upstream snapshots or generated game code. Follow [AGENTS.md](AGENTS.md) and run the tests for the area you change. Developer-only paths (LAN/relay/P2P transports through the API, the static Mii head converter, the Nextendo prototype) are documented in the [technical status](docs/STATUS.md).

| Location | Responsibility |
| --- | --- |
| `apps/launcher` | Native launcher, web interface, installer, updates and reports |
| `crates/installer`, `crates/manifest`, `crates/build-manager` | Installation, validation, Miis and build supervision |
| `crates/session`, `crates/transport` | Rooms, Steam, relay and authenticated transport |
| `toolchain`, `runtime/mk8` | Recompilation pipeline and MK8 runtime adaptations |
| `games/mk8d` | Version profiles, symbols and game configuration |
| `scripts`, `tests` | Preparation, packaging and regression checks |
| `upstream/manifests` | Pinned source manifests; downloaded snapshots stay outside Git |
| `local`, `target`, `prebuilt` | Private data and generated artifacts, ignored by Git |

## Credits and license

**MK8-online is created and maintained by RatOne.** Upstream authors retain credit and ownership of their contributions. Attribution and dependency notices are in [THIRD_PARTY.md](docs/THIRD_PARTY.md) and [docs/licenses](docs/licenses). The Mii creator draws faces with [FFL.js](https://github.com/ariankordi/FFL.js) (AGPL-3.0).

Project code is licensed under **[GPL-3.0-or-later](LICENSE)**. Dependencies keep their own licenses; the SMM2Recomp snapshot includes GPLv3, LASRecomp includes MIT notices, and the setup carries FFmpeg (LGPL 2.1) with its source attached to each release. The license grants no rights to Nintendo's game code, assets or trademarks.

An independent community project, not affiliated with or endorsed by Nintendo or Valve. Mario Kart and related names belong to their respective owners.

Conditional occlusion rendering waits for pending reports on either comparison operand before reading their actual sample counts. This addresses transient missing racers/objects during previews; it preserves legitimate hidden results. The fix is applied during runtime preparation and needs a game/runtime rebuild.
