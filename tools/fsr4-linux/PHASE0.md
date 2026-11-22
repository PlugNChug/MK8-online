# Phase 0 findings — 4.1.2 asset extraction and native bring-up

Date: 2026-11-03 (extraction) / 2026-11-04 (bring-up)
Source: `Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_vk.dll`
SHA-256: `4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7`

## Status: PASS

The native Linux runner reproduces AMD's signed Windows Vulkan implementation
**byte-for-byte** for two resolution classes:

| case | render | output | flags / config | result |
|---|---|---|---|---|
| A | 960x540 | 1920x1080 | flags 0, no sharpening | `1623d0c2…` identical |
| B | 1280x720 | 2560x1440 | flags 0, no sharpening | `d30daee2…` identical |
| C | 1280x720 | 1920x1080 | flags 0, no sharpening | `60525705…` identical |
| D | 1920x1080 | 3840x2160 | flags 0, no sharpening | `02776dda…` identical |
| E | 960x540 | 1920x1080 | auto exposure (flags 0x21) | identical |
| F | 960x540 | 1920x1080 | auto exposure + sharpening 0.9 | identical |
| G | 1280x720 | 2500x1406 | auto exposure + sharpening (game size) | identical |
| H | 1920x1080 | 3200x1800 | auto exposure + sharpening (bucket 1) | identical |

Case C proves the render/output separation (prepass at render size, tensors at
output size); case D covers the 4320 class and the 332 MB scratch tier.

Both runs use the 27-pass FP8-WMMA chain on RADV GFX1201 (RX 9070 XT), one frame
with `reset=1`, linear color, render-resolution motion vectors, inverted depth,
internal auto exposure. Inputs are deterministic and captured from the reference
run (`in_color.rgba8`, `in_depth.f32`, `in_mv.rg16f`).

## Corrections to the earlier Phase 0 notes

- **`CsTensorSizes` is 272 bytes (`uvec4[17]`), not 496.** The only 496-byte UBO
  in the DLL is `WatermarkCB` (`uint2 + uint + uint + PackedMessage[120]`). The
  272-byte block is what the model shaders declare; 42 lite-model shaders declare
  `uvec4[9]` (144 B).
- **The `pass5_post`/index-6 "mismatch" does not exist.** Padding pass N reads
  slot N+1 (`pass0_post` reads 0); `divisors[6] == 4`, exactly padding layer 5's
  resolution.
- **No push constants.** All 1,778 modules were scanned; none declares a
  `PushConstant` storage class. Every layer offset is baked into the SPIR-V.
- **No descriptor buffers.** AMD's DLL resolves `vkCreateDescriptorPool`,
  `vkAllocateDescriptorSets`, `vkUpdateDescriptorSets` and
  `vkCmdBindDescriptorSets`; it uses one flat descriptor set with the real
  binding numbers (0..320), one set per pass, only the bindings that pass uses.
- **The reference selects the FP8-WMMA (cooperative matrix) family**, not the
  INT8 dot-product family, on this GPU.

## Tooling

- `tests/reference/fsr4_trace.c` — Windows PE, runs the signed DLL under Wine,
  intercepts the backend through `VkDeviceContext::vkDeviceProcAddr`, and records
  shader hashes, pipelines, dispatch counts, descriptor layouts/writes, images,
  buffers and the constant UBOs. Build with `tests/reference/build.sh`.
- `tests/reference/prepare_case.py` — turns a trace directory into a runner
  manifest (exact blobs + group counts).
- `src/runner/fsr4_native_runner.cpp` — native Vulkan runner; links the
  extracted SPIR-V and reproduces the pass chain.
- `tests/compare.py` — byte/PSNR comparison.

Reproduce:

```sh
cd linux
sh tests/reference/build.sh
mkdir -p build/tests/reference/caseA && cd build/tests/reference/caseA
cp ../fsr4_trace.exe ../amd_fidelityfx_*.dll .
WINEPREFIX=<wine-prefix> WINEDEBUG=-all wine fsr4_trace.exe 960 540 1920 1080
cd ../../../..
cmake -S . -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/native
python3 tests/reference/prepare_case.py build/tests/reference/caseA build/assets build/run/caseA
cd build/run/caseA && ../../native/fsr4_native_runner case.txt
python3 ../../../tests/compare.py out_reference.rgba8 out_native.rgba8 1920 1080
```

## `CsTensorSizes` (272 bytes, set 0 binding 256)

`entry[i] = (uint32)(float(roundUp8(width_or_height)) * scale[i])`, z/w = 0,
computed from the **upscale size**. Scales:

```
[.5, 1, .5, .5, .25, .25, .25, .125, .125, .125, .25, .25, .5, .5, 1, 1, 1]
```

1920x1080 output gives:

```
T0  960x540   T1 1920x1080  T2  960x540  T3  960x540  T4 480x270
T5  480x270   T6  480x270   T7  240x135  T8  240x135  T9 240x135
T10 480x270   T11 480x270   T12 960x540  T13 960x540 T14 1920x1080
T15 1920x1080 T16 1920x1080
```

The table was also recovered statically from the DLL writer at RVA `0xbfc0`
(main model) / `0xa850` (lite model), which independently produced the same
values.

## Dispatch geometry (observed at two classes)

Every model pass is one invocation per tensor element with a fixed tile:

| pass | tile (x,y) | groups |
|---|---|---|
| prepass | 32x1 | `ceil(renderW/32), renderH` |
| pass1, pass2, pass12 | 16x8 | `ceil(t.W/16), ceil(t.H/8)` |
| pass3, pass6 | 32x1 | `ceil(t.W/32), t.H` |
| pass4, pass5, pass10 | 16x4 | `ceil(t.W/16), ceil(t.H/4)` |
| pass7, pass8 | 16x2 | `ceil(t.W/16), ceil(t.H/2)` |
| pass9 | 32x2 | `ceil(t.W/32), ceil(t.H/2)` |
| pass11 | 32x1 | `ceil(t.W/32), t.H` |
| postpass | 64x2 | `ceil(outW/64), ceil(outH/2)` |

where `t = CsTensorSizes[N+1]` for pass N.

Padding passes (`passN_post`) are flat 1-D:

```
N  = (w+pw+1)*(h+ph+1) - w*h
pw = min(bucketW+1-w, cap)      cap = 5 or 33 depending on the variant
ph = min(bucketH+1-h, 1)
groups = (ceil(N/32), 1, 1)
```

`bucketW/bucketH` are baked into the selected padding module (e.g. `1920x1080`
for the 2160 class, `256x135` for layer 7 at the 1080 class). The cap-5
variants are used when the tensor exactly fills its bucket; the cap-33 variants
when it does not. `pass6/7/8_post` write four slices per invocation; the dispatch
still covers `N` indices.

Observed group counts:

| pass | case A | case B |
|---|---|---|
| prepass | 30x540 | 40x720 |
| pass0_post | 94 | 848 |
| pass1 / pass1_post | 60x68 / 94 | 80x90 / 848 |
| pass2 / pass2_post | 60x68 / 94 | 80x90 / 848 |
| pass3 / pass3_post | 15x270 / 47 | 20x360 / 425 |
| pass4 / pass4_post | 30x68 / 47 | 40x90 / 425 |
| pass5 / pass5_post | 30x68 / 47 | 40x90 / 425 |
| pass6 / pass6_post | 8x135 / 93 | 10x180 / 214 |
| pass7 / pass7_post | 15x68 / 93 | 20x90 / 214 |
| pass8 / pass8_post | 15x68 / 93 | 20x90 / 214 |
| pass9 / pass9_post | 15x135 / 47 | 20x180 / 425 |
| pass10 / pass10_post | 30x68 / 47 | 40x90 / 425 |
| pass11 / pass11_post | 30x270 / 94 | 40x360 / 848 |
| pass12 / pass12_post | 60x68 / 94 | 80x90 / 848 |
| postpass | 30x540 | 40x720 |

## Resource layout (case A)

| resource | format | size | usage |
|---|---|---|---|
| history | RGBA16F | output | sampled + storage |
| reprojected | RGBA16F | output | sampled + storage |
| recurrent | RGBA8_UNORM | output | sampled + storage |
| exposure | R32F | 2x1 | sampled + storage |
| exposure dummy | RG32F | 1x1 | sampled, cleared to (1,1) |
| exposure counter | R32_UINT | 1x1 | created but unused |
| exposure mips | R32F | out/32 | created but unused |
| scratch | — | 20,880,256 / 83,232,256 / 332,352,256 B | storage |
| weights | — | 131,072 B | storage, byte-identical to `FP8_standard.bin` |

The DLL's own weight buffer was dumped and matches the extractor output exactly
(SHA-256 `ce1bd5f1…`).

## Bindings used by the chain

```
prepass : 0 history, 1 velocity, 2 depth, 3 color, 4 recurrent,
          6 exposure dummy, 17 auto-exposure, 21 weights,
          131 reprojected, 141 scratch, 257 constants, 320 sampler
model   : 21 weights, 141 scratch, 256 tensor sizes
padding : 141 scratch, 256 tensor sizes
postpass: 3 color, 6 exposure dummy, 9 reprojected, 21 weights,
          129 history, 130 output, 134 recurrent, 137 auto-exposure,
          141 scratch, 257 constants, 320 sampler
```

## Remaining work before M1

1. **Shader variant selection** for arbitrary sizes (the DLL's selector keys).
   Phase 0 used the exact blobs from the trace; the provider needs the rule.
2. **Scratch bucket layout per layer** (e.g. layer 7 is 256x135 at the 1080
   class, not 240x135). Needed to choose the matching padding variants; read the
   baked row strides from the model/padding SPIR-V to cross-check.
3. **Constants** for arbitrary frames (jitter, motion scale, pre-exposure,
   reset, `width_lr/height_lr`). The 104-byte layout is known and matches
   `fsr4_411::OptimizedConstants`; the provider must compute it.
4. **Device features** for the game: `VK_EXT_shader_float8` +
   `VK_KHR_cooperative_matrix` (RADV supports both). `VK_NV_cooperative_matrix2`
   is not exposed by RADV, yet AMD's DLL and this runner both work without it.
   `VK_EXT_descriptor_buffer` is not needed.


## M1.0 — auto exposure, RCAS and arbitrary sizes (2026-11-04)

All eight cases above are byte-identical. The additional passes and blocks:

### Auto exposure adds an SPD pass (first)

- Module: `debug/utility`, entry `main`, `LocalSize 256 1 1`, `DerivativeGroupLinearKHR`,
  64x64 tile, groups = `ceil(renderW/64), ceil(renderH/64)`.
- Bindings: `0` input color (sampled), `128` global atomic (`R32_UINT` 1x1),
  `129` mip 5 (`R32F`, render/16), `130` exposure (`R32F` 2x1), `256` UBO (32 B),
  `320` sampler.
- SPD constants (32 bytes): `mips`, `numWorkGroups` (= product of the group counts),
  `workGroupOffset` (0,0), `invInputSize` (1/renderW, 1/renderH), `preExposure`, pad.
  Observed mips: 960x540→9, 1280x720→10, 1920x1080→10 (`floor(log2(max(w,h)))`).
- The prepass/postpass then bind `17` to the same 2x1 exposure image.

### Sharpening adds an RCAS pass (last)

- Module: `fsr_rcas_pass`, `LocalSize 64 1 1`, 16x16 tile, groups =
  `ceil(outW/16), ceil(outH/16)`.
- Layout: `0` UBO `cbRCAS` (32 B), `11` storage output, `17` exposure
  (sampled), `18` RCAS input (sampled, RGBA16F), `320` sampler.
- RCAS constants (32 bytes): `[0] = exp2(2*sharpness - 2)` (e.g. 0.87055 for 0.9),
  `[1] = packed fp16 pair of the same factor` (`0x3AF63AF6`), `[2..3] = 0`,
  `[16] = preExposure`, `[20..31] = 0`.
- With sharpening, the postpass writes binding `130` to an internal **RGBA16F**
  image (the RCAS input); RCAS reads it at `18` and writes the caller output at
  `11`. Without sharpening the postpass writes the caller output directly.

### Selection keys

- The traced Phase 0 cases used `flags = 0` → key 2 (auto exposure off).
- The game config (`HDR | AUTO_EXPOSURE`, `flags = 0x21`) → key 6; the prepass and
  postpass blobs change accordingly. Both keys are now validated byte-exactly.
- Bucket selection follows `maxUpscaleSize` at context creation; arbitrary
  per-dispatch sizes (2500x1406 at bucket 1, 3200x1800) work with the same
  bucket shaders and per-layer strides.

### Runner/tracer support

`tests/reference/fsr4_trace.c` gained `FSR4_CREATE_FLAGS`, `FSR4_DISPATCH_FLAGS`,
`FSR4_SHARPEN`, `FSR4_SHARPNESS`, `FSR4_CTX_MAX_R/U`, and dumps `spd.bin`/`rcas.bin`.
`prepare_case.py` emits `spd`/`rcas` pass kinds; the runner creates separate
layouts/sets for them and uses the captured blocks.

## M1.1a — ABI computation and selector table (2026-11-04)

The recovered ABI is now C++ code, validated against the captured traces:

- `src/abi/fsr4_412_abi.{hpp,cpp}`
  - `classMaximum()` / `scratchCapacity()` (20,880,256 / 83,232,256 / 332,352,256)
  - `writeTensorSizes()` (272 B, `roundUp8(upscale) * scale[17]`)
  - `writeOptimizedConstants()` (104 B, includes context state, jitter-cancel and
    previous pre-exposure)
  - `writeSpdConstants()` / `spdGroups()` (32 B, `mips = floor(log2(max(render)))`,
    `numWorkGroups = ceil(rw/64)*ceil(rh/64)`)
  - `writeRcasConstants()` / `rcasGroups()` (32 B, truncating fp16 pack)
- `tools/gen_selector_table.py` + `tools/data/fsr4_412_selector_map.json`
  generate `src/abi/fsr4_412_selector_table.hpp`:
  - 27 canonical passes x 3 buckets
  - 64-key prepass table x 3 buckets
  - 8 postpass `(colour, auto-exposure)` variants x 3 buckets
  - SPD id 329; RCAS variants by colour/auto-exposure
- `tests/abi/abi_test.cpp` compares the computed tensor/constants/SPD/RCAS blocks
  byte-for-byte against every captured trace.
- `tests/run_abi_tests.sh` runs the block tests and then the native runner with
  `FSR4_RUNNER_COMPUTE_ABI=1` (no captured blocks) for all eight cases.

Results: all eight cases PASS; the selector table reproduces every traced blob id
(27 passes for A-D, 28-29 for the auto-exposure/RCAS cases); computed-ABI output
is byte-identical to AMD's DLL.

The selection rule implemented by the table:

```
bucket = smallest k with ceil(outW/(480<<k)) <= 4 and ceil(outH/(270<<k)) <= 4
key    = depthInverted | (!displayResMV)<<1 | autoExposure<<2 | colour<<3 | jitterCancel<<5
blob   = prepass:  kPrepassBlob[key][bucket]
         postpass: kPostpassBlob[(colour<<1)|autoExposure][bucket]
         others:   kPassBlob[pass][bucket]
```

## M1.1b — native FFX provider + loader (2026-11-04)

The validated chain is now exposed through the FFX API:

- `src/provider/ffx_provider.cpp` → `libamd_fidelityfx_upscaler_vk.so`
  - exports `ffxCreateContext`, `ffxDestroyContext`, `ffxConfigure`, `ffxQuery`,
    `ffxDispatch`;
  - accepts `FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK` (`0x01000003`) and the
    legacy `0x00000003` struct;
  - reports provider version `0x01001002` / "FSR 4.1.2";
  - creates all internal resources (history/reprojected/recurrent/exposure/
    RCAS input/neutral depth+MV, weights, scratch, UBOs) at context creation;
  - selects pipelines per bucket/key from `src/abi`, records the SPD pass,
    27-pass chain and RCAS pass into the caller's command buffer, and leaves the
    output in `GENERAL`;
  - never submits, waits or reads back; neutral depth/MV are used when the
    caller omits them.
- `src/loader/ffx_loader.cpp` → `libamd_fidelityfx_loader_vk.so`
  - forwards the five entry points to the provider next to itself or on
    `LD_LIBRARY_PATH`; never unloads the provider.
- `tests/provider/provider_test.cpp` exercises the full path (loader → provider
  → FFX create/query/dispatch) from captured inputs.

Results: `tests/run_provider_tests.sh` passes all eight cases byte-identically
(A-D flags 0, E-H auto exposure / sharpening / arbitrary sizes).

The provider currently reads SPIR-V/models from `FSR4_ASSET_DIR` (default `.`);
embedding them into the `.so` at build time is packaging work for M1.3.

## M1.2b — runtime asset discovery (2026-11-04)

The provider now defaults `FSR4_ASSET_DIR` to `<directory of the effect .so>/fsr4-assets`
and falls back to the current directory only if that is absent. `tools/stage_runtime.sh
<dest>` copies the provider, loader and the shader/model assets into a game build
or package directory:

```sh
sh tools/stage_runtime.sh /path/to/game-dir
```

Verified in-game with `FSR4_ASSET_DIR` unset: the provider still reports 4.1.2
and dispatches 1280x720 -> 2560x1440. The staged shader set is the full extracted
catalogue (102 MB); trimming to the selector-referenced blobs and embedding them
into the `.so` remain packaging work.

## M1.3a — trimmed runtime assets + sRGB validation (2026-11-04)

- `tools/trim_assets.py` copies only the blobs the selector can choose: 189
  SPIR-V modules + `FP8_standard.bin`, **16 MB on disk** (14.8 MB payload) versus
  the full 102 MB extraction. `tools/stage_runtime.sh` uses it.
- The provider defaults its asset root to `<effect .so dir>/fsr4-assets`, so the
  game needs no environment variable.
- The sRGB (display-referred) shader path is now validated byte-exactly against
  AMD's DLL at all three buckets:
  - bucket 0: 960x540 -> 1920x1080 (`caseI`)
  - bucket 1: 1280x720 -> 2560x1440 (`caseJ`)
  - bucket 2: 3840x2160 -> 7680x4320 (`caseK`)
- The recovered postpass table was missing sRGB+auto-exposure entries for
  buckets 1/2; they were captured (ids 1213 / 1300) and added, and
  `selectPassBlob` now falls back to the closest available postpass variant
  instead of returning id 0.
- In-game: `FSR4 dispatch active (provider 4.1.2) 1280x720 -> 2560x1440` with
  `dispatch.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB`, 60 FPS, 16 MB of
  assets staged next to the executable.

## M2a — FFX query parity + in-game performance (2026-11-04)

Provider queries implemented and tested:

- `GET_PROVIDER_VERSION` / `GET_VERSIONS` (4.1.2).
- `GETUPSCALERATIOFROMQUALITYMODE` (1.0 / 1.5 / 1.7 / 2.0 / 3.0).
- `GETRENDERRESOLUTIONFROMQUALITYMODE` (truncating display / ratio).
- `GETJITTERPHASECOUNT` (`8 * (display/render)^2`) and `GETJITTEROFFSET`
  (Halton 2,3 minus 0.5).
- `GPU_MEMORY_USAGE` / `GPU_MEMORY_USAGE_V2` (sum of the context's actual
  Vulkan allocations; 85.2 MB for the 1080 class in the test).
- `GET_RESOURCE_REQUIREMENTS` (color/depth/MV required; exposure/reactive/
  transparency optional).

`tests/provider/provider_test.cpp` asserts the ratio/resolution/phase/jitter
values and reports the memory usage; `tests/run_provider_tests.sh` still passes
11/11 byte-identically.

In-game performance at 2560x1440 (native Linux, vsync FIFO, 600-frame samples):

| effect | FPS | frame avg | GPU (guest passes) | presenting |
|---|---|---|---|---|
| bilinear | 60.0 | 16.67 ms | 0.60 ms | 0.08 ms |
| spatial FSR | 60.0 | 16.67 ms | 0.60 ms | 0.07 ms |
| native FSR4 | 60.0 | 16.67 ms | 0.60 ms | 0.07 ms |

The game's GPU counters only cover the guest renderer passes; the presenter's
FSR4 dispatch is not individually timed in-game, but it does not move the frame
time off the 60 Hz vsync boundary.

## M2b — Phase A hardening: Windows parity and GPU benchmark (2026-11-04)

- **Windows package**: rebuilt from the current SDK; `package.sh` now removes stale
  FSR4 DLLs, copies all four signed DLLs, asserts `rexruntime.dll` contains the
  FSR4 client code, and creates `out/dist/windows-lan.zip` (96.6 MB). Both
  renderers verified under Wine with AMD's DLLs:
  - Vulkan: `FSR4 dispatch active (provider 4.1.2)` at 1280x720 -> 1433x806,
    60 FPS.
  - D3D12: `FSR4 dispatch active (provider 3.1.6)` (vkd3d lacks amdags), 60 FPS.
  - **Null depth/MV is accepted by AMD's DLL on both backends** — no
    `ffxDispatch failed`; no neutral-resource fallback needed in the client.
- **D3D12 parity**: `fsr4_upscaler_dx12.cpp` no longer aliases the colour image as
  depth/MV, dispatches with `FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB`, and logs
  dispatch failures.
- **Patch**: `rexglue-sst-fsr4-native.patch` regenerated against
  `HEAD + rexglue-sst.patch + rexglue-sst-xma.patch`; `git apply --check` passes.
- **Benchmark** (`tests/run_bench.sh`, timestamp queries around the full chain):

  | case | GPU avg | CPU record |
  |---|---|---|
  | 960x540 -> 1920x1080 (AE + RCAS) | 0.64–0.84 ms | 0.03 ms |
  | 1280x720 -> 2560x1440 (AE) | ~1.10 ms | 0.016 ms |
  | 1920x1080 -> 3840x2160 | 1.48–1.63 ms | 0.03 ms |

  Well inside the 16.67 ms frame budget; GPU clocks cause the run-to-run spread.

## M1.3b — temporal inputs (2026-11-04)

- The native provider now accepts depth already resolved to a colour image:
  `VK_FORMAT_R32_SFLOAT` (and R16 variants) use `VK_IMAGE_ASPECT_COLOR_BIT` for
  the cached view and both image barriers instead of `DEPTH_BIT`. Native depth
  targets (`D32*`) keep the depth aspect.
- The game passes the scene depth (near = 0 / far = 1, so no depth-inverted
  context flag is needed) and camera-only motion vectors in pixel space
  (`motionVectorScale = 1,1`, normalised to UV by the ABI constants).
- Provider tests still pass 11/11 byte-identically (they use null depth/MV).
