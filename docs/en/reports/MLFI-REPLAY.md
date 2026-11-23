# Native MLFI replay: running AMD's captured 191-pass graph on RADV

Status 20/11/2026. Companion to `docs/FSR4-MLFI.md` (what is in the DLL), `docs/MLFI-CAPTURE.md`
(the host layout captured under Wine) and `docs/RADV-MLFI-CRASH.md` (why four modules did not
compile and how they were legalised). This document reports the first **native replay** of the
effect: the complete captured pass graph runs on a headless RADV device, on real inputs, and
produces a frame that is non-trivial and follows its input.

Short answer: **196 dispatches per frame — 5 preprocess + `pass0` … `pass190` — now execute
natively at 1280×720 with 195/195 pipelines, no device loss and no GPUVM fault.** The frame is read
back as `rw_prediction` (RGBA32F, 1280×720): every pixel is non-zero, it changes when the input
frame changes, and a frozen input converges to a bit-identical steady state. The validation layer
reports **zero errors** apart from three documented disagreements between the validator and this
driver/shader combination (§7). This is *not* a quality result: there is no vendor reference frame
on Linux, the inputs are synthetic (zero motion vectors, constant depth), and the FFX provider and
swapchain integration are a separate, later step.

What the replay is not:

* it is not an FFX provider: it does not export `ffxCreateContext`/`ffxDispatch` and has no
  swapchain;
* it does not produce the final backbuffer: the captured no-swapchain path never writes
  `outputs[0]` (MLFI-CAPTURE.md §4.1); the effect's own output is `rw_prediction`, which this tool
  reads back and previews;
* it does not claim vendor-identical image quality. `gameplay_verified` remains false for MLFI.

## 1. What was built

| file | purpose |
| --- | --- |
| `tools/mlfi-replay/mlfi_replay.c` | the replay: device, 195 pipelines, resources, 196 dispatches/frame, readback, PNG/raw dump, summary |
| `tools/mlfi-replay/prepare.py` | copies the 191 `mlfi_4k` modules + the four preprocess `main` modules into the user cache and applies `tools/radv-mlfi/fix_image_types.py` to the four that RADV cannot compile unchanged |
| `tools/mlfi-replay/build.sh` | `cc -O2 … -lvulkan -lm` wrapper; output goes to the cache, never the repository |
| `tests/test_mlfi_replay.py` | end-to-end test: prepare, build, two frames with validation, summary/non-zero/changing output, determinism with frozen input |

Nothing from AMD's DLL is stored in the repository. The module bytes, the model bank and the
capture stay under `$HOME/.cache/mk8online/`; the replay takes their paths as arguments and
refuses to run if the model upload does not match the captured bytes (§5).

## 2. Configuration replayed

The capture used the **4K module set** at 1280×720 with the 720p geometry, tensor table and
scratch size; the replay reproduces exactly that configuration (the 4K/8K threshold is not pinned,
MLFI-CAPTURE.md §12).

| item | value |
| --- | --- |
| render size | 1280×720 (the tool rejects other sizes in this milestone) |
| module set | 191 × `mlfi_4k_pass0..pass190` + 4 × `main` (9,008 / 4,204 / 5,004 / 8,980 B) |
| legalised modules | `pass151` → 2,680 B, `pass152` → 18,356 B, `pass190` → 18,180 B, 5,004-B `main` → 4,996 B |
| pipelines | 195 (191 model + 4 preprocess), created in the captured order |
| dispatches | 5 prepare at 54×30×1, then `pass0` … `pass190` with the captured group counts = **196/frame** |
| model buffer | 10,657,152 B = `model_blob[72 : 72+10,657,152]`, binding 0 at offset 0 / `WHOLE_SIZE` |
| scratch | 58,397,248 B (the captured 1280×720 size), binding 128 |
| uniform arena | 3,276,800 B; the per-pass `CsTensorSizes` (1,472 B) and `CsFrameInterpolationConstants` (80 B) are slices at the captured offsets |
| tensor table | the 92 `uint4` entries of `tools/mlfi-capture/derived/tensor_4k_1280x720.tsv`, written at all 175 captured table offsets |
| submission | one command buffer per frame, `vkQueueSubmit`, `vkQueueWaitIdle`, readback |

The four legalised modules are byte-validated with `spirv-val --target-env vulkan1.2` and all 195
pipelines compile on the release RADV driver (the crash path from RADV-MLFI-CRASH.md no longer
applies). The 8,980-byte `main` is created but **not dispatched**, matching the capture.

## 3. Resource layout and descriptor bindings

One descriptor set (set 0) per dispatch, 196 sets. Every binding number and buffer offset is the
capture's; the image attached to a binding is inferred (§4).

### 3.1 Buffers

| binding | name | size / range | usage |
| --- | --- | --- | --- |
| 0 | `InitializerBuffer` | 10,657,152 B, offset 0, `VK_WHOLE_SIZE` | storage buffer, device local |
| 128 | `ScratchBuffer` | 58,397,248 B, offset 0, `VK_WHOLE_SIZE` | storage buffer, device local |
| 256 | `CsFrameInterpolationConstants` (model) | arena + captured offset, 80 B | uniform buffer |
| 256 | `CsDilate` / `CsAMDSplatting64Constants` / `CsComputeFwdMVUsingMatConstants` (prepare) | arena + captured offset, 80 / 64 / 144 / 64 / 64 B | uniform buffer |
| 257 | `CsTensorSizes` | arena + captured offset, 1,472 B | uniform buffer (`uint4[92]`) |
| 320 | `g_sampler_linear` / `g_sampler_nearest` | sampler | linear / nearest, clamp-to-edge |

The capture has **no separate `CsTensorSizes` buffer**: it is a slice of the 3,276,800-byte arena,
and the replay does the same (`tensor_offset` column of the derived table). Arena offsets used:
prepare blocks at 0, 80, 144, 288, 352; `pass0` 416/496; `pass152` 200,688/200,768; `pass190`
256,704/256,784; the remaining 172 tables at 1,472-byte spacing. The arena is host-visible and
coherent; the three 80-byte model blocks are rewritten per frame (the reset/parity fields).

The derived table and the modules agree on the binding counts, and the tool asserts them at
startup against MLFI-CAPTURE.md §7:

```
bindings match capture: model0=78 scratch=175 constants=3 tensors=175 sampler=3
```

### 3.2 Images

All images are 2D, one mip, one layer, optimal tiling, `VK_IMAGE_LAYOUT_GENERAL` for their whole
life (no transitions between passes, so no layout bug can hide in the replay). `w × h` is
1280×720 unless stated.

| replay name | format | size | usage | bound as |
| --- | --- | --- | --- | --- |
| `rgb0`, `rgb1` | `R8G8B8A8_UNORM` | 1280×720 | sampled, transfer-dst | 1, 2 (sampled) |
| `depth` | `D32_SFLOAT` | 1280×720 | sampled, depth attachment, transfer-dst | `main` 1 (sampled) |
| `mv` | `R16G16_SFLOAT` | 1280×720 | sampled, transfer-dst | `main` 0 (sampled) |
| `distortion` | `R16G16_SFLOAT` | 1280×720 | sampled, transfer-dst | `main` 2 (sampled), zeros |
| `dilated_mv` | `R16G16_SFLOAT` | 426×240 | sampled + storage | `main` 128 storage / 0 sampled |
| `downscaled_depth` | `R32_SFLOAT` | 426×240 | sampled + storage | `main` 129 storage / 1 sampled |
| `fwd_mv` | `R16G16_SFLOAT` | 426×240 | sampled + storage | `main` 128 storage |
| `splat0to05`, `splat1to05` | `R32G32_UINT` (mutable) | 426×240 | sampled + storage | `R64_UINT` storage view at the prepare rows (`main` 128, and `main` 2 for the fwd-MV row); `R32G32_UINT` sampled view at `pass152/190` 5/6 |
| `flow0to05`, `flow1to05` | `R16G16_SFLOAT` | 160×90 | sampled + storage | `pass151` 129/130 storage, `pass152/190` 3/4 sampled |
| `hudless_ui` | `R8G8B8A8_UNORM` | 1280×720 | sampled, transfer-dst | `pass190` 7 (sampled), zeros |
| `prediction` | `R32G32B32A32_SFLOAT` | 1280×720 | storage + transfer-src | `pass190` 129 storage |

`426×240` is `(W/3, H/3)` and `160×90` is `(W/8, H/8)`. `426` is the exact value in the captured
`CsDilate` constants (`0x1aa`), not `ceil(W/3)` = 427.

The packed splat images use the 64-bit format compatibility class: the preprocess splat pass writes
`OpAtomicUMax` through an `R64_UINT` view (the shader declares `%ulong … R64ui`) and the model
passes fetch the same texels through an `R32G32_UINT` view (`%uint` sampled type, `.x` is the low
half). Each view restricts its usage (`VkImageViewUsageCreateInfo`) so the `R64_UINT` view does not
claim sampled usage, which R64 formats do not support.

### 3.3 Per-dispatch wiring

The model passes only bind images on four rows (pass0, pass151, pass152, pass190); the other 187
bind buffers only. The prepare wiring is inferred from the module code and the captured binding
numbers:

| row | module | binding 0 | binding 1 | binding 2 | storage out | constants |
| --- | --- | --- | --- | --- | --- | --- |
| 0 | `CsDilate` (9,008 B) | `mv` | `depth` | `distortion` | 128 `dilated_mv`, 129 `downscaled_depth` | 80 B |
| 1 | `CsAMDSplatting64Constants` (4,204 B) | `dilated_mv` | `downscaled_depth` | — | 128 `splat0to05` | 64 B |
| 2 | `CsComputeFwdMVUsingMatConstants` (5,004 B) | `dilated_mv` | `downscaled_depth` | `splat1to05` (previous frame) | 128 `fwd_mv` | 144 B |
| 3 | `CsAMDSplatting64Constants` | `fwd_mv` | `downscaled_depth` | — | 128 `splat1to05` | 64 B |
| 4 | `CsAMDSplatting64Constants` | `dilated_mv` | `downscaled_depth` | — | 128 `splat0to05` | 64 B |

Rows 1 and 4 both write `splat0to05`; that is what the capture shows (both dispatches bind the same
`VkImageView`, and the shader's `OpAtomicUMax` merges the full-scale and half-scale splats). Row 2
reads last frame's `splat1to05`, which is why the effect has a one-frame history and why frame 0 is
a reset frame.

Between every dispatch the replay records a full `VkMemoryBarrier` (shader write/read → shader
read/write). The capture harness did not intercept `vkCmdPipelineBarrier`, so the provider's own
barrier placement is **not** known; a global barrier is the conservative choice, and at 196
dispatches it is not measurable in the 720p run time.

## 4. What the capture pinned and what was inferred

`docs/MLFI-CAPTURE.md` is the source of the "pinned" column.

| item | state |
| --- | --- |
| pass order (5 prepare, then pass0…pass190 ascending) | **pinned** (per-frame log and `passes.tsv`) |
| dispatch geometry of all 196 dispatches | **pinned** (`derived/passes_mlfi_4k_1280x720.tsv`) |
| buffer bindings and offsets (0 / 128 / 256 / 257), model at offset 0 / `WHOLE_SIZE` | **pinned** (descriptor capture) |
| binding counts 78 / 175 / 3 / 175 | **pinned** and re-asserted by the replay |
| 92-entry `CsTensorSizes`, identical for every pass and frame | **pinned** (committed TSV; written at all 175 offsets) |
| model upload `model_blob[72 : 72+10,657,152]` | **pinned**; the replay checks its FNV-1a against the capture before running |
| prepare constants (5 blocks, sizes 80/64/144/64/64) | **pinned**; the replay reads them from `raw/` and warns if a block's FNV differs |
| image binding numbers (1–7, 129/130) and the 3 + 4 passes that use them | **pinned** |
| image **sizes** | **inferred**: 426×240 from the prepare guard constants and `W/3`; 160×90 from `tensor[70/71]` and the `pass151` guard; 1280×720 from `OpImageQuerySizeLod(rgb0)` and the `pass190` guard |
| image **formats** | **inferred** from the modules' `OpTypeImage` sampled types/formats and the harness descriptors (`R16G16_SFLOAT` MVs, `D32_SFLOAT` depth, `R8G8B8A8_UNORM` colour); `R64_UINT`/`R32G32_UINT` from the atomic packing in the splat module |
| image **identity** (which view belongs to which image) | **not recoverable**: the capture logs view handles but not `vkCreateImageView`, and the same handle appears in sets whose shaders declare different formats. The replay defines a coherent image set and says so; this is the one wiring detail the capture cannot settle |
| sampler states | **inferred**: `g_sampler_linear` linear, `g_sampler_nearest` nearest, both clamp-to-edge (the capture does not intercept `vkCreateSampler`) |
| subgroup size | **inferred**: every pipeline requests 32. The 78 cooperative-matrix modules use `LocalSize 32×1×1`, which is only valid as wave32; the capture did not log the pipeline create-info `pNext` |
| the remaining prepare semantics (which image is "flow" vs "depth") | **inferred** from the module bodies and the captured binding numbers |
| per-frame `CsFrameInterpolationConstants` | frame 0 **pinned**; frames > 0 use the frame-1 delta documented in MLFI-CAPTURE.md §10 |
| barriers between dispatches | **inferred** (see §3.3) |
| scratch/initial image contents | **chosen**: zeroed once at start (the capture records no clears) |

None of these inferences affect the dispatch graph; they affect the image the graph produces, and
that is why §7 states plainly that quality is unproven.

## 5. Run evidence

Host: AMD Radeon RX 9070 XT (RADV GFX1201), Vulkan 1.4.363, driver 109457507, Mesa 26.99.99.
Build and prepare:

```sh
tools/mlfi-replay/build.sh                       # cc -O2 … -lvulkan -lm, ~1.4 s
python3 tools/mlfi-replay/prepare.py             # legalises 4 of the 195 modules, ~0.5 s
```

Run (2 frames, validation layer on, `nice -n 19`):

```sh
nice -n 19 "$HOME/.cache/mk8online/mlfi-replay/build/mlfi_replay" \
  --modules "$HOME/.cache/mk8online/mlfi-replay/modules" \
  --model  "$HOME/.cache/mk8online/mlfi/model/model_blob.bin" \
  --capture "$HOME/.cache/mk8online/mlfi-capture/canonical-4k-1f" \
  --out "$HOME/.cache/mk8online/mlfi-replay/evidence/out-a" --frames 2
```

Output (excerpt; full log under `$HOME/.cache/mk8online/mlfi-replay/evidence/run-a.log`):

```
[mlfi-replay] loaded 195 modules (4 preprocess 'main')
[mlfi-replay] model   upload model_blob[72 : 10657224] fnv1a=b88e755565aa4249 (matches capture)
[mlfi-replay] device: AMD Radeon RX 9070 XT (RADV GFX1201) (api 1.4.363, driver 109457507)
[mlfi-replay] bindings match capture: model0=78 scratch=175 constants=3 tensors=175 sampler=3
[mlfi-replay] pipelines: 195/195 created (191 model + 4 preprocess)
[mlfi-replay] descriptor sets: 196 written
[mlfi-replay] arena: 175 tensor tables, 3 model constant blocks, prepare constants from capture
[mlfi-replay] model uploaded (10,657,152 bytes at offset 0)
[mlfi-replay] frame 0: hash=7544c7614ca10377 nonzero=921600/921600 min=0.1373 max=0.9216 mean=0.45337 stddev=0.1884 mad(rgb0=0.2395 rgb1=0.2298 blend=0.2324)
[mlfi-replay] frame 1: hash=0f99459b4f2f387e nonzero=921600/921600 min=0.1372 max=0.9218 mean=0.45015 stddev=0.1864 mad(rgb0=0.2335 rgb1=0.2323 blend=0.2306)
[mlfi-replay] summary: pipelines=195/195 dispatches=196/frame (392 total) frames=2 validation_errors=0 validation_warnings=0 known_cm_fconvert=10 known_int_fetch_linear=8 known_cm2_spirv=20 device=alive changed=yes
MLFI-REPLAY RESULT: OK
```

* `dispatches=196/frame`: 5 prepare + 191 model; the count is a real `vkCmdDispatch` counter, not a
  table length.
* `vkQueueSubmit` and `vkQueueWaitIdle` returned `VK_SUCCESS` for every frame
  (`device=alive`); `dmesg`/`journalctl -k` contain no GPUVM, ring or device-lost line after the
  runs.
* `validation_errors=0`, `validation_warnings=0`. The three `known_*` counters are the documented
  validator disagreements of §7 and are counted separately so a real error is never hidden.
* Every one of the 921,600 pixels is non-zero, with a spatial standard deviation of 0.188 (the
  output is not a constant fill).
* The two frames differ (`7544c7614ca10377` → `0f99459b4f2f387e`) because the two input frames
  differ. A second process run at the same configuration produced the same two hashes, so the
  replay is deterministic.
* Frozen input (`--static-input --frames 3`): frame 0 (the reset frame) is
  `7544c7614ca10377`, frames 1 and 2 are both `b9a047b0d3b7f31f`, and their raw files are
  byte-identical (`sha256 58be2cf3…`). With a frozen input the graph reaches a fixed point.
* Motion vectors reach the model. `--motion 37` and `--motion -37` (a constant horizontal MV of
  ±37 px/frame) leave the reset frame 0 at `7544c7614ca10377` but change frame 1 from
  `0f99459b4f2f387e` (zero MVs) to `8f4006292ad6d872` and `f3b90716702d0f75` respectively. The
  prepare → `dilated_mv` → splat → model path is therefore wired; with zero MVs it is degenerate
  and the reset frame is insensitive to the MV history, as the effect's reset semantics imply.

Saved output (`$HOME/.cache/mk8online/mlfi-replay/evidence/out-a/`):

| file | sha256 |
| --- | --- |
| `prediction_f0.png` | `8d17b0897a6a2dd9626c1e51ec45b950653d1eff2a67a15142308e7184a5d67d` |
| `prediction_f1.png` | `ee800ef8cba52965a98faa910bb8a0c2cdfb85aa4091dfd2c532f44098b1bae8` |
| `prediction_f0.raw` | `a1eea5b6c8ab4aa5a655f7ff7d3d0b900666a23c8626a3a9c995af2fa24b8fa3` |
| `prediction_f1.raw` | `7237f41dc245202779493ba4f8b466e398431e9adfdea4b28f4a2768ab965e2b` |

The PNG is `rw_prediction` after `clamp(0,1)` and a 1/2.2 preview gamma; it is not colour-managed.
Visually it is a smoothed, lower-contrast version of the input pattern: the moving bar is still
present but softened, the stripe structure is preserved. That is consistent with a frame
interpolation network running with **zero motion vectors and constant depth** — it has no motion
signal to work with, so it falls back to a blend-like prediction. No quality conclusion can be
drawn from this image.

Run time: 2 frames in 1.1 s wall, 3 frames in 1.5 s, on a loaded host with `nice -n 19`. Pipeline
creation (195 cooperative-matrix pipelines) dominates; the dispatches themselves are cheap.

## 6. Limitations of this milestone

1. **No reference.** The only vendor output on this host is the no-op-substituted capture
   (`0/921,600` non-zero pixels). Nothing here shows the replay matches AMD's frame.
2. **Synthetic inputs.** The colour inputs are a generated pattern, depth is a constant 0.5, and
   motion vectors are zero by default (`--motion PX` feeds a constant horizontal vector). The
   capture's own inputs were also synthetic, so a "real-ish" input would have to come from the
   game, which is the next milestone.
3. **Output is `rw_prediction`, not the backbuffer.** The final colour-space/UI composition in the
   captured path is the 8,980-byte `main`, which the provider creates but does not dispatch without
   a swapchain. The replay creates it and does not dispatch it, exactly like the capture.
4. **One resolution.** Only the captured 1280×720 layout is replayed; the 4K/8K module-selection
   threshold and the per-resolution tensor families are untouched.
5. **Image identity and sampler state are inferred** (§4); a provider that must match AMD's exact
   intermediate images needs the capture extension described in §8.

## 7. Validator disagreements (counted, not hidden)

`VK_LAYER_KHRONOS_validation` reports zero errors that the replay does not classify. Three classes
are counted separately and are all reproducible without the replay:

| counter | messages | why it is not a replay defect |
| --- | --- | --- |
| `known_cm_fconvert=10` | `VUID-VkShaderModuleCreateInfo-pCode-08737`: `Expected Use of Matrix type and Result Type to be identical` | AMD's compiler emits `OpFConvert` on cooperative-matrix types; `spirv-val` rejects it (FSR4-MLFI.md §2). RADV accepts and runs the modules; 10 of the 195 used modules contain it. |
| `known_int_fetch_linear=8` | `VUID-vkCmdDispatch-magFilter-04553` | The layer pairs every sampler in the set with every sampled image, but the packed splat images are read with `OpImageFetch`, which consumes no sampler. Integer formats cannot support linear filtering, so the message fires for pass152/190. |
| `known_cm2_spirv=20` | `SPV_NV_cooperative_matrix2` / `CooperativeMatrix*NV` capability VUIDs | The modules declare the NV extension names; RADV provides the same features through `VK_EXT_cooperative_matrix_maintenance1` (the renamed extension), which the layer does not accept for the NV extension/capability names. |

Any other validation error fails the run. The leak check is clean: the replay destroys every
buffer, image, view, sampler, pipeline, layout and pool before `vkDestroyDevice`.

## 8. Next step: FFX provider and swapchain integration

The replay is the frame body of a provider. The integration is a packaging problem around it,
with one open capture gap.

1. **Provider shell.** Export the five FFX entry points and answer the frame-generation effect id
   `0xf600000001000002` (MLFI, version 4.0.2) from `ffxQuery`, as the signed DLL does. Reuse the
   descriptor structs already used by the FSR 3.1.7 provider (`ffx_framegeneration.h`:
   `FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION`, `..._PREPARE_V2`, `..._FRAMEGENERATION`).
2. **Context creation.** Create the 195 pipelines and all resources once, during
   `ffxCreateContext`, with the user's device and `VkDeviceProcAddr` from `ffxCreateBackendVKDesc`.
   The replay's `init_device` feature set, module legalisation at *build* time (they are already
   legalised files on disk) and descriptor wiring carry over unchanged.
3. **Prepare mapping.** `ffxDispatch(PrepareV2)` must:
   * copy `presentColor` into `rgb0`/`rgb1` with a one-frame history (`rgb0` = previous,
     `rgb1` = current; reset both on the first frame or `reset=true`);
   * point `mv` at `motionVectors` and `depth` at the depth resource (the replay's `depth` view is
     `D32_SFLOAT`, the game's captured depth is `D32_SFLOAT`, so no conversion is needed at 720p);
   * keep `distortion` at zero until the SDK field is understood;
   * write the five prepare constant blocks from the descriptor (camera basis, sizes, `CsDilate`);
   * record the five prepare dispatches into the application's command buffer.
4. **Frame mapping.** `ffxDispatch(FrameGeneration)` must write the `CsFrameInterpolationConstants`
   for `pass0/152/190` from `frameID`/`reset`/jitter, then record `pass0…pass190` into the same
   command buffer. The 80-byte block is already synthesised from the capture (MLFI-CAPTURE.md §10);
   the frame-1 delta is the only per-frame part that is not byte-pinned.
5. **Output.** `rw_prediction` is RGBA32F. The game's `outputs[0]` is the FFX resource supplied by
   the app. The provider needs a composition step; two options, in order of preference:
   (a) dispatch AMD's 8,980-byte `main` (`WatermarkCB`, `rw_target`) once its constants are
   captured — the capture does not dispatch it, so this needs one more Wine run with a swapchain;
   (b) write our own resolve from `rw_prediction` to `outputs[0]`, which is a new shader (not AMD
   code) and must be documented as such.
6. **Swapchain.** The provider side must implement
   `FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK` and the acquire/present replacement functions,
   like the existing FSR 3.1.7 provider. The runtime bridge in `runtime/mk8/graphics/fsr_fg.cpp`
   already knows how to enable a 3.1.7 swapchain provider; the ML provider must report a version
   the bridge accepts (a new `MK8_FSR_FG=fsr4` value and a version check are the smallest change).
7. **Pacing/history.** The capture rotates the uniform arena per in-flight frame (the second frame
   starts at 258,256) and the effect keeps a one-frame splat history. The provider needs one
   resource set per in-flight frame and a reset path on `ffxConfigure`; otherwise the first frame
   after a resolution/state change is a reset frame, as here.
8. **In-game comparison against FSR 3 FG.** With `MK8_FSR_FG=fsr4`:
   * run the existing in-game qualification path (menu, race start, a full lap) and capture the
     same frames with `fsr3` and `fsr4`;
   * compare generated frames structurally (difference against both neighbours, ghosting on the
     HUD, text edges), not by eye alone;
   * run the 120-frame harness with the validation layer and require zero non-classified errors;
   * finally compare against AMD's own output by extending the Wine capture (§9) to *not*
     substitute the four modules and to read back `rw_prediction`, then diff the two predictions
     on the same input pair. Until that diff exists, "matches AMD" is unproven.

The single blocking capture gap for a byte-faithful provider is the image identity/wiring of §4 plus
the pipeline create-info `pNext` (subgroup size). A second Wine run that intercepts
`vkCreateImage`/`vkCreateImageView` (extent, format, usage) and `vkCreateComputePipelines`'s
`pNext`, and that dispatches the final composition pass, would close it. That is a bounded addition
to `tools/mlfi-capture/mlfi_capture.c`, not new reverse engineering.

## 9. Reproduction

```sh
# 1. extraction (once; user's own DLL)
python3 tools/extract_mlfi.py --dll <sdk>/Kits/FidelityFX/signedbin/amd_fidelityfx_framegeneration_vk.dll

# 2. capture (once; user's own DLL, Wine prefix)
FFX_DLL=<dll> tools/mlfi-capture/run.sh "$HOME/.cache/mk8online/mlfi-capture/canonical-4k-1f" \
  --width 1280 --height 720 --frames 1

# 3. legalise + build + run
python3 tools/mlfi-replay/prepare.py
tools/mlfi-replay/build.sh
nice -n 19 "$HOME/.cache/mk8online/mlfi-replay/build/mlfi_replay" \
  --modules "$HOME/.cache/mk8online/mlfi-replay/modules" \
  --model  "$HOME/.cache/mk8online/mlfi/model/model_blob.bin" \
  --capture "$HOME/.cache/mk8online/mlfi-capture/canonical-4k-1f" \
  --out /tmp/mlfi-replay --frames 2

# 4. test (skips when the extraction/capture/tools are absent)
python3 -m unittest tests.test_mlfi_replay -v
```

The test prepares, builds, runs two moving frames and one three-frame frozen run, and asserts the
summary, zero validation errors, 195 pipelines, 196 dispatches, a fully non-zero and changing
output, and the frozen-input fixed point. `MK8_MLFI_REPLAY_REQUIRE=1` turns a missing prerequisite
into a failure.

## 10. Verified vs still open

| claim | state |
| --- | --- |
| all 195 captured pipelines compile on the release RADV driver | **verified** (`pipelines: 195/195 created`) |
| all 196 captured dispatches record and execute | **verified** (counter, `vkQueueSubmit`/`vkQueueWaitIdle` = 0, device alive after 3 frames) |
| the model upload is the captured range | **verified** (FNV-1a `b88e755565aa4249` before the run) |
| the descriptor counts match the capture | **verified** (78/175/3/175 asserted at startup) |
| the generated frame is non-trivial | **verified**: 921,600/921,600 non-zero, stddev 0.188 |
| the generated frame follows the input | **verified** for the synthetic pair (hashes differ across two input pairs) and for determinism (two processes produce identical hashes; frozen input converges) |
| validation is clean | **verified except** the three documented validator disagreements (§7) |
| no GPUVM fault / device loss | **verified** for these runs (`dmesg` clean, `device=alive`) |
| the replay matches AMD's frame | **not demonstrated** — no vendor reference on Linux |
| image quality / ghosting / haloing | **not measured** — synthetic inputs, no reference |
| the FFX provider or swapchain works | **not implemented** |
| any resolution other than 1280×720 | **not replayed** |
| `gameplay_verified` | remains **false** for MLFI |
