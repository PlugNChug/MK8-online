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
default because it runs a copy on every depth pass. `MK8_FSR_VERSION=<name>`
selects a provider version through `ffxOverrideVersion`.

`MK8_FSR_LEGACY_ABI=1` builds the context with the deprecated 1.x Vulkan
backend descriptor. The ABI belongs to the provider build, not to the upscaler
version: the 3.1.6 provider built from the 2.4 SDK needs the current
descriptor, while the GPL 4.1.2 port (patched to accept the legacy layout)
needs the switch. Selecting `MK8_FSR_VERSION=3.1.6` no longer changes the
descriptor by itself.

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
- **FSR 3.1.6 (Windows provider, verified via Wine):** on the RX 9070 XT,
  AMD's signed 2.4 `amd_fidelityfx_upscaler_vk.dll` advertises `4.1.2`,
  `3.1.6` and `2.3.4` through `ffxQuery`. The 32-bit
  `ffxCreateContextDescUpscaleVersion` field is
  the API version the application was built against and must stay
  `FFX_UPSCALER_VERSION`; selecting another provider version requires the
  64-bit `ffxOverrideVersion` id returned by the query. A standalone Wine
  probe created a 3.1.6 context, reported `name=3.1.6`, dispatched, and wrote a
  full 1920x1080 output (8,274,240 of 8,294,400 bytes non-zero). `fsr.cpp` now
  performs the same query/select and logs `[fsr] selecting version 3.1.6
  (0x...)`. Requesting `0xc01006` through the old 32-bit field was the cause of
  the earlier `create failed 0x1`, not a shader-permutation defect in the
  signed provider.
- **FSR 3.1.6 / 2.3.4 on native Windows, NVIDIA RTX 5080 (20/11/2026):** the
  same signed DLL was run natively (not Wine) through
  `tools/windows-fsr/build.sh` (mingw-w64, `-static`) on a Windows 10 host
  with an RTX 5080, driver 621.01, Vulkan 1.4.357. `ffxQuery` lists only
  `3.1.6` (`0xf5a5ca1e00c01006`) and `2.3.4` (`0xf5a5ca1e00803004`); `4.1.2`
  is **not offered on this GPU**. Both listed providers create, dispatch and
  destroy at 1280x720 -> 2560x1440 and 1920x1080 -> 2560x1440 (`rc=0`; 14,712,960
  and 14,711,840 of 14,745,600 bytes non-zero, all varied; output corners
  match the input gradient). Forcing the FSR 4.1.2 id
  `0xf5a5ca1e01001002`, which is present in the DLL, returns `NO_PROVIDER`
  (rc 4) with no message even with `ffxConfigure(GLOBALDEBUG)` verbose. AMD's
  SDK sample documentation says the version list is populated by a query, so
  "the entries available depend on the installed driver"; FSR 4 upscaling
  requires RDNA 4, so the omission is the documented device/driver gate, not a
  probe failure. Commands and exit codes are in
  `tools/windows-fsr/README.md`; PNGs are under
  `$HOME/.cache/mk8online/win-rtx/evidence/` (never committed). No GPUVM
  fault, `VK_ERROR_DEVICE_LOST` or validation error was reported; the host
  has no Khronos validation layer installed.
- **FSR 3.1.6 (native Linux):** built and verified. The private provider is the
  SDK 2.4 Vulkan host compiled with the 3.1.6 pass shaders extracted from
  AMD's signed DLL; it advertises only `3.1.6` (`0xf5a5ca1e00c01006`). On the
  captured 960x540 input it matches the signed Windows 3.1.6 output byte for
  byte: MAE 0, max 0, PSNR 168.13. Standalone cost at 1920x1080 -> 2560x1440 is
  **0.551 ms GPU** (30 dispatches, RX 9070 XT/RADV). In-game it logs
  `[fsr] selecting version 3.1.6`, `[fsr] provider 3.1.6`,
  `upscale active 1920x1080 -> 2560x1440`, then `depth now game` and
  `motion now camera`; see `local/qualification/fsr316-*.log`, the menu frame
  `fsr316-final-01000.png` and the in-race frame `fsr316-race-02400.png`. A
  same-frame A/B against the GPL 4.1.2 provider at frame 1000
  (`fsr412-cmp1000b-01000.png`) shows the same image. The provider, the
  extracted blobs and AMD's
  signed DLLs stay private (`local/qualification/fsr316-native/`) and are
  never committed. In-game FPS numbers taken while other builds or GPU
  applications share the machine are not evidence; the profiler showed the
  frame stalls were CPU starvation, not upscaler cost.
- **GPL 4.1.2 provider (legacy ABI):** advertises only `4.1.2` and needs both
  `MK8_FSR_LEGACY_ABI=1` and `FSR4_ASSET_DIR` (its ML models live outside the
  provider); without them the context creation fails with `0x3`. Asking for
  `MK8_FSR_VERSION=3.1.6` logs that the provider does not offer it and
  continues on 4.1.2.
- **Frame generation:** FSR FG 3.1.6 source is available; native Linux FG 4.x
  is a research spike (the signed DLL contains 487 SPIR-V candidates, 359 of
  which validate, but the pass/model ABI is not reconstructed).
- **Windows/Proton:** the FFX loader/upscaler DLL path is verified at the FFX
  API level under Wine (context, provider name, dispatch and output) and now
  natively on Windows with an NVIDIA RTX 5080 (same API checks; see above),
  but this checkout has no Windows game build/data pair for an in-game run on
  that host. No signed binaries or extracted assets are committed.

## Licensing

- The FFX API headers under `runtime/mk8/graphics/ffx` are MIT (AMD notice in
  `LICENSE`).
- The FSR 4.1.2 native provider is the GPL-3.0 port from the custom SDK; it is
  loaded as an optional component and is not linked into the game library.
- AMD signed DLLs and extracted shaders/models are never committed.
