# MLFI and FSR 3 FG in-game quality notes

Status 20/11/2026. Companion to `docs/MESA-RACE-CRASH.md` (the Mesa 27 workaround that made these runs
possible), `docs/FSR4-MLFI.md` and `docs/MLFI-PROVIDER.md`.

Short answer: **both providers ran in a real race on Mesa 27** — FSR 3.1.7 at 1280×720 and the native
MLFI 4.0.2 provider at 1280×720 (191 model + 1 compose dispatches per generated frame, 1,800 generated
frames logged) — and the game's real render images are clean. The game's capture path reads the
replacement-buffer **real** frames, not the provider's composed output, so this document cannot judge
ghosting or detail of the generated frames themselves; that remains unverified.

## 1. Runs

All on RADV GFX1201, Mesa 27.0.0-devel (`f7dd61f78f8`), with `toolchain/radv_epilog.py` enabled (see
`MESA-RACE-CRASH.md`), 2,400-frame script, screenshot at frames 1,200 and 2,400, exit 0.

| tag | mode | window/swapchain | provider log | screenshot |
| --- | --- | --- | --- | --- |
| `mlfi-720b` | `MK8_FSR_FG=fsr4` | 1280×720 | `MLFI effect provider 4.0.2`, `MLFI effect context: 1280x720 format 14`, `191 model + 1 compose` | `mlfi-720b-02400.png` |
| `fsr3-720b` | `MK8_FSR_FG=fsr3` | 1280×720 | `frame-generation effect provider 3.1.6`, `active: FFX frame-interpolation swapchain 1280x720` | `fsr3-720b-02400.png` |
| `fgoff-fix` | off | 2560×1440 | none | `fgoff-fix-02400.png` |

The 1280×720 window is a 640×360 logical window on the 2× Wayland output (SDL's high-pixel-density
surface is 1280×720); the MLFI provider's implemented layout is exactly 1280×720.

MLFI details from `mlfi-720b.log`:

```
[fsr_fg] MLFI device extensions enabled (cooperative_matrix_maintenance1, image_int64_atomics)
[mlfi-provider] pipelines: 195 created (191 model + 4 preprocess); bindings match capture
[mlfi-provider] context: MLFI 4.0.2 id=0xf600000001000002 display=1280x720 render=1280x720
[fsr_fg] MLFI prepare frame 0: 5 preprocess + 2 copy dispatches (resets 1)
[fsr_fg] MLFI frame generation: 191 model + 1 compose dispatches, frames 1, resets 1
[fsr_fg] MLFI progress: 600 generated frames, 191 model + 1 compose dispatches
[fsr_fg] MLFI progress: 1200 generated frames, 191 model + 1 compose dispatches
[fsr_fg] MLFI progress: 1800 generated frames, 191 model + 1 compose dispatches
[vk] saved .../mlfi-720b-02400.png (1280x720)
```

FSR 3 details from `fsr3-720b.log`: `[fsr_fg] frame-generation effect provider 3.1.6`,
`[fsr_fg] active: FFX frame-interpolation swapchain 1280x720 format 44 (present family 1)`, screenshot
at 2,400, exit 0.

## 2. Real-frame A/B

`mlfi-720b-02400.png` and `fsr3-720b-02400.png` are the same race-start view ("1st Race", Mario Kart
Stadium): karts, track, grandstand, MKTV screens, HUD text and the finish-line checkers are present in
both. The kart line-up differs between the runs because the automated script and save state are not
frame-identical; these are not a pixel A/B of one frame. No tearing, missing UI, colour corruption or
obvious compositing seams were observed in either real frame; the MLFI real frame also shows no
artifact from the provider's input copies.

`fgoff-fix-02400.png` (FG off, 2560×1440) confirms the same game content without any provider.

These screenshots do **not** contain the provider's generated frames. The runtime's
`LARECOMP_WINDOW_SHOT` path copies the game's replacement-buffer image before the provider presents the
real+generated pair to the real window swapchain. The MLFI provider composes the generated image into
the provider's internal output; the bridge has no readback of that image, and the provider's optional
`MK8_MLFI_DUMP_DIR` readback belongs to the headless `mlfi_provider_test` (synthetic inputs), not the
in-game provider. A compositor/window capture or a new provider readback hook is needed to see
generated frames. No claim about generated-frame ghosting, detail or latency is made.

## 3. What this does and does not establish

- Established: the MLFI 4.0.2 provider creates its 195 pipelines and 196 descriptor sets, runs the
  191-model + compose graph in a real race, and the game presents/continues to the screenshot with
  exit 0; FSR 3.1.7 also runs the race at the same 1280×720; the Mesa 27 race crash is worked around.
- Not established: generated-frame image quality (no generated frame is in the captures), generated-vs-
  real pacing/latency, a real 120 Hz presentation, a full race/restart/save, or vendor-identical MLFI
  output. `MLFI-PROVIDER.md` already notes that the provider's final compose is this project's
  reconstruction (AMD's final `main` was not captured), so bit-identical quality is not expected.
- The MLFI provider is fixed to 1280×720; 2560×1440 MLFI is outside this milestone. FSR 3 FG was
  validated at 1280×720 here; the earlier 2560×1440 FSR 3 run was stopped before the race (host load),
  so no 2560×1440 FG race claim is made. `fgoff-fix` at 2560×1440 is FG off.
