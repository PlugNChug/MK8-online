# DLSS Super Resolution / DLAA: implementation notes

Status 20/11/2026. This is the first implementable DLSS milestone from
[DLSS-XESS-NEURAL.md](DLSS-XESS-NEURAL.md) (phase 1): an optional NGX Vulkan bridge
beside the FSR one. It is **compile-tested and loader-tested only**. This host has an
AMD RX 9070 XT (RADV) and no NVIDIA GPU, so feature creation and evaluation were not
executed and no image was produced. `gameplay_verified` stays false.

The implementation is deliberately small and fail-closed: no vendor binary is
committed, nothing is linked against NGX, and every failure returns
`VK_NULL_HANDLE` with one line that names the missing piece.

## Files

| File | Role |
| --- | --- |
| `runtime/mk8/graphics/dlss.cpp` | loader probe, NGX init, feature creation, per-frame EvaluateFeature |
| `runtime/mk8/graphics/dlss.h` | probe/status API and the `mk8_dlss_upscale` declaration |
| `runtime/mk8/graphics/ngx/mk8_ngx_abi.h` | hand-written minimal NGX Vulkan ABI (see licences below) |
| `runtime/mk8/graphics/fsr.cpp` | one dispatch hunk: `MK8_DLSS=1` calls the bridge before the FSR path |
| `tests/test_dlss.py` | native loader failure paths, real-plugin dlopen, ABI static asserts |

No `toolchain/dlss.py` and no `toolchain/host.py` change were needed: the host build
already compiles every `runtime/mk8/**/*.cpp`, and the dispatch lives in `fsr.cpp`,
so the prepared `exec.c` anchor from `toolchain/fsr.py` is reused unchanged. The
runtime-only build applies it automatically.

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

## Verified here

- `dlss.cpp` and the edited `fsr.cpp` compile with the runtime's C++17 flags.
- A fresh runtime-only host build in `local/dlss-sr/host` compiled every prepared unit,
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
Runtime x86_64-linux-gnu: 219/221 unidades
Runtime x86_64-linux-gnu: 220/221 unidades
Runtime x86_64-linux-gnu: 221/221 unidades
EXE .../local/dlss-sr/host/mk8drecomp

$ nm local/dlss-sr/host/mk8drecomp | grep -E ' T mk8_(dlss|fsr4)_upscale$'
0000000000351fb0 T mk8_dlss_upscale
00000000003576c0 T mk8_fsr4_upscale

$ ldd local/dlss-sr/host/mk8drecomp | grep -i nvidia
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

`python -m unittest tests.test_dlss` reports 8 tests OK. The probe harness is built from
the shipped `dlss.cpp` with `-DMK8_DLSS_PROBE_ONLY`, so its failure paths are the real
ones, not a re-implementation.

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

## DLSS Frame Generation: the hook point, not implemented

DLSS-G is a separate phase and deliberately absent. The next step is:

1. Fetch `libnvidia-ngx-dlssg.so.310.9.16` (already pinned by
   `tools/fetch_vendor_sdk.py`) and check it exports `NVSDK_NGX_VULKAN_CreateFeature`
   for `NVSDK_NGX_Feature_FrameGeneration`.
2. Require `VK_NV_low_latency2` on the device and implement
   `vkSetLatencySleepModeNV` / `vkLatencySleepNV` / `vkGetLatencyTimingsNV` around
   present. Without it the pacing and latency are wrong even when the images are.
3. Put the integration beside `runtime/mk8/graphics/fsr_fg.cpp`'s swapchain wrapper,
   not in this upscale bridge: NGX does not intercept `vkQueuePresentKHR`, so the
   application must own both output images and present the real and interpolated
   frames once each, in order. The current `post_process` hook returns a single view
   and cannot express that.
4. Create with `NVSDK_NGX_DLSSG_Create_Params{Width, Height, RenderWidth,
   RenderHeight, NativeBackbufferFormat, DynamicResolutionScaling}` and evaluate with
   `NVSDK_NGX_VK_DLSSG_Eval_Params` plus `NVSDK_NGX_DLSSG_Opt_Eval_Params`
   (`multiFrameCount = 1` for 2x, camera matrices without TAA jitter, `Reset`).
5. Validate on RTX 40/50 only; keep FG off by default, and never combine it with the
   FSR frame-generation proxy in the same run.

Do not use Streamline for the native Linux path: 2.14.16 ships Windows DLLs only.
