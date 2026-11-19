# FSR 3 / FSR 4 native Linux: upscaling and frame generation

Status 20/11/2026. Extends `docs/FSR.md`.

## What exists today

- **FSR 4.1.2 upscaling (native Linux)**: `runtime/mk8/graphics/fsr.cpp` loads the provider from `MK8_FSR_LIB`,
  creates a context, records one upscale after `vk_frame_suspend`, with scene depth capture, camera motion vectors and
  a Halton jitter. Verified in-game (`[fsr] provider FSR 4.1.2`, `[fsr] upscale active 1920x1080 -> 2560x1440`,
  screenshots `local/qualification/fsr4-*-03600.png`).
- **FSR 3.1.6 upscaling**: the 2.4 SDK provider advertises 4.1.2 / 3.1.6 / 2.3.4 through `ffxQuery`;
  `MK8_FSR_VERSION=3.1.6` selects it (with `MK8_FSR_LEGACY_ABI=1` for the GPL 4.1.2 port's legacy descriptor).
- **Frame generation: not implemented.** This document is the plan and the distribution decision.

## Findings (20/11/2026)

- The SDK 2.4 custom tree contains a **Vulkan frame-generation kit**
  (`Kits/FidelityFX/framegeneration/fsr3/vk/FrameInterpolationSwapchainVK.cpp`,
  `ffx_api_framegeneration_vk.h`, provider version **3.1.7**).
- AMD's signed `amd_fidelityfx_framegeneration_vk.dll` contains the exact shipped SPIR-V. Scanning it with
  `tools/extract_fg_spv.py` found **194 unique modules, 12,564,660 bytes**, including both
  **FSR 3.1.7 optical-flow FG** (`CsFrameInterpolationConstants`) and **MLFI** passes
  (`mlfi_4k_pass37/119/146`, `mlfi_8k_pass165`) — i.e. the DLL carries the FSR 3 FG and the FSR 4 ML
  frame-interpolation paths in one provider.
- The SDK source's `framereneration/fsr3/internal/ffx_provider_fsr3framegeneration.cpp` references MLFI, so a
  source-built provider can cover both paths.
- Feasibility of a source build: the FG HLSL compiles to SPIR-V with DXC
  (`dxc -spirv -T cs_6_0 -E CS -DFFX_GPU=1 -DFFX_HLSL=1 …` → 19,400-byte module). The SDK's generated
  `*_permutations.h` headers are not in this tree; they must be generated (or the shipped SPIR-V used).
- The 2.4.0 license MIT-lists the signed Vulkan DLLs, but this project's documented intent and the user's rule are
  **do not ship AMD binaries/assets**: the user downloads AMD's release and our patch derives the Linux artifacts.

## Distribution rule (user decision, 20/11/2026)

- **Nothing FSR/NVIDIA is bundled** in the AppImage, tarball or repository.
- The launcher offers an explicit **download and patch** step:
  1. show AMD's license text (fetched from the pinned SDK tag) and require acceptance;
  2. download only the pinned official SDK release/DLLs (size + SHA-256), never a mirror;
  3. run the local patch step (extract SPIR-V/model banks, build the Linux provider from AMD's sources where
     applicable);
  4. store everything beside the user data (`local/`), with the license text next to it.
- This mirrors the mature pattern: dkb-pc-recomp (license text → I accept → pinned DLL size/SHA-256), totk-pc-recomp
  (NVIDIA DLSS at a pinned tag), fsr4-vk (user runs `setup.py --dll`). dkb's FSR 4 URLs are pinned to
  `v2.4.0`: `amd_fidelityfx_loader_vk.dll` 39,184 B and `amd_fidelityfx_upscaler_vk.dll` 36,135,696 B, plus
  `docs/license.md` (26,620 B).

## Plan

1. **FG provider assets**: decide between (a) generating the FG permutations from SDK HLSL with DXC (needs the
   permutation table semantics: `FORCE_WAVE64`, `ALLOW_FP16`, `LOW_RES_MOTION_VECTORS`, `JITTER_MOTION_VECTORS`,
   `DEPTH_INVERTED`) and (b) using the 194 shipped SPIR-V modules extracted from the signed DLL (exact bytes, no
   compile flags to reconstruct). (b) matches the existing FSR 4 port's approach.
2. **Native provider**: build a Linux `.so` that exposes the FFX provider API for the FG swapchain
   (`FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK`) and dispatches FSR 3 FG and MLFI (FSR 4) depending on the
   selected mode/model availability.
3. **Runtime integration**: wrap/replace the present path (`vk_frame_begin`/`frame_end`) with the FFX frame
   interpolation swapchain contract: per-frame `DispatchDescFrameGenerationPrepare` (depth, motion vectors, jitter),
   UI composition flag, and present pacing; fall back to the current path when the provider is absent.
4. **Launcher**: an "FSR" page with upscaling version (3.1.6 / 4.1.2), frame generation (off / FSR 3 / FSR 4 MLFI),
   and the terms-gated download/patch button. Settings persist in `settings.ini`.
5. **Validation**: provider `ffxQuery` versions; in-game screenshots with FG on; compare inputs (UI, depth, MVs);
   measure on an idle host only.
6. **Packaging**: rebuild the AppImage and tarball with the launcher and runtime, **without** FSR artifacts; verify
   both packages start and the FSR page shows the download option.

## Build state (20/11/2026)

- The signed FG DLL's 194 SPIR-V modules split into: 23 `CS` entry points (FSR 3.1.7 FG pass variants),
  MLFI layers named `mlfi_4k_pass*/mlfi_8k_pass*` (FSR 4 ML), and UI/debug `main`/`mainPS` shaders.
- **All 20 SDK FG/optical-flow/UI HLSL shaders compile with DXC**: compute with
  `-T cs_6_0 -E CS -fspv-target-env=vulkan1.2 -DFFX_GPU=1 -DFFX_HLSL=1` (the pyramid/wave shaders need the
  Vulkan 1.2 target), UI composition with `-T ps_6_0 -E mainPS`. Result: `ok=20 fail=0`.
- The SDK's generated `*_permutations.h` blob tables are absent; the next build step is to generate them for the
  default permutation set (wave32, fp32, no low-res/jitter/inverted depth) and implement the provider's
  `frameInterpolationGet*PermutationBlobByIndex` to return those blobs, then add the other variants.
## Provider build state (20/11/2026)

- Generated `fg_blobs.cpp`: the 23 FSR 3.1.7 `CS` modules embedded, implementing
  `frameInterpolationGetPermutationBlobByIndex`/`IsWave64` and the optical-flow equivalents, so the SDK's missing
  generated permutation headers are bypassed.
- CMake build of the provider (API + VK backend + frame-interpolation/optical-flow + VK swapchain) reaches the
  SDK's loader-internal boundary: `ffx_api.cpp` needs `ffxProviderExternal`, a type **not shipped in the SDK
  source** (it exists only in the loader component). The provider class itself
  (`ffxProvider_Fsr3FrameGenerationSwapChain`) carries CreateContext/Query.
- Decision: write a thin provider export layer (`ffxCreateContext`/`ffxQuery`/`ffxDestroyContext`) around that
  class instead of linking the loader-internal API, then verify `ffxQuery` reports 3.1.7.
## Evidence so far

- `tools/extract_fg_spv.py` on `amd_fidelityfx_framegeneration_vk.dll`: 194 modules / 12.5 MB, names above.
- DXC compile of `ffx_frameinterpolation_pass.hlsl` → `/tmp/fi.spv` (19,400 B).
- Distribution research: 2.4.0 license MIT-lists the `_vk` signed DLLs; ecosystem practice is download-at-setup
  with a license gate (dkb), never bundle.
