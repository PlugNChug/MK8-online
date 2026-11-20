# DLSS Super Resolution / DLAA and Frame Generation: implementation notes

Status 20/11/2026. This covers phase 1 (DLSS Super Resolution / DLAA) and phase 3
(DLSS Frame Generation, DLSS-G) from [DLSS-XESS-NEURAL.md](DLSS-XESS-NEURAL.md). Both
are optional NGX Vulkan bridges in the runtime and both are compile-tested and
loader/ABI-tested here (this host has an AMD RX 9070 XT / RADV and no NVIDIA GPU). The ABI
the bridge uses was executed for real on a Windows machine with an RTX 5080 (driver
621.01): first by `tools/dlss_rtx_harness.cpp`, then through the bridge's own public
API (`mk8_dlss_upscale`, `mk8_dlss_fg_init`/`mk8_dlss_fg_evaluate`) by
`tools/dlss_bridge_harness.cpp`, after the four Windows runtime differences were
applied to `dlss.cpp`. Both SR and FG created features, evaluated frames and wrote
PNGs. The game itself was not run and `gameplay_verified` stays false. The pre-device hook
now exists in `toolchain/dlss.py` (registered before `toolchain/dlss_fg.py`): both the
Windows and Linux runtimes build with it, but an in-engine run still needs the game
data on an NVIDIA machine.

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
| `toolchain/dlss.py` | pre-device NGX instance/device extension gate (runs before `dlss_fg.py`) |
| `toolchain/dlss_fg.py` | gated low-latency device/swapchain patches, depth/matrix gates, device accessor |
| `tests/test_dlss.py` | native loader failure paths, real-plugin dlopen, SR and DLSS-G ABI static asserts |
| `tests/test_dlss_fg.py` | toolchain patch idempotence/gating and `-fsyntax-only` compile of the patched runtime |
| `tools/dlss_rtx_harness.cpp` | standalone Windows x86-64 harness: ran the raw NGX ABI for SR and FG on an RTX 5080 |
| `tools/dlss_bridge_harness.cpp` | standalone Windows harness that compiles the real `dlss.cpp` and calls `mk8_dlss_upscale` / `mk8_dlss_fg_*` on the RTX 5080 |
| `tools/dlss_bridge_include/` | shim `vulkan/vk.h`, its VMA subset and the camera-MV shader SPIR-V for that harness |

`toolchain/host.py` registers `toolchain/dlss.py` after `toolchain/fsr_fg.py` and
`toolchain/dlss_fg.py` after it; the host build already compiles every `runtime/mk8/**/*.cpp`. The SR
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
Runtime x86_64-linux-gnu: 219/222 unidades
Runtime x86_64-linux-gnu: 220/222 unidades
Runtime x86_64-linux-gnu: 221/222 unidades
Runtime x86_64-linux-gnu: 222/222 unidades
EXE .../local/dlss-fg-final/host/mk8drecomp

$ nm local/dlss-fg-final/host/mk8drecomp | grep -E ' T (mk8_dlss_fg_init|mk8_dlss_upscale|mk8_fsr4_upscale)$'
0000000000359350 T mk8_dlss_fg_init
0000000000356090 T mk8_dlss_upscale
000000000035eea0 T mk8_fsr4_upscale

$ ldd local/dlss-fg-final/host/mk8drecomp | grep -i nvidia
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

## Verified on RTX 5080 (Windows 11, 20/11/2026)

`tools/dlss_rtx_harness.cpp` is a standalone harness. It includes
`runtime/mk8/graphics/ngx/mk8_ngx_abi.h` unchanged, links nothing NVIDIA, loads the
driver core and the feature plugins with `LoadLibrary`, creates a Vulkan 1.3 compute
device, and runs the same sequence the bridge runs: `RequiredExtensions`,
`GetFeatureRequirements`, `Init`, `GetCapabilityParameters`, `CreateFeature1`,
`EvaluateFeature`, readback, `ReleaseFeature`, `Shutdown1`.

Machine and versions:

| Item | Value |
| --- | --- |
| GPU | NVIDIA GeForce RTX 5080, deviceID `0x2c02`, 16,303 MiB, VBIOS 98.03.3b.00.01 |
| Driver | 621.01 (Vulkan `driverVersion=0x9b404000`, conformance 1.4.3) |
| OS | Windows 10.0.26100.10066 |
| Vulkan loader | 1.4.357 |
| NGX core | `C:\WINDOWS\System32\DriverStore\FileRepository\nv_dispi.inf_amd64_20f49c6eb5c9b39b\_nvngx.dll`, 1,438,952 B, file version 32.0.16.2101 |
| SR plugin | `nvngx_dlss.dll` v310.9.16, 58,956,912 B, sha256 `d00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e` |
| FG plugin | `nvngx_dlssg.dll` v310.9.16, 7,506,544 B, sha256 `8d59d49b36410dd6b62fef2503a5871d347415bd85d943fa0848563a95dd50c7` |

Both DLLs are the official DLSS SDK v310.9.16 Windows x86-64 files
(`raw.githubusercontent.com/NVIDIA/DLSS/v310.9.16/lib/Windows_x86_64/rel/`), not
committed. They were copied to `C:/Users/Ronny/mk8-rtx/dlss/` beside the harness; the
core is the driver's own. `HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore\FullPath`
points at the DriverStore directory; **System32 has no `nvngx*.dll` on this driver**,
which matters for the loader probe (see point 4 below).

Build and run (cross build, source only in the repository):

```sh
x86_64-w64-mingw32-g++ -std=c++17 -O2 -static \
    -I"$HOME/.cache/mk8-dlss-rtx/include" \
    -o dlss_rtx_harness.exe tools/dlss_rtx_harness.cpp

scp dlss_rtx_harness.exe nvngx_dlss.dll nvngx_dlssg.dll \
    Ronny@192.168.8.197:C:/Users/Ronny/mk8-rtx/dlss/

ssh Ronny@192.168.8.197 \
  "cd /d C:\Users\Ronny\mk8-rtx\dlss && dlss_rtx_harness.exe . out"
```

`-I` points at a private copy of the Khronos Vulkan headers; the Nvidia `vk_video`
headers come from `Vulkan-Headers` 1.4.363.0. Set `MK8_HARNESS_NO_NGX_LOG=1` to route
NGX's verbose log through its own files instead of the app callback (the harness
prints the same result either way). The harness writes PNGs into `out/`.

### Super Resolution: real evaluated frames

```text
SuperSampling.Available = 1
NVSDK_NGX_VULKAN_CreateFeature1(SuperSampling): 0x00000001 (success)
SuperSampling.FeatureInitResult = 0x00000001 (success)
frame 0: EvaluateFeature 0x00000001 (success), pointer slot 0, float slot 6, jitter 0.000/-0.167, input layout GENERAL
frame 3: EvaluateFeature 0x00000001 (success), ... input layout SHADER_READ_ONLY_OPTIMAL
frame 9: EvaluateFeature 0x00000001 (success), ... input layout GENERAL
sr frame: bytes=14745600 nonzero_bytes=14745600 sentinel_pixels=0/3686400 distinct_pixels=32920
sr frame: checksum=0x76f74cc29ecdce23 min/max R=32/240 G=44/230 B=52/200 A=255/255 mean=106.41/114.92/112.32/255.00 horiz_gradient=3.166
SR verdict: STRUCTURED OUTPUT
```

Ten frames at 1280x720 -> 2560x1440 (PERFORMANCE, colour RGBA8, depth R32F,
motion RG32F, Halton jitter, `Reset` on the first frame). Before each evaluation the
output was cleared to magenta; `sentinel_pixels=0` proves DLSS wrote every pixel, and
the checksum changes on every frame. Frame 3 deliberately passed the inputs in
`VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`, the layout `dlss.cpp` assumes: it was
accepted. The other frames used `GENERAL`.

Output: `C:/Users/Ronny/mk8-rtx/dlss/out/sr_2560x1440.png` (copied back as
`~/.cache/mk8-dlss-rtx/out/sr_2560x1440.png`, sha256
`859a38b6921d6f434bd4243510b0e6a218c1bea7fa5fae33f8ff8a86b6ba7a8f`). The 2x input is
`sr_input_1280x720.png`.

### Frame Generation: compute path also evaluates headlessly

```text
FrameGeneration.Available = 1
FrameGeneration.NeedsUpdatedDriver = 0
FrameGeneration.MinDriverVersion = 520.0
NVSDK_NGX_VULKAN_CreateFeature(FrameGeneration): 0x00000001 (success)
FrameGeneration.FeatureInitResult = 0x00000001 (success)
fg frame 0: EvaluateFeature 0x00000001 (success)
fg frame 0: mean |interpolated - backbuffer| = 0.000
fg frame 1: EvaluateFeature 0x00000001 (success)
fg frame 1: mean |interpolated - backbuffer| = 6.583
fg frame 2: EvaluateFeature 0x00000001 (success)
fg frame 2: mean |interpolated - backbuffer| = 6.617
FG verdict: STRUCTURED INTERPOLATED FRAME
```

Frame 0 (reset) equals the backbuffer; frames 1-2 differ from it by 6.58/6.62 mean
absolute levels, so NGX generated pixels rather than copying the backbuffer. The
evaluation used app-owned images: 2560x1440 RGBA8 backbuffer and interpolated output,
1280x720 depth/motion vectors, `DLSSG.CameraViewToClip`, `ClipToCameraView`,
`ClipToPrevClip`, `PrevClipToClip`. **A swapchain is not required for the compute
path.** Presenting the real and interpolated frames once each in order and the
`VK_NV_low_latency2` sleep/pacing remain the application's job; the device exposes
`VK_NV_low_latency2`, but the harness does not call it. Multi-frame generation
(`DLSSG.MultiFrameCount > 1`) was not exercised.

Output: `out/fg_2560x1440.png` (sha256
`47ba9cbfcc1fd9736aa3a7c65c879eeef745fe2aca77bb60deed52c1dd288751`) and the
corresponding `out/fg_backbuffer_2560x1440.png`.

### What the ABI header got right, and what Windows requires

The struct layouts were accepted by the real binaries unchanged:
`sizeof(Mk8NgxResourceVk)=56`, `FeatureCommonInfo=40`, `FeatureDiscoveryInfo=56`,
`DlssCreateParams=28`. Resources built exactly as `dlss.cpp` builds them were
consumed correctly.

Four facts differ from the Linux shape. They were applied to the Windows branches of
`dlss.cpp` after this direct run and are exercised again through the bridge's public
API in the next section; the list stays here because it is the ABI evidence:

1. **Init entry point and arity.** The core exports
   `NVSDK_NGX_VULKAN_Init_ProjectID`, not the SDK's `..._Init_with_ProjectID`, and it
   takes nine arguments: project id, engine type, engine version, application data
   path, `VkInstance`, `VkPhysicalDevice`, `VkDevice`, SDK version,
   `NVSDK_NGX_FeatureCommonInfo*`. The two `vkGet*ProcAddr` pointers are absent.
   Calling it with the 11-argument SDK shape (what `dlss.cpp` did before this change)
   passes the engine-version string where the path list belongs and faults inside
   `_nvngx.dll` at `0xb999` with `0xC0000005` (confirmed in the Windows Application
   event log). `nvngx.dll` in the DriverStore is only a forwarder to the same
   signature. The Windows branch now calls the nine-argument form when it is exported;
   Linux keeps the eleven-argument helper.
2. **Parameters are the C++ object, not C exports.** The runtime exports no
   `NVSDK_NGX_Parameter_SetUI/SetI/SetF/SetVoidPointer/GetUI`. MSVC's slot order
   differs from the header's declaration order; the slots that matter here are
   `Set(void*)=0`, `Set(uint)=3`, `Set(int)=4`, `Set(float)=6` (slot 1 is an empty
   stub), `Get(uint)=11`. With `Set(void*)` on slot 7 (the header's position) the SR
   plugin reports `could not find Color parameter`; with `Set(float)` on slot 1 it
   reports `could not find Jitter.Offset.X`. The Windows branch now installs vtable
   thunks only when the C helpers are absent; Linux keeps the exported helpers.
3. **Feature dispatch comes from the core.** `CreateFeature1`, `EvaluateFeature` and
   `ReleaseFeature` resolved from `nvngx_dlss.dll` return `0xBAD00007
   (not-initialized)` when called directly. The same calls on `_nvngx.dll` succeed;
   `dlss.cpp` resolves them from the plugin.
4. **Core discovery.** The driver registers the core directory under
   `HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore\FullPath` and does not copy
   `_nvngx.dll` to System32. A bare `LoadLibraryA("_nvngx.dll")` therefore reports
   `no-core`. The Windows probe now reads that key, prepends the directory to the DLL
   search path with `SetDllDirectoryA`, and falls back to a copy beside the plugin and
   then to the bare names (older drivers).

Required extensions reported for this driver/plugin pair:
instance `VK_KHR_get_physical_device_properties2`; device `VK_NVX_binary_import`,
`VK_NVX_image_view_handle`, `VK_KHR_buffer_device_address`,
`VK_KHR_push_descriptor`. The bridge cannot enable them because the game device
already exists; `toolchain/dlss.py` now adds them before `vkCreateDevice` when a DLSS
mode is requested (see "Pre-device NGX extension hook" below).

### DLSS 4.5 / DLSS 5 features

Nothing in this run exposed a "DLSS 5" or neural-rendering feature. The NGX telemetry
from the core reports `ngxSDKVersion = 1.5`, `ngxCoreVersion = 1.4.0.0`,
`ngxFeatureVersion = 310.9.16`; the loaded SR/FG snippets are 310.9.16. The core's
plugin scan did look for `nvngx_dlssnr.dll` ("dlssnr" / neural rendering) in the app
directory, the DriverStore and its `ngx-builtins` subdirectory, and reported that no
such module exists in any search path, so no availability value exists to report.
`FrameInterpolation.Available` is defined by the SDK but this runtime returns
`0xBAD00010 (unsupported parameter)` for it; the relevant flag on this build is
`FrameGeneration.Available`. Do not infer support for anything beyond SR/DLAA and 2x
FG from this run: RR, MFG and any neural-rendering path were not exercised.

### Verified through the bridge API (RTX 5080, 20/11/2026)

The Windows corrections were applied to `runtime/mk8/graphics/dlss.cpp` / `dlss.h` /
`ngx/mk8_ngx_abi.h` (Windows branches only; the Linux path keeps the exported C
helpers, `Init_with_ProjectID` and plugin-side dispatch) and the bridge itself was then
executed through its public API. `tools/dlss_bridge_harness.cpp` compiles the real
`dlss.cpp` against a minimal `tools/dlss_bridge_include/vulkan/vk.h` (the runtime's
`vk.h` surface plus a three-function VMA subset), creates a Vulkan 1.3 device on the
RTX 5080 with a hidden Win32 swapchain, and calls `mk8_dlss_upscale` (SR) or
`mk8_dlss_fg_init` / `mk8_dlss_fg_evaluate` (FG) with synthetic images. The camera-MV
shader is the exact GLSL from `dlss.cpp`, precompiled to SPIR-V and embedded for the
harness.

Build and run:

```sh
x86_64-w64-mingw32-g++ -std=c++17 -O2 -static -DMK8_DLSS_TEST_API \
    -Itools/dlss_bridge_include -Iruntime/mk8/graphics \
    -I"$HOME/.cache/mk8-dlss-rtx/include" \
    -o dlss_bridge_harness.exe tools/dlss_bridge_harness.cpp runtime/mk8/graphics/dlss.cpp \
    -L"$HOME/.cache/mk8-dlss-rtx" -lvulkan-1 -luser32 -ladvapi32

scp dlss_bridge_harness.exe nvngx_dlss.dll nvngx_dlssg.dll Ronny@192.168.8.197:C:/Users/Ronny/mk8-rtx/dlss/
ssh Ronny@192.168.8.197 "cd /d C:\Users\Ronny\mk8-rtx\dlss && dlss_bridge_harness.exe sr . out"
ssh Ronny@192.168.8.197 "cd /d C:\Users\Ronny\mk8-rtx\dlss && dlss_bridge_harness.exe fg . out"
```

`MK8_DLSS_TEST_API` adds only `mk8_dlss_test_output_image`/`_format` so the harness can
read the feature-owned output; the runtime build does not define it.

**Super Resolution** (1280x720 -> 2560x1440 PERFORMANCE, six frames, camera-derived
motion vectors, output cleared to magenta before frames 1-5):

```text
[dlss] feature requirements: supported (min GPU arch 0x160, min OS 10.0.0)
[dlss] NGX init: project 6f1d2c3a-8b4e-4d7a-9c15-3e0a5b7c9d21, plugin ./nvngx_dlss.dll, data out\xdg/mk8online/nvidia/ngx-data
[dlss] using the driver Init_ProjectID form (9 arguments, no vkGet*ProcAddr)
[dlss] NVSDK_NGX_VULKAN_Init_ProjectID: 0x00000001 (success)
[dlss] provider NGX API 0x15 from ./nvngx_dlss.dll
[dlss] SuperSampling.Available = 1
[dlss] creating performance feature 1280x720 -> 2560x1440, depth standard
[dlss] NVSDK_NGX_VULKAN_CreateFeature: 0x00000001 (success)
[dlss] depth source: captured scene depth 1280x720 fmt 100
[dlss] motion vectors: camera-derived
[dlss] NVSDK_NGX_VULKAN_EvaluateFeature: 0x00000001 (success)
[dlss] upscale active 1280x720 -> 2560x1440 (quality performance, depth game, motion camera, jitter 0.000,-0.167)
```

The last frame reported `sentinel=0/3686400`, `distinct=31460`, `checksum=0xb9a947ab3245f65d`,
`gradient=3.254`; the PNG is `out/bridge_sr_2560x1440.png` (sha256
`854c66240a936da54a5c4572a4cc38e5562e37fa36751c7d0c1bc7a8eafc96f2`), with the 2x input at
`out/bridge_sr_input_1280x720.png` (local copies in
`$HOME/.cache/mk8-dlss-rtx/bridge-out/`). Zero sentinel pixels proves the clearance was
overwritten; the changing checksums and ~31k distinct values show structured output.

**Frame Generation** (1280x720 display, 960x540 render, 2x, three frames, real
`VK_NV_low_latency2` swapchain, app-owned backbuffer/depth/MV):

```text
[dlss_fg] GetFeatureRequirements failed 0xbad00002 (platform-error); trying Init
[dlss_fg] using the driver Init_ProjectID form (9 arguments, no vkGet*ProcAddr)
[dlss_fg] NVSDK_NGX_VULKAN_Init_ProjectID: 0x00000001 (success)
[dlss_fg] provider NGX API 0x15 from ./nvngx_dlssg.dll
[dlss_fg] NVSDK_NGX_VULKAN_CreateFeature(FrameGeneration): 0x00000001 (success)
[dlss_fg] feature created and output primed in a one-shot submission
[dlss_fg] active: NGX DLSS-G 2x 1280x720 (render 960x540, format 44), VK_NV_low_latency2 pacing
[dlss_fg] mvec scale 1/960,1/540 (pixel-space MVs); jitter converted to clip space
[harness] mk8_dlss_fg_ready = 1
[dlss_fg] NVSDK_NGX_VULKAN_EvaluateFeature(FrameGeneration): 0x00000001 (success)
[harness] fg frame 0: mean |interpolated - backbuffer| = 0.000
[harness] fg frame 1: mean |interpolated - backbuffer| = 10.443
[harness] fg frame 2: mean |interpolated - backbuffer| = 11.464
```

Frame 0 (`Reset`) equals the backbuffer; frames 1-2 differ, so the interpolated image is
generated, not copied. The PNG is `out/bridge_fg_1280x720.png` (sha256
`68541536b738a84d3db0fcc923900c1e4f4dcafbc36d162059ecf9e242b3a8a3`), with the corresponding
`out/bridge_fg_backbuffer_1280x720.png` (local copies in `$HOME/.cache/mk8-dlss-rtx/bridge-out/`).
`GetFeatureRequirements` on the FG plugin returns
`0xBAD00002 (platform-error)`; the bridge logs it and falls back to `Init`, which
succeeds, so this is a non-fatal provider quirk on this driver/plugin pair.

**Windows runtime cross-build** (`x86_64-windows-gnu` with Zig 0.17.0, the project's
cross-build driver):

```text
Runtime x86_64-windows-gnu: 234/234 unidades
Executável do runtime ligado; gameplay ainda não verificado.
host built: .../host-dlss-final2/mk8drecomp.exe (4523520 bytes)
```

`llvm-nm host-dlss-final2/obj/dlss.o` contains `T mk8_dlss_upscale`, `T mk8_dlss_fg_init`
and `T mk8_dlss_fg_evaluate`; the exe sha256 is
`25b417e4e4ee47fe56538d78630c4c3a1f06e826e91b520e64704bd60e405213`. It was **not executed**
(no in-engine run, no game images).

**Linux tests**: `python3 -m unittest -v tests.test_dlss tests.test_dlss_fg
tests.test_fsr_fg` -> 34 tests, `OK (skipped=1)`; the skip is the FSR frame-generation
provider fixture, which is not installed on this host. See the pre-device hook section
for the rebuilds.

### Pre-device NGX extension hook (20/11/2026)

`toolchain/dlss.py` is registered in `toolchain/host.py` after `fsr_fg.prepare` and before
`dlss_fg.prepare`. Strictly behind `mk8_dlss_requested()` (MK8_DLSS or MK8_DLSS_FG
non-zero; FSR frame generation wins when MK8_FSR_FG is active), it adds:

- instance: `VK_KHR_get_physical_device_properties2`, before the validation extension so
  the existing `enabledExtensionCount--` fallback still removes the right entry;
- device: `VK_NVX_binary_import`, `VK_NVX_image_view_handle`,
  `VK_KHR_buffer_device_address`, `VK_KHR_push_descriptor`, after the FSR/MLFI
  extension additions and before `free(ep)`.

Every entry is added only when advertised. If one is missing, the hook logs exactly

```text
[dlss] NGX device extension <name> is not advertised; DLSS will fail closed
```

(or `[dlss] VK_KHR_get_physical_device_properties2 is not advertised; NGX will fail closed`
for the instance extension), adds none of the NGX set, and the bridge fails closed at
`Init`; the existing FSR/bilinear path runs. With both variables unset/`0`, the three
gates are false and the instance/device extension lists are unchanged. The FSR FG/MLFI
entries are not reordered: `dlss.py` appends after them and `dlss_fg.py` still adds its
low-latency pair in its own earlier block.

Rebuilds and tests with the hook:

```text
Runtime x86_64-windows-gnu: 234/234 unidades
host built: .../host-dlss-ngx/mk8drecomp.exe (4525568 bytes)
sha256 719b74f24d3f518f0d2eeea1531ca90fd6409d6fadf56d4277d0106ae85aac90

Runtime x86_64-linux-gnu: 230/230 unidades
host built: .../host-linux-ngx/mk8drecomp (4850784 bytes)
sha256 7539336fbba0db28d7720eb0ff4a94ddb7414fe33da1e3646fcf5b7341240f31
```

`tests/test_dlss.py`, `tests/test_dlss_fg.py` and `tests/test_fsr_fg.py` ran 34 tests:
`OK (skipped=1)` (the FSR provider fixture). `tests/test_dlss_fg.py` now applies
`dlss.py` before `dlss_fg.py`, asserts the NGX names occur only inside the gated blocks,
runs a compiled truth table for `mk8_dlss_requested()` (unset/0 false, MK8_DLSS true,
MK8_DLSS_FG true, MK8_FSR_FG false, combined FSR-FG false), checks idempotence and
compiles the patched `device.c`/`exec.c`. The no-DLSS device path is inert, not a
different extension list.

What remains for an in-engine claim: run the built runtime on an NVIDIA machine with the
game data, confirm the hook logs the four NGX extensions enabled, then confirm
`SuperSampling.Available`/`FrameGeneration.Available`, feature creation, evaluation and
the final present with MK8 frames. No such run was made here.

## NOT verified

The harness proves the ABI and the NGX sequence, not the game. On this host the
following still did not run:

- In-engine execution. The bridge compiles into the Windows and Linux runtimes and ran
  through its public API in the harness, and `toolchain/dlss.py` now enables the NGX
  instance/device extensions before `vkCreateDevice`; the game was still not started on
  an NVIDIA machine with game data, so no MK8 image exists. That run is the remaining
  step.
- The NGX extension list on a device that does not advertise one of the four: the hook
  logs `[dlss] NGX device extension <name> is not advertised; DLSS will fail closed` and
  adds none of the set. The source/compile tests cover that branch; no driver in this
  run actually lacked the extension.
- Quality: the harness input is a synthetic pan with camera-only motion and a neutral
  depth ramp. Depth convention (standard/inverted), jitter/projection correctness,
  object motion vectors and the actual MK8 image are untested.
- Frame Generation presentation and pacing in the game: the harness created a real
  low-latency2 swapchain and `vkSetLatencySleepModeNV` succeeded, but
  `mk8_dlss_fg_present` (two-image interleave), `mk8_dlss_fg_latency_sleep` and the
  PRESENT markers were not exercised, and pacing/quality were not measured.
- Multi-frame generation (`DLSSG.MultiFrameCount > 1`), DLAA at native resolution,
  Ray Reconstruction and any DLSS 4.5/5 feature.
- Windows driver matrix, multi-GPU and the Linux NVIDIA path (`libnvidia-ngx.so.1`).

## DLSS Frame Generation (phase 3): implemented; bridge API executed, in-game present not run

Status 20/11/2026. The DLSS-G library is implemented, compiles into the Windows runtime
and was **executed through its public API** on the RTX 5080: feature creation,
evaluation and a real `VK_NV_low_latency2` swapchain. The in-game two-image present was
not run. It is **off by default**: `MK8_DLSS_FG` must be set (any non-empty value except `0`) and
`mk8_dlss_fg_init` must be called by the presenter. With the variable unset the module
has no effect: no constructor, no hook, and the existing FSR FG/upscaler/bilinear path
runs unchanged.

`MK8_DLSS` (Super Resolution) and `MK8_DLSS_FG` are **mutually exclusive in this
phase**. Both would call `NVSDK_NGX_VULKAN_Init_with_ProjectID` and share the capability
parameter map; separate maps plus a shared init are the next integration step. DLSS-G
does not need DLSS SR for a display-size backbuffer: the game's FSR1 upscaler (or a
future DLSS SR output) is enough.

### What the RTX 5080 run corrected

1. Feature creation uses `NVSDK_NGX_VULKAN_CreateFeature` (not `CreateFeature1`) with
   `Width`, `Height` and `DLSSG.BackbufferFormat` (a `VkFormat`; `37` =
   `VK_FORMAT_R8G8B8A8_UNORM` here). `RenderWidth`/`RenderHeight` in the SDK struct
   were ignored by the 310.9.16 helper, and the core warns that
   `NVSDK_NGX_Parameter_Width/Height` are deprecated in favour of
   `DLSSG.Width/Height`; the harness uses the generic names for compatibility.
2. Require `VK_NV_low_latency2` on the device and implement
   `vkSetLatencySleepModeNV` / `vkLatencySleepNV` / `vkGetLatencyTimingsNV` around
   present. Without it the pacing and latency are wrong even when the images are.
3. Put the integration beside `runtime/mk8/graphics/fsr_fg.cpp`'s swapchain wrapper,
   not in this upscale bridge: NGX does not intercept `vkQueuePresentKHR`, so the
   application must own both output images and present the real and interpolated
   frames once each, in order. The current `post_process` hook returns a single view
   and cannot express that.
4. Evaluate with the `DLSSG.*` resources and `NVSDK_NGX_DLSSG_Opt_Eval_Params`
   (`multiFrameCount = 1` for 2x, camera matrices without TAA jitter, `Reset`). The
   matrices must be reachable through the parameter object's `Set(void*)`; a missing
   matrix returns `0xBAD0000A (missing input)`, which is what the first harness run
   produced before the slot was corrected.
5. Validate on RTX 40/50 only; keep FG off by default, and never combine it with the
   FSR frame-generation proxy in the same run.

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
   `lowLatencyMode = VK_TRUE` and fails closed on any error. `toolchain/dlss_fg.py`
   enables the extension (and `VK_KHR_present_id`) behind the `MK8_DLSS_FG` gate before
   `vkCreateDevice`; the harness exercised the resulting swapchain and sleep mode.
3. **Init and capability.** `NVSDK_NGX_VULKAN_Init_with_ProjectID(..., 0x15)` on Linux;
   the Windows core uses the nine-argument `NVSDK_NGX_VULKAN_Init_ProjectID` instead.
   Then `FrameGeneration.Available`. A zero is reported with
   `FrameGeneration.NeedsUpdatedDriver` and the required driver major/minor read from
   the same parameter map. `GetFeatureRequirements` for
   `NVSDK_NGX_Feature_FrameGeneration` is decoded with the same bit table as SR
   ("driver too old", "GPU unsupported", ...).
4. **Create.** `Width`/`Height` are the display size, `DLSSG.BackbufferFormat` is the
   swapchain `VkFormat`, `DLSSG.InternalWidth/Height` the render size and
   `DLSSG.DynamicResolution = 0`; then on Windows
   `NVSDK_NGX_VULKAN_CreateFeature(cb, NVSDK_NGX_Feature_FrameGeneration, params,
   &handle)` first (the 310.9.16 helper's form) and Linux keeps `CreateFeature1`
   first. `FrameGeneration.FeatureInitResult` is logged on failure.
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

This subsection is the Linux-host evidence from before the RTX 5080 run; the Windows
bridge-API evidence is in "Verified through the bridge API" above.

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

`toolchain/dlss.py` runs after `fsr_fg.prepare` and before `dlss_fg.prepare`. Its
`mk8_dlss_requested()` gate excludes an active `MK8_FSR_FG`, so FSR frame generation
keeps the device exactly as before. The four NGX device extensions and
`VK_KHR_get_physical_device_properties2` are appended only when advertised; the missing
case names the extension in one line and adds none of the set.

`toolchain/dlss_fg.py` is registered in `toolchain/host.py` after `dlss.prepare`. It patches the prepared runtime, strictly behind a helper that is false when
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
device/swapchain startup (the only diff was romfs timing). The Windows cross-build later
compiled 234/234 units and linked `mk8drecomp.exe` (4,525,568 bytes with the NGX
pre-device hook); on the RTX 5080 the
bridge harness then executed `mk8_dlss_fg_init` and three `mk8_dlss_fg_evaluate` calls
through a real low-latency2 swapchain (see "Verified through the bridge API"). The
in-game two-image present, pacing/latency sleep and resize remain unexecuted.
`tests/test_dlss.py`, `tests/test_dlss_fg.py` and `tests/test_fsr_fg.py` pass on Linux
(34 tests, 1 provider fixture skipped); the Linux runtime with the hook built 230/230
units (`host-linux-ngx/mk8drecomp`, 4,850,784 bytes).

### NOT verified and exact remaining RTX 40/50 validation

Executed on hardware through the bridge harness: `FrameGeneration.Available = 1`,
`CreateFeature`, `EvaluateFeature`, a real `VK_NV_low_latency2` swapchain and
`vkSetLatencySleepModeNV`; the interpolated image differs from the backbuffer. Still
untested: the driver's Linux `libnvidia-ngx.so.1`, the second swapchain image
acquire/copy and the two `vkQueuePresentKHR` calls, pacing/latency in the game, and
resize/recreation. Also unverified with real game data: matrix conventions, clip-space
jitter, `mvecScale = 1/renderSize`, depth convention, `InternalWidth/Height`, the
`PRESENT_SRC`/`GENERAL`/`SHADER_READ_ONLY` layouts and render-size vs display-size depth
(currently DLSS-G is skipped when they differ). Windows compiles the path (234/234) and
the bridge API ran; the game itself was not started.
