# DLSS 4.5 / DLSS 5 / XeSS: Linux feasibility and integration surface

Status 20/11/2026. Research only: no vendor code is integrated in `runtime/` and no
vendor binary is committed. The runtime work is owned by the FSR/frame-generation
branch; this document records what the host can actually load, what the licences
allow, and the first milestone that is worth implementing.

Host limitation: this machine has an **AMD RX 9070 XT (Navi 48, RADV)** and no NVIDIA
GPU, so nothing below was executed on DLSS hardware. "Verified" means the file,
header, ABI or vendor statement was inspected; it does not mean an in-game run.

## Verdict in one table

| Feature | Linux native, Vulkan (this runtime) | Windows, Vulkan (this runtime) | Windows, D3D12 | Proton (Windows build) |
| --- | --- | --- | --- | --- |
| DLSS Super Resolution / DLAA (4.5) | **feasible** — official Linux `.so` + NGX VK | feasible | feasible | works (NVIDIA docs) |
| DLSS Ray Reconstruction | n/a — the game has no ray-traced buffers | n/a | feasible | works, n/a here |
| DLSS Frame Generation 2x | **feasible, more work** — NGX VK + `VK_NV_low_latency2` | feasible | feasible | works |
| DLSS Multi Frame Generation 3x/4x | unverified — API exposes `multiFrameCount`; RTX 50 only | unverified | feasible | works on RTX 50 |
| DLSS Dynamic MFG (6x) | **no** — D3D12 only, per NVIDIA | no | feasible | works on RTX 50 |
| DLSS 5 Neural Rendering | **no** — no public `sl.dlss_nr` plugin | no | vendor SDK only | community only |
| XeSS Super Resolution | **no** — SDK ships no Linux library | feasible (`libxess.dll`, Vulkan) | feasible | works (DP4a; XMX needs a community patch) |
| XeSS Frame Generation | **no** — D3D12 only | **no** — D3D12 only | feasible | community only |
| XeSS Low Latency (XeLL) | **no** — D3D12 only | **no** — D3D12 only | feasible | no |

The important asymmetry: **DLSS has a first-class native Linux Vulkan path; XeSS does
not have a Linux path at all.** Both are gated by licences that do not fit this
project's "no vendor binaries" rule, so either way the launcher must download them.

## What was downloaded and inspected

Everything stayed under `/tmp/vendor-sdk-research`; nothing was added to the
repository. Hashes are SHA-256 of the exact bytes fetched on 20/11/2026.

### NVIDIA DLSS SDK (NGX), tag `v310.9.16`

Repo: <https://github.com/NVIDIA/DLSS/tree/v310.9.16> (commit `9981c63`).
The tag publishes **Linux x86-64 ELF libraries**, not only Windows DLLs:

| File | Bytes | SHA-256 |
| --- | ---: | --- |
| `lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.310.9.16` | 59,481,944 | `30982c937afed85c96b8d313323de9a650a2eec6de1bb0f714b21750f4ce98f6` |
| `lib/Linux_x86_64/rel/libnvidia-ngx-dlssd.so.310.9.16` | 48,238,232 | `a5d59de18841f15779039dda567a15dd723ba737915f489271fdb5f109fbfb96` |
| `lib/Linux_x86_64/rel/libnvidia-ngx-dlssg.so.310.9.16` | 7,391,232 | `574839cae01c835de4d5845c36ca9cf75361f8160bd52397142c36a24e29e6f7` |
| `LICENSE.txt` | 26,620 | `d4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0` |

URLs are `https://raw.githubusercontent.com/NVIDIA/DLSS/v310.9.16/lib/Linux_x86_64/rel/<file>`
and `.../v310.9.16/LICENSE.txt`. The Windows directory of the same tag carries
`nvngx_dlss.dll` (58,956,912 B), `nvngx_dlssd.dll` (48,343,664 B) and
`nvngx_dlssg.dll` (7,506,544 B) — the same feature split, no `dlss_nr`.

ABI check on the Linux files:

```text
$ file libnvidia-ngx-dlssg.so.310.9.16
ELF 64-bit LSB shared object, x86-64, version 1 (SYSV), dynamically linked, stripped
$ nm -D --defined-only libnvidia-ngx-dlssg.so.310.9.16 | grep NVSDK_NGX_VULKAN | wc -l
12
$ nm -D --defined-only libnvidia-ngx-dlssg.so.310.9.16 | grep NVSDK_NGX_VULKAN | head -3
NVSDK_NGX_VULKAN_CreateFeature
NVSDK_NGX_VULKAN_EvaluateFeature
NVSDK_NGX_VULKAN_Init
```

The same 12 Vulkan exports are present in the `dlss` (SR) and `dlssd` (RR)
libraries. `nvsdk_ngx_loader.h` shows the Linux loader path:

```c
#define NGX_CORE_LIBRARY_NAME "libnvidia-ngx.so.1"
...
handle = dlopen(filename, RTLD_NOW);
```

That is the driver-provided NGX core; the three `.so` files above are the feature
plugins it loads. Which directory NGX searches for them (application data path or
the driver's own directory) must be confirmed on an NVIDIA host; the downloader
keeps them together so the runtime can add that directory to the loader search
path. The header set needed to build against it is pinned in
`tools/fetch_vendor_sdk.py` (`nvsdk_ngx.h`, `nvsdk_ngx_vk.h`,
`nvsdk_ngx_helpers_vk.h`, `nvsdk_ngx_defs*.h`, `nvsdk_ngx_helpers_dlssg_vk.h`,
`nvsdk_ngx_params.h`, `nvsdk_ngx_loader.h`).

### NVIDIA Streamline 2.14.16

Release: <https://github.com/NVIDIA-RTX/Streamline/releases/tag/v2.14.16>
(2026-10-16, commit `625db07`).

| Asset | Bytes | SHA-256 |
| --- | ---: | --- |
| `streamline-sdk-v2.14.16.zip` | 276,016,170 | `32041e51db0cab709a271b72a352281b690c2abbd669a827809401a5dd23956b` |
| `license.txt` (framework) | 1,591 | `7b6f23e7d6f3ad6292f9308d2b42cdc3d82ae4e9b2abb55f230279d83bedd43d` |

The archive contains `bin/x64/*.dll` (Windows PE) only: `sl.dlss.dll`,
`sl.dlss_g.dll`, `sl.dlss_d.dll`, `sl.reflex.dll`, `sl.common.dll`,
`sl.interposer.dll`, `nvngx_dlss.dll`, `nvngx_dlssg.dll`, `nvngx_dlssd.dll`,
`NvLowLatencyVk.dll`. There is **no Linux `.so` and no `sl.dlss_nr.dll`**, even
though `include/sl_core_types.h` defines `kFeatureDLSS_NR = 1004` and
`include/sl_helpers.h` maps it to the plugin name `dlss_nr`. The public 2.14.16
package declares DLSS 5 Neural Rendering but does not ship its implementation.

### Intel XeSS SDK 3.0.2

Release: <https://github.com/intel/xess/releases/tag/v3.0.2> (2026-07-24,
commit `8fe81bd`).

| Asset | Bytes | SHA-256 |
| --- | ---: | --- |
| `XeSS_SDK_3.0.2.zip` | 76,839,749 | `88b8a373f30e33f3558a77a93e634f11b8132fc3047ea1a8edeead32b8471990` |
| `LICENSE.txt` (repo, identical to the zip) | 4,194 | `784be42c39a4a4d03cf85d88e15e59d75828fca773ce45b8dbfffbd7eaf208df` |

The archive is Windows-only: `bin/libxess.dll` (77,795,704 B, SR),
`bin/libxess_fg.dll` (22,957,432 B, FG), `bin/libxell.dll` (415,368 B),
`lib/*.lib` import libraries, and `basic_sample_super_resolution_vk.exe`.
There is no `libxess.so` or any other ELF object. `libxess.dll` imports
`vulkan-1.dll` and contains `VK_KHR`/`VkDevice` strings — the SR library has a
Vulkan backend, but it is still a Windows DLL. `libxess_fg.dll` and `libxell.dll`
contain no Vulkan references at all.

## Licences: what is allowed

### NVIDIA RTX SDKs License (DLSS/NGX)

Quoted from `LICENSE.txt` (26,620 B, same text as
`external/ngx-sdk/license.txt` inside Streamline):

> 1. LICENSE. ... NVIDIA hereby grants you a non-exclusive, non-transferable
> license ... to: a. Install and use the SDK, b. Modify and create derivative
> works of sample source code delivered in the SDK, and c. Distribute any software
> and materials within the SDK ... as incorporated in object code format into a
> software application subject to the distribution requirements indicated in this
> license.

> 2. DISTRIBUTION REQUIREMENTS. ... a. An application must have material
> additional functionality, beyond the included portions of the SDK. ... d. You
> agree to notify NVIDIA in writing of any known or suspected distribution or use
> of the SDK not in compliance with the requirements of this license ...

> 4. LIMITATIONS. ... a. You may not reverse engineer, decompile or disassemble ...
> b. Except as expressly provided in this license, you may not copy, sell, rent,
> sublicense, transfer, distribute, modify, or create derivative works of any
> portion of the SDK. For clarity, you may not distribute or sublicense the SDK as
> a stand-alone product. ... e. You may not use the SDK in any manner that would
> cause it to become subject to an open source software license.

Consequences for this project:

- Redistribution *inside an application* is permitted by NVIDIA's text, but it is
  **not** compatible with this project's rule: nothing NVIDIA is bundled in the
  repo, AppImage, tarball or setup. The download-and-patch pattern is mandatory.
- Clause 4e forbids using the SDK in a way that would subject it to an open-source
  licence. The runtime must load the library as an optional, separately
  distributed component, never link it into the GPL build, and never ship it in
  the source tree.
- NGX needs an application id **or a project id**. The public header documents the
  project-id route explicitly:

  > Applications must provide a unique identifier to initialize the NGX SDK. If an
  > identifier was provided by NVIDIA, use NVSDK_NGX_Init instead. ... For other
  > engines, use NVSDK_NGX_EngineType CUSTOM. The driver will validate that the
  > Project ID is GUID-like, for example, "a0f57b54-1daf-4934-90ae-c4035c19df04".

  The project-id route is self-service: the driver only checks the GUID shape, and
  an NVIDIA-issued id uses `NVSDK_NGX_Init` instead. DLSS 5 Neural Rendering is
  different: its plugin is not in the public package (see below).

### Intel Simplified Software License (October 2022)

Quoted from `LICENSE.txt`:

> Use and Redistribution. You may use and redistribute the software, which is
> provided in binary form only, (the "Software"), without modification, provided
> the following conditions are met:
> * Redistributions must reproduce the above copyright notice and these terms of
>   use in the Software and in the documentation and/or other materials provided
>   with the distribution.
> * Neither the name of Intel nor the names of its suppliers may be used to endorse
>   or promote products derived from this Software without specific prior written
>   permission.
> * No reverse engineering, decompilation, or disassembly of the Software is
>   permitted, nor any modification or alteration of the Software or its operation
>   at any time, including during execution.

Intel's text **does allow unmodified binary redistribution** with the notice. The
project rule is stricter and still forbids bundling; the downloader stores the
licence next to the DLLs. Reverse engineering is prohibited, so extracting models
or shaders from `libxess.dll` is not an option.

## DLSS on Linux: what actually works

### Native Linux is an official DLSS platform

NVIDIA's current Linux driver guide
(<https://docs.nvidia.com/datacenter/tesla/driver-installation-guide/610/gaming.html>)
documents DLSS SR/FG/RR on both native Linux games and Proton:

> This applies to both native Linux games as well to Windows games runnning via
> Steam Play / Proton. On Linux, Proton must expose NVIDIA's APIs (NVAPI) for DLSS
> to function. This is done automatically with the included DXVK-NVAPI as part of
> a standard Proton installations.

It also shows the native Linux library naming and update path:

```text
$ ls -1sh *dlss*
42M libnvidia-ngx-dlss.so.3.7.20
$ wget https://github.com/NVIDIA/DLSS/raw/refs/heads/main/lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.310.5.3
```

and the Proton override path (`PROTON_ENABLE_NGX_UPDATER=1`, then
`ProgramData/NVIDIA/NGX/models/...`). The same page documents Reflex on Linux via
`VK_NV_low_latency2` directly; the DXVK-NVAPI Reflex layer exists for D3D games.

This is stronger evidence than the Streamline marketing FAQ ("Vulkan support
coming soon", which is stale): the DLSS SDK itself ships Linux Vulkan libraries,
and NVIDIA documents native Linux usage.

### Streamline is Windows-only as a package

Streamline's Vulkan support is real in the code path (changelog 2.14.0: "Fixed a
crash in `sl.dlss_g` on Vulkan when the host application created its window after
creating the swap chain"; Reflex switched to `VK_NV_low_latency2`), and the DLSS-G
guide says:

> DLSS-G runs optical flow in interop mode in Vulkan by default. To leverage the
> potential performance benefit of running optical flow natively in Vulkan, the
> client must meet the minimum requirements: NVIDIA driver version 527.64 or later
> on Windows, **525.72 or later on Linux**, and `VK_API_VERSION_1_1` (recommended:
> `VK_API_VERSION_1_3`).

But the shipped binaries are PE `.dll` only. A native ELF runtime therefore cannot
use Streamline as delivered; it must call NGX directly. That is not a downgrade for
this project: NGX exposes the same features on Vulkan, and the runtime already owns
its swapchain, so the Streamline interposer is not needed.

Streamline remains relevant only if a Windows D3D12 build is ever added (it is the
only path to Dynamic MFG). For the Vulkan runtime, NGX is the integration surface.

### Exact DLSS Super Resolution sequence (NGX Vulkan)

Headers: `nvsdk_ngx_vk.h`, `nvsdk_ngx_helpers_vk.h`, `nvsdk_ngx_params.h`.
The runtime must provide what FSR already provides: colour at render resolution,
depth, dense motion vectors, jitter in render pixels, and a reset flag.

1. `NVSDK_NGX_VULKAN_GetFeatureRequirements(instance, physicalDevice, discoveryInfo, &requirements)`
   before creating the device, and enable the returned instance/device extensions
   and Vulkan 1.2/1.3 features. `sl_helpers_vk.h` shows the expected feature set
   (descriptor indexing, timeline semaphores, buffer device address, etc.).
2. Resolve the entry points dynamically (`dlopen("libnvidia-ngx.so.1")` +
   `dlsym`) so an AMD build still starts. Do not link `libnvidia-ngx.so`.
3. `NVSDK_NGX_VULKAN_Init_with_ProjectID(projectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, engineVersion, appDataPath, instance, physicalDevice, device, vkGetInstanceProcAddr, vkGetDeviceProcAddr, featureInfo, NVSDK_NGX_Version_API)`.
   `appDataPath` is a writable directory for NGX logs/config; use
   `$XDG_DATA_HOME/mk8online/nvidia/ngx-data`.
4. `NVSDK_NGX_VULKAN_GetCapabilityParameters(&params)`; read
   `NVSDK_NGX_Parameter_SuperSampling_Available` (`"SuperSampling.Available"`) and
   `NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult`. If unavailable, log the
   reason and fall back to the existing path. Never fake success.
5. `NGX_VK_CREATE_DLSS_EXT(cmd, creationNodeMask, visibilityNodeMask, &handle, params, &create)`.
   Its `Feature` member carries `InWidth`/`InHeight` (render size) and
   `InTargetWidth`/`InTargetHeight` (display size), plus `InPerfQualityValue`
   (`NVSDK_NGX_PerfQuality_Value_MaxPerf`, `_Balanced`, `_MaxQuality`,
   `_UltraPerformance`, `_UltraQuality` or `_DLAA`; `_DLAA` is native size).
6. Every frame, fill `NVSDK_NGX_VK_DLSS_Eval_Params`:
   `Feature.pInColor`, `Feature.pInOutput`, `pInDepth`, `pInMotionVectors`,
   `InJitterOffsetX/Y` (render pixels), `InRenderSubrectDimensions`,
   `InMVScaleX/Y` (only if vectors are not in pixel units), `InReset` (1 on a cut or
   mode change), optional `pInExposureTexture`. Then
   `NGX_VK_EVALUATE_DLSS_EXT(cmd, handle, params, &eval)`.
7. `NVSDK_NGX_VULKAN_ReleaseFeature(handle)` on resize/shutdown and
   `NVSDK_NGX_VULKAN_Shutdown1(device)` before destroying the device.

The game-side gap is identical to the FSR one: `runtime/mk8/graphics/fsr.cpp`
currently generates **camera-only** motion vectors; karts and animated objects have
no object vectors. DLSS tolerates this less gracefully than FSR 3.1 (ghosting on
moving objects), so object vectors are a quality prerequisite, not a DLSS-specific
one.

DLSS cannot be evaluated on the RX 9070 XT: the driver is RADV and `libnvidia-ngx.so.1`
does not exist. The feature must be optional and must fall back cleanly.

### Exact DLSS Frame Generation sequence (NGX Vulkan)

Headers: `nvsdk_ngx_defs_dlssg.h`, `nvsdk_ngx_helpers_dlssg_vk.h`.
Unlike Streamline, NGX does not intercept `vkQueuePresentKHR`; the application
receives both output images and presents them in order. That suits this runtime.

1. Initialise NGX as above. Query
   `NVSDK_NGX_Parameter_FrameGeneration_Available`; DLSS-G needs an Ada (RTX 40) or
   newer GPU, Hardware-accelerated GPU Scheduling on Windows, and Reflex/`VK_NV_low_latency2`
   pacing. On Linux, the driver must expose `VK_NV_low_latency2` (NVIDIA 615.78.08
   added revision 3; the DLSS-G guide documents native Vulkan optical flow from
   525.72).
2. `NGX_VK_CREATE_DLSSG(cmd, creationNodeMask, visibilityNodeMask, &handle, params, &create)`
   with `NVSDK_NGX_DLSSG_Create_Params{Width, Height, NativeBackbufferFormat,
   RenderWidth, RenderHeight, DynamicResolutionScaling}`.
3. Per rendered frame, fill `NVSDK_NGX_VK_DLSSG_Eval_Params`:
   `pBackbuffer` (current frame), `pDepth`, `pMVecs`, optional `pHudless`, `pUI`,
   `pUIAlpha`, the required `pOutputInterpFrame` (generated frame), and the optional
   `pOutputRealFrame`/`pOutputDisableInterpolation`.
   Fill `NVSDK_NGX_DLSSG_Opt_Eval_Params` with `multiFrameCount` (1 = 2x,
   2 = 3x, ...), `multiFrameIndex`, the camera matrices **without TAA jitter**,
   jitter offset, motion-vector scale, camera position/up/right/forward, near/far,
   FOV/aspect, `DepthInverted`, `CameraMotionIncluded`, `Reset`.
4. Call `NGX_VK_EVALUATE_DLSSG(cmd, handle, params, &eval, &optEval)`.
5. Present: for 2x, present the real frame and the interpolated frame once each;
   the generated frame must be shown for the same duration. Use
   `VK_NV_low_latency2` (`vkSetLatencySleepModeNV`, `vkLatencySleepNV`,
   `vkGetLatencyTimingsNV`, present ids) to pace the queue; otherwise latency and
   pacing are wrong even when the images are correct.
6. Recreate the feature on resize; disable FG around mode changes and present
   deadlocks (the Streamline guide documents the same rule for `SwapChain::Present`).

What is not available on Vulkan even on Windows: **Dynamic MFG**. The guide says
`eDynamic` lacks support when "the application is using Vulkan (support is
currently limited to D3D12 only)", and VSync with FG is also D3D12-only. Fixed MFG
above 2x is expressed by `multiFrameCount`; it requires RTX 50 and must be
validated with `numFramesToGenerateMax`/`NVSDK_NGX_DLSSG_Parameter_MultiFrameCount`.

### DLSS 5 Neural Rendering

The public record:

- DLSS 5 launched in September 2026 in NBA 2K27 for **RTX 50 desktop and laptop only**,
  with Game Ready driver **616.64**; integration is via Streamline or the UE5
  plugin; it consumes the rendered frame plus motion vectors and optional engine
  buffers (albedo, normals, lighting), and is a strict one-frame-in/one-frame-out
  model (<https://www.nvidia.com/en-us/geforce/news/dlss-5-3d-guided-neural-rendering/>).
- Streamline 2.14.16 declares `kFeatureDLSS_NR = 1004`, the plugin name `dlss_nr`,
  and three buffer types for it:
  `kBufferTypeUpliftInputColor = 70`, `kBufferTypeUpliftOutputColor = 71`,
  `kBufferTypeUpliftControlMask = 72`.
- The same archive contains **no `sl.dlss_nr.dll`**. `bin/x64` lists only
  `sl.dlss.dll`, `sl.dlss_g.dll`, `sl.dlss_d.dll`, `sl.common`, `sl.reflex`,
  `sl.nis`, `sl.pcl`, `sl.nvperf`, `sl.deepdvc`, `sl.directsr`.
- NVIDIA's developer blog says DLSS 5 is "available now in NBA 2K27" and points
  developers at a notify-me page; access to the NR SDK/model is not the public
  Streamline download.

Verdict: **not implementable from public artefacts today.** The runtime hook, when
access exists, is `sl.dlss_nr` under Streamline (Windows) with color + motion
vectors + optional G-buffer/masks and the art-direction controls; a native Linux
path would need NVIDIA to publish an NGX `dlss_nr` plugin for Linux. Community
reports of DLSS 5 NR working under Proton use a Vulkan/NGX shim and are not a
supported integration surface.

DLSS Ray Reconstruction is a different feature: it replaces a ray-tracing denoiser
and needs diffuse/specular albedo, normals, roughness and hit distances. MK8 is a
rasterised game with no RT signal; RR is not applicable and is not a path to
"neural rendering" here.

## XeSS on Linux: no official path

XeSS SDK 3.0.2 is Windows-only by construction:

- The SR developer guide's requirements are "Windows 10/11 x64 — 10.0.19043/22000
  or later", "DirectX 12", "DirectX 11", "Vulkan 1.1"; deployment is
  `libxess.dll` (D3D12/Vulkan) and `libxess_dx11.dll` (D3D11), plus the MSVC
  runtime. There is no Linux requirement section because there is no Linux build.
- `inc/xess/xess_vk.h` is a real Vulkan 1.1 API (`xessVKGetRequiredInstanceExtensions`,
  `xessVKGetRequiredDeviceExtensions`, `xessVKGetRequiredDeviceFeatures`,
  `xessVKCreateContext`, `xessVKExecute`, `xessDestroyContext`). It requires
  `shaderStorageImageWriteWithoutFormat` and `mutableDescriptorType`
  (`VK_EXT_mutable_descriptor_type`), with optional `shaderInt8` and
  `shaderIntegerDotProduct`. RADV on this host exposes
  `VK_EXT_mutable_descriptor_type` and `shaderStorageImageWriteWithoutFormat`, so an
  ELF `libxess.so` would work here — Intel simply does not ship one.
- Frame Generation is D3D12-only: `inc/xess_fg/` contains only
  `xefg_swapchain_d3d12.h`; the guide's requirements are Windows 10/11, DirectX 12,
  Intel Arc/Core Ultra or a non-Intel GPU with Shader Model 6.4, XeLL 1.3+, and
  "on non-Intel GPUs the maximum number of generated frames is one".
- Intel's own issue for Linux support (<https://github.com/intel/xess/issues/37>)
  is still open (opened 2025-04-15; comments through 2026-09-16). The only Linux
  route is Proton with the Windows DLL, and even that uses the DP4a fallback unless
  a community patched Mesa/XMX shim is installed (e.g. `xess-xmx-linux`). Those are
  not distributable dependencies for this project.

So for the **native Linux Vulkan runtime**, XeSS SR is impossible without Intel
publishing a Linux library; XeSS FG/XeLL are impossible even on Windows in this
runtime because they require a D3D12 swapchain. XeSS SR remains a possible
Windows-Vulkan option for non-NVIDIA GPUs (`libxess.dll` + `xess_vk.h`), and that is
the only XeSS feature worth planning.

## Integration surface for this runtime

The FSR prototype already provides the expensive part: a post-`vk_frame_suspend`
dispatch point, a scene-depth capture, camera motion vectors and a Halton jitter.
DLSS SR reuses all of it. The differences:

| Input | FSR 4/3 (today) | DLSS SR (NGX VK) |
| --- | --- | --- |
| Colour | game image, 1920x1080 | same, before UI if possible |
| Depth | R32F capture, resampled | same; DLSS expects the same convention (`depthInverted` flag) |
| Motion vectors | camera-only, pixel space | same buffer; `MVScale` 1.0 if already pixel-space |
| Jitter | Halton (2,3), host copy of projection | same values; pass `jitterOffset` in render pixels |
| Reset | on resize/scene change | same, plus on cut/mode change |
| Output | swapchain image | a separate display-size image, then present |

DLSS SR additionally wants the colour **before** tonemap/UI for best quality; the
current FSR path upscales the composed image. That is a quality limitation shared
with FSR, not a blocker for a first milestone.

## Phased plan

**Phase 0 — this turn (done).** Research, pinned manifest, fail-closed downloader,
tests. No runtime change.

**Phase 1 — DLSS SR on native Linux Vulkan (first implementable milestone).**
Add an optional `MK8_DLSS=1` path beside the FSR path:
`dlopen("libnvidia-ngx.so.1")`, resolve the NGX Vulkan entry points, init with a
GUID-shaped project id, create a DLSS context for `1920x1080 -> 2560x1440`, and
evaluate after the existing depth/MV/jitter capture. It must:
- log `[dlss] provider <version>`, input/output resolution, and the exact reason
  when unavailable (no NVIDIA GPU, no NGX core, driver too old, feature init
  failed);
- fall back to the current FSR/bilinear path without changing behaviour when
  unavailable;
- never load or copy vendor files from the repository; use the downloader's
  `$XDG_DATA_HOME/mk8online/nvidia` directory;
- be validated on an NVIDIA host with screenshots and a full race, because this
  checkout cannot run it.

Acceptance evidence: `NVSDK_NGX_VULKAN_Init` OK, `SuperSampling.Available = 1`,
`upscale active 1920x1080 -> 2560x1440`, A/B screenshots at the same frame, and no
change to AMD behaviour. Falsifiers: NGX rejects the project id, the driver lacks
the feature plugin, or the model produces worse output than FSR on the same frame.

**Phase 2 — object motion vectors.** Needed by both DLSS and FSR; without them the
karts ghost. This is renderer work in the guest-to-host draw path, not a vendor
integration, and should be prioritised before judging DLSS quality.

**Phase 3 — DLSS FG 2x on native Linux.** Add `VK_NV_low_latency2` pacing and the
NGX DLSS-G evaluate/present pair; validate frame pacing and latency on RTX 40/50.
Fixed MFG is a separate query (`multiFrameCount`); Dynamic MFG stays out because it
is D3D12-only. Do not enable FG by default.

**Phase 4 — Windows build.** The same NGX Vulkan path works on Windows; Streamline
is only needed if a D3D12 backend is ever added. If XeSS SR is wanted for non-NVIDIA
users, add `libxess.dll` + `xess_vk.h` behind the same optional-component pattern.

**Phase 5 — DLSS 5 NR.** Blocked on NVIDIA granting access to the `dlss_nr`
plugin/model and on RTX 50 hardware. Revisit only when both exist; do not design
around the community Proton shim.

## Distribution rule and the downloader

`tools/fetch_vendor_sdk.py` implements the gate:

```text
$ python3 tools/fetch_vendor_sdk.py --list
$ python3 tools/fetch_vendor_sdk.py --accept-license nvidia
$ python3 tools/fetch_vendor_sdk.py --accept-license intel --only XeSS_SDK_3.0.2.zip
$ python3 tools/fetch_vendor_sdk.py --check
```

- Files go to `$XDG_DATA_HOME/mk8online/<vendor>/` (default
  `~/.local/share/mk8online/<vendor>/`), never into the repository or a package.
- The licence text is fetched first, verified and stored next to the artefacts, so
  the terms travel with the files. Without `--accept-license <vendor>` the tool
  exits 2 before any network access.
- Every file is pinned by URL, byte size and SHA-256; a mismatch removes the
  `.part` file and leaves any previous good copy untouched. A differing local file
  is not replaced without `--force`.
- It refuses non-HTTPS URLs (loopback HTTP is allowed only for the test fixtures),
  manifest paths that escape the vendor directory, and downloads that would leave
  less than the project's 4 GiB disk reserve.
- Exit codes: 0 success, 2 licence not accepted, 3 verification failure, 4 network
  error, 5 unsafe path, 6 not enough space.

The NVIDIA licence permits object-code distribution inside an application, but this
project's rule is stricter; the tool therefore never has a `--install-into-package`
mode. Intel's licence permits unmodified binary redistribution, but the same rule
applies.

## Unverified / open

- No NVIDIA GPU on this host: DLSS SR/RR/FG were not executed, and the
  `libnvidia-ngx.so.1` core was not loaded. The Linux ABI evidence is static
  (`file`, `nm -D`, headers) plus NVIDIA's documentation.
- The exact model family behind the `310.9.16` libraries is inferred from the
  `310.x` DLSS 4.5 family that NVIDIA's driver guide shows; NVIDIA does not label
  the Linux `.so` with a marketing version.
- Fixed MFG on Vulkan is exposed by `multiFrameCount` but is not documented as
  supported; only Dynamic MFG is explicitly D3D12-only. Treat fixed MFG as
  unverified until tested on RTX 50.
- DLSS 5 Neural Rendering was not tested; the public Streamline archive has no
  `dlss_nr` plugin and no model. Community Proton reports are not evidence for a
  supported integration.
- XeSS was not executed anywhere; the SDK is Windows-only and the Linux support
  request is still open. The claim that a future `libxess.so` would run on RADV is
  based on the documented Vulkan features, not on a binary.
- Proton claims come from NVIDIA's driver guide and the DLSS-G guide; no Windows
  build of this runtime exists in this checkout to run under Proton.
- The downloader's real-network path was exercised for the licence and one NGX
  header (26,620 + 40,289 B, both verified); the three large libraries and the two
  ZIPs were fetched manually with `curl` and hashed, not through the tool. The
  tests use a local HTTP fixture, and `--check` was exercised offline.
