# Technical log — Performance and memory

[Index](README.md) · [Português](../../pt-BR/log/performance.md)

## Pipeline cache persistence

Later manual validation with Nextendo: the maintainer confirmed the tests ran perfectly in `local/nextendo-playtest-iub0kdkc`, runtime `268ccc00`. Login confirmed by the log; a session of approximately 11 minutes ended with code 0, with no `FATAL`. Seven periodic cache writes recorded, now also exercised in the real game. The last five windows record 60 FPS and a maximum frame time of 18 ms; they are the end of the session, not a measurement of all races. Original saves preserved. Tracks, modes and number of races were not detailed; this confirmation does not automatically close the CTGP, split-screen, Mii or APU issues, nor does it qualify Steam multiplayer between two machines. `gameplay_verified` remains false.

Fixed a potential loss of the cache: the earlier write removed `vk_pipelines.bin` before the rename and ignored close/replace errors. The validated preparation now checks the complete snapshot, allocation, write and close, and uses replacement with `MoveFileExA` on Windows without deleting the destination beforehand. The public wrapper keeps its ABI; the periodic writer receives the real result, keeps builds pending on failure and tries again after the normal 60 s interval. It does not emit fictitious success nor enter fast repetition. This protects work already compiled; it does not demonstrate the cause of the reported stutters nor eliminate compilation on first use.

Six tests passed: five of runtime stalls and one new native test of the prepared function, with real files and failures of incomplete snapshot, partial write, close and blocked destination. The earlier cache remains intact; after unblocking, the replacement works. The scheduler was also tested with an initial failure and a retry without new compilations. Host recompiled (220 units, guest reused), SHA-256 `268ccc0083c572863b2a5006520f88cd23e3216673fc97a3ab9046ed73f24966`. Integration test in progress in `local/race-grand-prix-e9lev31t`, isolated saves and copied cache. No setup/publication. Headlights #49, white Miis, CTGP/split-screen and APU validation #52 remain pending.

## Memory recovery and Vulkan pre-test — continuation

Review of `memory-check-0h2eizvj` after a stutter report: exit 0 at approximately 265 s of runtime, no `FATAL` and no recorded trigger of the snapshot fallback. Capture 6300 confirms a race, lap 1/3; capture 14400 shows loading. The 16 periodic captures were correlated with the next frame summary: 15 coincide with maxima of 368–617 ms (the last, during loading, 131 ms). The synchronous diagnostic introduced pauses; these numbers must not be used as normal performance. During frames 4200–14000 there is also a 570 ms spike without a capture in the same interval, close to detailed VRAM statistics, cause not isolated. Pauses of 1831 ms/3782 ms coincide with transitions, uploads and 630/428 compilations, not proof of time spent exclusively on shaders. At the end, heap 0 records 1776 MB reserved in blocks and 876 MB in allocations; this is not a measurement of the peak nor a guarantee of the absence of memory pressure.

Private helper `open-memory-check.py` adjusted to remove periodic captures and detailed VRAM statistics, keeping the usual frame logs. A correction of the measurement method, not a gameplay patch.

Later automatic test completed in `local/race-grand-prix-jd01947u`: runtime `a92e1d9b`, exit 0 in 216.4 s, isolated saves and cache copied from the manual session. Spot captures at frames 3500 and 12000 confirm a race on Mario Kart Stadium, laps 1/3 and 3/3; there was no periodic capture in the measured stretch nor detailed VRAM statistics. Analysis of 67 windows of approximately 2 s, between 73.8 and 205.9 s (frames 3800–11899, excluding the captures): median 60.0 FPS, per-window minimum 57.6 FPS, maximum frame time 41 ms; two windows with a maximum above 33 ms and none above 50 ms. The 38/41 ms spikes coincide with windows with 3/15 compilations, without isolating the cause. No FATAL and no trigger of the snapshot recovery. Private summary and CSV in `performance-summary.json` and `performance.csv`. Does not measure 1% low, VRAM peak, a full race through to results or a full cup. A different track from the manual session and a reused cache prevent a controlled before/after comparison; it does not qualify CTGP, split-screen or the APU of #52. `gameplay_verified=false`.

Monitored manual rehearsal requested by the maintainer: `local/memory-check-0h2eizvj`, same runtime `a92e1d9b`, saves/profile/cache copied, `MK8_TRACE_VRAM=1` and captures every 900 frames. The process opened and heap/allocation logs are active. A full 150cc cup was requested followed by a normal close. Result still pending; the local RX 580 does not automatically reproduce the limits of the AMD APU of #52.

After `26a6dce`, `snapshot_memory.py` was added: the `snapshot_image` path previously aborted on the first `vmaCreateImage` failure, unlike the main textures. Recovery limited to `VK_ERROR_OUT_OF_DEVICE_MEMORY`: collects already completed deferred resources, repeats the allocation and tries a preference for compatible host memory. Format, dimensions, samples and flags kept; other errors return immediately and exhaustion remains a failure. A C test of the prepared helper covers initial success, recovery after collection, fallback, exhaustion and device-lost/host-OOM without undue repetition. 14 texture tests also passed. This addresses a gap in the snapshot path observed near the point of #52, without proving that the reporter's integrated AMD GPU now completes races.

The launcher's Vulkan pre-test now runs `SDL_Init(VIDEO)` and `SDL_Vulkan_LoadLibrary` with the local DLL in the subprocess, releases SDL/Vulkan when done and reports the real SDL error. Windows error 193 distinguishes an invalid format/architecture from a missing module (126); it does not fictitiously identify the culprit dependency. A test with a synthetic DLL that exports `vkGetInstanceProcAddr` but rejects queries confirms SDL's refusal; the real targeted test passed with missing/corrupted/missing-dependency/valid cases. Full suite: two cases passed, the third failed only in the cleanup of `C:/Users/<user>/AppData/Local/Temp/mk8-vulkan-bundle-h6pptnfo/runtime.exe` with WinError 5; file preserved, without working around the blocked deletion. Do not claim the entire suite passed. Launcher compiled, 36 Rust tests passed, Clippy and formatting passed. #47 requires a new diagnostic from the reporter to determine the causing DLL/architecture.

Host runtime recompiled, SHA-256 `a92e1d9be2087e9404e5f4de420a329728c9772b04381163d162991f060ef9e4`; short rehearsal started in `local/race-time-trial-uq7i4xq7` with isolated saves and `MK8_TRACE_VRAM=1`. No setup, commit or publication in this round. Controllers #50, lighting #49, white Miis, DLC variants #36 and import/portability features remain pending.

Rehearsal result: exit 0 in 70.2 s; the inspected capture `frame-03400.png` shows engine-class selection, not a race. The automatic sequence did not reach the track; the 60 FPS observed at the end are from the menu. No qualification of recovery on a low-memory GPU. Targeted Vulkan test repeated and passed in 6.34 s; the full suite's cleanup failure remains recorded above. No blocked file was removed by another mechanism.

## Stutters and fluctuating FPS in races — 2026-11-14

**Measurement on this machine** (RX 580 8 GB, AMD driver 26.1.1; GP Mushroom Cup, race 1, frames 3000–14000; `scripts/frame-stats.py`):

| Run | Cache | FPS | p99 | Worst | >25 ms |
| --- | --- | --- | --- | --- | --- |
| `race-grand-prix-odwx3qkw` | empty game (warm driver) | 59.95 | 17.74 | 47.4 ms | 17 |
| `xchoj6wg` / `oy047b4c` | warm, FIFO | 59.99 / 59.98 | 17.66 / 17.61 | 30.8 / 32.5 ms | 5 / 7 |
| `a8nu0oou` / `59bxbx65` | warm, FIFO_RELAXED | 59.99 / 60.00 | 17.66 / 17.62 | 29.4 / 30.1 ms | 4 / 3 |

Here the fluctuation in a race is not reproduced. Large stutters are in the transitions: end of the race, with 475–519 ms in `other` while a game thread waits ~265 ms for an emulated core occupied by another (`kernel.c` has no preemption inside guest code), and track loading with hundreds of compilations.

**Presentation mode (`toolchain/present_mode.py`, default).** The AMD driver on Windows does not offer MAILBOX, so the runtime used FIFO without logging it. Now the order is MAILBOX → FIFO_RELAXED → FIFO, the mode appears in the log (`[vk] present mode …`) and `LARECOMP_VK_PRESENT=fifo|relaxed|immediate|mailbox` still chooses. In the table, FIFO and FIFO_RELAXED measure the same, because here almost no frame misses the deadline. **The gain on machines that miss the deadline often is not proven.** `tests/test_present_mode.py` compiles the prepared choice (AMD without MAILBOX, MAILBOX available, FIFO only, explicit choices and refusals). Runtime recompiled with `--runtime-only`, candidate `6dde4ee3…`.

**Remaining spikes in a race (≤ 30 ms, warm cache).** They come mainly from shader prefetch. The first time a program appears *in the session*, `scan_draw` waits for its Maxwell→GLSL translation in the frame path (`job_run_or_wait(g_tq, …)`, a single translator), because the pipeline key depends on the translated text. That is 13–34 new programs per window, 3–7 ms each. This repeats every session because only SPIR-V and pipelines go to disk, not the translation. In addition, the scan costs ~0.6–0.9 ms per frame.

**Translation cache (`toolchain/translation_cache.py`, `runtime/mk8/graphics/translation_cache.c`, default; `MK8_TRANSLATION_CACHE=0` turns it off).**
- **Where it acts:** the four translations of the Vulkan backend in `exec.c` (three program stages and compute) go through `mk8_translate_cached`. `translate.c` (which the tests compile on its own) and `ahead.c` are left intact.
- **Key:** covers everything that `maxwell_translate` reads: header and the code words up to where the scan itself stops (reproduced), `ShaderKey`, translator globals and the runtime executable (size and date). Debug variables that change the output bypass the cache.
- **Format:** a single `cache/xlate.pack`, opened once and indexed in memory. Each record has a second hash of the key and a hash of the content; a record that does not match is translated again. Another runtime starts a new pack. A truncated end (failure during writing) ends the index there and is overwritten.
- **Why a pack:** the first version, with one file per shader, cost 0.2–0.6 ms just to open on Windows (measured), almost the same as translating. In the pack, a read costs ~0.04 ms, against ~0.9 ms of translation.
- **Gain already in the first session:** with an empty cache, more than half of the lookups already hit, because the same shader was translated again for each program and key.
- **Size:** race 1 wrote ~11 MB (615 translations).

A/B on the same build (`cb567004…`), with the same SPIR-V and the same pipelines of `race-grand-prix-a_k0sjcc`, frames 3000–14000:

| Run | Prefetch > 5 ms | > 10 ms | Prefetch before the race | Worst frame | FPS |
| --- | --- | --- | --- | --- | --- |
| `x_caqan8` / `rj4yge6a`, off | 22 / 27 | 10 / 14 | 1,629 / 1,506 ms | 34.8 / 41.2 ms | 59.97 / 59.97 |
| `tnd1lp2e` / `7pfxb1qx`, on | 5 / 7 | 2 / 2 | 1,240 / 1,125 ms | 37.4 / 29.9 ms | 59.94 / 60.00 |

Prefetch spikes fell ~75–85% and loading became ~0.3–0.4 s shorter. The average was already 60 FPS. The worst frame and the count above 17.5 ms vary between identical runs (127–295) and are not attributed to the change. Capture of frame 14200 with the cache on inspected: finish, 3/3, correct image.

`tests/test_translation_cache.py` uses the real translator in several processes over the same folder: field-by-field identical read, bytes beyond the read stretch, different key/code/global, a damaged record (second hash, GLSL text and a `ShaderInfo` field) never served, trailing garbage, new runtime, `=0` and debug bypass. Without the content hash check, the test fails. Runtime recompiled with `--runtime-only`; 49 related tests passed. **Not yet validated:** a weaker machine, where translation weighs more, and a session with all 4 tracks.

When repeating the races, the C: drive fell below the 4 GiB reserve: each session copies ~650 MB of cache, and the script itself interrupted a run. Only the `data/cache` copies of the 15 sessions created in this round were removed (no junctions, no game open); logs and captures stayed.

**Player report (#8, 0.1.8, RTX 3060 laptop, 830 s):** average of 59.3 FPS in a race, with 18 windows below 57 FPS. In 10 of them there were ≥ 10 compilations (up to 759 at the start, cold cache). In an 8 s stretch it stayed locked at 30.0 FPS with no compilation, with GPU waits of 99–495 ms per window, which indicates the GPU at its limit. The presentation mode did not appear in the log; on an NVIDIA it was probably MAILBOX. Reports from 0.2.x with the `[vk] present mode` line and the GPU of whoever sees the fluctuation are missing.

## VRAM in long sessions — 2026-11-14

**Measurement.** `race-grand-prix-ov14d8nd`: 180,000 frames, 50 min, 3 GPs of the Mushroom Cup (12 races) in the same process, `MK8_TRACE_VRAM=1`. Exit 0, no `FATAL`. **There is no exponential growth.**
- **1st GP:** the VMA reserved goes from 752 to 1,520 MB, double, while each track is seen for the first time. In Task Manager this looks "exponential".
- **Afterwards:** usage stays between ~900 and 1,100 MB. The reserved grows slowly, 1,520 → 1,904 MB in GPs 2 and 3, from fragmentation.
- **Vertex cache:** rose without stopping, 31 → 130 MB, up to the 768 MB cap (`VC_BUDGET`). On each load the game's buffers change address, and the old ranges only left on reaching that cap.
- **Allocations:** 4,056 → 10,160.
- **Textures unused for more than 1 minute:** fluctuate between ~340 and 510 MB.

**Retained textures.** The largest are render targets of varying sizes (38 distinct sizes in the session, from 1454×818 to 1905×1071) created over the **same addresses**, `refs 2`, `finalized 0`. The game reuses the memory for targets of another size and abandons the old objects without finalizing them. On the console this costs nothing; here each object has its own Vulkan image.

**Release of idle images (`toolchain/texture_retire.py`, default; `MK8_TEXTURE_IDLE_S=<s>`, 60 by default, 0 turns it off).**
- **When:** every ~10 s, on the translation thread, right after frame submission.
- **What leaves:** images that no use has touched in the last 60 s and that fit one of these cases:
  - content coming from guest memory (not written by the GPU): the next use recreates and reloads the image, as the console reads that memory;
  - a GPU-written target whose address received, after the last use, a texture of another format: on the console the old content also ceased to exist there.
- **What never leaves:** an image that is presented, with pending video or involved in a deferred depth copy.
- **What stays:** the backend record.

**Why it is safe (audit of the 47 direct accesses to `->backend`):**
- The render pass, `copy_texture`, sampling and the per-frame memo go first through `vk_view`/`vk_tex` → `tex_backend`, whose "no image" branch (`pool_release.py`) recreates the image.
- `pass_layout`, the flags and the logs read only metadata.
- The copy aliases validate the handle.
- The depth shadow re-copies on every submission.
- The descriptor sets are per submission.
- The release forgets the views (increments `vk_view_gen`), invalidates the snapshots of that image and defers destruction until the GPU finishes the submissions that could read it.
- The use stamp is now always recorded, not only with `MK8_TRACE_VRAM`, and the new backend is born stamped.

**A/B on the same build** (`bccd7dce…`), full GP with cache `xh0dhyxh`:

| Run | Reserved at the end | Peak in use | In use in races 2–4 | Idle > 1 min | FPS |
| --- | --- | --- | --- | --- | --- |
| `qa8ysx5_`, off | 1,776 MB | 1,249 MB | 731–869 MB | up to 719 MB | 60 |
| `0so9vrpg`, default 60 s | 1,392 MB | 834 MB | 561–690 MB | up to 237 MB | 60 |
| `xlgl6t9w`, stress 10 s | 1,264 MB | 658 MB | 501–549 MB | up to 180 MB | 60 |

In the three runs: exit 0, no `FATAL`/exception/device loss and p99 of 17.4–17.6 ms in all races.
- **Stress:** released 10,300 images, 2.3 GB in total with recreations; the replaced targets were also released.
- **Captures** (Sweet Sweet Canyon, Thwomp Ruins and the 5th race at the Stadium): correct image, no black or missing textures.
- **Cost:** all frames above 100 ms remain in the transitions between races. In the default mode, some transitions gained an extra frame of ~100–250 ms (16 frames/3.4 s against 13/2.9 s off), consistent with recreating textures released during the race when the screen comes back. To trade VRAM for less recreation, `MK8_TEXTURE_IDLE_S` can be higher.

**Test:** `tests/test_texture_retire.py` extracts `mk8_retire_image` from the prepared `textures.c` and covers: an idle CPU texture released, a recent one kept, a non-replaced target kept, a replaced one released if replaced after the last use and kept before, presented, with video or with alias kept, a single end of pass, snapshots forgotten. It also checks the always-on stamp and that recreation comes first in `tex_backend`. `tests/test_pool_release.py` gained the new field and extracts only up to the new code.

**Not validated:** a 4 GB or integrated GPU, where the gain matters most; screens outside the race (menus, MKTV, multiplayer); sessions of several hours.

**Hypothesis discarded.** The variable resolution does not come from the GPU measurement. With `MK8_TIMESTAMP_TOP_GPU=1` (start and end measured on the GPU, instead of mixing host and GPU clocks), `race-grand-prix-a_yjfgwl` used the same ~6 target sizes as the default (`x0sdgcp3`), with the same VRAM and 60 FPS. The default does not change.

**Vertex cache by age (`toolchain/vertex_cache_age.py`, default; `MK8_VCACHE_IDLE_S=<s>`, 60 by default, 0 turns it off).**
- **How it works:** every ~10 s, `vc_flush` marks the pending submission and discards the ranges unused since the mark of ~60 s earlier. The discard goes through `vc_drop`, which only frees the buffer after the GPU finishes the submissions that could read it. The scan stops before filling the discard list, so as not to wait for the GPU.
- **Measurement** (full GP `race-grand-prix-vunwgxoi`, against `race-grand-prix-2avu_1go`): the cache stayed at ~21–26 MB instead of rising to 98 MB, and the allocations fell from ~8,350 to ~4,100. FPS of 59.99–60.00 in the four races, p99 of 17.5 ms, no `FATAL`. The next track copies again only what it uses.
- **Test:** `tests/test_vertex_cache_age.py` uses the real table, `vc_drop` and `vc_free_dead` of the prepared `exec.c`. The idle entry leaves between 60 and 70 s after the last use, the used one stays, the one from the pending submission never leaves, buffers are only destroyed after the GPU and `=0` turns it off.

Runtime `--runtime-only`, candidate `ee1f252a…`.

**CPU headroom in the race.** `MK8_SAMPLE=6000:8`, `race-grand-prix-ofuel2rl`, at 60 FPS on this machine (Xeon E5-2666 v3 Haswell, 2.9 GHz, per core similar to an i7-3770). The translation thread, with draws and the AMD driver, uses 43% of a core, ~7 ms per frame; the game's main thread, 31%; the others, 26% and 18%. A CPU with half the per-core performance would still fit in 60 FPS in the race. The report of 1 FPS (#7: GTX 1070 + i7-3770) is not explained by CPU and needs their log. `LARECOMP_LOG_PACE` does not serve for this: the "busy" comes out equal to the period, because the pacing wait on that thread is not counted as blocking.

## Freeze when crossing the finish line: native Yaz0 and core preemption — 2026-11-14

**Symptom.** At the end of every race (and on the first load), frames of 475–564 ms only in `other`, with the log showing a game thread waiting ~260 ms for an emulated core. The pinned `kernel.c` hands over the core only on kernel calls (`core_check` in `rt_svc`), with no preemption inside guest code.

**Two priority inversions:**
- **Core 2:** a priority-52 worker decompressed the next screen's files in `f_7000a88a44`, which is the game's **Yaz0** decoder (constant `Yaz0`, returns -1/-2). That is two files of ~92 MB at the end of each race. As recompiled code, with registers in memory and accesses through the guest macros, the loop took ~0.5 s with the core held.
- **Core 0:** a priority-45 worker assembles the next screen for ~0.5 s without calling the kernel, while the main thread (44) waits. `MK8_SAMPLE` in the transition window found no dominant function: it is spread-out game logic.

**Native Yaz0 (`toolchain/yaz0_native.py`, `runtime/mk8/support/yaz0.c`; `MK8_YAZ0_GUEST=1` goes back to the guest loop).**
- **Patch:** one line before the loop, in the build copy of `main_230.c`, checked by SHA-256 and with CRLF normalized. The header (magic and size through the game's own byte-order helpers, returns -1/-2 and `size < 1`) remains in the guest.
- **Native loop:** performs the same reads and writes, in the same order, including the overrun of the last copy, and leaves x8–x13 and the flags as the guest left them. `mk8_yaz0_loop` was exported by the executable.
- **Measured:** 94,817 KB in 192 ms and 91,736 KB in 205 ms.
- **Effect:** the wait on core 2 disappeared from the logs. The visible freeze continued, because it came from core 0.
- **Test:** `tests/test_yaz0_native.py` extracts, at test time, the loop from the locally generated `main_230.c` (nothing private enters the repository) and compares the game's loop with the native one, alongside an independent Python decoder. It covers literals, short, long and overlapping copies, and a size smaller than the stream. With a length error or without the flags, the test fails.

**Preemption on indirect calls (`toolchain/core_preempt.py`, default; `MK8_NO_PREEMPT=1` turns it off).**
- **Check point:** `rt_dispatch`, through which every indirect guest call passes (virtuals, function pointers). It reads a global flag, set only while some core has a thread waiting with better priority than the owner's.
- **Slow path:** does the same as `rt_svc` (`core_check` under `g_klock`), with `trylock` so as never to wait for the lock there.
- **Why it is safe:** it remains one thread per core at a time. A better-priority thread taking the core on an indirect call is an interleaving that Horizon allows at any instruction, and a guest call could already yield the core on the next kernel call.
- **Test:** `tests/test_core_preempt.py` runs the real core functions of the prepared `kernel.c` with real threads. The wait of the better-priority thread drops to 0.0 ms (it was ~220 ms), with no overlap on the core. With equal priority or turned off, it waits for the whole job.

A/B on the same build (`b7180f73…`), cache `race-grand-prix-xh0dhyxh`, end of race 1:

| Run | Worst frame in the transition | Frames > 50 ms (sum) | Core wait | Race 3000–14000: > 20 ms |
| --- | --- | --- | --- | --- |
| `yscjaxvn` / `par8c_om`, off | 533 / 564 ms | 1,458 / 1,464 ms | 262 / 264 ms | 30 / 38 |
| `5p3rtv2d` / `3c3pphsa`, on | 337 / 300 ms | 760 / 685 ms | none | 7 / 7 |

The freeze dropped by half and the small 20 ms+ stumbles during the race also decreased. The average remains 60 FPS. The frame 15600 capture shows race 2 starting normally.

**Still remaining:** ~190 ms of game work and the first frame of the new screen, ~300 ms, of which ~250 ms is translation of the lists and prefetch.

**Full GP with everything on** (preemption, native Yaz0 and translation cache), `race-grand-prix-2avu_1go`, against the previous GP `race-grand-prix-xh0dhyxh`:
- **Stability:** exit 0 in 1,086 s, no `FATAL`/exception and no core wait recorded.
- **Yaz0:** 142 files decompressed natively, 579 MB in 1.3 s.
- **Races:** 59.95–60.00 FPS and p99 17.56–17.59 ms in the four.
- **Transitions:** before, each had three frames of 294–614 ms, with 455–613 ms stalled in `other`. Now they are one or two frames of 202–375 ms, with at most 219 ms in `other`. Frames 15999/31999/47999 are captures of the test itself and are left out of the comparison.

Final runtime `--runtime-only`, candidate `c9b9aaa4…`. 80 related tests plus `test_native` passed, with no change in Rust. **Not validated:** a weaker machine, multiplayer, audio and manual control during the transitions. `gameplay_verified` remains false.

## Extended investigation: shaders, concurrency and measurement — 2026-11-10

`toolchain/shader_storage.py` applied by the normal preparation, with replacement limits checked. The program tables (4,096 slots) and pipeline tables (8,192 slots) had unbounded probing: a full table did not finish the search for a new key. They now grow before 75% occupancy, preserve the object pointers and check overflow/allocation. This avoids search saturation; **it does not limit the consumption of the shaders/pipelines themselves nor implement their eviction from the GPU**.

Repeated requests for the same pipeline while the program compiles now share one request. When the separate pending table is full, it returns NULL so the caller releases the candidate and leaves the build to the draw, instead of compiling a pipeline that could not be recovered. The race between worker and inline execution now decides who runs under the queue's mutex: no pointer is left that was taken from the queue but could still be taken and completed by another thread. The queue and duplication tests failed on the expected assertions when only this adaptation was disabled in temporary copies; pending saturation also reproduced a failure in the earlier code.

Periodic saving includes pipelines built before the write thread starts. The simulated-clock test confirms writes at 60, 120 and 310 seconds, respecting the minimum interval and the end of bursts. The `scripts/frame-stats.py` analyzer accepts negative residues from asynchronous accounting without losing the complete frame and reports invalid lines rejected; null/non-finite totals do not enter the FPS.

**Correction of the earlier evidence:** the compiler with `-O1` defines `NDEBUG`. Some native tests, including the newly added descriptor ones, did not restore `assert`; the earlier run of the 39 tests did not prove all the checks described below. `-UNDEBUG` was added to the affected commands and the suite repeated. The discard fixture was fixed to accept invalidation of views by pointer even after the image is retired, as the real code does, and now explicitly checks seven unique discards, with no null handle or duplication.

Current validation: extended suite with 46 tests, no ignored tests; includes the 11 descriptor tests with assertions active, 20,000 entries in each shader/pipeline table with collisions, 20,000 repeated requests, a full pending table and a deterministic race between threads. They are native executables with GPU-free fixtures, plus checks of the preparation and the analyzer. Full Windows build: 212 units and link completed. New SHA-256 of `local/performance-validation/host/mk8drecomp.exe`: `4377658d6bcd5919b98e6267af04314759d0ad074e9fa72f5476a3d66a696172`. The executable is a development one, with no game library; it is not an independent playable package.

No match or VRAM/FPS measurement was run in this round. Qualification on a real GPU, pipeline caches on restart and retention of lists during long sessions remain pending. `MK8_RELEASE_POOL_TEXTURES` remains off by default and `gameplay_verified` remains false. Use the script below on the other machine, including one run with a cold cache and another with a warm cache, same resolution and tracks; repeat for at least one hour, recording transitions and checking whether memory stabilizes or grows with each cup.

## Descriptor release and validation without the game — 2026-11-10

The ownership fixes found in the audit below were implemented in the local sources. `toolchain/descriptor_lifetime.py` is applied by the host's normal preparation; it does not require enabling an experiment. Finalizing texture or sampler pools zeroes the guest handle and releases its reference, keeping the object while recorded/copied commands or the executor still use it. Prefetch borrows the references, without duplicating them. Destruction of the entries happens on the translation thread; the texture backends keep using the existing deferred GPU discard. The synchronous mode drains retired pools before running new submissions.

The cubemap history went from 64 static slots to per-pool records, removed when their last reference ends. It still preserves the cubemap when the game reuses the slot with a 2D texture: the lighting fix was not turned off. The reset now waits for translation before touching the renderer, eliminates unfinished recordings, active/retired lists and executor references, and releases pools before invalidating the backends. Command buffers are tracked so that their lists without a memory range also take part in this discard. The 20,000-list deadline is not brought forward during normal gameplay.

The integration review found two additional missing references: the queued presentation and `g_last_presented`, used to re-present the last frame. Both now keep the texture until its last use; re-presenting the same texture retains before releasing. Also fixed was the read of `it->handles` before checking whether the allocation of `it` failed. Invalid sizes and allocation failures of pools/entries stop with a diagnostic, without returning fictitious success. `MK8_TRACE_VRAM` now reports live pools, finalized ones still retained, references, pending discard and bytes of entries in the host; that last counter does not represent VRAM.

**Validation without ROM/save:** suite of the area with 39 tests, including 11 new native descriptor tests. They cover a finalized pool behind a blocked presentation, destruction on the correct thread, copied commands and persistent state, prefetch without an extra reference, 130 simultaneous pools, 200 cycles of creating/finalizing textures and samplers, reuse of the guest address while the previous pool is still recorded, eight threads/80,000 retain/release pairs, reset of the four categories of lists, repeated finalization/reset, queued/repeated presentation, synchronous mode and injected allocation failures. The failure test distinguishes the explicit diagnostic from a null-access exception. The suite also covers image aliases, shader budget, fences, pacing and cache saving that already existed.

Full Windows host build completed: 212 units and link, including the consumers of the modified `NvnDescPool` structure. Local validation artifact: `local/performance-validation/host/mk8drecomp.exe`, SHA-256 `d1203d7b632579098ddd819ebdc8ab95aaa267aaa6eca6c40a38580d3f70db4d`. It uses the development profile and does not contain the recompiled game library; it is neither an installer nor an independent replacement for a prepared installation. Upstream checked by the manifest, with no change to the original sources. Python 3.12, compiler and local dependencies were provisioned for these tests; the tool limitation described in the earlier audit was overcome. No match run and no release published.

### Reproduce and validate on another machine

1. Run `scripts/setup-tools.ps1` to provision Python 3.12 and packages. With `local/python/Scripts/python.exe`, run `scripts/fetch-upstream.py smm2recomp`, `scripts/setup-native.py` and `scripts/setup-runtime-deps.py`, in this order, to obtain pinned sources, compiler and graphics dependencies. The Python setup alone does not install the three.
2. Run `local/python/Scripts/python.exe scripts/verify-graphics.py`. The command verifies upstream hashes and refuses success with missing prerequisites or ignored tests. It does not require commercial data.
3. On the development installation with the game already prepared, recompile through the existing path: `local/python/Scripts/python.exe toolchain/build.py --config <config.json> --local <data-folder> --runtime-only`. Use the real paths of that installation, without replacing saves or copying DLLs/EXE in isolation. Changing structures requires all host consumers to be recompiled, which the pipeline's header cache invalidates automatically.
4. For the diagnostic session, start through the launcher with `LARECOMP_VK_PROF=1`, `MK8_FRAME_TIMES=1` and `MK8_TRACE_VRAM=1` in the environment. Keep `MK8_RELEASE_POOL_TEXTURES` off and the other defaults. Play three consecutive cups in the same process, visit menus between them and test returning to the title/restart. Continuously check the image, lighting/reflections, audio, controller, timer and save. Also test Time Trials and, separately, split screen/multiplayer if they are used.
5. Keep each comparison's `game.log` before rotation. Analyze equivalent windows with `python scripts/frame-stats.py <game.log> --from <first-frame> --to <last-frame>`. Compare the same machine, tracks, resolution and copies of the same initial cache. Separate first visit/repeat and exclude captures. Record p95/p99, maximum, frames above 33.4/50 ms and active/reserved/dedicated/shared memory at each transition. Close the launcher and remove the diagnostic variables before the final measurement without instrumentation.

**Limits:** these tests prove the release paths exercised and the build, not a universal absence of leaks or visual stability. Pools that the game does not finalize and lists still referenced remain alive. The VMA reserve/fragmentation, the cost of first compilation and the draws omitted by the shader budget remain separate investigations; their defaults were not changed without race evidence. The experimental failure of `MK8_RELEASE_POOL_TEXTURES` remains without a demonstrated solution. Resets with a real GPU, prolonged sessions, multiple drivers, clock, audio and saves are still to be validated; `gameplay_verified=false`. There is no new FPS measurement or VRAM reduction in this round.

## VRAM and stutter audit — 2026-11-10

Static review of the checkout, without running the game. This copy does not contain `local/`, build, private logs from earlier sessions or the configured native compiler. The measurements below are historical evidence from this document, not new measurements. The full upstream fetch failed when renaming the temporary folder (WinError 5); permissions were not worked around. The files `objects.c`, `cmdbuf.c` and `vulkan/textures.c` of the pinned commit `9dd1858fe8f4688ae5daa390c5e18d027300ada6` were read remotely, with SHA-256 checked against the manifest after LF/CRLF normalization. No runtime or default was changed.

Findings, in order of investigation:

1. **High — finalization of descriptor pools without release.** In the pinned upstream, `runtime/nvn/objects.c:554–571`, `desc_pool_init` allocates the pool and its entries, but `nvnTexturePoolFinalize` is empty. No adaptation in `toolchain`/`runtime/mk8` replaces this finalization. `toolchain/texture_lifetime.py` adds a strong reference per registered entry; that reference only drops when the slot is overwritten. Finalizing a pool does not release its entries, references or record. This leaves textures and potentially Vulkan images alive after the owner's finalization. It is a concrete lifecycle failure; its contribution to the reported race still needs tracing, since the historical session `ddpqpo1_` did not record finalization of a descriptor pool. Do not confuse this pool with `nvnMemoryPoolFinalize`, already handled by the earlier fixes.
2. **High — cubemap history without discard.** `toolchain/texture_pool.py:30–65` keeps tables for up to 64 pools and one reference per historical cubemap. There is no pool removal, table cleanup or reset hook for that history. Replacing the slot with a 2D texture deliberately preserves the cubemap to avoid the flicker already diagnosed; this also prolongs its life. After 64 distinct pools with history, creating another history fails silently. The current tests cover slot replacement, but not finalization/reset or saturation. The fix needs to preserve the references consumed by asynchronous translation; disabling the history indiscriminately reintroduces the lighting failure.
3. **Medium — command discard by count, not by completion of use.** `cmdbuf.c:63–88` keeps retired lists for more than 20,000 new lists, and `texture_lifetime.py` preserves that mechanism. `runtime/mk8/graphics/command_texture_refs.inc` only releases its textures when the list is discarded. This can keep resources after returning to the menu if list production decreases; the limit is in lists, not bytes. It is potential retention, not proof of unbounded leakage. Before replacing the deadline, track the last use by reusable commands, translation tickets and GPU completion; simply reducing 20,000 does not prove safety.
4. **High for visual fidelity — the shader budget omits draws.** `toolchain/shader_budget.py:43–100` uses 4 ms per frame by default and returns without the draw when compilation was not ready. It may explain objects/effects that momentarily disappear even at 60 FPS. Eligibility uses `TexBackend.scalable` as an approximation of a target redrawn every frame; that property does not prove the content is disposable. Other targets and compute continue waiting completely. Therefore, 4 ms is not a limit on frame duration nor does it eliminate compilation pauses. Separate historical evidence: Water Park had 343 ms waiting for a shader; repetitions with cache reduced those spikes.
5. **Medium — the VMA reserve is not a VRAM limit.** `toolchain/host.py:243` prefers 128 MiB blocks, without establishing a total budget. In the historical full GP `dka1cgd3`, heap 0 ended in the menu with 1,648 MiB reserved and 923 MiB active. There is excess reserve and resources still active; the numbers do not, on their own, demonstrate a leak or GPU exhaustion. `toolchain/vram_trace.py` clarifies that the VMA estimate does not use `VK_EXT_memory_budget`; shared memory also includes the imported guest heap. Data is missing to attribute the late stutters specifically to VRAM pressure/residency.
6. **Risk already demonstrated — aggressive discard did not solve the long session.** `toolchain/pool_release.py` keeps `MK8_RELEASE_POOL_TEXTURES` off by default. The historical comparison raised the reserved peak from 2,272 to 2,528 MiB and, on the fourth track, showed 50.91 against 59.52 FPS, with an increase in `SyncWait` even without new compilations. Do not promote this option as a solution for VRAM. The cause of that regression remains open.

Next validation: instrument creation/finalization and references per pool (descriptors, history, commands and guest owner), bytes of images and pending discard, together with frame times and shader/prefetch/SyncWait times. Repeat the same cup at least three times in the same process, recording track entry/exit and menu; use the same initial cache in the comparisons. Separate first visit from repeat, captures from timed windows, and active memory from reserved/dedicated/shared. Prioritize the pool ownership fix before any age-based eviction. Validate finalization with work still in the queue, multiple aliases, reset and more than 64 pool cycles. There is no validated performance fix nor new gameplay qualification in this audit.
