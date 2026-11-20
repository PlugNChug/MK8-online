# DLSS Super Resolution / DLAA and Frame Generation: implementation notes

Status 20/11/2026. This covers phase 1 (DLSS Super Resolution / DLAA) and phase 3
(DLSS Frame Generation, DLSS-G) from [DLSS-XESS-NEURAL.md](DLSS-XESS-NEURAL.md). Both
are optional NGX Vulkan bridges; both are **compile-tested and loader/ABI-tested only**.
This host has an AMD RX 9070 XT (RADV) and no NVIDIA GPU, so feature creation and
evaluation were not executed and no image was produced. `gameplay_verified` stays false.

The implementation is deliberately small and fail-closed: no vendor binary is
committed, nothing is linked against NGX, and every failure returns `VK_NULL_HANDLE`
(SR) or a zeroed frame with a reasoned one-time log (DLSS-G), so the existing path runs.

## Files

| File | Role |
| --- | --- |
| `runtime/mk8/graphics/dlss.cpp` | loaders, NGX init, SR feature, DLSS-G feature, evaluate, present helpers |
| `runtime/mk8/graphics/dlss.h` | probe/status API, `mk8_dlss_upscale`, `mk8_dlss_fg_*` |
| `runtime/mk8/graphics/ngx/mk8_ngx_abi.h` | hand-written minimal NGX Vulkan ABI, including the DLSS-G structs (see licences below) |
| `runtime/mk8/graphics/fsr.cpp` | one dispatch hunk: `MK8_DLSS=1` calls the SR bridge before the FSR path |
| `runtime/mk8/graphics/fsr_fg.cpp` | DLSS-G two-image present path when `MK8_DLSS_FG=1` and FSR FG is off |
| `toolchain/dlss_fg.py` | gated low-latency device/swapchain patches, depth/matrix gates, device accessor |
| `tests/test_dlss.py` | native loader failure paths, real-plugin dlopen, SR and DLSS-G ABI static asserts |
| `tests/test_dlss_fg.py` | toolchain patch idempotence/gating and `-fsyntax-only` compile of the patched runtime |

`toolchain/host.py` registers `toolchain/dlss_fg.py` immediately after
`toolchain/fsr_fg.py`; the host build already compiles every `runtime/mk8/**/*.cpp`. The SR
dispatch lives in `fsr.cpp`, and `fsr_fg.cpp` now contains both the existing FSR path and the
gated DLSS-G branch. `device.c` is patched through the toolchain, not edited upstream.

## Licences: why the NVIDIA headers are not vendored

The premise that the NGX headers are MIT-like is wrong for v310.9.16. Every file in
`include/` carries a proprietary SPDX tag (the copyright years vary per file, e.g.
2018-2023 in `nvsdk_ngx.h` and 2019-2026 in the loader):

```text
SPDX-FileCopyrightText: Copyright (c) 2018-2026 NVIDIA CORPORATION & AFFILIATES...
SPDX-License-Identifier: LicenseRef-NvidiaProprietary
```

and the SDK licence forbids distributing the SDK as a stand-alone product and using it
in a way that subjects it to an open-source licence. Committing those headers would be
exactly the bundling this project forbids (and would contradict
[DLSS-XESS-NEURAL.md](DLSS-XESS-NEURAL.md): "no vendor code is integrated in `runtime/`").

`ngx/mk8_ngx_abi.h` is therefore **not a copy**: it declares only the facts the bridge
uses — entry-point names, numeric enum values, parameter strings, and field order for
the five structs passed across the boundary. The downloaded SDK remains the normative
reference, and `tests/test_dlss.py` compiles a translation unit that includes both
headers and `static_assert`s size and `offsetof` equality for every struct. That test
passes against v310.9.16. The `.so` files are likewise never committed; they stay under
`$XDG_DATA_HOME/mk8online/nvidia/` with the fetched licence text.

## Exact API sequence implemented

`mk8_dlss_upscale(cb, color, depth, rw, rh, ow, oh, frame_ms)` runs inside the existing
`post_process` hook, after `vk_frame_suspend()` and before the FSR/SMAA passes.

1. **Probe (no Vulkan).** Resolve the plugin directory from `MK8_DLSS_DIR`, else
   `$XDG_DATA_HOME/mk8online/nvidia` (fallback `~/.local/share/...`). Pick
   `libnvidia-ngx-dlss.so*` (highest version first; `dlssd`/`dlssg` are excluded).
   Validate the ELF64 x86-64 `ET_DYN` program headers so a truncated file cannot
   SIGBUS inside `dlopen`, then `dlopen(RTLD_NOW|RTLD_LOCAL)` and check
   `NVSDK_NGX_VULKAN_CreateFeature1` (or `CreateFeature`), `EvaluateFeature`,
   `ReleaseFeature`. Load the driver core `libnvidia-ngx.so.1` (then
   `libnvidia-ngx.so`, then a copy beside the plugin) and require
   `NVSDK_NGX_VULKAN_Init_with_ProjectID`, `GetCapabilityParameters`,
   `Shutdown1`, `NVSDK_NGX_Parameter_SetUI/SetI/SetF/SetVoidPointer`, `GetUI`.
   `LICENSE.txt` beside the plugin is reported, never required.
2. **Feature discovery.** `NVSDK_NGX_VULKAN_GetFeatureRequirements(instance,
   physicalDevice, &discovery, &requirement)` with
   `NVSDK_NGX_Feature_SuperSampling`, the project id and a `FeatureCommonInfo` whose
   `PathListInfo` is the plugin directory. A non-zero `FeatureSupported` bitfield is
   decoded (feature missing / driver too old / GPU unsupported / OS too old / not
   implemented) and aborts before init.
3. **Required extensions (diagnostic).** If the core exports
   `NVSDK_NGX_VULKAN_RequiredExtensions`, log the instance/device extension names.
   The device already exists at this point, so the bridge cannot enable them; it says
   so and lets `Init` fail cleanly. This is the first item to check on an NVIDIA host.
4. **Init.** `NVSDK_NGX_VULKAN_Init_with_ProjectID(projectId, CUSTOM, "mk8-online 4.0.1",
   wideDataPath, instance, physicalDevice, device, vkGetInstanceProcAddr,
   vkGetDeviceProcAddr, &common, 0x15)`. The project id is a self-supplied GUID shape
   (`MK8_DLSS_PROJECT_ID` overrides it); the data path is
   `$XDG_DATA_HOME/mk8online/nvidia/ngx-data` (created if missing).
5. **Capability.** `NVSDK_NGX_VULKAN_GetCapabilityParameters(&params)`, then
   `GetUI(params, "SuperSampling.Available")` (fallback `GetULL`). Anything other than
   1 aborts with that exact value.
6. **Create.** Set `Width/Height/OutWidth/OutHeight`, `PerfQualityValue`,
   `DLSS.Feature.Create.Flags`, `DLSS.Enable.Output.Subrects = 0`,
   `CreationNodeMask = VisibilityNodeMask = 1`, then
   `NVSDK_NGX_VULKAN_CreateFeature1(device, cb, NVSDK_NGX_Feature_SuperSampling,
   params, &handle)`. Quality is derived from `rw/ow`: DLAA when render equals display,
   then ultra-quality, quality, balanced, performance, ultra-performance. Depth is
   assumed standard Z; `MK8_DLSS_DEPTH_INVERTED=1` sets the NGX inverted-depth flag.
7. **Evaluate (per frame).** Set `Color`, `Output`, `Depth`, `MotionVectors` to
   `NVSDK_NGX_Resource_VK` image views, `Jitter.Offset.X/Y` (same Halton values FSR
   uses; `MK8_DLSS_NO_JITTER=1` forces zero), `MV.Scale.X/Y = 1` (vectors are already
   render-pixel deltas), `Sharpness = 0`, `DLSS.Pre.Exposure = 1`,
   `FrameTimeDeltaInMsec`, `DLSS.Render.Subrect.Dimensions.*`, and `Reset = 1` on the
   first frame, on a resolution change or whenever the motion vectors are zeros.
   Then `NVSDK_NGX_VULKAN_EvaluateFeature(cb, handle, params, NULL)` and return the
   display-size output view. The colour image is left in `GENERAL` afterwards, as the
   FSR path does.
8. **Resize.** Release the feature, destroy the output/neutral-depth/motion/view
   caches and recreate on the next frame. There is no process-teardown hook in this
   runtime; NGX is shut down on fatal init errors and the OS reclaims the rest.

### Inputs reused, and their actual state

| Input | Source | State |
| --- | --- | --- |
| Colour | the presented game image, render resolution | composed image, post-tonemap/UI (same limitation as FSR) |
| Depth | captured scene depth (`MK8_FSR4_CAPTURE`) or a neutral R32F cleared to 1.0 | logged as `captured scene depth` / `neutral`; the bridge logs when it changes |
| Motion vectors | recomputed here with fsr.cpp's depth/invVP/prevVP algorithm | `camera-derived` or `zeros`; karts have no object vectors |
| Jitter | `g_fsr_jitter_x/y` from the same main-draw scan | passed as-is; whether the engine already baked it into the projection is unverified |
| Reset | first frame / resize / zero MVs | also `MK8_DLSS` mode changes recreate |

The FSR bridge's motion image is private to `fsr.cpp`, so this file duplicates the
~100-line camera-vector compute rather than growing `fsr.cpp` beyond the allowed
dispatch hunk. Both are camera-only; object motion vectors (phase 2 of the plan) are a
prerequisite for judging DLSS quality and are not in this change.

## Environment

| Variable | Meaning |
| --- | --- |
| `MK8_DLSS=1` | enables the bridge; the FSR path below stays untouched |
| `MK8_DLSS_DIR` | directory with the plugin/core (default `$XDG_DATA_HOME/mk8online/nvidia`) |
| `MK8_DLSS_LIB` | explicit plugin path (tests, unusual installs) |
| `MK8_DLSS_CORE` | explicit NGX core path (default `libnvidia-ngx.so.1`) |
| `MK8_DLSS_PROJECT_ID` | override the self-supplied GUID-shaped project id |
| `MK8_DLSS_DEPTH_INVERTED=1` | set `NVSDK_NGX_DLSS_Feature_Flags_DepthInverted` |
| `MK8_DLSS_NO_JITTER=1` | pass `0,0` instead of the FSR Halton jitter (diagnostic) |

Recommended run for a real NVIDIA machine (the capture hooks in `toolchain/fsr.py` are
gated on the FSR variables; that file is owned by the FSR work and was not edited):

```sh
MK8_FSR4=1 MK8_FSR4_CAPTURE=1 MK8_DLSS=1 ./mk8drecomp
```

`MK8_FSR4=1` only enables the depth capture and the view/projection scan; the DLSS
dispatch comes first and returns before the FSR4 provider is touched. Without the
capture variables, DLSS still runs but logs `neutral` depth and `zeros` motion and
resets every frame, which is not a quality result.

Reaching the hook at all also depends on the upstream present code: `post_process`
returns before the patched anchor when neither SMAA nor the FSR1 upscaler is enabled
(`if (!smaa && !fsr) return v;`). So the game must have its upscaler (FSR) or SMAA
selected. With FSR1 enabled and render smaller than the window, DLSS SR upscales; with
SMAA at native resolution the same hook gets `rw == ow` and selects DLAA. This is a
property of the existing anchor, not of the DLSS bridge, and changing it belongs to the
FSR/toolchain owner.

## Verified here

- `dlss.cpp` and the edited `fsr.cpp` compile with the runtime's C++17 flags.
- A fresh runtime-only host build in `local/dlss-fg/host` compiled every prepared unit,
  linked and produced `mk8drecomp`. The build applies `toolchain/fsr.py` and the other
  preparation steps to its own copy, so the present-hook anchor is the real one.
- `tests/test_dlss.py` (8 cases) exercises the compiled probe natively:
  no plugin, corrupted plugin, truncated real plugin, loaded library without NGX
  exports, directory/version selection, licence detection, core override, the fetched
  NVIDIA plugin, and the ABI static asserts.
- `tests/test_fsr.py` (5 cases) still passes; `toolchain/fsr.py` was not modified.
- The fetched `libnvidia-ngx-dlss.so.310.9.16` dlopens and exports the Vulkan entry
  points; on this host the probe then stops at the missing `libnvidia-ngx.so.1`.
- The exact unavailable line on this host:

  ```text
  [dlss] DLSS unavailable: no NGX core library (libnvidia-ngx.so.1): libnvidia-ngx.so.1: cannot open shared object file: No such file or directory; the core is installed by the NVIDIA driver, not by this project
  ```

  (With no plugin fetched, the first line is `[dlss] DLSS unavailable: no DLSS plugin
  (libnvidia-ngx-dlss.so*) in <dir>; fetch it with tools/fetch_vendor_sdk.py
  --accept-license nvidia`.)

### Evidence (20/11/2026)

```text
Runtime x86_64-linux-gnu: 218/222 unidades
Runtime x86_64-linux-gnu: 220/222 unidades
Runtime x86_64-linux-gnu: 221/222 unidades
Runtime x86_64-linux-gnu: 222/222 unidades
EXE .../local/dlss-fg/host/mk8drecomp

$ nm local/dlss-fg/host/mk8drecomp | grep -E ' T mk8_(dlss|fsr4)_upscale$'
0000000000355160 T mk8_dlss_upscale
000000000035dbe0 T mk8_fsr4_upscale

$ ldd local/dlss-fg/host/mk8drecomp | grep -i nvidia
(empty: NGX is loaded with dlopen, never linked)

$ sha256sum ~/.local/share/mk8online/nvidia/libnvidia-ngx-dlss.so.310.9.16
30982c937afed85c96b8d313323de9a650a2eec6de1bb0f714b21750f4ce98f6
(the pin in tools/fetch_vendor_sdk.py; 59,481,944 bytes)

$ nm -D --defined-only libnvidia-ngx-dlss.so.310.9.16 | grep NVSDK_NGX_VULKAN
NVSDK_NGX_VULKAN_CreateFeature
NVSDK_NGX_VULKAN_CreateFeature1
NVSDK_NGX_VULKAN_EvaluateFeature
NVSDK_NGX_VULKAN_GetFeatureRequirements
NVSDK_NGX_VULKAN_GetScratchBufferSize
NVSDK_NGX_VULKAN_Init
NVSDK_NGX_VULKAN_Init_Ext
NVSDK_NGX_VULKAN_Init_Ext2
NVSDK_NGX_VULKAN_PopulateParameters_Impl
NVSDK_NGX_VULKAN_ReleaseFeature
NVSDK_NGX_VULKAN_Shutdown
NVSDK_NGX_VULKAN_Shutdown1
```

`python -m unittest tests.test_dlss` reports 18 tests OK (the SR and DLSS-G cases
share one probe harness). The harness is built from the shipped `dlss.cpp` with
`-DMK8_DLSS_PROBE_ONLY`, so its failure paths are the real ones, not a
re-implementation.

## NOT verified

No NVIDIA GPU and no `libnvidia-ngx.so.1` on this host, so none of the following ran:

- `NVSDK_NGX_VULKAN_Init_with_ProjectID`, `SuperSampling.Available = 1`,
  `CreateFeature1`, `EvaluateFeature`, or any pixel output.
- Whether `device.c` already enables the extensions NGX lists; if not, `Init` returns
  `NVSDK_NGX_Result_FAIL_FeatureNotSupported` and the log names the extension list.
  Adding them requires a pre-device-creation hook in `device.c`/`toolchain/device.py`,
  which is outside this change.
- The image layout contract assumed here (inputs `SHADER_READ_ONLY_OPTIMAL`, output
  `GENERAL`). It mirrors the FSR bridge; NGX's Vulkan validation/logs are the check.
- The depth convention. The captured copy is the game's depth resampled to R32F; the
  bridge assumes standard Z until `MK8_DLSS_DEPTH_INVERTED=1` is needed. A/B on a
  scene with a known near/far order is required.
- Jitter correctness (whether the game's projection already contains the host's Halton
  offset) and DLSS quality with camera-only motion vectors.
- Windows: the `_WIN32` path (`LoadLibraryA`, `_nvngx.dll`/`nvngx.dll`,
  `nvngx_dlss.dll`) is implemented but **not compiled** in this checkout, which has no
  Windows dependency tree (`tools/deps` and the zig MINGW toolchain are absent).

## DLSS Frame Generation (phase 3): implemented, not executed

Status 20/11/2026. The DLSS-G library is implemented and **compile/loader/ABI tested**;
no NVIDIA GPU is present, so feature creation, evaluation and pacing were not executed.
It is **off by default**: `MK8_DLSS_FG` must be set (any non-empty value except `0`) and
`mk8_dlss_fg_init` must be called by the presenter. With the variable unset the module
has no effect: no constructor, no hook, and the existing FSR FG/upscaler/bilinear path
runs unchanged.

`MK8_DLSS` (Super Resolution) and `MK8_DLSS_FG` are **mutually exclusive in this
phase**. Both would call `NVSDK_NGX_VULKAN_Init_with_ProjectID` and share the capability
parameter map; separate maps plus a shared init are the next integration step. DLSS-G
does not need DLSS SR for a display-size backbuffer: the game's FSR1 upscaler (or a
future DLSS SR output) is enough.

### API sequence implemented

`mk8_dlss_fg_*` lives in `runtime/mk8/graphics/dlss.cpp`/`dlss.h`; the ABI additions
(`Mk8NgxDlssgCreateParams`, `Mk8NgxDlssgOptEvalParams`, the `DLSSG.*` parameter names)
are in `ngx/mk8_ngx_abi.h` and are static-asserted against the official
`nvsdk_ngx_defs_dlssg.h` / `nvsdk_ngx_params_dlssg.h` by `tests/test_dlss.py`.

1. **Probe.** `mk8_dlss_fg_probe` resolves `libnvidia-ngx-dlssg.so*` (highest version
   first; `dlss`/`dlssd` excluded), validates the ELF before `dlopen`, checks
   `CreateFeature1`/`CreateFeature`, `EvaluateFeature`, `ReleaseFeature`,
   `GetFeatureRequirements`, then loads the driver core and its Vulkan parameter
   entry points. Missing plugin/core, truncated files and an old plugin without the
   Create/Release ABI are rejected with a reason.
2. **Low latency gate.** `mk8_dlss_fg_init` requires `VK_NV_low_latency2` to be
   advertised by the physical device and the device to have been created with it
   (`vkSetLatencySleepModeNV`, `vkLatencySleepNV`, `vkSetLatencyMarkerNV` resolved
   through `vkGetDeviceProcAddr`). It calls `vkSetLatencySleepModeNV` with
   `lowLatencyMode = VK_TRUE` and fails closed on any error. The current `device.c`
   does **not** enable this extension, so a real run fails with the exact message
   "the device was created without VK_NV_low_latency2; enable the extension before
   vkCreateDevice (device.c)" until the hand-off below is applied.
3. **Init and capability.** `NVSDK_NGX_VULKAN_Init_with_ProjectID(..., 0x15)`, then
   `FrameGeneration.Available`. A zero is reported with
   `FrameGeneration.NeedsUpdatedDriver` and the required driver major/minor read from
   the same parameter map. `GetFeatureRequirements` for
   `NVSDK_NGX_Feature_FrameGeneration` is decoded with the same bit table as SR
   ("driver too old", "GPU unsupported", ...).
4. **Create.** `Width`/`Height` are the display size, `DLSSG.BackbufferFormat` is the
   swapchain `VkFormat`, `DLSSG.InternalWidth/Height` the render size and
   `DLSSG.DynamicResolution = 0`; then
   `NVSDK_NGX_VULKAN_CreateFeature1(device, cb, NVSDK_NGX_Feature_FrameGeneration,
   params, &handle)`. `FrameGeneration.FeatureInitResult` is logged on failure.
   A display-size interpolated output image is allocated (`STORAGE | SAMPLED |
   TRANSFER_SRC`) and primed to `GENERAL` in the init command buffer. The **real**
   frame is the caller's backbuffer; `OutputReal` is intentionally not set, so NGX
   does not touch it.
5. **Evaluate (per rendered frame).** Inputs: the display-size backbuffer, depth, MVs,
   jitter, row-major view (3x4) and projection (4x4), frame id, reset/pause flags.
   The module derives `cameraViewToClip`, `clipToCameraView`, `clipToLensClip`
   (identity), `clipToPrevClip` and `prevClipToClip`; camera position/up/right/forward
   from the view matrix; FOV/aspect from the projection when not supplied; jitter in
   clip space (`2 * pixels / render size`); and `mvecScale = 1/renderWidth,
   1/renderHeight` because the runtime's vectors are pixel-space. It sets
   `multiFrameCount = 1` / `multiFrameIndex = 1` (2x), `BackbufferFrameID`, the camera
   parameters and `Reset`, records `NVSDK_NGX_VULKAN_EvaluateFeature` into the caller's
   command buffer and restores the input layouts. `DepthInverted`, `ColorBuffersHDR`,
   `NotRenderingGameFrames` and `Reset` come from the caller/init.
6. **Copy and present.** `mk8_dlss_fg_copy_interpolated` copies the interpolated image
   into a second acquired swapchain image and transitions it to
   `PRESENT_SRC_KHR`. `mk8_dlss_fg_present` presents the real and interpolated images
   in order, with `VK_NV_low_latency2` PRESENT_START/PRESENT_END markers.
   `mk8_dlss_fg_latency_sleep` wraps `vkLatencySleepNV` for the simulation loop.
   **NGX does not present**: the module presents the two WSI images; the caller owns
   their acquisition, semaphores and queue submission.

### What is verified here

- A fresh host build of the wired tree in `local/dlss-fg-final/host` compiled all 222
  units and linked `mk8drecomp`; `mk8_dlss_fg_init/evaluate/copy_interpolated/present/active`
  and the existing `mk8_dlss_upscale`/`mk8_fsr4_upscale` are present, and `ldd` shows no
  NVIDIA link dependency (NGX is dlopen-only).
- `tests/test_dlss.py` runs 18 cases: all the SR cases plus the DLSS-G enabled gate
  (`unset = 0`, `1 = 1`, `0 = 0`), missing plugin, truncated real plugin, a library
  without NGX exports, an old plugin without Create/Release, version-selection/licence,
  the real pinned `libnvidia-ngx-dlssg.so.310.9.16`, the requirement-bit decoder, the
  DLSSG ABI static asserts, and a `-fsyntax-only` compile of the heavy path against the
  prepared runtime headers. `tests/test_dlss_fg.py` adds 4 patch/gating/compile cases.
- A short RADV run of the built runtime with `MK8_DLSS_FG=1` (Xvfb, 25 s) reached the
  swapchain and attempted DLSS-G. It logged:

  ```text
  [dlss_fg] VK_KHR_present_id enabled
  [dlss_fg] warning: VK_NV_low_latency2 is unavailable; DLSS-G will fail closed
  [dlss_fg] DLSS Frame Generation unavailable: no NGX core library (libnvidia-ngx.so.1): libnvidia-ngx.so.1: cannot open shared object file: No such file or directory; the core is installed by the NVIDIA driver, not by this project
  ```

  then presented the normal single-image path and kept running (exit 124 from the
  timeout). This is the expected fail-closed behaviour without an NVIDIA driver.
- `MK8_DLSS_FG` unset and `MK8_DLSS_FG=0` were run for 12 s each: both logs are identical
  through the Vulkan device/swapchain selection (`AMD Radeon RX 9070 XT (RADV GFX1201),
  swapchain 2560x1440`, no `[dlss_fg]` lines; the only difference is the romfs index
  timing). `tests/test_fsr_fg.py` remains 9 tests OK (1 provider test skipped because the
  FSR provider is not installed).
- The fetched DLSS-G plugin (7,391,232 bytes, SHA-256 `574839ca…e6f7`, matching the
  pin) dlopens and exports the 12 `NVSDK_NGX_VULKAN_*` entry points. On this host the
  probe then stops at the missing core:

  ```text
  [dlss_fg] DLSS Frame Generation unavailable: no NGX core library (libnvidia-ngx.so.1): libnvidia-ngx.so.1: cannot open shared object file: No such file or directory; the core is installed by the NVIDIA driver, not by this project
  ```

  With no plugin, the exact line is
  `[dlss_fg] DLSS Frame Generation unavailable: no DLSS-G plugin
  (libnvidia-ngx-dlssg.so*) in <dir>; fetch it with tools/fetch_vendor_sdk.py
  --accept-license nvidia`.
- The official `nvsdk_ngx_params_dlssg.h` is **not pinned by the current downloader**
  manifest (it was fetched manually for the ABI test); the test skips when it is absent.
  Pinning it is a small follow-up for the downloader's owner.

### Runtime wiring (implemented)

`toolchain/dlss_fg.py` is registered in `toolchain/host.py` immediately after
`fsr_fg.prepare`. It patches the prepared runtime, strictly behind a helper that is false when
`MK8_DLSS_FG` is unset/`0` or when `MK8_FSR_FG` is active:

- **device creation:** append `VK_NV_LOW_LATENCY_2_EXTENSION_NAME` when it is exposed, plus
  `VK_KHR_present_id` (or `present_id2`), which `VK_NV_low_latency2` depends on. The
  extension has **no feature struct** in the Vulkan headers; `vkSetLatencySleepModeNV` needs the
  enabled extension and a swapchain created with latency mode. A missing extension logs a
  warning and the runtime still starts (DLSS-G then fails closed).
- **swapchain:** add `VK_IMAGE_USAGE_SAMPLED_BIT` (DLSS-G samples the final image) and chain
  `VkSwapchainLatencyCreateInfoNV{latencyModeEnable = VK_TRUE}` when the extension was enabled.
  When DLSS-G is off, the swapchain create info is exactly as before.
- **exec.c:** the depth capture and the main-draw view/projection scan now also run for
  `MK8_DLSS_FG` (strictly `getenv(...) && strcmp(..., "0") != 0`), so DLSS-G gets real depth,
  vectors and matrices without `MK8_FSR_FG`/`MK8_FSR4_CAPTURE` also being set.
- **device.c:** the real window image is left in `PRESENT_SRC_KHR` on the DLSS-G path (the FSR
  proxy still uses `SHADER_READ_ONLY_OPTIMAL`), and `mk8_present_image_index()` exposes the
  current swapchain image to the bridge. Both are inert when DLSS-G is off.

`runtime/mk8/graphics/fsr_fg.cpp` now implements the present path:

- `mk8_fsr_fg_create_swapchain`: when `MK8_DLSS_FG` is the only FG mode, creates the normal
  swapchain (no FFX proxy), calls `mk8_dlss_fg_init` with a null command buffer (the bridge
  records creation into its own one-shot submission because no frame command buffer is open
  yet), and reports a clear refusal if `MK8_FSR_FG` is also set (FSR-FG then keeps the path).
- `mk8_fsr_fg_prepare`: on the DLSS-G path, takes the final display-size swapchain image, calls
  `mk8_fsr_fg_inputs` for depth/MVs, fills `Mk8DlssFgInputs` from `g_fsr_view`, `g_fsr_proj` and
  `g_fsr_jitter_*`, evaluates into the module's interpolated image, acquires a second swapchain
  image with a fence, and records `mk8_dlss_fg_copy_interpolated` into the same command buffer.
  If depth/MVs are not real (render size differs from display, capture missing) it presents the
  real frame only with one log line.
- `mk8_fsr_fg_present`: the runtime's frame submission signals one binary semaphore. A
  semaphore-only `vkQueueSubmit2` waits on the frame's timeline value and signals the second
  one; `mk8_dlss_fg_present` then presents the real image followed by the interpolated image with
  `VK_NV_low_latency2` PRESENT markers. On init/evaluate/acquire failure it falls back to the
  normal single-image present with one clear line; a failed frame never returns a fabricated
  image.
- `MK8_DLSS` (SR) and `MK8_DLSS_FG` remain mutually exclusive (the module refuses the shared
  NGX init); `MK8_FSR_FG=fsr3/fsr4` takes priority and DLSS-G logs the refusal.

### Compiled vs executed

The full runtime was built from the wired tree (222 units, `mk8drecomp` linked) and a short
RADV run with `MK8_DLSS_FG=1` attempted DLSS-G and failed closed with the exact no-core line
above; the normal present path then ran. `MK8_DLSS_FG` unset and `=0` produced the same
device/swapchain startup (the only diff was romfs timing). `tests/test_dlss.py` 18/18 and
`tests/test_dlss_fg.py` 4/4 passed; `tests/test_fsr_fg.py` 9/9 passed with one provider test
skipped. No NVIDIA GPU is present, so nothing in `Init`, `FrameGeneration.Available`,
`CreateFeature`, `EvaluateFeature`, the two-image present or pacing was executed.

### NOT verified and exact remaining RTX 40/50 validation

Still untested on hardware: the driver `libnvidia-ngx.so.1`, `VK_NV_low_latency2` being exposed
and enabled, `FrameGeneration.Available = 1`, `CreateFeature`, `EvaluateFeature`, the second
swapchain image acquire/copy and two `vkQueuePresentKHR` calls, and pacing/latency. Also
unverified: matrix conventions, clip-space jitter, `mvecScale = 1/renderSize`, depth convention,
`BackbufferFormat`, `InternalWidth/Height`, the `PRESENT_SRC`/`GENERAL`/`SHADER_READ_ONLY`
layouts, render-size vs display-size depth (currently DLSS-G is skipped when they differ), and
resize/recreation. An RTX 40/50 Linux host must run a full race with the NVIDIA driver core,
confirm `vkSetLatencySleepModeNV` succeeds, check the two-image order and frame pacing, and
exercise a window resize. Windows compiles the same `#ifdef` path but was not built here.
