# Project status

**English** · [Português](../pt-BR/STATUS.md) · Updated 2026-11-20 · Release 0.5.0

This page is the current summary. Dated investigations, measurements and per-issue evidence are kept in the [technical log](log/README.md). Open work is tracked in the [roadmap](ROADMAP.md).

## Qualification

`gameplay_verified` and `multiplayer_verified` are both `false`. Qualification needs, together: menu, a full race, restart/save, image, audio, controller and clock speed. Multiplayer needs separate validation on two machines. The milestones below are observations, not a completion percentage.

| Area | Current progress | Next checkpoint |
| --- | --- | --- |
| Launcher | Native window, guided preparation, installer, updates, uninstall, reports; English, Brazilian Portuguese, Spanish | Upgrades from earlier versions on more PCs |
| Solo racing | Full Mushroom Cup with replays and cup progress persisting across a restart; battles, MKTV and Time Trial reached; all 160 courses (48 base, 48 DLC, 64 CTGP) loaded in Time Trial | Full image, audio, controller and restart/save qualification |
| Mii | Creator, profiles and the game's own Mii renderer (portraits, suits, driver, minimap) | Mii in LAN lobbies, comparison with console resources |
| Multiplayer | Native Nextendo matchmaking through Online Play; Steam rooms with invites; local LAN tests | A Nextendo race to results; a full Steam race between two PCs |
| Graphics | Vulkan runtime; FSR 1, FSR 3.1.6 (run on RX 580), FSR 4 / DLSS (capability-tested only); experimental ultrawide and frame generation | DLSS on RTX, FSR 4 on RDNA 4; more courses and GPUs |
| Performance | Around 60 FPS in measured race windows on the test PC | VRAM stability over repeated cups, cold-cache stutters, more hardware |
| Linux | AppImage/tar.gz built and smoke-tested in CI on Ubuntu 24.04; rendering on Intel Iris Xe | Physical Steam Deck race |

## 0.5.0 release

- **Windows:** the installer and portable ZIP are the maintainer-validated revision. The ZIP opened from a temporary folder and reused the prebuilt runtime after key and hash checks.
- **Linux:** the AppImage and tar.gz come from mitch030504's port ([PR #83](https://github.com/Vnnc1/MK8-online/pull/83)). CI on Ubuntu 24.04 passed Rust and ELF tests, built both packages, started the packaged launcher and importer, and ran a headless tar.gz update and rollback.
- **Tests:** 109 Rust tests and 35 launcher/installer browser tests passed, with formatting and Clippy. The optional Rust test that creates a real Steam room was not run.

## Latest local validation

- A clean build recompiled all 484 MK8 4.0.1 units in 31 min 47 s; an immediate repeat took 46 s.
- 430 of 431 discovered Python tests passed on Windows; the dedicated graphics suite passed all 46 cases.
- Time Trial sweep at 1920 × 1080 on an RX 580 2048SP: median FPS 60 on base and CTGP courses, 59.1–60 on DLC. Worst ~2 s window 48.1 FPS (DLC). Cold-cache compile bursts remain (a 6.3 s load with 503 compilations on Squeaky Clean Sprint).
- Two DLC particle shaders that failed the `TMML.B` mip-level query now translate and compile.

## Open items

- Track voting with missing Mii models (RTX 5070 Ti report): fixed locally, needs a retest on the player's machine.
- Solo start without DLC crashing on an RTX 4090 (driver 621.24): not reproduced on an RX 580; needs confirmation on that hardware.
- A Steam race between two PCs and Nextendo reconnection.
- VRAM growth between courses and first-use shader stutters.

See the [roadmap](ROADMAP.md) for the full prioritized list.

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
