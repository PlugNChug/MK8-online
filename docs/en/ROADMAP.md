# Open work and roadmap

**English** · [Português](../pt-BR/ROADMAP.md) · [Status summary](STATUS.md)

MK8-online is **experimental**: it reaches the menu, completes races and has run two multiplayer instances on one machine, but it has known defects. Nothing here is qualified (`gameplay_verified=false`). Technical detail and evidence for each item are in the [status summary](STATUS.md) and the dated [technical log](log/README.md).

Legend: **[blocker]** prevents normal use · **[visible]** gets in the way but can be tested · **[validation]** works in testing, still needs proof in real use.

## After 0.5.0

- Validate on a physical Steam Deck: Desktop Mode preparation, a full race, audio, controls and save reload. The amd64 AppImage and Ubuntu dependency list are in the [Linux guide](LINUX.md).
- Repeat the solo-without-DLC run on the reporter's RTX 4090, validate DLSS and FSR 4 on matching GPUs, and record connection, results and reconnection between two PCs on Nextendo and Steam.
- Track cold-cache stutters and the cube-map warning on the identified courses. [0.5.0 status](STATUS.md) · [Release notes](../releases/0.5.0.md)

## Ultrawide and native menu — experimental delivery

- **Implemented and reviewed in the isolated candidate:** Original/Stretch/Ultrawide option in the launcher and in game; solo cameras validated between 16:9 and 32:9, proportional HUD at the edges, transitions and class banner. Opt-in native menu, private derived resources, persistence and CTGP integration. Included in 0.4.0.
- **Automated verification done:** native/browser regressions, Rust/format/Clippy, host/guest build with consumers checked, and independent review. The new candidate ran Stadium through Water Park and persisted a Settings preference across two real processes. Human qualification of audio/controls/clock and reload of new progress is still pending; manual evidence from the earlier combination does not replace this retest. Keep the 16:9 fallback on paths without demonstrated support.
- **Time Trial extension:** the opening close-up was reproduced in 16:9 and included with its own identity/mode/flag; native regressions and rebuild done, with close-up/race/Settings exercised at 21:9 and 32:9. Keep the fallback on other unknown paths; full qualification pending.
- **Future validation:** every course/effect, paint equivalence, MKTV/replays, team Battle/VS, online and split-screen; another GPU and a physical ultrawide monitor. Frame generation with the wide camera needs its own validation before it is enabled. Do not extrapolate one race's evidence to every mode.
- **Future camera:** investigate character distance separately from aspect/FOV, then opt-in first person; keep the default and test clipping, culling, rear view, boost, water, glider, anti-gravity and rescue before exposing options.
- **Motion:** extend motion-steering testing to other controllers after physical validation of the Switch Pro Controller and Joy-Con.
- **Multiplayer deferred by the maintainer:** the laptop has the game but lacks a separate Steam account. Require compatible versions/DLC/CTGP, two accounts, a full race with results and reconnection on two PCs, with sanitized logs. Does not block the experimental solo release.
- **Motion steering and HD Rumble:** in a race, the driver confirmed motion steering and gliding with the Switch Pro Controller over Bluetooth. Two sideways Joy-Cons also steered independently by motion in the same race. Physical tests covered side separation, frequency changes and stopping vibration.

## Priorities after experimental 0.4.0

0.3.0 brought CTGP to the 4.0.1 profile (64 courses / 16 extra cups), the default redesign, optional DLC, Miis and the accepted `fix-chore` PRs. 0.4.0 adds Windows portability, controllers, graphics improvements and save fixes. See the [bilingual notes](../releases/0.4.0.md). This does not mean full compatibility; the original CTGP plugin is not executed.

- **[blocker]** Fix Steam DLL discovery when an incompatible local copy hides another installed one. Validate the SteamUser and SteamFriends interfaces on the setups from issues #69–71.
- **[blocker]** Correctly renew the authenticated session on Steam reconnection to the same room (#32), keeping anti-replay protection, and validate rejoin/race between two PCs.
- **[blocker]** Investigate SDL/Vulkan startup failure despite a passing preflight (#5) and memory crashes on Intel/AMD integrated GPUs (#11/#17).
- **[visible]** Retest graphics defects #8/#26 and performance profiles #7; shader pauses still exist. More CTGP courses, GPUs and three/four players need coverage.
- **[validation]** Upgrade 0.3.0 → 0.4.0, game rebuild, saves/restart, Mii parts, DLC and local libraries on testers' machines. Full race, audio/controls/clock and multiplayer remain globally unqualified.

The per-version entries below are historical; the current review of all issues is in the status documents.

- **0.1.7 preparation:** private Python 3.12 x64 downloaded automatically, with repair of incompatible/incomplete environments and backup. Installations on other machines still need follow-up.
- **0.1.8 preparation (testing):** cache recovery, Python/pip isolation, process coordination and handling of locked files/unavailable drives. Vulkan loader included. Still needs validation on the reporters' machines with real antivirus/devices; does not change gameplay qualification.
- **0.2.0 re-release:** setup approved by the user on 2026-11-13, with verified Mii library download, Python preparation without PowerShell and Mii conversion progress shown on the Play screen. GameSir via USB receiver confirmed with Steam connected after disabling Steam Input for Spacewar and restarting game and launcher. Other controllers and installations still need follow-up; this does not qualify a multiplayer race.

## 1. Multiplayer

- **[validation — 0.2.0]** The public interface uses Steam rooms, friend invites and experimental automatic entry into the LAN lobby. A Steam host and two games over local transport on the same PC were exercised. A guest from another account/PC and full races between two machines still need confirmation; AppID 480 and a local `steam_api64.dll` remain requirements. LAN/relay/P2P below remain API-only development paths.
- **[Nextendo on the 0.4.0 re-release]** On 2026-11-16 the maintainer played with other people on MK8 4.0.1, with Miis and without CTGP. The launcher offers browser sign-in and matchmaking happens in the game's Online Play. The F1 panel queries rooms and friends. The runtime validates the ZNC CA and hostname and uses the racing-server key accepted by the user. Still to retest on this revision: a race through to results, reconnection and interaction between two independent installations. [Third-party sources](THIRD_PARTY.md)
- **[validation]** A communication error appeared once with two instances on the same machine (not reproduced on repeat). If it appears between two machines, test with `MK8_ASYNC_SUBMIT=0` and send reports from both PCs.
- **[validation]** Race between **two machines** over direct LAN (UDP 47820 open on the host's firewall). Transport and procedure exist; needs repeating with two real PCs.
- **[validation]** Internet room through the HTTPS/UDP **public relay** (`mk8-relay`). Only tested on loopback so far; NAT, latency and MTU not measured.
- Verified on 2026-11-08 with the then-current build: loopback relay and direct LAN with two games in the same race, 60 FPS, no communication error. VS race and Battle work. Menu videos fixed.
- **[visible]** In the LAN lobby, the second player's name appears mirrored.
- **[visible]** Split-screen: after the controller applet, two-player character selection ignores both players' buttons, although the game reads and receives both controllers (diagnostics in `toolchain/split_screen.py`).
- **[validation]** More than four real games (up to 12), with network loss/latency and reconnection.

## 2. Image

- **[visible]** The bloom halo of the stadium sign disappears in about 1 of every 4 frames (secondary bloom chain parameters).
- **[visible]** Coloured dots, shadows and general image fidelity.
- Fixed: scene brightness flicker at 30 Hz (environment cube map read through a reused texture ID).
- Fixed: blown-out exposure (shader-generated BC3 specular reflection at race start written to an invalid view; `toolchain/bc_render.py`). 7 of 7 GPs clean.

## 3. Performance (current priority: VRAM)

- **[visible]** Video memory grows when switching courses. Command lists that outlived their buffers/pools were fixed; some large old images still belong to textures the game never finalizes. The candidate with 128 MiB VMA blocks ran four courses and returned to cup selection, at ~60 FPS in measured sections with a 1,648 MiB reserved peak on heap 0. Still to isolate the effect of smaller blocks and check stabilization over repeated cups/courses; active memory dropped in the menu, but the reservation remained.
- **[validation]** Direct release of pool images remains opt-in (`MK8_RELEASE_POOL_TEXTURES=1`). The possible FPS regression in the Thwomp Ruins GP did not recur in the partial Time Trial, which had no compute dispatches in the measured section; the specific dependency is not yet identified.
- **[visible]** First use of effects: with the bounded wait (default, 4 ms per frame) an effect may be missing for a few frames instead of freezing the screen; the disk cache fixes it on later races.
- **[visible]** Smaller 25–45 ms spikes when entering new areas (shader prefetch) and in command translation.
- Improved: 32-submission ring, command translation on its own thread and bounded shader wait. Full GP: 59.99 FPS, p95 17.3 ms, p99 17.6 ms, worst frame 38 ms (at the start of that day: 57.9 FPS, p95 21.2 ms, 160–370 ms spikes). The game runs at 99.7% of real speed.
- Fixed: crash (`c0000005`) at race start when comparing samplers copied by the game.
- **[validation]** Stable 60 FPS and correct game speed: MK8 advances one step per frame, so below 60 FPS the game runs in slow motion.

## 4. Race qualification

- **[validation]** Audio, physical controller, restart/save, clock and image verified together in a full race.
- **[validation — 0.2.0]** Mii creator with thumbnails, game-body preview and resource preparation for the original renderer: portraits, Mii Suits, driver and minimap observed locally. Still to qualify every part, the LAN lobby, comparison with console resources and a full race. The AFL library is downloaded from the Internet Archive with verified hashes or selected locally by the user, and is not included in GitHub/the installer.

## 5. Launcher and distribution

- Included in setup 0.1.6: English and PT-BR in the launcher and installer, with selection and persistent preference. The READMEs follow both languages.
- **[validation]** Before making the repository public, review authorship name/email and committer in Git history. The 2026-11-08 review found these personal metadata in the 21 reachable commits; the content scan found no credentials with the patterns used. History was not rewritten.
- **[validation]** Update through **Update now** against a really published release (unit tests cover version, asset and SHA-256).
- Single instance: two launcher windows would compete for the same data folder.
- Closing the window does not interrupt a running preparation or game.
- XCI/XCZ import, DLC and other game versions are out of scope until the current path is qualified.

## How to help

1. Install from the latest release's `setup.exe` and prepare with files from **your** console.
2. When you hit a problem: **Support → Generate report**. Open an issue with the `.zip`, what you did and what you expected.
3. To test multiplayer on two machines, follow **Multiplayer → How to test on two machines** and send reports from both PCs.
