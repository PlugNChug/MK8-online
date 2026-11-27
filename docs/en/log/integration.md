# Technical log — PR integration, issues and verification

[Index](README.md) · [Português](../../pt-BR/log/integration.md)

## Spanish (Latin America) (#177)

The local 4.0.1 NACP declares this game language. The launcher and F1 game language lists expose its Switch code, `es-419`. In the November 25, 2026 Windows test build containing both locale fixes (runtime SHA-256 prefix `df0d13b2`), the automated menu run read the menu, message and font archives from `UI/USes`.

## Device loss at race startup on a Quadro M5000M (#165) — 2026-11-26

Both logs from 0.6.3 on the Quadro M5000M (`10de:13f8`, driver 582.53) report `VK_ERROR_DEVICE_LOST` from `vkWaitSemaphores` when loading a race. The occlusion fallback logged immediately before it already handled unavailable counts as visible. The driver supplied no fault detail that identifies the draw. The signature resembles [#114](https://github.com/Vnnc1/MK8-online/issues/114). The shader workaround from [#7](https://github.com/Vnnc1/MK8-online/issues/7) moved invariant texture-size queries outside the shader dispatch loop and was previously restricted to Pascal.

The candidate extends that rewrite to the reported NVIDIA PCI ID. The device-selection test checks the added ID, nearby Maxwell IDs and other vendors. Four native shader, driver and occlusion tests passed. The runtime-only rebuild preserved the guest library. On the RTX 5070 Ti Laptop GPU, a cold-cache Grand Prix on Mario Kart Stadium reached frame 3420 on the baseline `5603bfb`, with and without Vulkan validation, and on the candidate. A separate test build forced the rewrite on the RTX, emitted 49 fragment shader files with hoisted texture-size queries, and reached the same capture point. The workaround still needs confirmation on the Quadro M5000M. The existing `LARECOMP_VK_DEVLOST_TRACE=1` captures recent GPU operations if that retest still loses the device.

## 0.6.0 cycle — summary of 2026-11-20 to 2026-11-22

The detailed record of this cycle is in Portuguese, in the [Portuguese log](../../pt-BR/log/integration.md) and its topic files (graphics, performance, controllers, launcher, releases). Summary:

- **Linux merged into the main tree** (PR #83): the upscaler runtime takes the Linux FSR/DLSS code with Windows absolute-path loading and provider version refusal; Steam combines the cross-platform loader with the verified interface versions; guest pins accept LF and CRLF. Post-merge fixes: the pipeline prefetch waits by default again (an empty shader cache left the first race black), the duplicate point-size patch was dropped, `view_batch.py` stays off until ported over #104/#105, and a lost `import hashlib` in `settings_menu.py` was restored.
- **Integrated PRs:** #75, #92, #102, #104–#110, #116, #117 (with videos served through the mod overlay) and #100 (this documentation layout; `AGENTS.md` kept as it was). #118 duplicated `toolchain/desired_language.py`.
- **Fixes:** browser language when none is saved (launcher and Menu); ultrawide kept with Vulkan frame generation (report from a 5120x1440 monitor); superseded render targets retired every frame under VRAM pressure (Twisted Mansion at 2x ran out of device memory in 3 of 4 runs, 0 of 3 after); SDL gamepads initialized in the Linux input entry point (#120); Python 3.12 lookup on Linux (#112, #119).
- **Checks:** 140 Rust tests, formatting and Clippy; 196 Python tests in 48 modules; a clean install through the launcher (tools, build, DLC, Mii, race) and in-game scenarios for ultrawide frame generation, RomFS mods, Chinese menus and 4K.

## Open 0.4.0 issues — review of 2026-11-20

The 12 open issues and the attachments were read (#69, #74 and #78 with ZIP; #68 with Markdown; #76 and #77 with screenshots). None of this was integrated or published; the changes are in the working tree of `fix-chore`.

- **#76 and #77 — F1 panel always in Portuguese.** All the texts in `runtime/mk8/ui/hub.h` were Portuguese literals. The Nextendo social messages in `social.inc` too. They now go through `mk8_ui_text(pt, en, es)` (`runtime/mk8/net/social.h`). The launcher passes the game `MK8_UI_LANGUAGE` with the language it displays: the saved choice or, with no choice, the Windows language, with the same criterion as `web/i18n.js`. The inherited inner tabs (Video, Audio, etc.) were already in English. The room pop-up ID was fixed (`###hub-room`).
- **#78 — `SDL_Vulkan_CreateSurface: VK_KHR_win32_surface extension is not enabled`.** The generated shim loaded `vulkan-1.dll` by name. When another module, such as an overlay, has already loaded the System32 copy, Windows returns that copy. SDL uses the runtime's copy (`SDL_VULKAN_LIBRARY`). The instance and the surface end up in different loaders, and the second interprets the other's instance with a different layout, which is undefined behavior. In ctypes, with the System32 1.3.250 loader and the build's, the return was `NULL`. The preparation (`host.patch_vk_shim`) now loads `SDL_VULKAN_LIBRARY` first and records `[vk] loader <path>` in `game.log`. Without the variable, the earlier behavior continues.
- **#74 — error connecting to Nextendo.** The reporter's log records `online status: refused: discord_unlinked`, that is, the account has no Discord linked on nextendo.network. The launcher showed only `exit code: 2`; it now reads the reason from `game.log` and explains what to do (pt-BR/en/es) for `discord_unlinked`, `unverified`, `elsewhere`, `disabled` and `unknown`.
- **#69, #70, #71 and #73 — incompatible Steam DLL.** The launcher required exact versions (`SteamUser_v023` and `SteamFriends_v017`), present only in SDK 1.57–1.61. Older DLLs do not have User v023; the newer ones (SDK 1.62+, like that of Counter-Strike 2) have Friends v018, and 1.65 has Utils v011. The `steam_api.json` files of SDKs 1.52, 1.53, 1.57, 1.60, 1.61, 1.62 and 1.65 (Steamworks.NET) have identical signatures for the 25 flat functions used and the same IDs/layouts of callbacks and structs. User v021–v023, Friends v017–v018 and Utils v010–v011 are now accepted. Versions not checked are still refused. An incompatible DLL in the data folder no longer hides the installed game libraries. Rooms, invites and transport with those versions remain without live testing.
- **#79 — DLC with “exit 1”.** The issue has no log. The importer refuses with `ValueError` any package that does not match the Booster Course Pass pinned by hash. The failure of a step now shows the message of that exception, and not only the exit code. The cause on the reporter's PC remains unknown.
- **#72 — duplicate files.** `game-data/<id>/extracted/romfs` contains hard links to `imports/mk8-4.0.1/romfs` (`fsutil hardlink list` shows two names for the same file). Explorer adds up both names; there are not 11.3 GB duplicated on disk.
- **#67 and #68** were reported on 0.3.0 and fixed before 0.4.0 (section “Shell Cup 50 cc” below). A retest by the reporter on 0.4.0 is missing.
- **#77 (suggestions)**: country flag, FXAA, FSR 4 and DLSS are feature requests, not failures.

Validation: `--runtime-only` compilation of the 228 runtime units with the changes, using `MK8_NATIVE_TOOLS` of the `mk-test` installation. The update of candidate `local/pr-integration` was refused by the hash check (the referenced `mk8dgame.dll` changed), and the candidate was not altered. The generated executable contains the English and Spanish texts and the new shim. Passed: 15 Nextendo tests against that executable, `test_ui_language.py` (native C with each `MK8_UI_LANGUAGE`), `test_vulkan_loader_shim.py`, settings, menu, package and Vulkan tests in a venv with pinned hashes, 102 Rust tests of the workspace (one optional live Steam test ignored), Clippy with `-D warnings` and `cargo fmt --check`. In-game capture: in `local/f1-i18n`, an isolated copy of `local/nx-c400n7aw` with host hard links and copied saves, the new runtime opened F1 through `scripts/test-race.py` and exited with code 0. With `MK8_UI_LANGUAGE=en`, the four pages appear in English (Play, Settings, Quick/Video/Audio/Controls/Account/Mods/Quit); with `es`, in Spanish. `game.log` recorded `[vk] loader` with the host's copy. Pending: a race on a machine with an overlay that loads the System32 loader; live Steam with a v018/v021 DLL.

## Validation of PRs #65 and #66 on fix-chore

[PR #65](https://github.com/Vnnc1/MK8-online/pull/65) passed Windows CI. On the integrated tree, the ten controller tests passed, including per-player motion, reconnection and interruption of vibration on virtual SDL devices. The `--runtime-only` build completed the 227 units and verified 13 executables and DLLs in a test copy. On Boo Lake, two scripted entries reached the race on the first lap, with both players visible in the frame 6100 capture. The process ended with code 0. The physical gyroscope and vibration checks described below were done by the PR contributor.

In total, 39 tests of controllers, settings and runtime integration and eight more of settings in the browser passed. Boo Lake again recorded the same texture-type warning as the session before the PR. The race was run while other tests were compiling, so the FPS measurements of that session do not allow attributing a variation to the controller change.

[PR #66](https://github.com/Vnnc1/MK8-online/pull/66) adds Discord to the English and Portuguese READMEs. The invite corresponds to the **MK8-online Community** server, and the two badge images answered with HTTP 200.

## Integration of PRs #56 and #62 on `fix-chore`

- [#56](https://github.com/Vnnc1/MK8-online/pull/56) opens the launcher in the installer window and reuses the selected data folder. The real WebView2 passed a fresh install with the program on C: and data on D:, an update from 0.3.0 with language and test saves preserved, and completion with the automatic open unchecked. The browser test also verifies a single automatic attempt, presentation of the error and a new manual attempt. The flow reuses the launcher's common configuration and preserves the `--play`, portable and silent-update entries.
- [#62](https://github.com/Vnnc1/MK8-online/pull/62) applies the shader adjustment only to the desktop GTX 1070 (`10de:1b81`). The native test recompiles the GLSL to SPIR-V and confirms identical GLSL output with the option disabled, plus the `ShaderKey` layout. The ahead-of-time compilation routine receives the same key. The 4.0.1 runtime was recompiled in `--runtime-only` mode, with the game libraries preserved. The performance gain reported in the PR comes from a GTX 1070 on Linux/Proton. Physical measurement on a GTX 1070 on Windows remains pending.

On the RX 580 2048SP, candidate `1c977d963a5b` ran in full screen at 1920×1080, with default quality and two local players. Both tests used scripted inputs, reused caches and isolated saves. Both ended with code 0 and no Vulkan error. The captures show the track, lighting and HUD on both screens. The data below cover windows of the first lap, excluding the captures.

| Track | Median FPS | Lowest FPS window | Longest frame |
|---|---:|---:|---:|
| Boo Lake | 60.0 | 48.4 | 69 ms |
| Luigi Circuit (CTGP) | 59.8 | 46.8 | 49 ms |

Luigi Circuit reached 1,974 MiB of sampled dedicated VRAM and ended at 1,718 MiB. Boo Lake had partial memory collection, with a sampled peak of 1,974 MiB. Loading pauses and texture/cubemap warnings already seen before persisted. Private evidence in `local/audit-fullscreen/pr62-boo` and `local/audit-fullscreen/pr62-luigi`. Activation remains off on the RX 580. These targeted rehearsals keep `gameplay_verified=false`.

## Integration of PRs #55, #58, #59 and #60

`fix-chore` received the merges `02b32c4` (#55, Boo Lake target scale and RGB565), `a252360` (#58, depth clamp), `7680814` (#59, solo ultrawide and experimental settings) and `eaf6c04` (#60, interpolation). The translator of #60 was already in `13bcaee`, and the merge records its origin. Conflicts were resolved by keeping the documentation and attributions from both sides.

Joint validation: 96 Rust tests passed, with one live Steam test ignored. Launcher build, formatting and Clippy passed. Also passed were 43 native and pipeline tests, with no skips, and eight settings tests in the browser. The combined build recompiled 226 host units and eight guest units, reusing 476 objects checked by hash. Relink of the three modules, preparation of the settings layouts and 13 binary checks completed.

In a real process, the graphics combination #55/#58/#60 ran Boo Lake in split screen at 1920×1080, with the Rock Cup colors checked at selection. It ended normally on the first lap at 110.8 s. The final build `7e9870d0f027`, which includes #59, opened in-game settings in full screen via F1 with the experimental option enabled. It ended at 34.2 s. Rehearsals with isolated saves, recorded in `local/lighting-fullscreen/merged-graphics` and `local/lighting-fullscreen/merged-pr59-menu`. These are integration tests, with no qualification of full gameplay or of the Rock Rock Mountain shadow in this combination. #56 remains a draft and #62 points to `master`.

## Integration of PRs #42, #43 and #44 on fix-chore

The history of the three PRs was integrated with separate merge commits. The save/Mii code (#42), the cubemap history guard (#43) and compact commands (#44) were already present in the local commits `07126fd` and `26a6dce`. Comparison with `53ae368` confirms the merges did not change runtime/toolchain/tests; conflicts were resolved preserving the current documentation, output-aspect, memory recovery and the other later fixes. The README received the PRs' clarifications.

Joint validation: 54 native tests passed with no skips (177 s), covering saves in separate processes, Mii, layout, textures, recording of the 62 command types, asynchronous submission, descriptors, compute and occlusion. `--runtime-only` build completed from the sources: 220 host units recompiled, dependencies verified and guest library preserved. The first attempt used a diagnostic candidate with an external DLC reference and was refused; the repetition used `local/pr-integration`, with entries mirrored by hard links and without modifying the installation or the original saves. Private logs: `local/open-prs-tests.log` and `local/open-prs-runtime.log`.

Later automatic rehearsal: `local/race-grand-prix-l3qggao2`, 214.9 s, exit 0, no `FATAL`. Captures at frames 3500 and 12000, inspected, show Mario Kart Stadium on laps 1/3 and 3/3. The last eight windows record 59.9–60 FPS; they do not represent all tracks nor the absence of stutters. Saves/cache copied; there was no qualification of a full cup, trophy persistence, audio, physical controller or multiplayer in this round; `gameplay_verified=false`. The first attempt (`onujjp1j`) entered two players through the old sequence and was interrupted; the repetition removed the initial DOWN to choose one player. Do not count that first attempt as an approved race. Evidence: `local/open-prs-race-final.log`.

## Review of open Issues and readiness for testing — 2026-11-16

Review of the **14 open Issues**, including the available comments and attachments, against `fix-chore 8fafe81`. The public release `v0.2.0`, published on 2026-11-13, does not contain all the changes of this branch; although its text says experimental, the GitHub flag `isPrerelease` is false. The various local setups also use `0.2.0`: that number alone does not identify the code tested. This review did not publish, comment on or close Issues and did not generate a setup.

**Recommendation:** there is enough progress for a new controlled round of Windows testing, but not to declare stability or qualified multiplayer. Before broadening the promotion of multiplayer, fix the reconnection reproduced below and validate joining/leaving/returning to the same room on two PCs. Also investigate the Vulkan initialization of #5; memory failures on integrated GPUs and visual defects remain explicit pending items. Publishing a distinct version as a pre-release in the future, with commit/hash and limitations, avoids mixing reports from different builds called `0.2.0`. Frame generation remains optional/off by default; 120 presentations do not mean 120 Hz simulation.

| Issue | Conclusion on this branch |
| --- | --- |
| [#32 — Steam between two PCs](https://github.com/Vnnc1/MK8-online/issues/32) | **Reconnection failure reproduced locally on the current Steam path.** The original report confirms the gateway, but not the same room inside the game/race. Auto-LAN and controller interaction still require testing between PCs. |
| [#26 — graphics defects](https://github.com/Vnnc1/MK8-online/issues/26) | Capture at Music Park with rectangular/irregular regions; RTX 2060 log with no fatal. Not reproduced nor considered fixed in this review. The shader-wait improvements do not demonstrate a fix of that rendering. |
| [#23 — pending installation](https://github.com/Vnnc1/MK8-online/issues/23) | Only a capture and a description, with no log that determines the cause. The new installation flow and HTTP block passed; not enough evidence to claim the case is the same Mii freeze. |
| [#22 — CTGP Deluxe](https://github.com/Vnnc1/MK8-online/issues/22) | Feature implemented: integration `8202c32`, import/activation, cups and tracks already exercised in earlier sessions. Two UI/API tests passed again. This does not qualify all tracks nor eliminate the initial stutters. |
| [#17 — crash after updating](https://github.com/Vnnc1/MK8-online/issues/17) | Report on an integrated AMD: image creation fails with `-2` (device memory allocation). There is a mitigation to avoid importing the whole guest heap into a small shared memory; two native tests passed, but a retest on the affected APU is missing. |
| [#16 — buttons/Mii blocked](https://github.com/Vnnc1/MK8-online/issues/16) | Symptoms compatible with the HTTP block that was fixed, with no proof of a single cause for all the comments. Creating/editing/saving a Mii and responsive APIs passed; confirm on the reporters' computers. |
| [#15 — Mii creator freezes launcher](https://github.com/Vnnc1/MK8-online/issues/15) | Specific cause fixed: a large unread AFL response blocked WASM delivery on the serial server. A real test keeps a large body unread and obtains the second resource; it passed again. It does not validate every combination of parts or a race with each Mii. |
| [#14 — Linux/Android](https://github.com/Vnnc1/MK8-online/issues/14) | Portability not implemented/qualified; current platform Windows x64. Platform request, not blocking an explicitly Windows pre-release. |
| [#11 — Vulkan/Intel Iris Xe](https://github.com/Vnnc1/MK8-online/issues/11) | The log detects Vulkan **1.4.323**, then fails in `vmaCreateImage` with `-1` (host memory allocation). It is not evidence of the absence of Vulkan 1.3. Do not consider it resolved by the AMD mitigation nor recommend just a driver without diagnosis. |
| [#8 — menu, MKTV, fonts and FPS](https://github.com/Vnnc1/MK8-online/issues/8) | Specific fixes already integrated for OfflineWeb/guide, MKTV texture reading and font import/fallback. Four native applet/readback tests passed. The attachment's `svcBreak` crash is compatible with the old applet; blue color/text geometry and FPS drops remain without a retest on the report's RTX 3060 Laptop. Old invalid JPEGs are not repaired automatically. |
| [#7 — 1 FPS](https://github.com/Vnnc1/MK8-online/issues/7) | i7-3770/GTX 1070; the game.log attachment returns size zero, with no useful runtime evidence. Setup/build are not enough to attribute it to CPU, GPU or shader. Ask for a new performance capture/log with an identifiable version when authorized contact is available. |
| [#6 — community and comments](https://github.com/Vnnc1/MK8-online/issues/6) | Community request, with additional old reports of Nextendo/restart and DLC. DLC already available; there is no proof of a fix of the Nextendo crash (`c0000005`). The current Steam flow does not automatically qualify the old service. |
| [#5 — game does not open](https://github.com/Vnnc1/MK8-online/issues/5) | **Most recent report, from 2026-11-15 on 0.2.0, still fails.** The manifest declares `vulkan-1.dll` and the preflight reports success, but SDL fails to load the module in the game. Do not close it as the simple absence of the DLL fixed in earlier versions. Three local bundle/probe regressions passed; the cause of the new case remains unreproduced. |
| [#2 — DLC](https://github.com/Vnnc1/MK8-online/issues/2) | Import/activation already implemented; four UI/API tests passed again. DLC/CTGP compatibility between participants is required by the room. Functional request met on the branch, without closing the Issue in this review. |

**Reconnection #32:** `SteamRoom::host_with_backend` keeps one invite per Steam ID in `assigned`, returning the same one on the next JOIN. `Gateway` recreates `Channel` with the send sequence at zero, but the host keeps the old channel's anti-replay window. The private test calls the real public APIs of `mk8-session`, uses local UDP sockets and the existing simulated Steam backend: the first entry and 50 authenticated datagrams reach the host; after destroying/recreating the guest with the same identity, there is no READY in 6 seconds; another identity in the same room gets READY within 3 seconds. Result repeated twice. It confirms the defect in the reconnection code without claiming that every failure in the remote report has that cause. The anti-replay window must not be removed; the solution needs to renew the authenticated session and handle a repeated JOIN without consuming slots indiscriminately. Private test in `local/issues-review/reconnect-probe`, with no connection to Steam users/servers.

**Preflight gap #5:** `apps/launcher/src/vulkan.rs` opens DLLs through `LoadLibraryExW` and checks symbols; it does not run `SDL_Vulkan_LoadLibrary` nor the window creation used by the game. So its success does not guarantee SDL/Vulkan initialization. The bundle regression runs the real SDL loader locally, but did not reproduce the reporter's machine. Without additional diagnostics, do not attribute it to antivirus, driver or a specific missing file.

**Verification of this round:** 41 existing tests passed: 11 launcher ones (unread HTTP response, Mii, installation, DLC, CTGP), 10 native/pipeline (Vulkan, OfflineWeb, readback, shared memory, auto-LAN) and 20 Rust session/transport. One optional real Steam test remained ignored. The first Vulkan command omitted the executable and skipped the probe; the final round with `MK8_LAUNCHER_EXE` ran the three cases with no skips. Embedded release launcher of the local setup, with no web override; long installation/Steam actions in the browser tests use controlled states and do not replace a real installation/download or race. Rust session/transport recompiled in this review. An additional reconnection reproducer confirmed the expected failure; it is not a fix.

Attachments kept private in `local/issues-review`, ZIPs inspected without executing content. Captures #23/#26 inspected. No new race in this round, test on the Intel/RTX of the reports, measurement of clock/audio/controller, restart/save or multiplayer between PCs. The performance and FG measurements below remain valid only for their recorded scenarios. `gameplay_verified=false`.

## Review and integration of PRs on fix-chore — 2026-11-16

Integration on top of `d443092`, in a separate worktree. The diffs, commits and discussions of the nine open PRs were examined, as well as the ancestry of `fix-chore`, `master` and `ctgp-recomp`. PRs #9, #12 and #25 were already ancestors of the base and were not reapplied. No change was made to the checkout, the build artifacts or the private data of `ctgp-recomp`.

| PR | Decision and scope |
| --- | --- |
| #27 | Integrated: local guide and validated OfflineWeb return. Removed the duplication of `mk8_dlc_storage_path` in the Mii harness: that part had existed since `cec9eb0`. |
| #28 | Integrated: limited readback, synchronous through the translator queue, with texture retention; it does not repair old JPEGs. |
| #29 | Integrated: semantic comparison and single finalization of copied textures; keeps retirement, descriptors and the MIT license of the reference. |
| #30 | Integrated: TMML.B only with demonstrated static provenance; unknown variants remain refused. |
| #31 | Integrated: private font import, Windows fallback and PT/EN/ES controls. Manual and fonts are configured together in the supervisor. |
| #33 | Integrated: Steam discovery/API, large datagrams, re-entry after closing the game, manual/automatic navigation, optional DLC and the Mii glasses channels. Preserved the two existing DLC readiness/lock tests that the PR removed, including the new font checks of #31. The earlier Mii freeze fix remains; it was not duplicated. |

**Verification of this integration:** 81 Rust tests passed; one optional real Steam test ignored. `cargo fmt --all -- --check`, workspace/all-targets Clippy with `-D warnings` and the launcher debug build completed. 50 focused runtime/converter/language tests, 42 related regressions (including CTGP phase A, Mii, audio, occlusion and precompiled), 37 browser/API/process tests and two catalog tests passed; the optional external relay was left unconfigured. In the 42 related ones, the duplicated Mii harness was fixed and retested; the FFmpeg test required installing the already pinned Zstandard wheel, only in the private test environment, and passed on retest. The first Rust run shared `MK8_NATIVE_TOOLS` between lock tests; removing that override from the Cargo process restored the expected isolation, without changing the product.

Full host runtime compiled and linked: **218 units**, profile 4.0.1, key `c4048033cd1b61a239018714dea0b1cfc43eb2b252c220908409f9d99323c8be`. No guest was regenerated and no public ABI changed. The first isolated assembly was without FFmpeg; the existing libraries were copied with hashes checked and the build resumed the compiled objects. Build/tests in folders exclusive to the review, without replacing the installed runtime. Native tests exercised the behavior and rejections; UI covered fonts, the WebView2 guide, 724 simulated friends, room modes, DLC, languages and coexistence with CTGP. Captures of fonts and the mobile room inspected. Real Steam was isolated by a fixture, without sending invites.

**Coexistence with CTGP:** the `git merge-tree` simulation against the commits of `ctgp-recomp` up to `6bb8a5b` predicts additive conflicts in `web/en.json`, `web/es.json` and `toolchain/host.py`. In the future merge, keep the texts of both features and the calls to `prepare_entry_trace` and `prepare_texture_readback`. This is not validation of the changes not yet committed on that branch. Additional cups, table relocation and phase B exports remain exclusively on `ctgp-recomp`; fix-chore keeps phase A.

**Limits:** no new full race, visual validation inside the game, hardware/clock test or Steam race between two machines. The fixed glasses require regenerating the derived resources; existing files are not migrated automatically. Steam DLLs without the required interfaces remain refused. `gameplay_verified=false` and multiplayer not qualified. No new setup, tag, release or merge into master. The reports below were preserved as history of the source branches; their counts and private sessions do not replace the evidence of this integration.

## PRs #18, #19 and #25 integrated into fix-chore — 2026-11-15

- **#18 (graphics, occlusion before conditional rendering)** and **#19 (audio, ramp at the start of voices)**: merges with no conflict. The runtime build (`--runtime-only`, 216 units) contains `mk8_oq_wait_pair(q);` in `exec.c` and `v->prev_volume = 0.f;` in `audio.c`. Sections for each PR further below.
- **#25 (Spanish launcher)**: the base was `master`.
  - Conflicts in `en.json` and `tests/test_launcher_languages.py`: the entries and tests from both sides were kept.
  - `es.json` received Spanish for the 120 messages of fix-chore (DLC, Settings, DLC rule in Steam rooms) and for `Volume`, which the Settings tab showed in Portuguese.
  - The DLC NSP picker uses `language::pick` in the three languages.
- **Test fixed, prior to the PRs:** `test_local_mii` had not compiled since the DLC integration (#12) because the `mk8_dlc_storage_path` stub was missing in the harness.

**Validation:**
- Rust: `cargo fmt --check`, Clippy `--workspace -D warnings` and 67 tests.
- Native/pipeline Python: 238 tests; the remaining failures are environmental:
  - Playwright modules imported by the project's Python, which does not have Playwright;
  - `test_mii_native_head`, which requires PIL, absent from both Pythons;
  - real launcher tests without `MK8_LAUNCHER_EXE`; the Cargo target is `<build-folder>/target`.
- With the system Python and `MK8_LAUNCHER_EXE`: languages, real launcher (large response), DLC, e2e, Mii GLB/launcher and process lifetime passed (35 tests, 1 skip).
- Real release launcher in Chromium, no `pageerror`:
  - Spanish through the `es-ES` locale;
  - six tabs;
  - es/en/pt-BR switching and persistence after reload;
  - 390 px with no horizontal scrolling.

**Game not tested in this integration.** The first race (Grand Prix) was interrupted by me: the test suite was competing for CPU and would invalidate the FPS. On the second attempt (12:31), `local/imports` no longer existed, nor `local/game-data`, `local/cache` and the `race-*` sessions. The Recycle Bin shows files from `local` deleted at 12:30–12:31, which indicates manual cleanup; no command in this session uses the Recycle Bin. Builds, config, profiles, `user*` and saves are intact. To test, the game must be re-imported (NSZ + keys) and the data rebuilt.

**What to measure when the game is back:**
- **#18:** how many `conditional samples` waits appear in `LARECOMP_VK_PROF` and the GP's FPS against `race-grand-prix-0so9vrpg` (60 FPS in the texture retire, deleted). The wait does `renderer_flush` when the report is in work not yet sent.
- **#19:** listening test at Electrodrome.
- `gameplay_verified=false`.

## Crash reported at the end of the third consecutive race — 2026-11-14, not reproduced

No report with a log for this case arrived (GitHub or `Downloads`). Local reproduction with the current candidate (`f079dd11…`, RX 580 8 GB, copied saves, `MK8_TRACE_VRAM=1`): `local/race-grand-prix-78bcqglg` ran the whole Mushroom Cup GP and started a fifth course in the same process. The frame 44,000 capture shows the start of Thwomp Ruins, after the end of the 3rd race, and the frame 64,000 one shows Mario Kart Stadium. The test ended with exit 0 in 1,102.7 s, with no `FATAL`, exception or `svcBreak`. VMA went from 880 to 1,904 MB reserved in heap 0, with a peak of about 1.2 GB in use. A 4 GB GPU or an iGPU can run out of memory on a track transition. Reports #11 (Intel Iris Xe, `vmaCreateImage` -1 at `exec.c:2300`) and #17 (AMD APU, -2) show exactly this kind of `FATAL`, but still in the first race. This is a hypothesis, not a demonstrated cause: the `mk8drecomp.log` of whoever saw the crash in the third race is missing.

Finding in the automation: `scripts/test-race.py grand-prix` held A without releasing it, and the current game does not leave the first race's scoreboard that way (`race-grand-prix-3jar2huo`: frames 20,000 to 52,000 on the scoreboard with 15/12 points). The "long" GPs with that script did not get past race 1. Now the GP mode releases A one frame in every 120; that input is the one in the test above, and `tests/test_race_script.py` covers the pattern. The two earlier runs of this round ended with `window closed` (`SDL_EVENT_QUIT`) because they shared the console with other commands; on a console of their own this did not occur. Do not count those runs as a crash.

Triage of player reports that prevent playing: (1) launcher frozen by the Mii creator, fixed above; (2) GPU memory exhausted on an integrated GPU (#11, #17), with a mitigation only for the AMD APU (`shared_vram.py`), not validated on real hardware; the Intel Iris Xe already did not import the heap and failed with -1 at around 186 s, after a repeated "type mismatch" on a 4096×2936 texture and uploads of ~1 GB every 2 s; (3) MKTV/Info in the menu (#8): applet 0x17 (offline web) ends without data, and the game calls `SetTerminateResult` and `svcBreak` at `70040b15d4`, still without a fix; (4) #13 (0.1.8, `0xc0000409`) came with an empty `game.log` and has no diagnosis.

## Initial ramp of the audio voices — 2026-11-13

Validation of this branch: **11 native/Python tests passed**, zero skips: seven for the mixer and four for hot paths. A game build was not redone nor was there an auditory retest in this PR separation.

Focused patch on fix-chore `15d1106`: on creation/reuse of a voice, it initializes prev_volume at 0.f instead of the current volume. The existing ramp covers the first block (240 samples/48 kHz, 5 ms); decoder, resampling, global gain and SDL buffers remain the same. Native tests use the prepared mixer/decoder with synthetic PCM/ADPCM, new/reused voices, stereo, splitter, late arrival of buffers, pitches 1/1.498, continuous loop, constant volume and mute. Earlier auditory evidence is from the integrated branch: the user confirmed the reproducer at Electrodrome fixed; it is not equivalent to qualification of this isolated branch. `gameplay_verified=false`.


## 0.2.0 crash on an AMD integrated GPU (report `mk8-report-1794582637`) — 2026-11-13

The report (launcher 0.2.0, Windows build 26300, integrated "AMD Radeon(TM) Graphics", 1920×1080, `resolution = match`) has four consecutive runs. All ended between 60 and 115 s of play with `VK_ERROR_OUT_OF_DEVICE_MEMORY` (-2): two in `textures.c:366`, after the retry in host memory and the RGBA8 reserve also failed (targets 1496×841 and 1890×1063), and two in `exec.c:2300` (`snapshot_image`). With the `FATAL`, the process exits with `0xc0000409`. The failure is GPU memory, not the launcher nor DLC.

Cause: the pinned runtime does not import the 3.25 GB guest heap when the largest heap visible to the host is **device-local**, as on Intel iGPUs. On AMD APUs on Windows, the device-local reserve is small and the shared RAM sits in a host-visible heap **without** that flag. So the import (`guest heap imported … 3100 MB` in the log) occupied the same memory into which textures and targets overflow. `toolchain/shared_vram.py` applies the same upstream limit (heap < `HEAP_MAX` + 3 GiB) to any `VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU`. In that case the already existing per-draw copies are used, and dedicated GPUs do not change. The log now lists each heap with size and flag. `tests/test_shared_vram.py` compiles the prepared decision and covers an APU with 8 and 32 GB, a dedicated GPU, a single-heap Intel iGPU and invalid indices.

**Not validated:** there is no AMD APU on this machine. The size of the user's heaps does not appear in the report, which predates this log. With about 16 GB of RAM, the shared heap may exceed the limit, keeping the import. In that case the next report should show the `[vk] memory heap` lines. The per-draw copies leave out storage buffers above 8 MB, which are then read as zero, and this may affect the image. Also missing is a comparison with 0.1.8 on the same machine: `descriptor_lifetime.py`, new in 0.2.0, keeps descriptors alive longer and has not yet been ruled out as a VRAM increase.

## Consolidation on fix-chore — 2026-11-10

Merge `d7eb2d3` brings together the local commits `ba071f9` and `2fe5497` of master with the four implementations already present in fix-chore up to `8f9b770`. Documentation conflicts resolved preserving the Mii and Nextendo references and the most recent state. Local master went back to `073d88f`, equal to origin/master; the two commits remain in the fix-chore history. No push was made in this step.

After the merge, 46 graphics regression tests, 16 Mii data/hooks tests, four voting/portraits/mode-adaptation tests and eight OAuth/Nextendo tests with a local server passed (74, no skips). The simulation exercised 20,000 profile cycles, 70,112 queries and rejection of 50 corrupted fields. `cargo fmt --all -- --check` passed. Runtime recompiled with `--runtime-only`: 213 units, link and DLL verification completed; game library preserved.

After the transfer of the build folder, launcher debug and release and relay release compiled. The 44 Rust tests passed; workspace Clippy with all targets and warnings treated as errors passed. The Mii browser test passed with the merged debug launcher: creation, persistent identity, byte-for-byte import/export, detailed editing, refusal of invalid resources and operation without WebGL. The capture `local/performance-validation/mii-editor.png` was inspected. Two language tests also passed. The browser tests used the system Python, which already has Playwright/Chromium; the private Python on this machine does not have Playwright. To repeat with the transferred folder, set `MK8_LAUNCHER_EXE=<build-folder>/target/debug/Launcher.exe` and run `python -m unittest discover -s tests -p test_mii_launcher.py -v`.

The general regression in Chromium with release launcher and relay passed all 16 tests, with no skips, including invites, bidirectional UDP, native window and configuration preservation. Total for this step: 74 native/pipeline/OAuth tests, 44 Rust and 19 browser (137). The PowerShell development scripts were exercised with the process start replaced by a path capture: both selected the executable on D:; the multiplayer script also queries the directory through Cargo. No result of these tests qualifies a public network or two machines.

At the maintainer's request, the debug launcher was opened in a visible browser using `local/mii-preview-ui`, with a copy of the active profile. The rendered preview was checked; changes in that session are kept separate from the original profile and the game's saves. The session address is in `local/mii-preview-ui/launcher.json`. This is the editor's procedural geometry, not MiiModel models loaded in MK8.

Real smoke test with isolated saves in `local/race-time-trial-8xmovdhh`: exit 0 after 79.7 s; the inspected capture `frame-03400.png` shows Mario at Mario Kart Stadium, lap 1/3 and timer at 0:10.488. It is not a full race nor a test of Mii models. The original exefs path on D: is unavailable, but the prepared copy in `local/game-data` made it possible to run the game. No real login or test between machines was done; `gameplay_verified` remains false.

The initial Rust compilation was interrupted before consuming the 4 GiB reserve. With the maintainer's authorization, the derived folder `target` was moved to `<build-folder>/target`, after checking absolute paths, processes and the absence of reparse points. The local configuration `.cargo/config.toml`, excluded from Git, points to that destination; development scripts query `cargo metadata`. Saves, profiles, game configuration and inputs were not moved. Existing packaging artifacts are also in the transferred folder; the distribution scripts still use their explicit paths in `target/` in the project.

Current priority: creation/editing and integration of the 3D Miis into native MK8, followed by multiplayer. The user does not yet have the MiiModel resources; there is no `local/mii-resources.json`. The existing experimental option still depends on the six files of title `0100000000000802`. FFL.js offers an alternative renderer with AFL resources and may help the preview, but needs its own adaptation for models, materials, expressions, portraits and fitting into the engine. It was not incorporated nor qualified as a replacement for MiiModel; references and limits in [THIRD_PARTY.md](../THIRD_PARTY.md).

## Diagnostic by simulation without the game — 2026-11-10

`local/python/Scripts/python.exe scripts/verify-simulation.py --browser` brings together the native graphics regression, the conditional hooks, Mii data and the real editor in Chromium. The new synthetic caller compiles the real `local_mii.inc` adapter with assertions active and debug symbols. It goes through 10,112 combinations of capacity/source/layout, checks memory sentinels, rejects corruption in the 50 bytes of appearance/padding and alternates identity/color over 20,000 cycles, with three queries per cycle. The phases represent query patterns; they do not run race logic, renderer, full IPC or MK8's network. There is no fake firmware nor modification of the game's acceptance criteria.

Prerequisites: Python and native compiler provisioned, pinned upstream and runtime dependencies per the preparation procedure below. `--browser` also requires a compiled launcher, Playwright and Chromium; `MK8_LAUNCHER_EXE` allows indicating the executable. Omitting `--browser` runs only the adapter tests. `--cycles 100000` increases the repetition; it accepts up to 1,000,000. Test profiles go in temporary directories, without changing personal saves/profiles. The command verifies upstream integrity and returns an error if there is a failure or ignored test. The summary is updated in `local/performance-validation/simulation.json`, including duration, result and adapter hash. The terminal output contains the tests/assertions that fail.

This diagnostic broadens coverage of data contracts, buffer limits and lifecycles. It does not measure VRAM, FPS, model fidelity, driver stability or Nextendo acceptance. `gameplay_verified`, `gpu_rendering_verified` and `nextendo_verified` remain false in the summary, even when all tests pass.

Validation run with `--browser`: 63 tests passed (46 graphics regression and 17 Mii/hooks/browser), with no skips. The simulation completed 20,000 cycles and 70,112 queries, rejecting the 50 field corruptions tested. No new failure was found in these scenarios; this does not replace qualification in a real game.

## What works today

| Area | Observed | Not yet validated |
| --- | --- | --- |
| Launcher | Native `Launcher.exe` (Tauri 2/WebView2), dark theme, sections Install/Play/Multiplayer/Support; per-user `setup.exe` with data folder choice; update through Releases with SHA-256; log report (manual and after a game failure) | End-to-end update against a published Release; XCI/XCZ import; single instance |
| Build | Three real modules generated and linked. Preparation with empty caches in 14 min 16 s on the test Xeon, after installing the tools | Repeating this measurement on other machines and with different loads |
| Runtime/game | Boot, menu and three laps at Mario Kart Stadium in the automated Grand Prix 50cc, with Mario in second on the scoreboard | Full race with correct image, return and saving after restart |
| Graphics | Textures recovered in controlled tests; adaptation of TLD4S F32; brightness flicker between frames (environment cubemap on a reused ID) and blown-out exposure (BC3 reflections generated by shader, `bc_render.py`) fixed in the captures | Intermittent halo on the sign, colored dots, shadows and image fidelity |
| Performance | Current candidate went through four tracks and returned to cup selection; 59.97–59.99 FPS in the stretches measured, reserved peak of 1,648 MiB in VMA heap 0; direct image release off | Memory stabilization, 60 FPS and correct speed in all conditions |
| Mii | Persistent name/appearance, multiple IDs, 3D preview and GLB per profile | Head inside MK8, portraits, body and animations |
| Multiplayer | 12 transport clients; two real instances through the local relay code, three laps and return to track selection | Race between two machines, 12 real games, external NAT/relay, MTU and congestion |

Current local configuration: `local/config.json` (entries extracted from the 4.0.1 update outside the repository). MK8 4.0.1 uses `main`, `sdk`, `subsdk0`. The adapter supports up to subsdk4 in fixtures, but those modules do not exist in this ExeFS. The 1.0.0 base is AArch32 and must not feed this translator.

## Contracts that must keep working

- `local/candidate.json`: experimental manifest, hashes of executable/DLLs/entries; `gameplay_verified=false`. Qualified installation is separate. IDs `mk8d-smm2-*` and existing upstream directories were preserved for compatibility with data already written.
- Profiles: active `local/mii-profile.json`; archived `local/profiles/<id>/profile.json` and `head.glb`. Saving appearance preserves the ID; creating generates another. The in-game applet edits the name and preserves appearance. The save in `local/user-no-mii` is still shared, not separated by profile.
- Own head: Y up, front +Z, per-vertex color, a single GLB node `head`, static. Scale/fit not yet calibrated. RomFS has bodies in `Driver/MiiM.szs`, `MiiF.szs`, animations in `DriverAnim` and `UI3D/MiiBodyMiddle/MiiBodyMiddle.bfres` (FRES 0.5.0.3). A neck matrix adapter, buffers, materials and portraits are missing; do not replace the system's Shape/Texture with fake files.
- Textures: commands, pools and render targets retain references; guest finalization does not release an object that is still recorded. Retirement of lists and full release of the pools still need prolonged memory testing.
- Video: three-argument Decoder constructor and file source adapted. Never call guest events while holding the decoder's native mutex; yielding the core in that state caused a deadlock. Releasing the output signals the input after freeing the slot.
- Without Mii models: hooks by hash omit preparation/wait/request of portraits. CharInfo is still loaded. A missing portrait must not become a valid model pointer.
- Network: session in the launcher, up to 12 protocol slots, individual/expiring invite, XChaCha20-Poly1305, 64-position replay window. `MK82` external; `MMKJ`/`MMKDVL1` only on the loopback bridge, room `MK8ONLINE`. New invite on reconnection. The 1,489-byte datagram limit requires MTU qualification. Closing the launcher ends the relay.
- Old shader/SDK/CMake/overlay settings are accepted for reading, but have no functional control in the current launcher and were removed from the interface.

## Verification and priorities

After the folder reorganization: 38 native/pipeline tests, 12 Rust tests and 10 browser tests passed (nine in the suite and one additional configuration-preservation test). `cargo fmt`, Clippy with warnings treated as errors, the launcher release build and the runtime build with 207 units completed. The game library was reused. These results verify the reorganization; they neither solve nor qualify the graphics defects.

Native launcher (2026-11-06): 16 Rust tests (4 new ones run the sequence of tools steps with real processes: order, stop on failure, missing program and reading of the pins) and 12 browser tests, including the installation screen with no false success and the native window opened via WebView2 (title, installation screen, Browse buttons and blocking of external navigation). `cargo fmt` and Clippy with no warnings. The tools step was run end to end through the interface with the real scripts (everything already present; nothing changed), and the folder picker was opened and closed in the window with the API responding. No game was started in that verification.

Existing coverage: geometry/GLB and persistence, profiles/manifests, transport, API/browser, ABI layouts, hooks by hash, video events, texture lifetime/update and TLD4S→GLSL→SPIR-V translation. The SDK's private test accepts 62,208 editor combinations and rejects an invalid field; this validates CharInfo, not the model in the game.

Prioritized pending items for publication: [docs/ROADMAP.md](../ROADMAP.md). The user's current priority: create and edit Miis and integrate their 3D models into native and online MK8 on the fix-chore branch. The VRAM investigation remains pending. Track active and reserved memory, frame time and image integrity in prolonged sessions. Multiplayer remains pending on two machines and over the internet with a public relay, including more than four real games. Lighting/frametime/clock, full race qualification and integration of Mii models remain pending. Do not broaden versions, DLC, platforms or frequencies before qualifying this path. Update this file with new evidence; avoid keeping a duplicate diary of each boot.
