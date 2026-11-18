# FSR 3.1.6 / 4.1.2 — branch `feature/fsr`

Separate from the Linux port. The provider is optional and host-side; no guest
code or ABI changes.

## Working prototype (2026-11-18)

- `runtime/mk8/graphics/fsr.cpp` loads an FFX upscaler provider, creates a
  context for the game image and the window, and records one upscale in the
  present command buffer.
- `toolchain/fsr.py` adds the present call, tracks the last scene depth target,
  and enables `VK_KHR_cooperative_matrix` + `VK_EXT_shader_float8` when the
  driver exposes them.
- The FSR 4.1.2 provider from the custom SDK passes its 11 provider cases
  byte-identically on the RX 9070 XT.
- In-game evidence: `[fsr] provider FSR 4.1.2`, `[fsr] upscale active
  1920x1080 -> 2560x1440`, correct colors with
  `FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB`; screenshots `fsr4-srgb-03600.png`
  and `fsr4-3600` in `local/qualification`. The dispatch now runs after
  `vk_frame_suspend`, outside the window render pass.

The switch is diagnostic only: `MK8_FSR4=1`, `MK8_FSR_LIB=<provider.so>`,
`FSR4_ASSET_DIR=<assets>`. It is not exposed in the launcher yet.
`MK8_FSR4_CAPTURE=1` enables the pass-end depth research path; it is off by
default because it runs a copy on every depth pass.

## Performance at native internal resolution

The 50% test (`LARECOMP_SCALE=0.5`) was only a way to force a large upscale;
it makes the scene genuinely 960x540. At the default internal resolution
(1920x1080 -> 2560x1440), one early-race section measured:

| run | frame ~2340 |
|---|---:|
| FSR 1 / bilinear (FSR4 off) | 56.4 FPS |
| FSR 4.1.2 (neutral depth/MV) | 48.0 FPS |

The provider benchmark measures 1.28 ms GPU at 1280x720 -> 2560x1440 and
2.11 ms at 1920x1080 -> 3840x2160, so the ~3-4 ms in-game difference is the
provider plus the extra command-buffer work, not a 3x slowdown. The late-race
drop to 20-26 FPS is also present with FSR off (12-22 FPS in the same
section), so it is the game's existing late-race degradation, not FSR.

**FPS caveat:** Astro can run on the same GPU on this desktop. Any in-game FPS
number taken while another GPU application is active is not a valid
comparison; use the isolated provider benchmark for FSR cost and only compare
FPS runs on an otherwise idle desktop.

## Not qualified / open

- **Real depth:** the pass-end hook captures a 16:9 scene depth target
  (2304x1296 observed) into a persistent R32F image and resamples it to the
  presented 1920x1080 color. The log shows `[fsr] capture scene depth
  2304x1296` and `[fsr] depth now game (1920x1080 fmt 100)`; the frame at
  `fsr4-capture3600-03600.png` uses that depth. `MK8_FSR4_CAPTURE=1` enables
  the capture; it is off by default until its cost is measured.
- **Motion vectors/jitter:** active. The main draw's `vcb3` provides a 3x4
  view matrix at offset 0 and a 4x4 projection at offset 28; `fsr.cpp` keeps
  the current/previous view-projection, reconstructs world positions from the
  captured scene depth in a compute pass, and writes camera motion vectors in
  pixel space. A Halton (2,3) jitter is applied to the host copy of the scene
  projection, leaving guest memory and UI/2D untouched; FSR receives the same
  offset in `jitterOffset`. Logs: `[fsr] view/proj captured in main draw`,
  `[fsr] depth now game (1920x1080 fmt 100)`, `[fsr] motion now camera`; frame
  `fsr4-jitter2400-02400.png` uses all three inputs. Object motion (karts) is
  not in the vectors yet.
- **FSR 3.1.6:** the MIT SDK source builds a provider with the local
  permutation-table workaround, but `ffxCreateContext` for version 3.1.6 fails
  or stalls before a provider query (`[fsr] requesting version 0xc01006`, then
  no dispatch). The provider is not integrated; a correctly generated Windows
  SDK/permutation set or a fixed Linux generator is required. The context
  attempt now logs `[fsr] create failed 0x1` (generic FFX error) and stops
  retrying. The available provider source is the `rexglue/FidelityFX-SDK` fork
  at FSR **3.1.4**, not 3.1.6; requesting 3.1.4 also fails with 0x1, so the
  permutation/blob set is the blocker, not just the version. The official
  v2.4.0 GitHub tag contains only `Kits` (no `sdk` backend source or generated
  headers), and the public forks commit only the `#include` lines, not the
  generated `*_permutations.h` files. FSR 4.1.2 remains the only working
  version.
- **Frame generation:** FSR FG 3.1.6 source is available; native Linux FG 4.x
  is a research spike (the signed DLL contains 487 SPIR-V candidates, 359 of
  which validate, but the pass/model ABI is not reconstructed).
- **Windows/Proton:** not wired yet. The Windows path should load AMD's signed
  upscaler/FG DLLs from the user's SDK; no signed binaries or extracted assets
  are committed.

## Licensing

- The FFX API headers under `runtime/mk8/graphics/ffx` are MIT (AMD notice in
  `LICENSE`).
- The FSR 4.1.2 native provider is the GPL-3.0 port from the custom SDK; it is
  loaded as an optional component and is not linked into the game library.
- AMD signed DLLs and extracted shaders/models are never committed.
