# User guide

**English** · [Português](../pt-BR/GUIDE.md)

**English** · [Português](../pt-BR/GUIDE.md)

This guide covers installing, configuring and playing MK8-online. On Linux, start with the [Linux guide](LINUX.md) and come back here for the launcher and in-game features. Everything below is experimental; the [technical status](STATUS.md) (in Portuguese) records what has been verified and every change.

- [Requirements](#requirements)
- [Install and play](#install-and-play)
- [Updating, portable mode and uninstalling](#updating-portable-mode-and-uninstalling)
- [Mii](#mii)
- [Graphics](#graphics)
- [Controllers](#controllers)
- [In-game menu and settings](#in-game-menu-and-settings)
- [Online play with Nextendo](#online-play-with-nextendo)
- [Steam rooms with friends](#steam-rooms-with-friends)
- [Booster Course Pass DLC](#booster-course-pass-dlc)
- [CTGP Deluxe course pack](#ctgp-deluxe-course-pack)
- [RomFS mods](#romfs-mods)
- [Console fonts](#console-fonts)
- [Reporting a problem](#reporting-a-problem)
- [Troubleshooting](#troubleshooting)

## Requirements

- **Windows x64** with WebView2. The launcher installs its own private **Python 3.12 x64**.
- **Linux amd64 (experimental):** the AppImage or tarball plus the packages in the [Linux guide](LINUX.md), including **Python 3.12**.
- A GPU and driver exposing **Vulkan 1.3 or newer**. No general minimum CPU/GPU specification has been qualified.
- Your own base game, the **4.0.1 update** and `prod.keys` from your console, or a supported extracted game folder.
- About **28 GiB free** for extraction and preparation (including a 4 GiB safety reserve), plus room for tools and caches. An SSD is recommended.
- Internet for the first tool download (about 0.4 GiB) and for updates.
- **For Miis:** the creator downloads `AFLResHigh_2_3.dat` on first use (see [Mii](#mii)).
- **For Steam rooms:** Steam signed in, plus a compatible 64-bit Steamworks DLL from a game you have installed (see [Steam rooms](#steam-rooms-with-friends)). No Valve file is bundled.

## Install and play

1. Download `MK8-online-<version>-setup.exe` from [Releases](https://github.com/Vnnc1/MK8-online/releases) and choose the application and data folders. Installation is per user and needs no administrator rights. The installer can open the launcher in the same window when it finishes.
2. Open **Install** and complete **Tools**.
3. In **Game files**, select the base game and the 4.0.1 update. The launcher finds keys next to the game, or you can select your own. You can also add an optional Booster Course Pass NSP/NSZ here.
4. Run **Build the game**. A clean build takes around 30 minutes; later rebuilds reuse finished work.
5. Choose **Play solo**.

**Extracted game folder:** the folder must contain `exefs`, `romfs` and `control.nacp` (or `control/control.nacp`). Choose **I already have an extracted game folder**. Direct NSP/NSZ import accepts two verified variants of the base program. The active profile is 4.0.1 AArch64; other game versions and XCI/XCZ import are not validated.

**Data folder:** defaults to `%LOCALAPPDATA%\MK8-online` and survives updates and uninstalling.

**Language:** the launcher and installer offer English, Português (Brasil) and Español (**Language / Idioma**). The game's language is set separately, above **Play** or in **Settings**.

The **Tools** step prepares private Python directly, without PowerShell or .NET ZIP loading. CPython comes from the pinned official NuGet package, and archive and dependency hashes are checked. Keep your antivirus enabled.

## Updating, portable mode and uninstalling

**Updating:** use **Support → Update now**. Your data folder and saves are kept. If **Install** then offers **Update the build** or opens **Tools**, finish those steps: the game is recompiled with the new runtime.

**Direct play:** `Launcher.exe --play` starts an already prepared solo game without the launcher window. Add `--data-dir "D:\MK8\data"` to select existing data. It still validates files, Vulkan and resource locks, but does not install tools or build the game. Errors return a nonzero exit code. Steam rooms still need the launcher.

**Portable package:** extract `MK8-online-<version>-portable.zip` to a writable folder and keep `portable.txt` next to `Launcher.exe`. Sources, tools and data are stored in the adjacent `portable` folder. `--data-dir` overrides only the game-data location. Existing installed data is not moved automatically. To update, replace the executable and keep `portable/`; the setup-based updater is disabled in this mode.

**Moving a portable copy:** open the new version once, close the game and launcher, then copy or move the **whole folder**, including `portable/`. On the next start, internal paths and the Python environment are rebased automatically; the original metadata is kept in `portable/.relocation-backups/`. Saves and binary game assets are not rewritten. Files selected outside the portable folder stay external dependencies.

**Uninstalling:** run **Uninstall.exe** next to **Launcher.exe**. By default it keeps saves, Miis, settings and the prepared game; select the same data folder when reinstalling to use them again. Full removal shows the data location and asks for confirmation.

## Mii

1. Open **Mii profile** and wait for the first library download. The creator fetches `AFLResHigh_2_3.dat` from an Internet Archive copy of the former Miitomo CDN, checks the ZIP and library by size and SHA-256, and caches it in your data folder for offline use. To use a file you already have, expand **Use a local file**, then **Browse** and **Use**.
2. Type a name (1–10 characters) and choose **Create Mii**. Go through the tabs and pick options; each button previews your Mii with that option. **Body** has type, height, build and favourite colour.
3. Choose **Save Mii**. The launcher prepares the in-game Mii models the first time; **Play** shows progress and unlocks when it finishes.

To move a Mii between builds or PCs, select it under **Mii profile**, save any edits and choose **Export .charinfo**. In the other launcher, choose **Import Switch Mii (.charinfo)** and open the saved file. The imported Mii becomes active. If it already exists, select it in your list.

Solo, Steam rooms and Nextendo all use the active Mii. You can keep several Miis and switch between them at the top of the screen. **Mii profile** in the in-game **F1** menu brings this screen forward. Without the library, options stay numbered and faces are not drawn.

## Graphics

Open **Play → Graphics** or **Settings → Video and controls → Image**.

**Upscaling:** choose a lower internal resolution such as **75%**, then a method:

| Method | Availability |
| --- | --- |
| Bilinear, FSR 1 | Built in |
| FSR 3.1.6 | Optional component |
| FSR 4.1.2 | Optional component, compatible RDNA 4 GPUs |
| DLSS Super Resolution | Optional component, compatible RTX GPUs |

Optional components are downloaded from the vendor after you accept their terms in the launcher. The panel shows the method and dimensions reported by the game; actual input dimensions can be rounded to fit the game's render targets. If a provider is rejected, the game falls back to built-in scaling. In game, **F1 → Settings → Video** lists the same methods; reopen the game through the launcher to load a different temporal provider.

On Linux, the temporal upscalers and FSR 3 frame generation are built on your PC from the vendor SDK on the launcher's **FSR** page after you accept the license; the Windows optional components are not used there.

**Aspect ratio:** Original 16:9, Stretch, or **Ultrawide (experimental)**. Ultrawide widens supported solo Time Trial, Grand Prix and no-team VS cameras between 16:9 and 32:9 and keeps the HUD proportional. Unsupported scenes, split-screen, online, replays and frame generation stay at 16:9. Click **Save settings** before starting the game.

**Driving camera distance:** 75% to 150% (100% is the original). It moves the solo forward and rear driving cameras in Time Trial, Grand Prix and no-team VS; the opening close-up keeps its framing.

**Split-screen 60 FPS:** with three or four local players, tries to update every view each frame instead of alternating. The rate you get depends on your GPU.

**Bloom:** on by default. Turning it off removes the glow around bright surfaces while the lights themselves stay lit.

**Frame generation (experimental, off by default):** interpolates one image between real frames while the game stays at 60 Hz. It needs a high-refresh display and GPU headroom, and does not remove shader stutters.

**FPS counter:** shows game FPS, output FPS and milliseconds per game frame.

## Controllers

SDL-compatible controllers, the keyboard and Switch controllers are supported. Steam Input can stay enabled for Spacewar when playing solo or in a Steam room; an idle virtual Steam controller leaves the player slot free for a physical controller detected through Windows GameInput.

**Choosing player 1:** at the title screen, press a button on the controller you want. A key, mouse click or tap selects the keyboard (**K** on the keyboard also works). Press **SL** or **SR** for a sideways Joy-Con. With one left and one right Joy-Con, a face button selects the pair; with several halves connected, hold **L** on the left half and **R** on the right half you want to pair, then release both. Return to the title to choose again.

**Adding players:** on the way to character selection, **Controller assignment** highlights the first empty slot. Press **L + R** on a controller, **SL + SR** on a sideways Joy-Con, or **Enter** on an available keyboard to join. **Cancel**, **Esc** or P1's Back button cancels; when everyone has joined, P1 confirms with **OK**.

**Reassigning:** at character selection with two to four local players, press **+** on P1's controller (or the mapped Start key, or click **−/+**) to open the controller menu. Select a row to assign a spare controller or swap two players, then **Apply**.

**Keyboard Splitter on Windows:** MK8-online has one native keyboard slot. If a key moves two players, open the controller menu at character selection and choose **Clear all**. Press the mapped **L + R** on each of the two virtual controllers, in player order, then choose **Apply**. This removes the native keyboard assignment, so only the virtual controllers send input to those players.

**Splitting a pair of Joy-Cons:** in the controller menu, select the player who will receive a half and hold **SL + SR** on that Joy-Con. **Apply** splits the pair and **Cancel** keeps it. Each half then controls its own player. A Pro Controller and the keyboard can be mixed with Joy-Cons in any order.

**Disconnects:** in a local split-screen race, a disconnected player's controls go neutral and others keep racing. An identifiable controller returns to its slot on reconnect.

**HD Rumble:** supported on the original Switch Pro Controller, individual Joy-Cons and one identifiable pair of official Bluetooth Joy-Cons. Other controllers use SDL rumble.

**Rumble intensity** defaults to 100% of each controller's calibrated output. Set it to 0% under **Controls** in the F1 menu to stop vibration for all players. You can also use the slider in the launcher's **Settings → Video and controls → Controls** before starting the game. Earlier On and Off selections carry over to 100% and 0%.

**Motion steering:** connect a controller that exposes a gyroscope and accelerometer to SDL. In the race pause menu, press **Y** to enable **Tilt Controls**, then hold the controller like a steering wheel.

## In-game menu and settings

The Menu uses the theme chosen in the launcher (**Video → Menu theme**): Native, Simple Dark, Simple Light, NES Dark or NES Light. **F1**, **Home/Guide**, **Back + Start** and the main-menu wrench open it; **Shift+F1** opens the plain overlay. It follows the launcher's language.

Press **F1** or **Back + Start** to open the in-game menu. **Play** resumes or opens your Mii profile; **Settings** has video, audio, controls and mods. After signing in to Nextendo, it also lists live MK8 rooms and friends. The optional **native settings menu** adds a wrench entry to the game's main menu.

The main menu's **Info** button opens a local **How to Play** window covering controls, drifting, items, assists and modes. Close it with **Back to game** or Esc.

In the launcher, **Settings** covers video, audio, game language, button icons, controller order and keyboard/controller mapping. It shares `user-no-mii/mk8d.ini` with the F1 menu. **Save settings** applies them on the next start; **Reload from game** reads values saved by the F1 menu. Settings cannot be saved while the game or a preparation is using the data.

## Online play with Nextendo

1. Verify your Nextendo account email and link Discord to it. [Manage your Nextendo account](https://nextendo.network/account)
2. Update the game build if asked, then select **Play → Play on Nextendo**.
3. On first launch, the launcher shows the racing server's self-signed public-key fingerprint and asks whether to trust it on this PC. Other service connections verify the Nextendo CA and hostname.
4. Sign in through your browser. Keep the launcher open: the authorization lives in memory while its window is open.
5. In Mario Kart, open **Online Play → Worldwide → Race**.

Nextendo sessions use the original courses; CTGP is disabled for the session. Reconnection is experimental. If Nextendo returns `discord_unlinked`, link Discord to the same account and select **Play on Nextendo** again.

## Steam rooms with friends

Each player needs their own prepared game (same 4.0.1 version, same DLC and CTGP state), a Mii and Steam open. Keep the launcher open on **Multiplayer**: Steam shows it as **Spacewar** (AppID 480, Steam's test app), which is how invites reach it.

1. **Host:** **Multiplayer → Create room**, then **Invite** next to a friend (or **Invite through Steam**). The host's game opens and creates the LAN room.
2. **Friend:** accept the invite in the launcher (**Accept**), in Steam chat, or with **Join** under the friend's room, and choose **one or two local players**. The game opens the LAN room list and joins; with two players, the second one registers and picks a Mii first.
3. Everyone picks a character and kart; the host sets the rules.

If automatic entry stops, continue by hand: at the main menu hold **L + R** and click the **left stick** to reveal **LAN Play**. The host uses **Create Room**; guests pick the host's room in **Lobby**.

**Steam DLL:** the launcher tries the configured DLL, the data folder and the executable folder, then searches installed Steam libraries, moving to the next candidate if one cannot initialize the required interfaces. If it still reports a missing `SteamAPI_SteamFriends_v017`, set `MK8_STEAM_DLL` to the full path of a compatible `steam_api64.dll` (`libsteam_api.so` on Linux) and restart the launcher.

**Known limits:** a friend whose launcher is closed will see Steam try to open Spacewar; open the launcher and use **Join** instead. A full race between two PCs is not yet qualified, and only two players have been tested (the game supports up to 12 on LAN).

## Booster Course Pass DLC

Import it during preparation (**Game files**) or later in **Install → Additional content**: select the Booster Course Pass v65536 NSP or NSZ (title `0100152000023001`) and, if needed, your keys, then **Import DLC NSP/NSZ…**. The importer verifies NCA hashes, the CNMT and the extracted files. A failed reinstall keeps the previous installation.

Enable or disable it in **Settings → Additional content → Enable Booster Course Pass**. Disabling keeps the imported files and saves. **Reinstall or select another NSP/NSZ** lets you pick a new package; files are only replaced after validation.

## CTGP Deluxe course pack

CTGP Deluxe 1.1.1 targets MK8 3.0.3. MK8-online adapts its course resources to 4.0.1 without running the original plugin. Compatibility is incomplete: track-specific bugs, graphical defects and performance drops remain.

1. Download CTGP Deluxe from [ctgpdx.com](https://www.ctgpdx.com/download) (personal use) and extract it.
2. **Install → Additional content → CTGP Deluxe**: choose the extracted folder (the one with `atmosphere/`) and click **Import CTGP…**. On the same disk as the data folder it takes seconds and no extra space (hard links). Choose where the courses go:
   - **Replace the base game's cups** — 48 courses, +16 with the DLC.
   - **Replace the DLC's cups** — 48 courses next to the 48 originals; needs the Booster Course Pass.
   - **16 new cups** — all 64 courses on two more cup pages (L/R), with their own names, images, music and icons.
3. **Enable CTGP Deluxe** to switch it on; turn it off to play the original game. With CTGP on, the game uses its own saves, created once as a copy of yours.

In Steam rooms everyone needs the same CTGP state. CTGP's own menus and random button are unavailable; Time Trial staff ghosts are the original course's; online sessions use the game's cups. MK8-online does not distribute CTGP.

## RomFS mods

Mods that only replace game files (textures, models, music, layouts) for MK8 Deluxe 4.0.1 can be installed by hand. Put the mod's extracted files in `<data folder>/user-no-mii/mods/<mod name>/romfs/`, with the same paths as the game's RomFS. In the game, open **F1 → Settings → Mods**, set **Show** to **Installed**, select the mod, enable it, then close and restart the game. To go back to the original files, turn the mod off and restart again. When several mods replace the same file, the one later in the Mods list wins.

Only RomFS files are loaded: ExeFS patches, IPS patches, cheats and executable plugins are not supported. A mod that replaces the menu layout (`UI/cmn/common.sarc` or `menu.sarc`) turns off the launcher's native Menu entry while it is on. Nextendo sessions start without installed mods. Steam rooms compare the DLC and CTGP state, not manually installed mods, so agree on them with your friends. Use the importer above for CTGP Deluxe.

## Console fonts

By default, dialogs use substitute fonts (Segoe UI before Arial on Windows). To use fonts dumped from your own Switch:

1. With homebrew working, put the `.nro` from [SharedFontDumper releases](https://github.com/simontime/SharedFontDumper/releases) under `switch/` on the SD card and launch it from the Homebrew Menu. Press **A** to extract individual TTF files, then **+** to exit.
2. Copy the SD card's `fonts` folder to your PC.
3. In **Settings → Game fonts**, use **Import Font Folder**. `FontStandard.ttf` or `nintendo_udsg-r_std_003.ttf/.bfttf` is required; regional/button fonts are optional.

**System Default** disables imported fonts without deleting them; **Imported Console Fonts** re-enables them. Changes apply on the next launch. Combined `shared_font.bin` dumps and firmware archives are not accepted.

## Reporting a problem

In **Support → Create report**, create a diagnostic ZIP and attach it to an [issue](https://github.com/Vnnc1/MK8-online/issues) with your CPU/GPU, launcher version, what you did and what happened. For multiplayer, attach reports from **both** PCs. Reports include game, build, tool and DLC logs; personal paths and account names are redacted, and saves and keys are excluded. Review attachments before sharing. Never post ROMs, console keys or private files.

The **Support** tab can also prepare a GitHub issue draft for you to review and publish yourself; nothing is uploaded automatically.

## Troubleshooting

| Symptom | What to do |
| --- | --- |
| Build stopped with the RomFS `MAX_PATH` message | Install 0.5.0 or later, keep the same data folder and retry **Install → Build the game**. Completed work is reused. |
| Game exits unexpectedly | The launcher shows the last fatal runtime message and saves a diagnostic ZIP under **Support**. Attach it to an issue. |
| A fix from a new version does not appear | Rebuild: **Install → Update the build** (or **Tools → Build the game**). Most runtime fixes need a rebuild. |
| Stutter the first time an effect appears | Expected with a cold shader cache; the disk cache helps on later races. |
| Steam friends list is empty or `SteamAPI_SteamFriends_v017` is missing | See [Steam DLL](#steam-rooms-with-friends). |
| `discord_unlinked` on Nextendo | Link Discord to the same Nextendo account and try again. |
| Controller does not respond | Include its model and connection type with the report from **Support**. |
| Linux: **Tools** says Python 3.12 was not found | Install Python 3.12 (Ubuntu: `python3.12 python3.12-venv`; Arch/CachyOS: `python312` from the AUR, pyenv or uv), or set `MK8_PYTHON312` to its path, then reopen the launcher. |
