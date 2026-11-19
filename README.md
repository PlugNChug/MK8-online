<div align="center">

<img src="docs/tag-ratone-tend.png" alt="RatOne — creator and maintainer of MK8-online" width="560">

# MK8-online

### Prepare once. Race natively. Connect with friends.

**Created and maintained by [RatOne](https://github.com/Vnnc1)**

**English** · [Português (Brasil)](README.pt-BR.md)

![Platform](https://img.shields.io/badge/Windows-x64-0078D6)
![Graphics](https://img.shields.io/badge/Graphics-Vulkan-AC162C)
![Stack](https://img.shields.io/badge/Rust-C%2FC%2B%2B-555555)
![Release](https://img.shields.io/badge/Release-0.3.0-ef795d)
![Status](https://img.shields.io/badge/Status-Public_test-e6a23c)
[![License](https://img.shields.io/badge/License-GPL--3.0--or--later-blue)](LICENSE)

An experimental **Mario Kart 8 Deluxe 4.0.1** static recompilation project for Windows,
with a native launcher, a Vulkan runtime, a Mii creator and Steam rooms with friends.

[Download / Releases](https://github.com/Vnnc1/MK8-online/releases) · [Report an issue](https://github.com/Vnnc1/MK8-online/issues) · [Roadmap](docs/ROADMAP.md)

</div>

> [!IMPORTANT]
> Bring your own game files and console keys. This repository and its releases do not include ROMs, game assets, keys, saves or recompiled game code. The game is compiled locally from your own files.

> [!NOTE]
> **0.3.0 is an experimental public pre-release, with known bugs.** Solo races complete in local tests. Visual defects, first-use shader stutters and VRAM growth remain. Multiplayer between two separate PCs has not been qualified yet: that is exactly what this release asks testers to try.

New audio voices start at zero previous volume and use the mixer’s existing first-block gain ramp. This addresses popping with repeated road sound effects; it requires a game/runtime rebuild. Other sound effects and full audio qualification remain open.

Development builds preserve the previous Vulkan pipeline cache when writing or replacing its file fails, and retry failed background saves. This avoids losing cached work on those failures; it does not eliminate first-use shader compilation stutters. Rebuild the runtime to apply this change.

Development builds preserve visibility-counter reports across the 256-query submission limit. The Maxwell shader translator also honors the fragment-input interpolation modes declared by the game, which reduces lighting flicker in the tested Boo Lake split-screen sequence. Rebuild the runtime to apply the shader correction.

Controller discovery now tries subsequent devices for player one if the first device cannot be opened, and ignores duplicate connection events without changing existing player assignments. The separate DSX compatibility report still needs testing on the affected setup.

After rebuilding the development runtime, the original Nintendo Switch Pro Controller and individual Joy-Con can receive HD Rumble when SDL opens them directly through HIDAPI. The game controls each grip’s low and high frequencies and amplitudes independently. Other rumble-capable controllers, including those exposed through Steam Input or SDL’s paired Joy-Con driver, use basic two-motor vibration. Opening Settings or switching away stops the effect. Two virtual Pro Controllers passed the split-screen test. Physical Bluetooth confirmation is pending.

The development DLC importer can select the verified Booster Course Pass from a combined NSP containing other content. Only the pinned DLC files and its matching ticket are extracted; all existing hash and content checks remain. Other DLC revisions and compressed DLC are not enabled by this change.


Developer preview on `fix-chore`: `MK8_MENU_PROTOTYPE=1` enables an in-game overlay with Play, Nextendo, Social and Settings pages (F1 or Back + Start). Account state and settings use the runtime. Social requests the additional `friends` OAuth permission and reads friends/presence; Nextendo lists live MK8 lobbies and reads the current lobby. Selecting a row shows its details; it does not join the room. Queries run in the background every 30 seconds while either page is open. Permission/network failures are displayed explicitly; illustrative players and rooms have been removed. With Nextendo signed in, enter races through the game's Online Play menu. The panel's lobby list is informational; direct joining from this panel is out of scope. Race invitations are not implemented. This preview is not part of the published 0.3.0 setup.

Development builds add **Ultrawide (experimental)** under Video settings in the launcher and in-game menu. Click **Save settings** in the launcher before starting the game. It widens supported solo Time Trial, Grand Prix and no-team VS cameras between 16:9 and 32:9, preserves vertical framing, and keeps the HUD proportional at the outer edges. Supported views include forward/rear driving in these solo modes, GP/VS course flyovers and GP/VS/Time Trial opening character close-ups. Original 16:9 and Stretch remain available. Unsupported scenes/modes, split-screen and frame generation use 16:9; this does not enable ultrawide online or replay cameras. Rebuild the game, including the affected guest units, to apply it.

The optional **native settings menu** switch is saved automatically for the next launch. It adds the wrench entry to the game's main menu and preserves the existing F1/Back + Start fallback and hub shortcut priority. It shares the runtime settings file, including the aspect choice. The private menu layouts are derived locally from verified game resources; none are distributed. Full qualification remains open; local manual evidence and limits are in [STATUS.md](docs/STATUS.md).

## What's new in 0.3.0

Development builds restore Rock Rock Mountain's missing start-banner shadow by honoring the game's depth-clamp state. Rebuild the runtime to apply this fix; it is not in the published 0.3.0 setup. See [technical status](docs/STATUS.md) for the tested conditions.

The development installer can open the launcher in the same window after setup. The option is selected by default, and the launcher uses the data folder chosen during setup, including a folder on another drive.

After an update, Installation opens **Tools** if the new launcher needs to repair its toolchain. Once verification finishes, it opens **Game build** if your game files are ready, or **Game files** if an input is missing. A successful tool installation removes recognized obsolete tool copies from earlier launcher versions.

Open **Uninstall.exe** next to **Launcher.exe** to uninstall. The default keeps saves, Miis, settings and the prepared game. Select the same data folder when reinstalling to use them again. The full removal option shows the data location and requires confirmation.

Development runtimes include a Vulkan shader workaround for the desktop GTX 1070 (PCI device `10de:1b81`). It moves eligible constant texture-size queries outside fragment-shader branches. Other GPUs keep the existing shader path. Rebuild the runtime to apply it. See the [technical status](docs/STATUS.md) for verification and hardware still to test.

Development builds add two startup options (not included in the published 0.3.0 setup):

- **Direct play:** `Launcher.exe --play` opens an already prepared solo game without the launcher window. Use `Launcher.exe --play --data-dir "D:\\MK8\\data"` to select existing data. It still validates files, Vulkan and resource locks; it does not install tools or build the game. Errors return a nonzero exit code. Steam rooms still use the launcher.
- **Portable Windows package:** extract `MK8-online-portable.zip` to a writable folder. Keep `portable.txt` beside `Launcher.exe`; sources, tools and data are stored under the adjacent `portable` folder. `--data-dir` overrides only the game-data location. Existing installed data is not moved automatically. To update, replace the executable and keep `portable/`; the setup-based updater is disabled in this mode.

The portable package avoids setup/registry installation. Open this version once before moving it, close the game and launcher, then copy/move the **whole folder**, including `portable/`. On the next start, internal game/build/Mii/font paths and the Python environment are rebased automatically; original metadata is retained under `portable/.relocation-backups/`. Saves and binary game assets are not rewritten. Files selected outside the portable tree remain external dependencies; include required resources inside the tree when preparing a self-contained copy. Windows x64 and compatible Vulkan drivers are still required; this does not add Linux/AppImage/Steam Deck support or guarantee performance on another GPU.

- **CTGP Deluxe on MK8 4.0.1:** import the course resources of CTGP Deluxe 1.1.1 (originally for 3.0.3), including 64 courses in 16 additional cups with names, images, icons and music. This adaptation does not run the original CTGP plugin; compatibility is incomplete and bugs remain.
- **Redesigned launcher:** horizontal installation stages, modular settings, accessible game language, guided Mii creation, clearer Steam rooms/friends and support reports.
- **Optional DLC:** import your Booster Course Pass during preparation or later, then enable/disable it without deleting it. Game files, DLC and keys are never distributed.
- **Fixes and improvements:** Mii resource-loading freezes and glasses, Vulkan dependency checks, memory/texture handling, MKTV thumbnails, local manual, fonts, particle shaders, audio, controller setup and shader waits in two-player split-screen.
- **Settings and presentation:** CRLF INI parsing is fixed. Optional experimental frame generation can present about 120 FPS in measured sections while simulation stays at 60 Hz; it remains off by default.

Read the [0.3.0 release notes in English or Portuguese](docs/releases/0.3.0.md) and [technical status](docs/STATUS.md). Steam reconnection, Vulkan startup on some PCs, integrated-GPU memory failures, graphical defects and shader stutters remain known limitations. **This is an experimental pre-release, not a fully working or stable build.**

## Where it stands

Development builds compensate slope-based depth bias for uniform internal-resolution scaling. The user confirmed that Mount Wario’s broad snow/ice bands are gone at 2×, 3× and 4×; a 2× frame dump also passed independent visual review. Other courses and GPUs still need testing. Rebuild the runtime to apply the change.

**120 FPS presentation:** the existing experimental frame-generation option interpolates one image between real frames while the game remains at 60 Hz. It needs a high-refresh display and GPU headroom; it does not unlock native simulation or remove shader stutters. Local investigation measured about 120 presented frames per second in a stable solo section on an RX 580 2048SP / 180 Hz display. This is not full-race or multiplayer qualification. The 0.3.0 runtime also fixes CRLF INI parsing, which could silently disable `frame_gen = 120` and misread other text settings. Details and limitations are in [STATUS.md](docs/STATUS.md).

Performance fixes in 0.3.0 use the Windows high-resolution timer for bounded shader waits, apply that budget to the two-player split-screen scene, and cancel controller setup when fewer players are connected than requested. Rebuild the game/runtime to apply them. For keyboard plus one gamepad in local multiplayer, enable **Controllers start at player 2** in Video and controls. First-use shader compilation can still cause pauses; these fixes do not qualify every CTGP track or multiplayer between PCs.

Version 0.3.0 integrates PRs #27–31 and #33: the local manual, replay readback, texture lifetime fixes, bounded shader support, private font imports, and Steam/Mii/DLC improvements. Commit `8202c32` also merged CTGP phase B (additional cups and its guest hooks). Integration checks and remaining limits are recorded in [STATUS.md](docs/STATUS.md).

A bounded TMML.B adapter now translates the static texture-handle pattern found in the failed particle effects. Synthetic tests and three complete private shaders pass translation/SPIR-V checks; live rendering and the blur defect remain unqualified.

Steam improvements in 0.3.0: Steam DLL discovery supports additional libraries and nested game layouts, and loads versioned C interface exports. All friends are searchable, including offline friends. Oversized LAN datagrams use Steam's reliable fragmentation; smaller packets keep their previous mode. Closing the game releases its Steam session so another invitation can be accepted without restarting the launcher. Both PCs must update; a complete two-PC race is still unqualified.

Development builds prevent a Windows child process from keeping the gateway's UDP ports open through inherited socket duplicates after the session closes. Rebuild the launcher to apply this correction. It does not qualify Steam reconnection or a full race between two PCs.

Multiplayer offers **Create Room (Automatic)** and **Play (Manual Room)**. Both create a Steam host lobby; guests accept an invite and can choose automatic or manual in-game navigation. Automatic entry keeps the active Mii selected. The Mii converter corrects the glasses texture channels: regenerate derived Mii resources and rebuild the runtime for these Mii fixes.

**Install → Game files** accepts an optional Booster Course Pass NSP. It is imported after a successful build in the same preparation flow, using the existing verified importer. An empty field preserves existing DLC. Later import remains under **Install → Additional content**, with a shortcut from **Play**; enabling/disabling DLC remains under **Settings**. Steam still requires matching DLC and CTGP states. No Valve DLL or game assets are distributed.

| Area | Current progress | Next checkpoint |
| --- | --- | --- |
| Launcher | Native window, guided preparation, installer, updates, reports; Spanish, English and Brazilian Portuguese | Upgrade from 0.2.0 to 0.3.0 on more PCs |
| Solo racing | Mushroom Cup completed; battles, MKTV highlight reel and time trials reached | Full image, audio, controller and restart/save qualification |
| Mii | Creator, profiles and the game's own Mii renderer (portraits, suits, driver, minimap) | Mii in LAN lobbies, comparison with console resources |
| Multiplayer | Steam rooms with invites; two game instances on one PC entered the same LAN room automatically | A guest joining over Steam from another PC, full races between two PCs |
| Performance | Around 60 FPS in measured race sections on the test PC | Repeated cups, VRAM stability and more hardware |

These are observed milestones, not a percentage-complete estimate. `gameplay_verified` and `multiplayer_verified` stay `false` until full qualification passes.

MKTV thumbnail readback now copies the rendered GPU image when recording a replay. Existing replay files can already contain corrupted thumbnail JPEGs; this change preserves those files and applies to newly recorded thumbnails.

Historical cube-map fallback now reuses only an existing GPU-rendered image. A retained old texture could otherwise upload guest memory that had been reused, introducing invalid HDR pixels and bright/black boost streaks. Active CPU-backed cube maps remain supported. Native regressions pass, and a multi-course retest of the combined local build found no recurrence. Rebuild the game/runtime to apply this fix; it does not establish that unrelated MKTV lighting/editor defects are fixed.

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

1. Download `MK8-online-0.3.0-setup.exe` from [Releases](https://github.com/Vnnc1/MK8-online/releases) and choose the application and data folders. Installation is per user, without administrator rights. Already on an earlier version? Use **Support → Update now**: your data folder and saves are kept.
2. Open **Install**. Complete **Tools**, then select the base game and the update in **Game files** (the Launcher finds keys next to the game, or select your own).
3. Run **Build the game**. After updating from an earlier version, build again: the game is recompiled with the new runtime.
4. Choose **Play solo**.

The Launcher's **Tools** step prepares private Python directly, without PowerShell or .NET ZIP loading. CPython comes from the pinned official NuGet package; archive and dependency hashes are checked. Interrupted environments are preserved before repair. Keep antivirus protection enabled.

An extracted folder must contain `exefs`, `romfs` and `control.nacp` (or `control/control.nacp`): choose **I already have an extracted game folder**. The active profile is 4.0.1 AArch64; other game versions and XCI/XCZ import are not validated. Supported DLC is imported separately.

Data defaults to `%LOCALAPPDATA%\MK8-online` and survives updates and uninstalling. To use another drive with the portable launcher: `.\Launcher.exe --data-dir E:\MK8-Local`, and keep using the same folder.

**Language:** the Launcher and installer offer **English**, **Português (Brasil)** and **Español** (**Language / Idioma**). The game's own language is separate.

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

## CTGP Deluxe course pack (optional, experimental)

CTGP Deluxe 1.1.1 targets MK8 3.0.3 (32-bit). Version 0.3.0 adapts its course resources to MK8 4.0.1 (64-bit),
without running the original plugin. Choose 16 additional cups with all 64 courses, or a replacement layout as
described below. Compatibility is incomplete: track-specific bugs, graphical defects and performance drops remain.

1. Download CTGP Deluxe from [ctgpdx.com](https://www.ctgpdx.com/download) (personal use) and extract the ZIP.
2. **Install → Additional content → CTGP Deluxe**: choose the extracted folder (the one with `atmosphere/`) and click **Import CTGP…**.
   On the same disk as the data folder it takes seconds and no extra space (hard links).
   Choose where the courses go: **in place of the base game's cups** (48 courses, +16 with the DLC), **in place
   of the DLC's cups** (48 courses next to the 48 original ones; needs the Booster Course Pass imported and enabled)
   or **in 16 new cups** (all 64 courses on two more pages of the cup screen, L/R, with their own names, pictures,
   music and cup icons; no original course moves).
3. **Enable CTGP Deluxe** switches it on; turn it off to play the original game. The game, the DLC and your saves are
   never changed; with CTGP on, the game uses its own saves, created once as a copy of yours.

In Steam rooms everyone needs the same pack (the same CTGP enabled, or everyone without it). MK8-online does not
distribute CTGP: it is read from your copy and stays on your PC. Known limits: CTGP's own menus and random button need
its code (not available); time-trial staff ghosts are the original course's; in the new cups, each CTGP course keeps
its records together with those of the original course whose slot it borrows (in CTGP's own saves), and online
sessions use the game's cups (each machine would otherwise decide on its own which races are a CTGP cup's). See
[STATUS.md](docs/STATUS.md).

## Build and contribute

Feedback snapshots now retry device-memory exhaustion after collecting GPU-completed retired images, then try host-preferred compatible memory without changing image format or masking allocation errors. The launcher's isolated Vulkan probe now initializes SDL video and actually loads Vulkan through SDL rather than checking exports alone. These mitigations need retesting on the machines affected by issues #52 and #47; they are not included in the published setup.

Current development changes: Steam tries the available candidate libraries until all required exports can be resolved, without accepting unverified interface versions. MK8's stretch-to-window setting now fills wide/tall windows while retaining the original camera field of view; original 16:9 keeps its proportions. Historical cube-map fallback only reuses existing GPU-owned images, and command-record headers use 72 rather than 88 bytes. Rebuild the launcher for Steam selection and all host runtime consumers for the graphics changes. These changes are not in the published 0.3.0 setup and do not establish fixes for every lighting defect or GPU-memory crash.

Game-data preparation uses short temporary filenames so valid long game filenames do not exceed Windows' filename limit during hard-link creation. Existing hardlinks are reused without duplicating file data. This fixes a reproduced filename-boundary failure; issue #38 still needs verification on the reporter's installation.

The save-data routing fix keeps existing account progress in `save/save_0` and separates device, temporary and other save types into `save/save_type_<type>`. Mii refresh/index queries now return valid local-profile data when progress is reloaded. Together these prevent cross-save cleanup and the reload crash exposed by retaining progress. Rebuild the game/runtime to apply them. Existing saves are not moved or deleted; already lost progress and corrupted thumbnails are not recovered. Native tests pass, and a new MKTV replay/thumbnail survived a process restart in copied-save testing of the combined local build. Full gameplay and trophy persistence remain unqualified.

The main menu's **Info** button opens a local **How to Play** window. Close it with **Back to game**, Escape, or the window's close button to resume. It covers default controller buttons, drifting, items, assists, and modes; PC mappings are under F1 → Controls. No manual asset extraction or internet connection is needed. Launcher and runtime must both be rebuilt for this feature.

To try motion steering in a development build, connect a controller that exposes both a gyroscope and an accelerometer to SDL. In the race pause menu, press **Y** to turn on **Tilt Controls** (the yellow curved arrow). Hold the controller like a steering wheel and tilt left or right. Rebuild the runtime once to get this feature.

This branch compares texture descriptions semantically and releases each texture initializer reference once, including copied guest texture objects. It does not establish a fix for Stadium flashing or a general VRAM leak.

On Windows, shared-font dialogs use Segoe UI before Arial when no console font dump or explicit `LARECOMP_FONT` is supplied. This avoids the tall, narrow appearance of the Arial fallback in race-exit confirmations. These remain substitute fonts; dumped console fonts retain priority.

In **Settings → Game fonts**, use **Import Font Folder** to copy fonts dumped from your own Switch. Requires `FontStandard.ttf` or `nintendo_udsg-r_std_003.ttf/.bfttf`; regional/button fonts are optional. SharedFontDumper filenames are normalized automatically. Font controls follow the launcher’s English, Brazilian Portuguese, and Spanish language selection. The launcher checks TrueType table bounds and verifies hashes before launch; this does not authenticate Nintendo provenance. **System Default** disables imported fonts without deleting them; **Imported Console Fonts** re-enables them. Changes take effect next launch, without another game rebuild after installing the launcher/runtime with this support. Legacy folders and explicit `LARECOMP_FONT` overrides remain supported.

With homebrew already working: get the `.nro` from [SharedFontDumper releases](https://github.com/simontime/SharedFontDumper/releases), place it under `switch/` on the SD card, and launch it from the Homebrew Menu. Press **A** for individual TTF files; **+** exits after completion. Copy the SD's `fonts` folder to your PC and import it. The [tool source](https://github.com/simontime/SharedFontDumper/blob/master/source/main.c) documents these controls and filenames. This older tool's compatibility with current firmware/homebrew has not been tested here. Combined `shared_font.bin` dumps and firmware archives are not accepted.

Development happens on `fix-chore`. You need Rust, Python 3.12 x64, PowerShell 7 and .NET 10 x64 (for the importer). From the repository root:

```powershell
.\scripts\start-launcher.ps1
```

It downloads the pinned upstream sources, verifies their hashes, builds the launcher and opens it. To package the portable launcher: `python scripts/package-launcher.py` (output: `target/portable/Launcher.exe`). Release steps are in [RELEASING.md](docs/RELEASING.md).

To build a local test installer without publishing a release, run `python scripts/release.py`. The setup is written to `<Cargo target directory>/dist/MK8-online-<version>-setup.exe`. Install same-version test builds manually; the updater only offers newer versions. The packaged launcher includes the default redesigned interface and real installation/build operations, without the debug storyboard.

The launcher uses the approved charcoal/orange design in its embedded interface, including packaged builds. Installation progresses horizontally through **Tools → Game files → Build → optional DLC/CTGP**. Tool readiness and file checks come from the backend; incomplete prerequisites keep later actions locked. Existing tools and prepared files are reused. Downloads, compilation, cancellation, errors and logs use the existing pipeline. Select the base game, update 4.0.1 and console keys (automatically detected where supported), or an extracted game folder. An optional DLC package can accompany preparation. Additional content remains accessible after preparation and can be managed in Settings.

**Settings** has freely selectable Video and controls (Image, Audio, Controls), Game fonts, and Additional content modules. Switching modules preserves drafts. Contextual previews illustrate menu theme, aspect ratio, fullscreen, audio, bindings and cup layouts; they are not game captures or performance measurements. The font example uses the browser font, not imported console files. **Game language** is available above Play and in Settings; it saves only that setting immediately, preserving other drafts and applying next launch. The launcher language remains independent; the interface supports Portuguese, English and Spanish.

**Mii Profile** starts with a required name (1–10 characters), then separate Face and Body areas. Existing profiles open directly in customization; explicit saving and unsaved-change confirmations remain. Missing preview resources are identified, with numbered options available until the library loads. **Multiplayer** provides a short interactive guide, automatic/manual hosting, searchable/filterable friends and a separate invitations/rooms tab. The guide performs no Steam actions. Multiplayer remains experimental and requires separate validation on two machines.

**Support** retains update checking and confirmed installer launch. A bug form prepares a GitHub Issue draft with a preview; users review and publish on GitHub, attaching the diagnostic ZIP manually if desired. Copy and Markdown export support other channels and long reports. No credentials, automatic Issue publication or automatic upload are introduced. The native window opens only the project's Issue URLs in the system browser. Authenticated ZIP download serves only the last generated report, with path, link and size checks. Review manually entered text and attachments before sharing.

For isolated visual development, run `python scripts/preview-launcher.py`. It compiles debug and opens the real interface with separate data under `local/design-preview`. Use `--target-dir D:/path/to/target` to reuse a Cargo folder; `--no-build` skips compilation. Keep the terminal open; Ctrl+C stops the preview. Explicit `MK8_WEB_ROOT` enables an allowlist of live interface files only in debug builds. Edit HTML/CSS/JS and reload; **Ajustar visual** offers browser-local palette, spacing, corners and CSS export. Release builds use embedded assets and omit these debug controls.

The earlier installation simulation is available only in the live debug preview at `/?storyboard=1#install`. It is labeled as a simulation, uses sample files and in-memory state, and invokes no installation/build API. Normal debug and packaged launchers use real operations. The isolated preview does not reuse installed profiles, resources or saves and does not qualify gameplay.

Keep adaptations in `runtime/mk8` and `toolchain`; preparation applies them to a verified build copy. Do not edit upstream snapshots or generated game code. Follow [AGENTS.md](AGENTS.md) and run the tests for the area you change. Developer-only paths (LAN/relay/P2P transports through the API, the static Mii head converter, the Nextendo prototype) are documented in the [technical status](docs/STATUS.md).

Nextendo testing has resumed on `fix-chore`: the maintainer reports successful online play with other players in an isolated test session. The opt-in adapter uses the documented OAuth API and our registered public client ID. `scripts/test-race.py time-trial --nextendo --interactive` supports manual testing with copied saves and a verified local candidate. This is not enabled as a public launcher feature; production TLS trust, reconnection and full race/restart qualification remain pending. Tokens remain in memory for the current process.

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

Host command-record headers use 72 instead of 88 bytes by sharing argument slots that are never used together. Command retention and GPU synchronization stay the same. Rebuild all host consumers together; `--runtime-only` is sufficient when the guest is already current. Native recorder/lifetime regressions and the isolated host rebuild passed on current `fix-chore`; earlier two-cup comparisons remained near 60 FPS in measured race windows. Full gameplay qualification remains pending.
