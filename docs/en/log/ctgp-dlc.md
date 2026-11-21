# Technical log — CTGP and DLC

[Index](README.md) · [Português](../../pt-BR/log/ctgp-dlc.md)

## Crash opening the main menu with CTGP and native settings in 0.4.0

The report from 19 November records a build completed in 65.1 s and the Vulkan pre-test completed. With CTGP Deluxe, Booster Course Pass, Mii resources and the native menu active, creation of the main menu ended in a null access in the guest function `f_7000732a30` (exception `0xc0000005`, process exit `0xc0000409`). The published runtime reproduced the same failure at the same point with a copy of the saves. In the controls, both CTGP with the native menu turned off and the native menu without CTGP got past the opening.

The 0.4.0 preparation added the settings components to the shared layout `cm_Page_Dummy`. The fix on `fix-chore` keeps that original resource and derives a dedicated page for the selector. With the same executables and the regenerated layouts, with CTGP and the DLC active, the key button and F1 opened the selector in two isolated sessions. It was possible to close it, save and return to the menu; both ended with code 0. A separate attempt stopped at startup with the SDL notification `0x406d1388`, before loading the menu; the F1 retest completed. This investigation covers opening the menu.

With the base game and the corrected layouts, F1 also opened and closed the menu with exit 0. 24 settings tests and 14 CTGP tests passed, including the cup icon texture test run with the local MK8 4.0.1 file.

In the original 0.4.0 installer, **Settings → uncheck “Enable the native settings menu”** allows keeping CTGP on. The fix depends on regenerating the private layouts with the updated tool.

## DLC import from a combined NSP — #36

Four browser tests of the DLC module also passed with the existing debug launcher and the development Python (the first attempt on the bundled Python did not start because Playwright was missing). They verify interface controls/states/API; they do not exercise decryption of a commercial package.

The importer refused a package with the verified NCA pair whenever any additional NCA existed. It now requires the presence of the two pinned names and extracts only those files, checking the same sizes and hashes. It selects only the ticket with the DLC title prefix; zero or multiple matching tickets are still an error. Other contents/tickets stay in the package, without extraction. LibHac validation and the RomFS hashes remain mandatory. It does not enable unknown versions, compressed DLC or an integrity bypass.

A regression with a self-authored combined NSP failed on the previous code and passed after the change. Eleven DLC tests passed, including a standalone package, missing/tampered content, missing/ambiguous ticket, preservation of configuration/backup and six native service cases. The import test uses a simulated extractor and self-authored files; it does not validate a commercial combined package. No private file was imported/reinstalled. #36 still lacks the reporter's package metadata, so this compatibility fix does not close the report. No runtime/Rust change or new setup.

## CTGP performance and local selection — 2026-11-16

Investigation of the private report `mk8-report-1794858711.zip`, with reproduction on Luigi Circuit (CTGP29) and Koopa Troopa Beach (CTGP21). The report records 18–40 FPS and pauses of up to 1.76 s on entering CTGP29, together with hundreds of shader compilations, followed by stretches at 60 FPS. It does not by itself demonstrate a VRAM leak or identify a new crash. Tests use copies of saves/settings/cache, a single instance and the already compiled guest; no commercial data is published.

**Shader wait:** `toolchain/shader_budget.py` replaces the Winpthreads timed wait with the high-resolution monotonic timer already available on the platform. On the native control, the previous code takes about 16 ms for a wait limited to 4 ms and fails the new latency test. The fix stays near 4 ms. The budget is still shared per frame; compute, probes, lighting and unknown targets keep the full wait. There is no indiscriminate removal of draws and no ABI/guest change.

**Manual session and split screen:** private session of roughly 13 minutes on GPDX Kingdom Way (CTGP53), Alpine Skyway (CTGP54) and Dry Dry Falls (CTGP45, two players), with an RX 580 2048SP, 1080p fullscreen, SMAA and frame generation off. The user confirmed stutters on the first pass, none when revisiting the same stretches, and no crash; the process ended with code 0. The log shows pauses of up to 2.43 s associated with full compilations. The VMA reservation in the main heap reached 1648 MiB and then dropped to 1136 MiB; allocation reached 852 MiB and ended near 438 MiB. This does not demonstrate a continuous leak nor measure all of the driver's VRAM; the HUD in MB shows process memory.

The optional diagnostic `MK8_TRACE_SHADER_TARGETS=1` identified waits of 60–311 ms in the split scene: 8:9 color/depth targets, from 720×810 to 960×1080, do not receive the `scalable` mark reserved for 16:9. The budget now recognizes this specific MK8 4.0.1 pair, with validation of dimensions, rounded aspect ratio, formats, flags, samples and backends. Auxiliary targets, cubes, probes, unknown combinations and compute remain synchronous; the resolution does not change. A native test runs the real queue and verifies both the bounded return and the full wait for excluded targets. Three/four players and other aspect ratios were not qualified by this extension.

Automated comparison on Dry Dry Falls, two players, 1080p/SMAA, 9000 frames per run, application cache initially empty in both: on frames 3004–8990, the average went from **54.96 to 59.21 FPS**, the worst frame from **518.81 to 60.54 ms** and frames above 50 ms from **35 to 2**. On the 6100–8990 stretch, with continuous acceleration, it was 54.29 → 59.93 FPS and 19 → 2 frames above 50 ms. Captures and their three neighboring frames excluded. Positions vary with the simulation and the driver cache was not isolated: this is not a guarantee of 60 FPS on every track. There was no process failure; captures 5000/7000 confirm track, karts and lighting visible. Mandatory preparation pauses before that stretch persist. Private evidence: `race-grand-prix-vuxzye_7` (before) and `race-grand-prix-chrwe8_a` (fixed). An independent attempt to normalize pipeline keys was discarded for showing no gain (59.74 versus 59.73 FPS solo); it is not part of the runtime.

**Local selection:** there was success with only one player when the game requested two. The guest reopened ShowControllerSupport every few frames, even though the renderer kept running at 60 FPS. It now returns `ControllerSupportCanceled` and preserves the focus notification. In the real reproduction, an insufficient request returned to the engine class, with a single call to the applet. Two players with automated inputs chose Luigi/Yoshi and advanced to the karts, without the cycle. For keyboard P1 and a P2 controller, use the existing option **Controllers start at player 2**; the user's personal preference was not changed.

**Measurements and limits:** Luigi Circuit held an average of 60.00 FPS on frames 3600–5990 in both runtimes. That stretch includes time off the track, so it does not represent a full lap. On Koopa Troopa Beach, frames 2400–4490, including loading: original 58.99 FPS/peak 361.73 ms; fixed with empty application cache 53.76 FPS/peak 2358.66 ms; fixed repeat reusing its cache 59.40 FPS/peak 262.12 ms. The driver cache was not isolated and the translation cache is invalidated when the runtime changes: these numbers do not demonstrate a general FPS gain. The repeat confirms the weight of the first compilation, but mandatory pauses persist. Nearby captures are excluded by `scripts/frame-stats.py`; images of both tracks and of the selection/karts were inspected. Local evidence in the `race-time-trial-*` directories, recipes in `local/ctgp-perf-check`.

The manual session above added several consecutive tracks, memory and a normal shutdown. Full laps with image, audio, controller, clock speed and restart/save, as well as the other reported crashes, still need to be qualified. No crash was reproduced in these trials; no multiplayer qualification between PCs. `gameplay_verified=false`. 120 frame generation already exists, keeping the simulation at 60; native FPS above 60 requires a separate investigation of the clocks and guest logic, not just removing the limiter.

**Regressions:** a new round of 25 tests in the area passed (applet, shader budget including the split scene and excluded targets, stalls/cache/lifetime, shader queues/tables, prefetch and precompiled). The negative control with the previous implementation failed as expected: median 15.94 ms/p95 16.05 ms for 4 ms requested; fixed median 4.23 ms/p95 4.68 ms. In the new round, median 4.11 ms/p95 4.43 ms. The test uses the real Windows timing functions. The queue harnesses had to declare the new platform dependency; their doubles abort if the timing, outside the scope of those cases, is called. The Zstandard dependency missing from the global Python was resolved by using the private environment already installed. Host runtime compiled and linked with 220 units; guest preserved.

**Setup for testing:** `MK8-online-0.2.0-setup.exe`, 59,518,464 bytes, SHA-256 `d67bae4173969a711f458f3d35835d8483411b2666df971012102cbfbbc47a77`. Release launcher recompiled and host embedded with key `b70c0d85c11163cde636056ae750c10d2e612d00c3d36bae7189c216a36d27c7`. Audit of the 966 files passed: current sources, manifests/hashes checked, no game data/keys/saves/personal paths. Isolated install, open and uninstall passed, keeping the test data and restoring the registry/shortcuts of the personal installation. No visual change in this round; the full UI tests of the redesign remain those recorded below. Reinstall manually and update the game build to apply to the existing installation; the version remains `0.2.0`. No commit, tag or publication.

## CTGP Deluxe as an optional track pack (Phase A) — 2026-11-15

**What it is:** the 64 tracks of CTGP Deluxe 1.1.1 in the existing 4.0.1 track slots, without the mod's code (the plugin is 32-bit ARM, for 3.0.3). Optional and reversible: the launcher imports, enables and disables it. Base game, DLC and saves are not changed, and all four combinations work (base; base + DLC; base + CTGP; base + DLC + CTGP).

**Parts:**
- `toolchain/ctgp_pack.py` / `import_ctgp.py` build `<data>/ctgp/pack`:
  - from the folder extracted from the official ZIP, with hardlinks on the same disk (4.6 s, no copy);
  - CTGP stays only in the player's data folder; the repository keeps only numbers (CT → name label, read statically from the 1.1.1 plugin, `PLUGIN_111`) and file names.
- **Slots:**
  - CTGP cups 1–12 in the 12 cups of the base game (48 tracks);
  - cups 13–16 in the first 4 cups of the Booster Course Pass (`Cnsw_11`–`28`), visible only with the DLC active, since the BCP tracks are in the base RomFS and the DLC only unlocks them.
- **What goes into each slot:**
  - the track folder (without CTGP's own sound bank; the slot's one stays);
  - the N/F music and variants (LAP2, REPLAY);
  - the selection image, the cup icons;
  - the cup/track names in the 4.0.1 `message.sarc` for 15 languages, with MSBT tags preserved and the platform prefixes (Wii, GBA…) empty;
  - the title/menu music, the menu sounds and the Lakitu.
  - CTGP's `common.sarc`/`menu.sarc` crash the 4.0.1 menu and are never used.
- **Runtime:** `runtime/mk8/support/romfs_overlay.c` implements the weak hook `game_romfs_overlay()` with `MK8_ROMFS_OVERLAY`. It is a second overlay after `RECOMP_MOD_DIR`, which proceeds to the Mii head. An invalid folder ends the game instead of silently opening the base game.
- **Launcher:**
  - card **Settings → CTGP Deluxe** with import, enable/disable, state and log, in pt/en/es;
  - `crates/installer/src/ctgp.rs` verifies the pack before each launch (size of each file, nothing extra);
  - with CTGP enabled the game uses `save-ctgp`, created once as a copy of `save`;
  - a build without `pack_supported` refuses the enabled pack.
- **Steam:** the lobby publishes `mk8_ctgp` (pack identity or `0`) and whoever joins needs the same state (`check_pack`).

**Problem found and fixed in the runtime:**
- **Symptom:** VRAM at 7.3 GB and game RAM at 7.8 GB during the GP's initial flyover. One race ended in `VK_ERROR_OUT_OF_DEVICE_MEMORY` (`race-grand-prix-oz51459d`).
- **Bisection** (GP, first race, up to the flyover):
  - it is not the Lakitu, the music or a model file;
  - it is the object list `course_muunt.byaml` of `CT_00`, and in it the 7 `FireworksFc` (the original Stadium has 2);
  - without the fireworks the peak drops to 1,648 MB, the same as the base game.
- **Mechanism:** the fireworks weigh on the GPU, the game's dynamic resolution rebuilds the targets every few frames at new sizes (1499x843, 1546x870, 720x405…), and the runtime created one image per size. `texture_retire` only freed replaced targets after 60 s unused: ~700 images/s accumulated.
- **Fix** (`toolchain/texture_retire.py`): a replaced target is released after `MK8_TARGET_GRACE_FRAMES` (30, half a second), with the sweep on the same cadence. Its content no longer exists even on the console. Textures with content keep the 60 s.

| GP, 1st race (frames 3600–14000) | Average FPS | p99 | worst frame | VRAM reserved / used (peak) |
|---|---|---|---|---|
| base game (`race-grand-prix-x2kyzd8r`) | 59.89 | 17.7 ms | 72 ms | 1,648 / 1,459 MB |
| CTGP before (`-70aahhh4`) | 56.22 | 35.1 ms | 138 ms | 7,373 / 7,314 MB |
| CTGP, 2 s grace (`-7dpwm7t1`) | 58.24 | 21.1 ms | 64 ms | 5,616 / 2,107 MB |
| CTGP, 0.5 s grace (`-cgxrwi9u`) | 57.84 | 22.6 ms | 80 ms | 2,928 / 1,732 MB |

In that race 6,043 images (17 GB) were created and discarded: the dynamic-resolution churn continues, it just no longer accumulates. Reusing images across sizes would be the next improvement.

**Full Grand Prix with CTGP** (`race-grand-prix-tovnrm2s`, 64,000 frames, 18 min, cold cache):
- the four CTGP tracks in place of the Mushroom cup:
  - Omatsuri Circuit;
  - Vanilla Lake 1;
  - Broken Pier;
  - Mystic Tangle.
- 12 drivers, CPUs on the racing line, own items and minimap; exit 0, no `FATAL`;
- average FPS per race: 58.2 / 59.2 / 59.8 / 59.8; p99 20.7 / 19.8 / 17.6 / 17.6 ms;
- isolated peaks of 230–600 ms on track changes: first-use compilation with a cold cache, as in the base game;
- VRAM at peak: 3,696 MB reserved, 2,361 MB used.

**Base game with the same fix** (`race-grand-prix-bm9gt9k4`, 1st race): 59.91 FPS and p99 17.6 ms, as before. The reserved VRAM at peak dropped from 1,648 to 1,136 MB.

**Validation:**
- Rust: fmt, Clippy and tests, including 3 in `ctgp.rs` and the Steam rule.
- Python: 6 in `test_ctgp_pack.py`, with synthetic data only — SARC/MSBT round trip, slot mapping, names, assembly/verification/import and a native test of `romfs_overlay.c`.
- `test_texture_retire` with the new cases.
- Browser: `test_launcher_ctgp` (states, import, enable/disable, block with the game open, en/es, 390 px), DLC and languages: 10 tests.

**Not yet tested:**
- the CTGP cups in the BCP slots (no DLC imported here yet);
- tracks other than the 4 of the first cup;
- multiplayer with the pack on two machines;
- the time-trial staff ghosts and the prize screen background, which remain those of the original track;
- the 5 names the plugin reading did not resolve (`CT_08`, `0A`, `0D`, `37`, `3B`), which appear as "CTGP NN".

`gameplay_verified=false`.

### Imported DLC and Phase B — 2026-11-15

**Booster Course Pass imported:**
- source: `Downloads/…[0100152000023001][v65536].nsp`, 121 KB, with the `prod.keys` from `config.json`, via `toolchain/import_dlc.py`;
- verified and attached to the candidate by the `--runtime-only` build.
- **Pack in the base layout:** the DLC's first cup showed "Moo Moo Mountain" (`CT_30`), ran at 60 FPS and brought the CTGP Lakitu (`race-time-trial-ujfapcmy`).
- **Cup screen with DLC + CTGP:** "Boo Cup" in place of the Mushroom cup, with names and images of the 4 CTGP tracks (`race-time-trial-f6_8i48_`).
- **The cup icons on that screen are still the original ones.** On the selection screen they come from inside the 4.0.1 `menu.sarc`; the loose `UI/cmn/race/ym_CupIcon*` files that the pack replaces are the in-race ones.

**Second layout: in place of the DLC cups** (`--layout dlc`, selector on the launcher card):
- the 12 Booster Course Pass cups receive CTGP cups 1–12;
- the base game stays 100% original, and with the DLC active there are the 48 original tracks and 48 CTGP tracks at the same time;
- CTGP cups 13–16 are left out in this layout; switching the layout re-imports in seconds (hardlinks).
- **Tested:**
  - base game page: Mushroom cup and original tracks, and the race loaded the original `Gu_FirstCircuit` (`race-time-trial-m8_mnqio`);
  - DLC page (R): "Boo Cup" with the 4 CTGP tracks, and the race loaded `Cnsw_11` with the CTGP files (`race-time-trial-7b9v6o3a`).
- **Visual limit of this layout:** the CTGP names inherit the slot's platform label ("Tour", "3DS", "N64", "Wii"). These labels (1461–1468) are the same as the original tracks of the base game, which in this layout remain in effect.

The real order of the DLC cups (image `DLCnn` ↔ folder `Cnsw_xy`) does not follow the folder number (Rock Cup = `Cnsw_31`, `33`, `34`, `62`). It comes from the game's own table, repeated by the plugin, and was checked against the 4.0.1 `117xx` names (11717 London Loop … 11720 Maple Treeway). The base layout, which uses only the first 4 DLC cups, does not change: there the order coincides.

**Phase B, the no-code part — completed:**
- **Cup icons on the selection screen:**
  - **Where they are:** in 4.0.1 they are `ym_CupIcon<Name>^h` textures of 200×200 in tiled RGB565. They sit in a BNTX inside `menu.sarc` → `mn_L_CupSlctBtn_00.szs` (Yaz0, a SARC of hash-only files).
  - **Conversion:** the pack decodes the colored CTGP icons (`ym_CupIcon*_130x130^h`, 210×210; 15 in BC3 and the Blooper in BC7), downsamples by area average and writes them over the slot icons in a copy of the 4.0.1 `menu.sarc`. The rest of the file does not change and the Yaz0 is written uncompressed (valid).
  - **BC7 decoder:** modes 1 and 3–7; the 3-subset modes (0 and 2) are not used by the icons and are refused. An unreadable icon keeps the original and the import warns.
  - `*_Nml^l` are normal maps, not the icons.
  - **On screen:** base game cups with the 12 CTGP icons and the first 4 of the DLC with Life Mushroom, Ice Flower, Prankster Comet and Coin (`race-time-trial-x0ys7od8`).
- **Third layout `dlc-late`:** CTGP cups 5–16 in the 12 DLC cups. With `dlc` and `dlc-late`, all 64 CTGP tracks are reachable alongside the originals.
- **Tests:** 10 in `test_ctgp_pack`:
  - converted icon checked texel by texel;
  - BC3 and BC7 blocks (modes 6 and 1, refusal of mode 0);
  - `dlc-late` layout;
  - SARC of nameless files and Yaz0 round trip.

**Phase B, the code part — not started, by scope decision:** cups beyond 24, random button and CTGP's own menus. The hook structure comes in together with the first real limit changed, in the pattern already used (`yaz0_native.py`: generated-unit patch validated by hash), and not as unused infrastructure.

**Phase B, step 1 — tracing with the game running (branch `ctgp-phase-b`, 2026-11-16, read-only):**
- **Tools:**
  - `scripts/fnprof-diff.py` compares two `LARECOMP_FNPROF` profiles and points to the generated unit of each function;
  - `MK8_TRACE_ENTRY=<address>[:x<reg>:<bytes>]` (`runtime/mk8/support/entry_trace.c`, hooked into `fnprof_hit` by `toolchain/entry_trace.py`) records x0–x3, the caller and bytes pointed to by a register on entry to chosen functions. It changes no registers, memory or flow; without the variable the hook is off. Native test: `tests/test_entry_trace.py`.
- **Cup screen profiles** (base game + DLC, no CTGP, frames 1760–1880): idle 2,805 functions; moving the cursor across 6 cups 3,233 (428 new); switching page (R).
  - 15 functions run exactly 24 times (4 tracks × 6 moves). Among them `f_700043c2e8` (`main_089.c`), which receives the track record in x1 and uses field `+8` plus 100 as the message (labels 14xx → 15xx).
- **Observed cup record** (`MK8_TRACE_ENTRY=700043c2e8:x1:64`): per cup, 4 records of 16 bytes `{track ID u32, -1, name label u32, 0}`, with the cups 0x80 bytes apart in a heap object.
  - Mushroom = IDs 27, 28, 19, 17 (labels 1401–1404); Flower = 16, 18, 20, 21; Star = 26, 29, 25, 24.
  - The internal track IDs do not follow the cup order.
- **Constructor:** `f_70003f96b0` (`main_081.c`) is the only function with all 96 track labels.
  - It writes the cups as `object + cup × 0x80`, with the index limited to 25 (`< 0x19`) and the count at `+0x1430`.
  - It also writes a track → (cup, position) map of 8 bytes per ID, limited to 123 (`< 0x7b`).
- **Size of the work for extra cups (count in the generated code):**
  - access to the cup vector with the limit of 25 appears at 131 points in 10 functions;
  - the limit of 123 track IDs appears at 129 points in 17 functions (among them `f_700050ccc0`, the icon one);
  - the ID → folder path (`f_700089ed04`, through virtual objects) and the saves are still missing.
  - 16 CTGP cups would require 40 cups (the object has room for 25) and track IDs ≥ 123: the object would have to be reallocated or enlarged and all these points fixed.
- **Next step (2):** locate the allocation of the cup object (fixed size), the ID → folder map and the save vectors indexed by track or cup ID. Only then decide between enlarging the existing object or creating a second set of cups only for CTGP, with records in a separate file.

**Phase B, step 2 — track IDs (2026-11-16, read-only):**
- **The track list is a sead enum:** a string of 124 comma-separated names at `main+0xed765d`: `Invalid, Gu_Menu, Test, Reserved01 … Reserved14, Gu_MarioCircuit … Cnsw_68`.
  - The track ID is the position minus 1. This matches the trace: Gu_MarioCircuit = 16, Gu_FirstCircuit = 27, Gu_WaterPark = 28.
  - The last ID is Cnsw_68 = 122, hence the limit of 123 (`0x7b`).
  - The folder name is the name itself in the enum (`Course/%s/%s`).
  - `f_700085bcd8` (`main_181.c`) and `f_7000a5f6d4` (`main_228.c`) use this string.
- **Free IDs:** Nintendo left 14 reserved (`Reserved01`–`Reserved14`, IDs 2–15) and `Test` (ID 1). A track with those IDs would be read from `Course/Reserved01/…`, a folder the overlay can create.
- **Exact format of the cup object** (constructor `f_70003f96b0`, 1,700 lines, a single `return`):
  - cup = 0x80 bytes at `object + index × 0x80`, with up to 8 records of 16 bytes `{ID u32, -1, label u32, flag u8}` (unused slots are -1);
  - reverse map at `object + 0xC80`: 123 × `{cup u32, position u32}`;
  - count at `+0x1430`: zeroed and incremented 24 times (12 base cups + 12 DLC), ending at 24. The 25th entry (index 24 = the count) is the battle cup, with the 8 arenas up to `Bu_DekaLine` (ID 70), written without incrementing;
  - second map at `object + 0x1058` (123 × 8 bytes): the reverse map of the arenas. The object's constructor (`f_70003f952c`) initializes both maps with -1;
  - none of the functions that read cups reads the count: they use fixed positions.
- **There is no free slot:** the 25 entries are occupied and end exactly at `+0xC80`, where the reverse map begins. A 25th race cup, even as a proof of concept, requires reallocating the cup vector to a larger area and fixing every access:
  - the limit of 25 appears at 163 points in 20 functions: 107 writes by the constructor itself and 56 reads in 19 functions (full count from step 1 below);
  - the reverse map base (`+0xC80`) and the count (`+0x1430`);
  - the cup screen (third page);
  - the reserved IDs 2–15 do **not** work for new tracks (see "Reserved IDs" below).
- **Step 1 done (branch `ctgp-recomp`): cups in an area with room for 41.**
  - `toolchain/cup_relocation.py` changes the build copy of 11 generated units, each checked by SHA-256. `main_082.c` and `main_115.c` are accepted as they are after the Mii dialog and LAN voting hooks (`toolchain/experimental.py`), which remain applied.
  - The constructor stays the same and, at its single `return`, hands the object to `mk8_cups_built` (`runtime/mk8/support/cups.c`), which copies the 25 entries to the new area; the 16 extra entries start empty (records -1).
  - Reads now accept up to 41 (`0x29`) and compute the entry through `mk8_cup_entry`: the new area for the constructor's object, the original calculation (with limit 25) for any other.
  - Each limit is followed through the index register to the calculation `base + (index << 7)`: labels are ignored, `goto` within the function is followed and calls are accepted only with the index in a preserved register (`x19`–`x28`). There are 58 limits and 56 calculations in 19 functions (two limits of `f_700048e190` reach the same calculation); each function is checked against these two numbers and a limit that does not reach the calculation refuses the build.
  - The first version (commit `65d0bc4`) only caught limits with the calculation right after: 42 calculations in 9 functions. The other 14 (for example `f_70004a3940`, which copies the chosen record to the selection) kept reading the original object; an added cup would be shown on screen and ignored when choosing the track.
  - The reverse map and the count remain in the original object. `MK8_CUPS_RELOCATE=0` turns everything off.
  - Tests: `tests/test_cup_relocation.py` (5: synthetic change, including labels, `goto` and calls; the 11 units verified as the build receives them; refusal of a different count; native harness for the copy/empty entries/disabling).
  - In the game (2026-11-15, with DLC): time trial with the change on and off shows the same cup screen; the track panel tracer (`MK8_TRACE_ENTRY=700043c2e8:x1:64`) read the 24 records of the new area with the change on and the 24 of the original object with it off. Full Grand Prix at 60 FPS, no errors in the log. Repeated with the 56 calculations: same screen, time trial at 60 FPS, Grand Prix up to lap 3/3 at 59–60 FPS, no exceptions.
  - It does not show a new cup yet: it is only the base.
- **Reserved IDs do not work for new tracks** (experiment of 2026-11-15, local only):
  - The game converts the ID into a position in the per-track tables (image, music, records, ghosts…): `ID − 16` for `ID < 75` and `ID − 27` above. For `Reserved01` (ID 2) the position is -14.
  - Experiment: a 26th cup with IDs 2–5 and folders `Course/Reserved01–04` (copies of a game track, with the `.barslist` rewritten), shown in place of the Mushroom Cup. With the 42 calculations, the screen showed the chosen names and the images of the DLC01–04 tracks, and the race loaded the original track. With the 56, the game stopped on the cup screen reading `0x18` (`f_70004a5c48`, called by `f_70004a3940`), with `x9 = -14`.
  - Using them would require changing this conversion at the 129 points limited to 123 and enlarging all the per-track tables.
  - Also recorded: the menu image is `UI/cmn/menu/ym_CoursePict_<suffix>^u.bntx`, with a fixed list of suffixes (48 base tracks, 8 arenas, DLC01–48, Random); the trophy background is `UI/cmn/ending/ym_AwardBG_<name>_00^o.bntx`; the music comes from `pBGM_…` names in the binary, chosen by ID.
- **Chosen path (2026-11-15): CTGP cups reuse the IDs of the original tracks and divert the files.**
  - The RomFS index is built once at startup (`runtime/services/fs.c`, `upstream`); there is no per-file-open hook. So the diversion is done in the game, by swapping the folder **name**.
  - `f_700089ec04` (`Course/%s/%s`) and `f_700089ed04` (`Course/%s`), in `main_186.c`, receive the race configuration object: track ID at `+0x1a0`, and the name comes from `f_700001b4ac(ID + 1)` (the enum). For ID 1 (`Test`) the game uses its own name stored at `+0x1a4`.
  - Trace (2026-11-15, `MK8_TRACE_ENTRY=700089ec04:x2+180:64`): `+0x1a0` = `0x1b` coming from the Mushroom Cup and `0x10` coming from the Flower Cup; nothing nearby indicates the cup. The chosen cup will be annotated by the runtime in the readers that copy the chosen record to the selection (`+0x2084` cup, `+0x2088` track: `f_70004980a0`, `f_7000498e90`, `f_70004a3940`).
  - Plan: (1) those readers report the chosen cup; (2) after choosing the name, the two path builders ask the runtime, which returns `CTGPnn` when the cup is an added one and the track belongs to it; (3) the import builds `Course/CTGPnn` with the files of the CT track (`Course_CTGPnn.bars`/`.barslist` renamed, format already checked byte by byte); (4) afterwards, names (new labels), panel image and music.
  - Known limits: records and ghosts of a CT track stay together with those of the original track whose ID it uses (in the separate CTGP save); a random race right after a CTGP cup needs to clear the annotation.
  - The tracer accepts an offset on the register (`x2+180:64`), with a test.
- **Added CTGP cup working (2026-11-15, branch `ctgp-recomp`, local test):**
  - `MK8_CUPS_EXTRA=<ID>:<label>:<folder>,…` (4 per cup, `;` between cups): only accepts IDs that are already in a race cup (the game's tables exist for them); the reverse map and the count stay as they are. Invalid: the game stops with the reason.
  - The readers that copy the chosen track (`f_70004980a0`, `f_7000498e90`, `f_70004a3940`, to the selection at `+0x2088`) and the Grand Prix one (`f_70004fb290`, which copies the cup's 4 tracks to the GP list) call `mk8_cup_select`, which annotates the cup. The two path builders (`main_186.c`, checked by SHA-256) ask `mk8_course_folder` for the name, which returns the track's folder when the annotated cup is an added one and has it.
  - `MK8_CUPS_PREVIEW=<cup>` (diagnostic) shows the first added cup in place of that one.
  - Folders: `Course/CTGPnn` = files of the original track of the borrowed ID with those of the CT track on top (the new folder has no fallback to the original). The game reads the sound bank under both names (`Course_CTGPnn.bars`/`.barslist` and `Course_<original>.bars`/`.barslist`), so both stay in the folder.
  - Test with the first 4 tracks of the pack (IDs 27, 28, 19, 17 → `CTGP01–04`) in place of the Mushroom Cup: time trial loaded `Course/CTGP01` at 60 FPS; Grand Prix with `Course/CTGP01` and then `Course/CTGP02` (race 2), 8 racers, 57–60 FPS, no exceptions. The original tracks remain in their cups.
- **Third page on the cup screen (2026-11-15, branch `ctgp-recomp`):**
  - The game has two fixed pages: `f_7000494b44` builds the 12 buttons with cups 0–11 and `f_7000495564` with 12–23 (each button at `screen + 0x4c0 + i×8`, its cup at `+0x3d0`); navigation (`f_70004966c8`, a state machine) toggles the page byte `screen + 0x5a3` between 0 and 1 with L/R (bits 11/12 of the input) or by passing the edge.
  - With added cups, the runtime keeps the page shown (base, DLC and one per 12 added cups): `mk8_cup_page_turn` annotates the direction right after each change of the byte, and the two builders ask `mk8_cup_page_id` for the cup of each button. The game's byte keeps toggling 0/1; only the list of cups changes. On opening the screen, the page is that of the chosen cup.
  - The per-cup DLC check (`f_7000863428`, `main_182.c`), which the state query (`f_7000494e44`) makes for cups from the 13th onward, is answered by the runtime for the added ones: 0 (no restriction, like a purchased DLC cup) with tracks, 1 empty. Trace: with 3 the cup appeared with `???` and could not be chosen.
  - `f_70004a3940` chooses the track by ID and finds the cup in the reverse map (always a game cup): it now calls `mk8_cup_by_course`, which keeps the added cup when both have a track in common (before, it switched the cup to 0 right after the choice and the race loaded the original track).
  - In the game, with no diagnostic mode: R, R goes from base to DLC and to the CTGP page; L from the base goes straight to it; choosing the cup and racing loaded `Course/CTGP01` at 60 FPS. Without added cups the screen is the same.
  - Each page change has a stop of ~380 ms (49 FPS over the 2 s interval) also without added cups (same input, 2026-11-15): it already exists in the game, it does not come from this change.
  - Missing on this screen: icons for the added cups (the 24-icon table gives the last one, Spiny, for all of them).
- **"In new cups" import (2026-11-16, branch `ctgp-recomp`):**
  - `ctgp_pack` layout `added` (`import_ctgp.py --layout added`, option in the launcher): each CT track `i` gets `Course/CTGPnn` (files of the slot track that the `base` layout gives it, with the CT ones on top; the borrowed track's sound bank also as `Course_CTGPnn.bars`, and its `.barslist` renamed — rebuilt identically to the game's on the 108 tracks), `CTGPnn_N/F.bfstm`, `ym_CoursePict_CTGPnn_00^u.bntx`, names under new labels (`30000 + cup`, `31000 + i`, `31100 + i` at the opening) in all languages, icons (`cup_select_add_icons`) and the `MK8_CUPS_EXTRA` in the manifest (`cups_extra`). With hardlinks: 1,309 files in seconds. The IDs come from the track enum (`COURSE_IDS`); the runtime only accepts IDs of race cups (the Animal Crossing slot is `Du_Animal_Spring`, ID 64, accepted).
  - Launcher: `cups_supported` in the build manifest (only the full build sets it; `--runtime-only` keeps that of the game library); `ctgp::launch_cups` gives the game the `MK8_CUPS_EXTRA` of the `added` pack and refuses a library without support; `/api/ctgp/import` accepts `added`; texts in pt/en/es.
  - Cup area: 49 places (25 of the game and two pages of 12). With 41, the fourth page pointed the 8 empty slots at nonexistent cups (the game would fall back to the Mushroom Cup); they are now empty, locked cups with the empty icon.
  - Images: the menu texture manager (`f_7000405f74`) is created with room for 150 (trace: count 150 of 150); the game's 105 images plus 64 did not fit and the last 16 did not get in (the panel kept the previous one). It gained one place per added track (`mk8_course_picture_places`, counted from `MK8_CUPS_EXTRA`, because it is created before the cups). Discarded attempts: the `UIHeap_CourseTex` pool (124) and its heap size were not the limit; images reduced to RGB565 did not solve it and looked washed out (the originals are sRGB).
  - In the game (real pack, 16 cups): pages 3 and 4 with their own icons, names and images; the empty slot on page 4 does not open a race; time trial of cup 13 (`CTGP49`, DLC ID 75) with its own music at 60 FPS; Grand Prix of cup 1 (`CTGP01` → `CTGP02`) at 57–60 FPS; without added cups, time trial and Grand Prix equal to the game at 60 FPS.
- **Icons of the added cups (2026-11-16, branch `ctgp-recomp`):**
  - Each button's icon is a frame of the `P_CupIcon_00` animation (texture pattern `FLTP`, stepped curve) from `mn_L_CupSlctBtn_00.szs`: the frame = the cup number; with 24 frames, the cups from the 25th on stayed on the last one (Spiny).
  - `ctgp_pack.bntx_add_textures` adds textures to the BNTX (copies of the `BRTI` of a model texture with a new name and image) and rebuilds the whole layout through the `_RLT`: pointer vector, `_STR` with the `_DIC` (Patricia tree, rebuilt identically to the game's), `BRTI` blocks, aligned `BRTD` data and the new `_RLT`. With no textures to add, the file comes out identical up to the `_RLT` (which only changes how the same pointers are listed).
  - `flan_add_pattern` adds keys to the `FLTP` (frame 25 + k = icon k) and the missing names to the texture list; with no new keys, the file comes out identical. `cup_select_add_icons` joins the two: CT icons (130×130, BC3/BC7 decoded and reduced to RGB565 as in phase A) on the added cups and the game's empty icon (`ym_CupIconBack^h`) on the slots without a cup up to the 16th.
  - In the game (local test, CTGP Boo icon): the third page shows the Boo on the added cup and the empty icon on the other 11; the base and DLC pages keep the game's icons (the Spiny on the last DLC cup).
- **CT track images on the panel (2026-11-16, branch `ctgp-recomp`):**
  - `f_7000418ed0(names, &id, load)` builds `UI/cmn/menu/ym_CoursePict_<suffix>^u.bntx` (suffix = `names + 0x80`, index ID + 1): with 1 it loads the image, with 0 it finds the one already loaded. The menu loads all of them at once (`f_7000418e3c`: IDs 16–63, 67–122 and -1). The screens pass a copy of the ID on the stack.
  - The runtime adds the `<folder>_00` images that the overlay has: the menu loads them after its own, with IDs from 123, and the name comes from `mk8_course_picture`. The cup panel (`f_700043c2e8`) annotates on entry the record it shows and, where it reads the ID (`+0x3cc`) for the image, receives that of the added image; the time trial screen (`f_7000446068`, `+0xb4`) receives that of the chosen cup's track. The texture's internal name in the BNTX does not matter (CTGP brings `ym_CoursePict_Gu_City_00^u` inside its own).
  - In the game (CTGP images `CT_00`–`CT_03` as `CTGP01–04_00`): the menu loaded all 4; the third page and the time trial screen show the CT images; the Mushroom Cup page keeps the game's.
- **Music of the CT tracks (2026-11-16, branch `ctgp-recomp`):**
  - The game reads the RomFS as an image (`IStorage`) and the SDK's own RomFS driver resolves the paths; the sound system opens the music through `nn::fs::OpenFile` (`sdk+0x50080`, found by the exported name in the SDK NSO) with `content://Audio/Resource/Stream/pBGM_<track>_N.bfstm` (and `_F` on the last lap), checked with the tracer.
  - A hook at the entry of `OpenFile` (`sdk_008.c`, SHA-256) asks `mk8_stream_path` for the path: in a race of an added cup's track (the folder swap happened), `pBGM_*_N|F.bfstm` becomes `<folder>_N|F.bfstm` when that file exists in the overlay's `Audio/Resource/Stream`; otherwise, the music of the borrowed track. The new file has been in the RomFS index since the start and is read at its own size.
  - In the game (local test, CT music from the phase A pack renamed): the Grand Prix from the third page read `CTGP01_N.bfstm`, `CTGP01_F.bfstm` on the last lap and `CTGP02_N.bfstm` in race 2, at 59–60 FPS; menu, opening and finish with the game's music. The sound itself was not heard (test without audio).
  - Icons: the button's icon is a frame of a texture-swap animation (`f_700043bb68` calls `f_70008ad3c4` with frame = cup); with 24 frames, the added cups show the last one (Spiny). Adding icons requires new textures in the BNTX of `mn_L_CupSlctBtn_00.szs` and new frames in the animation.
- **Own names (2026-11-15, branch `ctgp-recomp`):**
  - New labels, without touching the game's: `ctgp_pack.msbt_add` adds labels to an MSBT with only `LBL1`/`TXT2` (each language's `Common.msbt`), rebuilding the 101-bucket table by the MSBT hash (`h = h × 0x492 + character`); rebuilt with no additions, the `Common.msbt` of USen, EUpt and JPja comes out byte for byte identical. The game reads only `UI/USen/message.sarc` in this configuration.
  - Track name: the record's label (`n`) and, at the race opening, `n + 100` with the game's control marks. Cup name: `f_700041fd6c` reads a table of 24 labels (`main+0xf26c44`) with no limit; the runtime answers for the added cups (`MK8_CUPS_EXTRA` accepts `<cup label>=` at the start of each cup). `main_084.c` is accepted as it is after the Mii hook.
  - Screens that find the track by ID: 8 readers use the reverse map (`+0xC80/+0xC84`) to find a track's cup and position and show it (the name on the time trial screen, for example). With an added cup chosen, `mk8_cup_by_slot` gives them the game's cup with the added cup's records in place of the borrowed tracks; the cup screen keeps reading the game's cup as is. `f_70004a3940` also receives the track ID (`x25`) and returns the added cup's record; with another track, the race goes back to being a game-cup race.
  - In the game (local test with the pack's first 4 tracks): the third page shows "Boo Cup" and "GPDX Omatsuri Circuit", "Vanilla Lake 1", "Broken Pier", "Mystic Tangle"; the time trial screen and the confirmation show "GPDX Omatsuri Circuit"; Grand Prix from the third page with `CTGP01` and `CTGP02` at 60 FPS. The race opening text (`n + 100`) was not captured in the images.
  - Races that do not come from a cup (2026-11-16): in VS with random tracks the game calls none of the cup readers (trace) and the race configuration does not store the mode (only the ID at `+0x1a0`), so an added cup chosen earlier remained in effect. New rule: only "armed" races use the folders — choosing an added cup (`mk8_cup_select`) arms the race of the chosen track and its repeats; the Grand Prix (`mk8_cup_select_gp`, `f_70004fb290`) arms the 4 in order; any race of another track disarms (IDs below 16, the menu course, do not count). Limit: a random VS right after, whose first drawn track has the same ID as the last CTGP track raced, would still use the CTGP folder in that race.
  - Online: the launcher does not pass `MK8_CUPS_EXTRA` in network sessions (each machine would decide on its own; the game exchanges track IDs, which the added cups borrow). Online, the game's cups and tracks apply, with the pack's other files.
  - Native test of the rule (`armed`) and in the game: time trial of cup 13 (`CTGP49`) and Grand Prix of cup 1 (`CTGP01` → `CTGP02`) at 60 FPS; time trial without added cups equal to the game. The Grand Prix without added cups was not repeated in this version (stopped for lack of system memory); the random VS after a CTGP cup was covered only by the native test. Records and ghosts of the CT tracks stay with those of the borrowed track (separate CTGP save).
- **Consequence for a CTGP set of its own:**
  - up to 15 new tracks fit without enlarging the ID space, but the original cup vector has no free slot (25 places: 24 race cups and the battle one; step 1 above opens 16) and the cup screen only has two pages of 12;
  - the 64 CTGP tracks as new tracks would require enlarging the enum and the 129 points limited to 123, the cup vector (131 points) and the cup screen (third page), and separating the records in the save.

**Phase B (code) — reverse engineering map (2026-11-16), no change to the game:**
- **There is no static cup → track table:** no permutation of 0–95 (nor of 0–47) in u8/u16/u32 in the `main` data segments. The mapping is built at run time.
- **Track path:** `Course/%s/%s` (`main+0xf0d209`) and `Course/%s` (`+0xf0d223`) are used by `f_700089ed04` (`main_186.c`). It gets the race object via `0x70012f9e98`, reads a limited index at `+0x58` (u16), reads a byte at `+0x6a` and calls a virtual method (`rt_dispatch`). The folder name comes from an object, not from a table.
- **Menu UI initialization:** `f_700040c5f4` (`main_083.c`) creates the menu resource pools (sizes 0x35, 0x2a, 0x17, 0x10…) and uses the track image `*_00` strings.
- **Platform label:** 1461 (`0x5b5`) only appears in the code as date arithmetic (days in 4 years, in `f_70005645f8`…); the label comes from data, not yet located.
- **Next step for extra cups:** trace dynamically, with the game running, the track and cup objects at the two points above (`FN_ENTER` with an address filter, without changing behavior). Only then decide where to raise the limits, with save tests.

**Phase B (code) — what reading 4.0.1 showed:**
- **Cup table:** `main+0x12840b8` is the static table of 24 pointers to the icons (Mushroom…Thunder, DLC02–05, Charger00–11), with the pointers in `R_AARCH64_RELATIVE` relocations and not in the data.
- **Fixed limit in the code:** `f_700050ccc0` does `min(cup, 0x17)` before reading this table, that is, 24 fixed cups.
- **Track names:** `Gu_FirstCircuit_00`… at `main+0xee7de9` are built by code (ADRP/ADD), with no pointer table.
- **What adding cups requires** (instead of replacing slots): hooks in several routines with fixed limits (24 cups, 96 tracks), in the track/cup tables, in saves sized for 96 tracks, in the cup screen pages and in online play. It is weeks of reverse engineering, with risk to saves.
- **Plan:** first, a native test of the hook table in `FN_ENTER`; then each limit with its own test.
- **Still depending on this work:** random button, menus and extra CTGP cups.

## CTGP Deluxe — investigation, no implementation — 2026-11-14

**What the project states** (ctgpdx.com: download, FAQ and notice): version 1.1.1 "only works on Mario Kart 8 Deluxe version 3.0.3", and the site does not explain why. The installation is a 3.86 GB ZIP (4.52 GB extracted) placed at the root of the SD card of an unlocked Switch. Online Play was removed, and local/LAN remain. The terms say "fan creations intended for personal use only and are not for sale or distribution". The package was not downloaded: the terms forbid redistribution and extraction would leave C: below the 4 GiB reserve.

**What this recompilation accepts today:**
- **Files:** `RECOMP_MOD_DIR` overlays `romfs/` and gives the mod its own saves (`runtime/mods.c`). `mods.h` itself says "no game code".
- **Code:** only `main`, `sdk` and `subsdk0` of the 4.0.1 profile are recompiled, with fixed hashes.
- **Hooks:** `rt_dispatch` accepts replacements by address only on indirect calls. Direct calls between recompiled functions do not go through it. Every guest function starts with `FN_ENTER(address)`, currently used only by the profiler.

**Why a code mod does not work as on the console.** Switch code mods (exlaunch in `exefs/subsdk9` + `main.npdm`, or IPS patches) do two things, and both break here:
- **Hooks by address:** they write branch instructions into the `main` `.text` using fixed addresses of the version they know (hence the lock to 3.0.3).
- **Recompiled code:** in a static recompilation the code has already become C. Writing instructions into `.text` at run time changes nothing, in any version.

**Paths, from smallest to largest:**
1. **Files only on top of 4.0.1** (already possible with `RECOMP_MOD_DIR`): replacements of tracks, music and interface may work. Extra cups and tracks, menus and new options depend on the code and would not appear. Formats that changed between 3.0.3 and 4.0.1 may break.
2. **Port the mod's code to 4.0.1:**
   - recompile the mod module as one more pipeline unit;
   - capture the hooks it installs at boot (writes into the `main` `.text`) and apply them as a branch at function entry, with an `FN_ENTER` that consults a hook table (requires recompiling the whole game once);
   - map each 3.0.3 → 4.0.1 address by binary comparison of the two `main`s, which requires the user's own 3.0.3 `main`.
   Hooks in the middle of functions would need points inserted in the recompilation.
3. **Recompile 3.0.3 as a second profile:** dispenses with the address mapping, but all the fixed-address guest adaptations (Mii, LAN, setjmp, Yaz0 and others) would have to be redone for 3.0.3. The host ones generally survive. The qualification would also have to be redone.

**Scan of the package extracted by the user (2026-11-15, read-only, outside the repository in `Documents/CTGPDX`; nothing was executed or copied):**
- **Size and title:** 1,120 files, 4.52 GB, all in `atmosphere/contents/0100152000022000/` (MK8 Deluxe).
- **Code:** `exefs/subsdk1` (Skyline, 42,869 B, build id `782d51e7…`), `exefs/main.npdm` and the plugin `romfs/skyline/plugins/CTGPDX.nro` (548,864 B). There is no `exefs_patches`.
- **Architecture — the decisive fact:** the mod's `main.npdm` declares a **32-bit** process (flags `0x4`, 32-bit address space). This project's 4.0.1 `main.npdm` declares **64 bits** (`0x7`). `subsdk1` and the plugin are **32-bit ARM**: ARM `b` entry `0xEA00001E`, `.dynamic` and ELF32 symbols and imports `A32HookFunction`, `A32InlineHook` and `__aeabi_*`. The MK8 3.0.3 that the mod serves is therefore a 32-bit executable, and 4.0.1 is AArch64. This explains the lock to 3.0.3: the move to 64 bits changed all of the machine code.
- **What the plugin does:** it imports no game symbol; it installs hooks by address in the 32-bit `main`. From the strings:
  - 64 tracks `CT_00`–`CT_3F`, with music and files read from `content://Course/` and `content://Audio/Resource/Stream/`;
  - extra cup icons (`L_CupIcon_18`–`25`) and a random button (`Random_00`, `RandomBtnOnOff`);
  - menu background swap;
  - version `CTGPDelux010101`.
- **Files (romfs):**
  - **new, with no effect without the plugin** (1,020 files): 64 new tracks in `Course/CT_xx` (816 files, 3.4 GB), 128 `pBGM_CT_xx` music tracks, 64 `ym_CoursePict_CT_xx` images. None replaces an original track; the game only loads them if the code registers the `CT_xx` names.
  - **replace original files** (99): the `BGM_MENU_*` and `BGM_TITLE*` music, `StaticSysMenu.bars`, `JugemObj.bfres`, `message.sarc` in 15 languages, `common.sarc`, `menu.sarc`, `race.sarc`, title and loading screens, and 80 `ym_CupIcon*` cup icons.

**Consequence for the three paths:**
1. **Files only on top of 4.0.1:** would apply only the 99 files that replace originals. No new track appears. The message and menu SARCs come from 3.0.3 and may not match those of 4.0.1, with wrong text or layouts or a crash. It serves only as a format compatibility test, not as "playable CTGP".
2. **Port the code:** there is no possible binary comparison between a 32-bit `main` and a 64-bit one. It would be rewriting the plugin's logic (registration of 64 tracks, cups, menus, music) as hooks of its own in the AArch64 functions of 4.0.1, with reverse engineering of 4.0.1. It is a new mod project, not a conversion; the 1,020 track files would be reused if the formats are the same.
3. **3.0.3 profile:** the recompiler (LASRecomp/SMM2Recomp) translates only AArch64; there is no A32/Thumb support. It would require an entire 32-bit ARM front-end, plus redoing all the guest adaptations. It is the most expensive path.

None of the three gives the complete CTGP on 4.0.1 without a large project. Step 4 of the roadmap below (path 1) should only be attempted as a format test, after the game is re-imported.

**Roadmap for testing later (path 1, files only; no step changes code):**
1. Download the ZIP from the official page, for personal use. Never put it in `upstream`, tests, releases or the setup.
2. List without extracting and keep the list outside the repository:
   `local/python/Scripts/python.exe -I -c "import zipfile,sys; z=zipfile.ZipFile(sys.argv[1]); [print(i.file_size, i.filename) for i in z.infolist()]" "<CTGP.zip>" > local/ctgp-lista.txt`
   In the list, look for the title (`atmosphere/contents/0100152000022000/`, MK8 Deluxe), the `romfs/` folder and the code part (`exefs/subsdk*`, `main.npdm`, `exefs_patches/`). This decides between paths 1, 2 and 3.
3. Extract only the title folder into a new, empty directory, on a disk with room for the ~4.5 GB **in addition to** the 4 GiB reserve, for example `<disk>/ctgp/0100152000022000/`. Do not execute anything from inside it.
4. Run through the test script, which already forces `LARECOMP_SAVE_DIR` to an isolated copy. This matters because `RECOMP_MOD_DIR` alone redirects the saves to the mod folder. Start with menu and time trial:
   `local/python/Scripts/python.exe scripts/test-race.py time-trial --last-frame 4800 --shots 3 --shot-step 1400 --timeout 300 --env RECOMP_MOD_DIR=<disco>/ctgp/0100152000022000 --env LARECOMP_LOG_FILES=1`
   `RECOMP_MOD_DIR` must point to the folder that **contains** `romfs/`. The launcher refuses `RECOMP_MOD_DIR` when a profile Mii is active (`mii_native.rs`), so in this test the Mii profile stays disabled.
5. Record as evidence:
   - the files read from the mod (`LARECOMP_LOG_FILES`);
   - format errors and crashes;
   - images of the menu, the cup selection and the race;
   - which tracks and menus changed and which stayed original (the expected result without the mod's code).
   `gameplay_verified` remains false.
6. After the scan of 15 November, path 2 is a reimplementation of the plugin in AArch64, and path 3 requires an A32 front-end (see above). Either of them needs a decision from the user before starting. Path 2 begins with a native test of the hook table in `FN_ENTER` with synthetic functions; only then comes the recompilation of the whole game.

**Path 1 tested — 2026-11-15.**

*Conditions:* game re-imported from the NSZ (`Verified 4.0.1 import`), full build, PRs #18/#19 included. Time trial of 4,800 frames with `LARECOMP_LOG_FILES=1`. The partial overlays are hardlinks in `local/ctgp-overlay-*`; the `Documents/CTGPDX` folder was not changed and the `skyline/` plugin was never used.

| Session | Overlay | Result |
|---|---|---|
| `race-time-trial-o33nrkca` | none (base) | 60 FPS to the end |
| `race-time-trial-m5um97f1` | full CTGP romfs | **crash** at 16 s on entering the menu: `c0000005` at `0x90`, `f_70008bab3c`, right after reading `UI/cmn/menu.sarc` |
| `race-time-trial-n4fawqy7` | without `UI/cmn/menu.sarc` | **crash** at the same point: `c0000005` at `0`, `f_70005908a8` (`common.sarc` remains) |
| `race-time-trial-ssw00gbi` | without `UI/` | 60 FPS to the end; title/menu music, `StaticSysMenu`, CTGP Lakitu loaded |
| `race-time-trial-lzz6p1u3` | without `UI/`, only `message.sarc` | 60 FPS to the end |
| `race-time-trial-79ux_uty` | `CT_00` in place of `Gu_FirstCircuit`: track files, `MapObj`, music and image; original track's sound bank | **CTGP track raced**: own model, scenery, objects, minimap and music; 60 FPS, max 18 ms, no error |

**Conclusions:**
- The CTGP romfs **as is** does not work on 4.0.1: the interface SARCs `common.sarc` and `menu.sarc`, built for 3.0.3 and for the plugin, crash the menu.
- The track formats are the same as 4.0.1: model `FRES 0.5.0.3`, KCL, AAMP, `VFXB`, BNTX and BYAML. The `FSTM` v3 music and the `FRES 0.5.0.2` Lakitu also loaded.
- **A CTGP track races on 4.0.1 with no code at all**, occupying the slot of an existing track.
- `gameplay_verified=false`: the test covered one track, a partial lap and only the time trial.

**Path 2 — plan, to be decided with the user (nothing was implemented):**
- **Phase A — tracks in slots, no code.**
  - A local tool (`toolchain/ctgp_slots.py`) builds a private overlay in `local/`, with hardlinks when the disk allows, from the user's copy. It never goes to a repository, release or setup.
  - Maps `CT_xx` to existing slots: files of `Course/<slot>`, `pBGM_<slot>_N/F`, `ym_CoursePict_<slot>_00^u` and the track name in `message.sarc` (MSBT inside the SARC).
  - Track sound banks: each `Course_CT_xx.barslist` (ARSL) points to the bank of an original track (`CT_00` → `Gu_DossunIseki`). The first version uses the slot's bank.
  - Limit: 48 original slots, or 48 more from the Booster Course Pass (`Cnsw_*`, same structure) with the DLC imported. The 64 tracks do not all fit at the same time; the user chooses or switches sets.
  - Launcher: "Track pack" option with activation, like the DLC, and `RECOMP_MOD_DIR` only in the child process. In Steam lobbies, require the same pack (hash), like the DLC rule.
  - Tests: unit test of the assembly with synthetic files, and a full GP with four CTGP tracks (image, audio, laps, restart and save).
- **Phase B — CTGP behavior, with code.** Extra cups and tracks beyond the slots, random button and CTGP menus.
  - Reverse engineering, in the AArch64 of 4.0.1, of the track and cup table and the selection screens.
  - Own hooks applied by the pipeline: per generated unit, like `yaz0_native.py`, or through a hook table in `FN_ENTER`, with hash validation.
  - Menu layouts rebuilt on top of the 4.0.1 SARCs, because CTGP's crash the menu.
  - Each hook with a native test and proof in a race.
- **Phase C — multiplayer** with the pack: validation on two machines, as the project requires.
- **Legal limits:** CTGP is for personal use and cannot be redistributed. The tool only reads the user's own copy; nothing from the package goes into sources, public tests or releases. Credits and terms go in `docs/THIRD_PARTY.md` when there is code.

## PR #12 (DLC and Settings) and PR #9 integrated — 2026-11-13

Merge of `pr-12` (PlugNChug: verified Booster Course Pass, second verified variant of the base NSP, **Settings** tab with 58 F1 options, dismissible fixed notices); PR #9 (recompilation accessible after the build) comes included in its first commit. The 17 conflict blocks were additive and were resolved by keeping both sides: Mii checks + DLC root and `SDL_VULKAN_LIBRARY` + `MK8_DLC_ROOT` at game launch; Mii/lobby states + `dlc`/`dlc_import` in the status; `steps(root)?` with generic `start_steps_guarded`; DLC import now blocks **Play** inside `start_game`; translations merged as JSON (116 new keys; "Automatic" kept); redesigned Mii section from fix-chore + new Settings section. The PR's username test was not brought over, because the field was removed in fix-chore, where multiplayer uses the Mii.

**Semantic conflict fixed:** `toolchain/dlc.py` and the MiiModel hook in `toolchain/host.py` adapt the same system file path in `services/fs.c`. The Mii hook runs first, and the DLC refused the already adapted form (`test_mode_patches`/`test_host_layout` failed). Now `dlc.py` accepts exactly one of the two known forms and, in the adapted one, composes DLC → `MK8_MII_RESOURCE_DIR` → `sysdata`, keeping the refusal of truncation. New `test_adaptation_composes_with_mii_resource_mount` uses the real upstream files. The simulated router of the recompilation test (#9) now serves `.cjs`/`.wasm` and rewrites the `three` import like the real server.

Validation: formatting, workspace Clippy with `-D warnings` and 65 Rust tests; Python: DLC (11), settings, import, pipeline, MiiModel converter, experimental adaptations, `test_mode_patches`, `test_host_layout` and `test_prebuilt_host`; browser: e2e (21), languages (3), DLC in the launcher (4), Mii editor and settings. Runtime recompiled with `--runtime-only`. `local/race-time-trial-q45u1qzo`: exit 0, new `aoc:u` service (`installed=0` on this machine without DLC), `system data 0100000000000802` mounted from the generated Mii resources, portraits in the Mii grid and the Mii with face and helmet. DLC in multiplayer remains unqualified: host and guest with different DLC states are still neither blocked nor warned.

## NSP/NSZ import — second verified variant of the base game

The error “MK8 base or 4.0.1 update content missing from the package” was reproduced with an NSP whose base program is `6d1fd4733f0e29b754081d2bf0c56d702ad89a86ea30e67b69bb84a0a66c8b0d`. The importer accepted only `e1ace5248218fa6393ec1637ce8c2c7943fb1d126c2dea93135d641cfd692183`. Both have the same size but different binary content; renaming the file does not satisfy the integrity check.

The selection now accepts these two explicit identities of the base program. It keeps the full SHA-256 of each reconstructed NCA, the pinned update contents, LibHac integrity and the exact hashes of the 4.0.1 AArch64 ExeFS. Unknown bases or packages with both variants are refused. The disk reserve considers one base per import, without adding the space of the two alternatives.

Validation: 5 importer tests, 5 file access tests and 3 storage tests passed. Self-authored fixtures cover both identities, NCA and NCZ, import of PFS0 containers, divergent size/hash, ambiguous selection and disk budget. On the private files, the new variant was imported with the update in 47.3 s; LibHac integrity, the ExeFS hashes and the NACP version passed. All 6,597 RomFS files (12,131,696,580 bytes) are identical to the extraction that already worked. The full build using the NSP configuration completed in 41.2 s with the existing caches, and the hashes of the resulting candidate were checked. These durations do not represent a build without cache. Saved settings, user files and the original installation were preserved; the new outputs stayed in the isolated development data. There was no new run of the game nor gameplay qualification; `gameplay_verified` remains false.

## DLC — import and control in the launcher

Readiness of the DLC fields: the same tiles of step 2 receive a green outline and confirmation when the NSP/keys are found. The query API is read-only and reuses the importer's key selection (explicit field, saved configuration or automatic detection). The client checks again when typing, choosing files and refreshing the state, discarding responses for old paths. Color does not replace validation of content, hashes or decryption. Self-authored tests cover present/absent files, explicit/automatic key selection and the rejection of a late response for a previous path. Outlines and marks were checked on desktop and mobile; release launcher recompiled.

Interface integration: the private script still opened `target/release/Launcher.exe`, but the DLC branch had split from master without the recompilation commit that was still separate. The branch now incorporates the original commit `d09565e`, preserving the layout with **Recompile game** and **Play now** together below the green banner. There was no change to the private script or the data directory.

Explicit reinstallation: the installed DLC card offers **Reinstall or select another NSP**. The API forwards the boolean option all the way to `--reinstall` in the importer; this path validates/extracts the selected package even when the current files are intact. Publication keeps the previous installation as a backup, without changing the activation preference, configuration or saves. Validation failures leave the current installation and the candidate intact. The profile remains limited to the same verified Booster Course Pass; this does not widen the accepted versions or titles. Validation: 10 DLC import/IPC tests, 43 Rust tests, Clippy and formatting passed; 24 browser tests passed and 1 relay test was skipped, including visible recompilation and reinstallation of an installed DLC. Reinstalling the private NSP through the real API and supervisor ended with a backup created, candidate and settings identical, preserving the DLC preference. Release launcher recompiled; runtime/guest were not changed.

The additional NSP supplied by the user (121,056 bytes) was inspected in a private development folder. The CNMT identifies `AddOnContent` with title `0100152000023001`, associated with application `0100152000022000`, and its content record matches the size and SHA-256 of the data NCA. Decryption with the user's keys and extraction by LibHac with `ErrorOnInvalid` passed. The additional RomFS contains only `contents.dat` (4,096 bytes) and `version.txt` (5 bytes). The combined RomFS of version 4.0.1 has 48 `Cnsw_*` course folders; this is compatible with the track resources already being in the update, but does not prove their execution in this build.

The confirmed blocker was `aoc:u`: count and listing returned zero. `toolchain/import_dlc.py` now imports only this verified package into an already prepared experimental 4.0.1 candidate, with a preparation lock, disk reserve, link rejection and publication after validating the NCA hashes, the CNMT association with the game and the two extracted files. The user's ticket and keys are used in memory by LibHac; no key is written to the manifest. A versioned binary manifest and three records in the candidate allow the installation and its hashes to be checked before playing. An incomplete or divergent installation is refused, with no automatic replacement.

`runtime/mk8/services/dlc.c` implements discovery, preparation and event for the installed content; `toolchain/dlc.py` adapts only the verified private copy of `aoc:u` and `OpenDataStorageByDataId`. Without a manifest, the count remains zero and DLC preparation/mounting fails. With a manifest and intact files, index 1 is listed and the storage mounts the two real files. There is no environment variable that simulates the installation. In the real run, MK8 used commands 8, 3, 7 and 5, then opened the storage of title `0100152000023001`. Protocol and access references in `THIRD_PARTY.md`.

Isolated local validation: import of the real NSP completed and runtime recompiled with `--runtime-only` (213 units, link and 13 dependencies verified), preserving the game's recompiled library. 7 DLC tests, 5 base importer tests and 5 services/runtime tests passed. The native tests run the service with self-authored files: absence, installed index, input and output limits, event, wrong title and altered files/manifests. The import covers publication, reuse, extractor failure, altered NCA, a path outside the selected folder and preservation of the configuration/candidate on failure.

The game displayed the Golden Dash Cup and started Paris Promenade. A three-lap Grand Prix race on that track ended on the results screen (1st place), with a normal exit after 16,500 frames, in 284.2 s of session. The captures recorded the start, the race and the results; assisted steering and scripted inputs were used. The copied save was updated after the finish and loaded in a new run, which returned to the selection of the additional tracks without error. Without DLC, the same selection sequence stayed on the cup menu and the service recorded zero installed contents. These tests used copied saves and private session folders; the original installation and personal saves were not used as a destination.

The control without DLC also started a base race at Mario Kart Stadium, with a capture at 3,400 frames and a normal exit in 66.0 s. This verifies entry to the base track, not a complete base race in this round.

The user manually confirmed that the prototype works. The interface now offers the optional **Additional content** card, after the three installation steps: NSP/keys selection, supervised import with state/log and repair, in Portuguese and English. The **Enable Booster Course Pass** control sits in the **Settings** tab. The importer reuses the existing process and lock infrastructure; concurrent operations, including direct preparation through the CLI, refuse changes. While the game is running, the controls are disabled and the API also rejects the operation.

The verified files stay in `dlc/` in the data folder, independent of the build identity. The preference stays in `dlc.json` and is read when the game starts; disabling does not remove the files nor modify saves. The launcher gives the runtime the verified folder or an explicit disable option, which also prevents discovery, preparation and mounting. The pipeline preserves the DLC in new builds and migrates the old copy after checking its files, without deleting the source. A repair only publishes new files after validating them, keeping the previous destination in `dlc-backup-*`. Access errors are not treated as corruption to work around.

The candidate records `dlc_supported`; the absence of this field in old manifests is equivalent to false. These builds remain available for the base game, but the card asks for **Update the build** before importing/toggling DLC, with an accessible update button in step 3. Showing an activation preference is not enough if the executable does not yet implement the service.

Interface validation: tests written first reproduced the absence of persistence, runtime blocking and controls; afterwards 10 DLC tests, 9 pipeline tests, 5 base import tests and 5 services/runtime tests passed. The self-authored tests also cover repair with backup, preservation after failure, migration when the build folder changes and the disabled preference. 41 workspace Rust tests, formatting and Clippy passed without warnings. In the browser 19 tests passed (16 general, with 1 skipped for lack of a configured relay, plus 2 for languages and 2 for DLC), including import, errors, toggling, translation, update of an old runtime and mobile layout without horizontal scrolling. Release launcher and a 213-unit runtime were compiled, keeping the game's library.

Two runs through the real interface/API, with copied saves, reached the captures at 2,000 frames and exited with code zero. Disabled: `aoc:u` returned zero and the selection showed the DLC purchase notice. Enabled after restarting the launcher: the disabled preference was recovered, the control could be turned on and Paris Promenade became selectable; there was real preparation and mounting of the content. Attempts to toggle during the sessions were refused. The hashes of the original development user files remained identical. The idempotent import of the real NSP through the API/supervisor completed, with candidate and settings identical. An earlier attempt lost the capture to Vulkan swapchain errors (`VK_ERROR_OUT_OF_DATE_KHR`); the private process was terminated, without counting that attempt as a pass or changing graphics code.

Limits: there is no toggling during a game session, automatic update/removal, generic support for other NSPs/NSZs or qualification of the 48 tracks. Audio, physical controller, clock speed and multiplayer with DLC were not qualified in this round; the 60 FPS indicator in the captures does not replace those measurements. Pre-existing visual defects remain visible. `gameplay_verified` remains false. The DLC changes are recorded on the `explore/dlc-support` branch for review; there was no PR or release. Instructions in `README.md`.
