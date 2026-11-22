# FSR 4 ML frame generation (MLFI) on Linux: contents of AMD's DLL and a delivery verdict

Status 20/11/2026. Companion to `docs/FSR3-4-FG.md`, which covers the **FSR 3.1.7** analytic frame
generation provider that this project already builds and validates natively. This document answers a
narrower question: **FSR Frame Generation 4.0.2** — AMD's ML frame interpolation ("MLFI") — what is
inside the signed Vulkan DLL, what a Linux implementation would need, and whether the
download-and-patch path can deliver it.

Short answer: the shaders and the model weights are in the DLL and are extractable (tool committed,
`tools/extract_mlfi.py`); RADV on RDNA 4 accepts the shaders (with a driver defect on seven storage-image passes);
but the *host driver* that turns them into 191 dispatched passes is Windows x86-64 code that AMD does
not publish. That host logic, not missing data or a missing API, is what blocks a native provider.
A native Windows check on an NVIDIA RTX 5080 adds a gate before that: the signed DLL does not register
the 4.0.2 provider on that GPU at all (§6.1).

## 1. What the signed DLL is

`amd_fidelityfx_framegeneration_vk.dll` from the official FSR SDK 2.4.0 archive:

| property | value |
| --- | --- |
| size | 36,806,928 bytes |
| SHA-256 | `d002dabff8c5e71eaa715fc99438cd4dc81230262a7897b16bc402efec3498b4` |
| format | PE32+ x86-64, 6 sections |
| .text | 1,146,819 B virtual (RVA 0x1000) |
| .rdata | 35,475,274 B virtual (RVA 0x119000) |
| .data | 146,944 B virtual (RVA 0x22ee000) |
| exports | `ffxConfigure`, `ffxCreateContext`, `ffxDestroyContext`, `ffxDispatch`, `ffxQuery` |
| imports | `vulkan-1.dll`, `MSVCP140`, `VCRUNTIME140(_1)`, ucrt, `KERNEL32`, `USER32`, `VERSION` |
| version resource | FileVersion `4.0.2.4555` |
| provider version string | `4.0.2` (end of the pass-name pool) |

The import list contains **no file I/O**: the only stdio functions are
`__stdio_common_vsprintf`/`vswprintf_s`, KERNEL32 is imported for `LoadLibraryA/Ex` (to reach
`vulkan-1.dll`) and thread/memory primitives, and `CreateFileW`/`ReadFile`/`fopen` are absent. The SDK
archive contains no `.bin`/`.dat`/model files at all. That answers the first question: the MLFI model
does not live next to the DLL and is not downloaded separately by the installer — it is embedded in
the image.

RTTI strings show one DLL implementing two providers:

```
.?AVffxProvider_MLFrameGeneration@@
.?AVffxProvider_MLFrameGenerationSwapChain@@
.?AVMLFrameGenerationSwapChainVK@@
.?AVIMLFrameGenerationSwapChainVK@@
.?AVffxProvider_Fsr3FrameGeneration@@
.?AVffxProvider_Fsr3FrameGenerationSwapChain@@
```

AMD's own documentation for the effect is
`Kits/FidelityFX/docs/techniques/frame-interpolation-ml.md` ("AMD FSR™ Frame Generation 4.0.2",
2026-10-28): ML interpolation, RX 9000 or later, Vulkan 1.2 at feature parity, and a mandatory call
order of `ffxConfigure` → `ffxDispatch(PrepareV2)` → `ffxDispatch(FrameGeneration)`. The SDK source
ships the *API* and the FSR 3 implementation, but no MLFI implementation and no model loader.

## 2. Module set: the previous count was wrong (194 → 416)

The SPIR-V is stored uncompressed in `.rdata`, but the kernels are packed **back to back**, sometimes
separated only by a descriptor record with no zero padding. The original scanner
(`tools/extract_fg_spv.py`) walked until the first zero word and treated one kernel plus its
successor as a single module; kernels that were followed directly by a descriptor were rejected as
malformed. Result: 194 modules / 12,564,660 bytes, which is an undercount.

The scanner now stops at a top-level `OpFunctionEnd` unless the next instruction is another
`OpFunction` (the only thing the SPIR-V spec allows after a function). That yields a set that is
consistent with the pass pool:

| kind | modules | bytes | entry points |
| --- | --- | --- | --- |
| `mlfi_4k` | 191 | 11,754,920 | `mlfi_4k_pass0` … `mlfi_4k_pass190` |
| `mlfi_8k` | 191 | 11,754,268 | `mlfi_8k_pass0` … `mlfi_8k_pass190` |
| `fsr3fg` | 23 | 287,692 | `CS` (FSR 3.1.7 analytic FG) |
| `ui` | 11 | 47,788 | `main`, `mainPS`, `mainVS` (swapchain UI composition) |
| **total** | **416** | **23,844,668** | |

191 modules per resolution is exactly the number of names in the pass pool (below), which is the
strongest available check that no kernel is being swallowed or double-counted. Module sizes range from
184 B (a no-op stub kernel) to 658,788 B; median 7.5 KiB.

`spirv-val` accepts 326 of the 416. The other 90 fail on a single rule —
`Expected Use of Matrix type and Result Type to be identical` for `OpFConvert` on cooperative-matrix
types — and none fail structurally (no invalid opcodes, no truncated streams). The construct comes
from AMD's compiler (Google spiregg) emitting `OpTypeCooperativeMatrixKHR` + `OpFConvert` for the
FP8/FP16 matrix path. Whether that is a validator limitation or a real spec violation is discussed in
§6, because the driver is the authority that matters.

## 3. The pass graph and the descriptor ABI the shaders expect

`.rdata` holds a contiguous, 16-byte-aligned name pool at file offset `0x22e2508` (6,406 bytes):

```
mlfi_4k_pass0, CsFrameInterpolationConstants, CsTensorSizes, r_input_rgb0, r_input_rgb1,
ScratchBuffer, g_sampler_linear, InitializerBuffer, rw_output_flow_0to05, rw_output_flow_1to05,
r_input_packed_splatted0to05, r_input_packed_splatted1to05, mlfi_4k_pass1 … mlfi_4k_pass190,
r_hudless_ui, rw_prediction, mlfi_8k_pass0 … mlfi_8k_pass190, 4.0.2
```

396 strings = 191 + 191 pass names + 13 resource names + the version. This is the runtime pass list.
The `4k`/`8k` split is the output-resolution class of the context (AMD's docs describe one ~195-pipeline
context per effect, so the two sets are alternatives, not both per frame). The SDK's own comment
(`Kits/FidelityFX/api/internal/ffx_internal_types.h:117`) says MLFI creates "~195 compute pipelines per
effect context (up to `FFX_MLFI_*_PASS_COUNT` model passes + preprocess passes)"; 191 named model passes
plus the preprocess passes lands on that number (the four preprocess kernels are not separately named in
the pool, so the exact split is inferred, not proven).

Every model pass binds the same set-0 layout, read from the modules' own decorations (350 of the 382
passes declare the tensor table; the rest are stubs or pure preprocess):

| binding | name | type | role |
| --- | --- | --- | --- |
| 0 | `InitializerBuffer` | storage buffer (ByteAddressBuffer) | **model weights** |
| 128 | `ScratchBuffer` | storage buffer | per-dispatch tensor workspace |
| 256 | `CsFrameInterpolationConstants` | uniform buffer | frame/camera constants |
| 257 | `CsTensorSizes` | uniform buffer, `uint4[92]` (1,472 B) | tensor shape/stride table, filled by the host |
| 320 | `g_sampler_linear` | sampler | resampling |

This is the same binding convention the project's FSR 3.1.7 Vulkan backend already uses (SRV buffers
at 0+, UAV buffers at 128+, CBVs at 256+, samplers at 320), so a Linux provider would not need a new
descriptor model.

The composite passes (`mlfi_4k_pass151/152/190`, `mlfi_8k_pass151/152/190`) additionally bind images:
`r_input_rgb0/1` (1/2), `r_input_packed_splatted0to05/1to05` (5/6), `r_hudless_ui` (7),
`rw_output_flow_0to05/1to05` (3/4, or 129/130 in `pass151`) and `rw_prediction` (129).

What the shaders expose is only the *shape* of the ABI. The contents that the host must compute —
the 92 `uint4` tensor descriptors per pass, the offset of each layer's weights inside
`InitializerBuffer`, the dispatch geometry (workgroup counts per pass), and the pass order — exist
only in the DLL's machine code.

## 4. The model is in the DLL

The largest non-SPIR-V data region starts at file offset `0xdf7468`:

| property | value |
| --- | --- |
| offset | `0xdf7468` |
| size | 10,720,312 bytes |
| SHA-256 | `43a77c641f8ba3cd6ee08939661578097bbf0d9f43007939ccb64c161349da90` |
| structure | 4 KiB descriptor/scales table, then fp32 tensors, then ~9.4 MiB of fp16 tensor data, then four small tables |

Evidence that this is trained tensor data and not code or a cache:

* 98.3 % of its 16-bit words have an fp16 exponent in 2^-13 … 2^-3, with the mode at 2^-6 (24.7 %),
  2^-7 (21.0 %), 2^-5 (16.8 %), 2^-8 (13.0 %) — the smooth, narrow magnitude distribution of weights.
  Random bytes would put only ~63 % of words in a "normal fp16" range and spread the exponents flat.
* The first kilobyte contains a table of `{u64, 3×fp16, padding}` records and a small set of fp32
  tensors (`0xbe9f9d3c` = −0.312, `0x3c962f5f` = 0.018 …), i.e. the usual mix of low-precision
  weights and full-precision scales.
* Splitting the region at its low-density boundaries (4 KiB windows with > 40 % zero bytes) yields 20
  deterministic segments: 4,096 + 36,864 + 8,192 + 36,864 + 4,096 + 4,096 + 20,480 + 9,854,976 +
  4,096 + 4,096 + 12,288 + 356,352 + 8,192 + 4,096 + 4,096 + 286,720 + 12,288 + 4,096 + 53,248 +
  1,080 bytes. The 9.85 MB segment is the bulk of the network; the small ones are per-pass tables.

No path strings, no archive magic (the upscaler's `FXZF` marker appears once in the whole image, far
from this region), and no file I/O imports: the bank is linked into the image and referenced by the
provider's code.

## 5. ABI: MLFI reuses the frame-generation API

The DLL exports exactly the five FFX API entry points, and `ffxProvider::CanProvide` matches on
`descType & FFX_API_EFFECT_MASK`. The descriptors are the ordinary frame-generation ones from
`Kits/FidelityFX/framegeneration/include/ffx_framegeneration.h`:

| descriptor | value |
| --- | --- |
| `FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION` | `0x00020001` |
| `FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2` | `0x0002000c` |
| `FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION` | `0x00020003` |

Both the FSR 3 provider and the ML provider answer the same effect id; which one creates the context
is decided inside the DLL (RDNA 4 + Windows 11 on the vendor path), and the documented call order is
mandatory for 4.0.2. The ML path also requires `cameraPosition/cameraUp/cameraRight/cameraForward` in
the `PrepareV2` descriptor; the SDK source prints warnings when they are missing
(`ffx_provider_fsr3framegeneration.cpp:746`).

So a Linux provider is **not** blocked by a different API or a different effect id: it must implement
the same five exports and the same descriptors as the FSR 3.1.7 provider this project already ships.
What is undocumented is the *context sub-type that selects ML over FSR 3* (both providers accept
`0x00020001`) and everything host-side.

## 6. Driver support on Linux (measured, not assumed)

The MLFI modules declare SPIR-V 1.5 with `SPV_KHR_cooperative_matrix`,
`SPV_NV_cooperative_matrix2`, `SPV_EXT_float8` (FP8 cooperative matrices), plus `Float16`, `Int8`,
the Vulkan memory model and variable pointers. RADV on this host (Mesa, RADV GFX1201, API 1.4.363)
reports:

```
VK_KHR_cooperative_matrix (rev 2)
VK_EXT_cooperative_matrix_maintenance1 (rev 1)   # the renamed NV_cooperative_matrix2
VK_EXT_shader_float8 (rev 1)
cooperativeMatrixConversions / PerElementOperations / Reductions = true
shaderFloat8CooperativeMatrix = true
```

`tools/mlfi_probe.c` (committed) enables exactly those features and creates a compute pipeline for
each extracted module. Result on the 416-module set:

| probe outcome | modules | notes |
| --- | --- | --- |
| compute pipeline created | 405 | every model pass outside the composite set, all 23 FSR 3 `CS`, 6 UI compute modules |
| driver segfault in `vkCreateComputePipelines` | 7 | `mlfi_4k_pass151/152/190`, `mlfi_8k_pass151/152/190`, one UI splat compute `main` |
| not probed (graphics stages) | 4 | `mainPS`×3, `mainVS` — pixel/vertex entry points, not compute |

Device: `AMD Radeon RX 9070 XT (RADV GFX1201)`, Vulkan 1.4.363, Mesa driver 109457507.

Seven modules crash RADV inside `vkCreateComputePipelines` (segfault in `libvulkan_radeon.so`, not a
clean `VkResult`): `mlfi_4k_pass151/152/190`, `mlfi_8k_pass151/152/190`, and the UI splat compute
`main` (5,004 B, writes `rw_output_mv_0_to_1` through `Int64ImageEXT` atomics). They are exactly the
modules that bind storage images; the smallest reproducer is `pass151` (2,648 B, two storage images,
`StorageImageWriteWithoutFormat`, which the device supports and the probe enables). The crash
reproduces with the correct entry point, with the layout derived from the module's own decorations,
with an **empty** descriptor layout (`MLFI_PROBE_EMPTY_LAYOUT=1`), with cooperative-matrix/float8
features disabled, and — for the splat module — with `VK_EXT_shader_image_atomic_int64` enabled (the
extension Cauldron adds for exactly this shader). So it is a Mesa defect in shader compilation, not a
descriptor or feature mistake in the probe. It is a fixable driver bug, but "works on today's RADV" is
not yet true for the composite/splat stage.

## 6.1 Native Windows / NVIDIA: the 4.0.2 provider is not registered

On 20/11/2026 the signed DLL was run natively on Windows 10 with an NVIDIA RTX 5080, driver 621.01 and
Vulkan 1.4.357 (not Wine), using `tools/mlfi-capture/mlfi_capture.c` cross-compiled by
`tools/windows-fsr/build.sh`. The Vulkan device enables the full set the modules need
(`VK_KHR_cooperative_matrix`, `VK_NV_cooperative_matrix2`/maintenance1, `VK_EXT_shader_float8`,
`VK_EXT_shader_image_atomic_int64`, memory model, variable pointers); every related feature query is true.
Results:

- `ffxQuery(FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION)` returns **only** `3.1.6`
  (`0xf600000000c01006`). `4.0.2` is absent even though its id `0xf600000001000002` is present in the DLL.
- Forcing that id through `ffxOverrideVersion` returns `ffxCreateContext=4` (`FFX_API_RETURN_NO_PROVIDER`),
  context null: 0 pipelines, 0 dispatches, no device loss. The global debug callback at verbose level emits
  no diagnostic, so the refusal is silent.
- The FSR 3.1.6 provider in the same DLL does run: 1280x720, three frames, 18 pipelines, 97 dispatches
  (3 prepare + 94 framegen), 0 substituted, all `ffxConfigure`/`prepare`/`framegen`/`submit` return 0.

AMD's FSR SDK sample documentation states that "the entries available depend on the installed driver", and
the MLFI technique documentation requires RX 9000 or later. The 4.0.2 omission on the RTX 5080 is therefore
the documented device/driver gate, not a probe error. It also means the shipped-DLL workaround cannot
deliver MLFI on a non-RDNA4 machine: the provider is never created, so no prepare/dispatch trace exists to
capture. The layout in §3 was measured on an AMD GPU; it cannot be measured on this host.

## 7. Verdict

**Can a Linux provider be built that loads these extracted modules and models?** Not from the data
alone. Three things are in place and one is not:

| item | status |
| --- | --- |
| SPIR-V modules (416) | present, extracted, driver-accepted except 7 storage-image passes |
| model bank (10.72 MB) | present in the DLL, extracted, hashed |
| FFX API surface and descriptor bindings | same as the working FSR 3.1.7 provider |
| host driver: 191-pass order, `CsTensorSizes`, weight offsets, dispatch geometry, model upload | **only as Windows x86-64 machine code; no source, no documentation** |

The last row is the blocker. A provider that merely uploaded the 10.72 MB bank and dispatched 191
pipelines with guessed tensor tables would produce garbage, and there is no reference output on Linux
to compare against. Calling this "supported" would be a fictitious success, which this project
explicitly forbids.

**Minimal next step with the best information gain**: run the Windows DLL once under Wine (Proton's
Vulkan path already works for the vendor DLL) with a thin Vulkan layer or `vkCreate*` interposer that
records, for one 4 K frame:

1. every `vkCreateComputePipelines` (module bytes, specialization constants),
2. the contents of `InitializerBuffer` and `ScratchBuffer` uploads,
3. the contents of the `CsTensorSizes` uniform buffer per dispatch,
4. the `vkCmdDispatch` group counts and the order of dispatches.

That turns the undocumented host layout into data and reduces "reimplement the driver" to "replay a
recorded frame". It is a bounded experiment (one frame, one resolution) and the natural follow-up to
this document.

**Genuinely blocked by AMD**: the host source, the model's tensor layout documentation, and the
context-selection rule for ML vs FSR 3. Nothing here requires a missing download: the weights ship in
the DLL.

**Legal**: the FSR SDK 2.4.0 licence file MIT-lists the signed binaries. The list includes

```
- `Kits\FidelityFX\signedbin\amd_fidelityfx_framegeneration_vk.dll`
```

and the paragraph after the list reads

```
are subject to the following license:

Copyright (C) Advanced Micro Devices, Inc.

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
associated documentation files (the “Software”), to deal in the Software without restriction, ...
```

so extraction and derived work are permitted for that file; the base licence that covers the rest of
the SDK forbids reverse engineering.

This project's preference is still to observe public Vulkan calls (the Wine capture) rather than
disassemble the provider. Note that the older licence copy vendored under
`linux/third_party/fsr4-vk/LICENSES/` only lists the DX12 DLLs; the authoritative list is the one in
the 2.4.0 archive. The project rule still applies: **nothing AMD is bundled**; extraction happens on
the user's machine from the user's download.

## 8. Official download and hashes (nothing bundled)

There is no AMD-hosted "FSR 4 model" download for frame generation. The official artefact is the SDK
archive; other projects' `fsr4-assets` folders (rexglue/GiantsRecomp, fsr4-vk) are **upscaler** assets
extracted from `amd_fidelityfx_upscaler_vk.dll`, not a separate AMD download.

| artefact | value |
| --- | --- |
| landing page | `https://gpuopen.com/download-fsr-sdk/` |
| redirect target | `https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/archive/refs/tags/v2.4.0.zip` |
| archive size | 254,235,565 bytes |
| archive SHA-256 | `27120e3bbe0d0b30213825ad40f19077563f04f04572e252109a392d5fb522cb` |
| DLL inside | `Kits/FidelityFX/signedbin/amd_fidelityfx_framegeneration_vk.dll`, 36,806,928 B, SHA-256 `d002dabf…498b4` |

Caveat for the download-and-patch step: GitHub tag archives are generated on demand and are not
contractually byte-stable. Pin the size and hash above, and if they differ, fall back to the
structural checks (`--allow-unknown-layout`, which verifies the PE shape, the 191+191 name pool and a
dense fp16 bank instead of exact offsets) rather than trusting a mirror.

## 9. What this repository now has

* `tools/extract_mlfi.py` — extracts the 416 modules (named by entry point), the pass pool, the
  model bank and its 20 segments into `$HOME/.cache/mk8online/mlfi` (or `--out`), with a
  `manifest.json` (per-file SHA-256, content root, descriptor bindings per pass, fp16 histogram) and
  an `index.tsv`. It refuses the pinned-build mismatch, refuses unknown builds without
  `--allow-unknown-layout`, refuses an output directory inside the repository, and refuses to
  overwrite an existing extraction without `--force`. Optional `--validate-spirv` runs `spirv-val`:
  structural failures abort, while the 90 modules that trip only the cooperative-matrix conversion
  rule are recorded under `spirv_val` in the manifest and reported as a warning.
* `tools/mlfi_probe.c` — creates a compute pipeline per module with the features the shaders require;
  used for the driver measurements above.
* `tools/extract_fg_spv.py` — the generic front end, now using the corrected module-boundary rule.
* `tests/test_extract_mlfi.py` — synthetic-PE tests for module boundaries, pool validation, model
  discovery, the output guard and fail-closed behaviour, plus an opt-in pinned test
  (`MK8_MLFI_DLL=…`).

The extractor's own summary on the pinned build:

```
pinned provider 4.0.2
modules: 416 (4k 191, 8k 191, fsr3 23, other 11)
passes:  191 4k + 191 8k names
model:   10720312 bytes at 0xdf7468, 20 segments
output:  /home/<user>/.cache/mk8online/mlfi (content_root 647d112d…)
```

Reproduction:

```
python3 tools/extract_mlfi.py --dll <sdk>/Kits/FidelityFX/signedbin/amd_fidelityfx_framegeneration_vk.dll
python3 tools/extract_mlfi.py --dll … --validate-spirv
cc -O2 -o mlfi_probe tools/mlfi_probe.c -lvulkan
./mlfi_probe ~/.cache/mk8online/mlfi/modules/*.spv
MK8_MLFI_DLL=<dll> python3 -m unittest tests.test_extract_mlfi
```

## 10. Corrections this work implies elsewhere

* `docs/FSR3-4-FG.md` states the DLL carries "194 unique modules, 12,564,660 bytes". The corrected
  figures are **416 modules / 23,844,668 bytes**; the 194 figure came from the scanner bug fixed here.
  The FSR 3.1.7 provider itself is unaffected — it builds its 23 `CS` passes from SDK HLSL with DXC,
  not from the DLL.
* The licence file vendored at `linux/third_party/fsr4-vk/LICENSES/AMD-FidelityFX-SDK-license.md`
  predates the `_vk` DLL entries; use `Kits/FidelityFX/docs/license.md` from the 2.4.0 archive.

## 11. Verified vs hypothesised

| claim | state |
| --- | --- |
| model weights are embedded in the DLL | **verified** (dense fp16/fp32 bank, no file I/O imports, no assets in the archive) |
| the DLL contains 416 distinct SPIR-V modules, 191 per resolution | **verified** (extractor + pass-pool agreement) |
| MLFI uses the same five FFX exports and frame-generation descriptors | **verified** (export table, RTTI, SDK headers/docs) |
| RADV exposes the required cooperative-matrix/FP8 features on GFX1201 | **verified** (`vulkaninfo`, probe device creation) |
| RADV accepts the model passes | **verified** for the passes that create a pipeline; 7 storage-image passes crash the driver |
| the 191-pass host order, tensor tables and weight offsets can be reconstructed from the DLL alone | **not verified** — no documented format; needs the Wine capture described in §7 |
| a native Linux MLFI provider can reach parity with AMD's output | **not demonstrated**; depends on the host reconstruction and on Mesa fixing the storage-image crash |
| MLFI requires Windows 11 on the vendor path | **verified** in AMD's documentation; the shaders themselves are plain Vulkan 1.5 compute |
| the signed DLL registers MLFI 4.0.2 on a non-RDNA4 GPU | **false** — on an RTX 5080 `ffxQuery` lists only 3.1.6 and forcing `0xf600000001000002` returns `NO_PROVIDER` (4); 0 pipelines, 0 dispatches |

`gameplay_verified` remains false for MLFI: no frame has been generated by this project.
