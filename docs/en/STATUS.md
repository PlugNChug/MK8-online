# Project status

**English** · [Português](../pt-BR/STATUS.md) · Updated 2026-11-26 · Release 0.6.3

This page is the current summary. Dated investigations, measurements and per-issue evidence are kept in the [technical log](log/README.md). Open work is tracked in the [roadmap](ROADMAP.md).

## Qualification

`gameplay_verified` and `multiplayer_verified` are both `false`. Qualification needs, together: menu, a full race, restart/save, image, audio, controller and clock speed. Multiplayer needs separate validation on two machines. The milestones below are observations, not a completion percentage.

| Area | Current progress | Next checkpoint |
| --- | --- | --- |
| Launcher | Native window, guided preparation with a fallback Zig download on Windows, installer, updates, uninstall, reports; English, Brazilian Portuguese, Spanish | Upgrades from earlier versions on more PCs |
| Solo racing | Full Mushroom Cup with replays and cup progress persisting across a restart; battles, MKTV and Time Trial reached; all 160 courses (48 base, 48 DLC, 64 CTGP) loaded in Time Trial | Full image, audio, controller and restart/save qualification |
| Mii | Creator with numbered options when the 3D preview is unavailable, profiles and the game's own Mii renderer (portraits, suits, driver, minimap) | Mii in LAN lobbies, comparison with console resources |
| Multiplayer | Native Nextendo matchmaking through Online Play; Steam rooms with invites; local LAN tests | A Nextendo race to results; a full Steam race between two PCs |
| Graphics | Vulkan runtime with scaled bloom correction for Vancouver Velocity; FSR 1, FSR 3.1.6 (run on RX 580), FSR 4 / DLSS (capability-tested only); experimental ultrawide (also with frame generation) and frame generation | DLSS on RTX, FSR 4 on RDNA 4; more courses and GPUs |
| Performance | Around 60 FPS in measured race windows on the test PC | VRAM stability over repeated cups, cold-cache stutters, more hardware |
| Linux | AppImage/tar.gz built and smoke-tested in CI on Ubuntu 24.04; rendering on Intel Iris Xe | Physical Steam Deck race |

## Changes after 0.6.3

- Fullscreen hides the idle mouse pointer by default. The preference is shared by the launcher and the in-game Video settings. [Fullscreen cursor auto-hide](log/graphics.md#fullscreen-cursor-auto-hide)

## 0.6.3 release

- **0.6.3:** races run at 60 FPS again. Since 0.6.0 the upscaler and frame-generation checks read environment variables on every draw; a 12-racer Grand Prix on an RX 580 spent a third of the render thread there and ran at 27-45 FPS in slow motion (#133). Each option is now read once. 0.6.3 also carries the 0.6.2 fixes below.

- **0.6.2:** on Linux the launcher recognizes the build again (the split-screen and LAN capabilities were read from a Windows DLL, #128); with Steam on Wayland the game uses XWayland so the Steam Controller leaves desktop mode (#131); occlusion results the driver does not return no longer end the game (#127).

- **0.6.1:** Nextendo now has 45 s to refresh the sign-in and fetch the game token; before, on a slow connection the token was never requested and the game closed with code 2 (#125). The log records why and the launcher explains the failure.

## 0.6.0 release

- **Linux in the same tree:** the Linux port ([PR #83](https://github.com/Vnnc1/MK8-online/pull/83)) is now built from the main tree together with Windows. The Linux launcher finds a 64-bit Python 3.12 (#112, #119) and initializes controllers through SDL (#120).
- **Integrated PRs:** Menu themes (#92), camera distance (#107), 60 FPS with 3–4 players (#108), bloom option (#109), Steam entry with 1–2 players (#110), controllers (#75), Vulkan fixes (#102, #104, #105, #106), Mii resources after moving the portable folder (#116), RomFS mods (#117) and documentation (#100).
- **Fixes in this round:** black 3D scene in the first race with an empty cache; browser language in the launcher and the Menu; Simplified and Traditional Chinese (#99); ultrawide kept with frame generation; VRAM exhaustion at high resolution; videos replaceable by mods; Pascal/GTX 10 (#114).
- **Tests:** 140 Rust tests with formatting and Clippy; 196 Python tests in 48 modules of the changed areas; the full flow through the launcher on a clean install (tools, build, DLC, Mii, race).

## Latest local validation

- Clean build with game, update, DLC and keys at once: 26.7 min, DLC imported in about 20 s, race opened from the launcher at 60 FPS.
- RX 580 2048SP: Mario Kart Stadium at 60 FPS with an empty cache; Twisted Mansion at 2x (4K internal) without running out of VRAM in 3 of 3 races (before, 3 of 4 ran out); 32:9 ultrawide with 120 of 120 generated frames.
- Main menu in Simplified Chinese; a RomFS mod serving the menu video.

## Open items

- A hard-edged rectangle of light on the floor of dark courses (Twisted Mansion) and kart lights in CTGP.
- The game's dynamic resolution remakes its render targets every frame when the GPU cannot hold 60 FPS.
- Mii preview differs after swapping in another save (the save keeps a copy of the Mii).
- Intel integrated GPUs, a 4K monitor in fullscreen and a real 32:9 monitor need retests with player logs.
- A Steam race between two PCs and Nextendo reconnection.

## Benchmarks

Xeon E5-2666 v3 / 16 GB RAM / RX 580 2048SP, race sections of a full Mushroom Cup:

| Track | Average FPS | p95 frame time |
| --- | ---: | ---: |
| Mario Kart Stadium | 59.99 | 17.46 ms |
| Water Park | 59.98 | 17.45 ms |
| Sweet Sweet Canyon | 59.97 | 17.46 ms |
| Thwomp Ruins | 59.99 | 17.33 ms |

These exclude capture windows and do not guarantee 60 FPS in every scene or on other PCs.

## Updating this page

Keep this page short and current: replace outdated lines instead of appending. Put dated detail (reports, logs, measurements, reasoning) in the technical log, one section per topic, newest first. Update the [Portuguese summary](../pt-BR/STATUS.md) in the same change.
