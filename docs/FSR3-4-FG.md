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
## Provider built and validated on Linux (20/11/2026)

- `tools/gen_fg_blobs.py` compiles every frame-interpolation and optical-flow pass from the SDK HLSL with DXC
  (`cs_6_0`, `-fspv-target-env=vulkan1.2`), reflects the SPIR-V descriptors, rewrites the bindings to the scheme the
  SDK Vulkan backend expects (samplers 320/321, CBVs 128+, SRV textures 0+, UAV textures 64+, SRV buffers 192+,
  UAV buffers 224+), reassembles with `spirv-as`, and emits `fg_blobs.cpp` with a `FfxShaderBlob` per pass:
  **11 frame-interpolation passes x 2 depth conventions + 7 optical-flow passes**.
- The SDK's loader-internal `ffxProviderExternal` and the per-effect provider list are not part of the published
  source; the build adds a minimal external-provider stub and `fg_provider_list.cpp` registering
  `ffxProvider_Fsr3FrameGeneration` and `ffxProvider_Fsr3FrameGenerationSwapChain`.
- Linux build adaptations (in the prepared SDK copy, not committed AMD source): `Windows.h` compatibility
  (events, threads, QPC, critical sections), `_countof`/`strncpy_s`/`wcscpy_s`/`swprintf_s`, zlib-backed
  `mz_uncompress`, `<codecvt>` removal for GCC 16, `0ui64` literals, backslash includes, Win32-only
  full-screen-exclusive swapchain structs, and a doubled `FFX_FRAMEINTERPOLATION_CONTEXT_SIZE` because `wchar_t` is
  4 bytes on Linux (the private context embeds 259 `wchar_t[64]` bindings per pipeline).
- **Verification (real hardware, RADV GFX1201, AMD RX 9070 XT):**
  - `ffxQuery` version enumeration for `FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK` returns `3.1.7`.
  - `ffxCreateContext(FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION)` returns `FFX_API_RETURN_OK` for
    1920x1080 and 1280x720, with normal depth (`flags=0x2`) and inverted depth (`flags=0xa`, the separately compiled
    d1 blob set): the full optical-flow + frame-interpolation pipeline set and all internal resources are created
    from the generated blobs; `ffxDestroyContext` returns OK in every case.
- The SDK source does **not** implement MLFI (FSR 4 ML frame interpolation). It only has the
  `FFX_EFFECT_MLFRAMEGENERATION` id and warning strings about the camera matrices MLFI needs; the MLFI SPIR-V and
  its model banks exist only in AMD's signed DLL. The source-built provider is therefore **FSR 3.1.7 frame
  generation**; FSR 4 MLFI delivery is tracked in `docs/FSR4-MLFI.md`.

## Runtime integration: native frame generation (20/11/2026)

- **Toggle:** `MK8_FSR_FG=fsr3` (default off). Any other non-empty value is ignored with one log line. Because the
  current source-built provider is not dispatch-ready (see the blocker below), enabling also requires
  `MK8_FSR_FG_ALLOW_UNVALIDATED_PROVIDER=1`; without it the bridge logs the exact precondition failure and leaves
  the normal swapchain/present path. The provider is loaded from `MK8_FSR_FG_LIB`, else
  `$XDG_DATA_HOME/mk8online/fsr/libamd_fidelityfx_framegeneration_vk.so`, else
  `$HOME/.local/share/mk8online/fsr/libamd_fidelityfx_framegeneration_vk.so`.
- **Patch:** `toolchain/fsr_fg.py` runs last in `toolchain/host.py` (after the WSI profiling patches it wraps). It
  finds a second present-capable queue family with two queues (RADV: the compute family), requests game/present/
  acquire queues at device creation, replaces the window swapchain with the FFX proxy, and records the per-frame
  `ffxDispatchDescFrameGenerationPrepareV2` in the game's command buffer before its submission. The present is
  routed without the game's queue lock (the provider's present thread submits through that lock).
- **Bridge:** `runtime/mk8/graphics/fsr_fg.cpp` enables only when the swapchain provider reports exactly
  **3.1.7**, parses that into `ffxCreateContextDescFrameGenerationSwapChainVersionVK`, creates the swapchain
  context with `ffxCreateContextDescFrameGenerationSwapChainVK` and the frame-generation effect context with
  `ffxCreateContextDescFrameGeneration` + `ffxCreateBackendVKDesc`, and routes acquire/present through
  `ffxQueryDescSwapchainReplacementFunctionsVK`. Any failure disables the bridge, restores the original swapchain
  and logs one reason; it never returns a fake success.
- **Inputs:** `runtime/mk8/graphics/fsr.cpp` shares the captured scene depth and the camera motion-vector compute
  with the prepare dispatch. Without a captured view/projection it uses a neutral depth and zero motion vectors,
  and logs `[fsr_fg] WARNING: motion vectors are zeros ...`; without a depth copy it logs the neutral source.
  When both `MK8_FSR4` and `MK8_FSR_FG` are set, the upscaler stays off: combining is a TODO and FG runs on the
  game's native window image.
- **Provider build fix:** `tools/fg-linux/CMakeLists.txt` now defines `FFX_FRAMEGENERATION=1`. Without it the
  Vulkan backend leaves `fpSwapChainConfigureFrameGeneration` null and `ffxConfigure` on the frame-generation
  context dereferences null; a provider built before this fix cannot use the swapchain path.
- **Provider blocker (20/11/2026):** the generated shader blobs are not dispatch-validated. The Khronos validation
  layer reports storage-image format mismatches during the first prepare/interpolation: `rw_dilated_motion_vectors`
  declares `Rg32f` but the SDK resource view is `R16G16_SFLOAT`, `rw_optical_flow_input*` declares `R32ui` but the
  view is `R8_UINT`, `rw_optical_flow` declares `Rg32i` but the view is `R16G16_SINT`, and `rw_output` writes three
  components into a `B8G8R8A8_UNORM` image. `rw_counters`/`r_counters` are also declared but absent from the bound
  descriptor sets. On the second present the GPU faults (`radv: GPUVM fault detected at address 0x8001198000`), the
  composition command lists never complete, and the provider's command pool dereferences a null entry. This is a
  blob-generation issue (`tools/gen_fg_blobs.py` needs the SDK's resource-format override table and the correct
  component widths), not a runtime-path issue: context creation, the 3.1.7 check, the first prepare and the first
  present all succeed. Until the blobs are fixed, the bridge refuses to enable without
  `MK8_FSR_FG_ALLOW_UNVALIDATED_PROVIDER=1`; enabling it reproduces the fault and is for provider development only.
- **Tests:** `tests/test_fsr_fg.py` applies the real patch chain (`fsr` → `submit_profile` → `acquire_bound` →
  `fsr_fg`) to the pinned runtime and runs `tests/fixtures/fsr_fg_swapchain.cpp`: a headless 1920x1080 device that
  checks 3.1.7, creates both contexts, queries the replacement functions, records a prepare dispatch and presents.
  It exits non-zero on any failure; `MK8_FSR_FG_REQUIRE_NATIVE=1` turns a missing provider into a failure instead
  of a skip.
- **Evidence:** the native smoke test passes on RADV GFX1201 (RX 9070 XT) with provider 3.1.7 (3.1.7 check,
  swapchain context, 1080p effect context, replacement functions, prepare dispatch, present, `vkDeviceWaitIdle`
  clean). The runtime builds (221 units). The FG-disabled boot reaches the start screen and presents (frame 597 at
  1920x1080, no `[fsr_fg]` activation lines). `MK8_FSR_FG=fsr3` alone logs the unvalidated-provider gate and keeps
  the normal path; with `MK8_FSR_FG_ALLOW_UNVALIDATED_PROVIDER=1` the log shows `swapchain provider offers 3.1.7`,
  `swapchain provider 3.1.7`, `active: FFX frame-interpolation swapchain 1920x1080`, the first prepare and
  `first interpolated frame recorded`, then the provider GPU fault described above. Image quality, pacing/latency,
  a real 120 Hz presentation and combining with the upscaler are **not** verified; `gameplay_verified` stays false.

```sh
# native smoke test (no game, no window)
MK8_FSR_FG_LIB=$XDG_DATA_HOME/mk8online/fsr/libamd_fidelityfx_framegeneration_vk.so \
  MK8_FSR_FG_REQUIRE_NATIVE=1 python3 -m unittest tests.test_fsr_fg -v

# runtime: normal path (FG off), the safe gate, and provider development
./mk8drecomp <game-root>
MK8_FSR_FG=fsr3 ./mk8drecomp <game-root>                     # logs the unvalidated-provider gate
MK8_FSR_FG=fsr3 MK8_FSR_FG_ALLOW_UNVALIDATED_PROVIDER=1 \
  MK8_FSR_FG_LIB=/path/to/libamd_fidelityfx_framegeneration_vk.so ./mk8drecomp <game-root>
```

## Reproducing the provider and the checks

```sh
# from the user's own FidelityFX SDK 2.4.0 download (MIT); nothing AMD ships as a binary is used.
# Pinned official source archive: 253,592,010 bytes,
# sha256 111f9b19c0c9b8a1f24d2a5104095b8e626f12deb4eca53cd0c29921aa1fd43a
python3 tools/build_fg_provider.py --sdk-archive FidelityFX-SDK-v2.4.0.tar.gz --sdk-size 253592010 --sdk-sha256 111f9b19c0c9b8a1f24d2a5104095b8e626f12deb4eca53cd0c29921aa1fd43a --dxc /path/to/dxc --self-test

# DXC can also be fetched by the launcher: official v1.9.2609 Linux archive,
# https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2609/linux_dxc_2026_09_28.x86_x64.tar.gz
# 13,297,763 bytes, sha256 96faadc7f5c282d2ffda49804beb4c3ee38127bc252b723234e3c5cdf7aa39a1
# tools/build_fg_provider.py --dxc-archive <archive> --dxc-size 13297763 --dxc-sha256 96faadc7...39a1

# native provider tests (skip when the provider/SDK headers are absent)
MK8_FG_LIB=$XDG_DATA_HOME/mk8online/fsr/libamd_fidelityfx_framegeneration_vk.so MK8_FG_SDK=$HOME/.cache/mk8online/fsr-build/sdk-copy python3 -m unittest tests.test_fg_provider -v
```

A full from-archive run with the pinned hash, DXC shader generation, CMake build and the headless 1080p context self-test completed with exit 0.

## Evidence so far

- `tools/extract_fg_spv.py` on `amd_fidelityfx_framegeneration_vk.dll`: 194 modules / 12.5 MB, names above.
- DXC compile of `ffx_frameinterpolation_pass.hlsl` → a 19,400-byte SPIR-V module (`tools/fg-linux` keeps the build recipe).
- Distribution research: 2.4.0 license MIT-lists the `_vk` signed DLLs; ecosystem practice is download-at-setup
  with a license gate (dkb), never bundle.
