# Native FSR 4 MLFI FFX provider

Status 20/11/2026. Companion to `docs/FSR4-MLFI.md` (what is inside the signed DLL),
`docs/MLFI-CAPTURE.md` (the host layout captured under Wine), `docs/RADV-MLFI-CRASH.md` (the four
modules RADV cannot compile unchanged) and `docs/MLFI-REPLAY.md` (the native replay of the pass
graph). This document reports the first **FFX-compatible provider** for AMD's ML frame
interpolation: a native Linux shared library that exports the five FFX entry points, answers the
MLFI provider version 4.0.2 (`0xF600000001000002`), creates its resources from
`ffxCreateContextDescFrameGeneration` + `ffxCreateBackendVKDesc`, records the captured pass graph
into the application's `VkCommandBuffer` and writes `outputs[0]`.

Short answer: **the library builds and runs headlessly at 1280×720.** One context creates 199
pipelines (195 from the captured module set + 3 input-copy + 1 compose), `PrepareV2` records 5
preprocess dispatches plus 2 input copies, `FrameGeneration` records `pass0`…`pass190` plus one
composition pass. On RADV GFX1201 the two frames are fully non-zero (921,600/921,600 pixels),
change with the input, are bit-identical across processes, and the validation layer reports **zero
errors and zero warnings** apart from the same three documented validator disagreements as the
replay. This is **not** a quality result: there is no vendor reference frame on Linux, the inputs
are synthetic, the final composition is ours (the vendor's final `main` was never captured), and
the swapchain provider is not implemented.

## 1. Files

| file | purpose |
| --- | --- |
| `tools/mlfi-provider/mlfi_provider.c` | the whole provider: SPIR-V descriptor scanner, 195 graph pipelines, provider images/buffers, descriptor sets, `PrepareV2` + `FrameGeneration` recording, composition |
| `tools/mlfi-provider/mlfi_provider_ext.h` | `mk8MlfiGetStats`, a diagnostic export that reports the provider's measured dispatch counters (not part of the FFX ABI) |
| `tools/mlfi-provider/mlfi_provider_test.c` | headless program: Vulkan device, two frames, validation layer, readback and checks |
| `tools/mlfi-provider/shaders/*.comp|vert|frag` | our four shaders (input copies + composition); no AMD content |
| `tools/mlfi-provider/build_shaders.py` | compiles the shaders into `mlfi_shaders.h` (committed, regenerable, `--check` in the tests) |
| `tools/mlfi-provider/gen_graph_header.py` | turns the committed capture TSVs into `mlfi_graph.h` at build time (no AMD bytes) |
| `tools/mlfi-provider/build.sh` | builds the library and the test program into `$XDG_CACHE_HOME/mk8online/mlfi-provider/build` |
| `tests/test_mlfi_provider.py` | prepares the modules, builds, runs the program twice, asserts the summary and determinism |

Nothing from AMD's DLL is stored in the repository. The module bytes and the model bank stay under
`$HOME/.cache/mk8online/`; the provider reads their paths from `MK8_MLFI_MODULES` and
`MK8_MLFI_MODEL` (defaults: `…/mk8online/mlfi-replay/modules` and
`…/mk8online/mlfi/model/model_blob.bin`, produced by `tools/extract_mlfi.py` and
`tools/mlfi-replay/prepare.py`).

The provider is compiled against the MIT FFX headers already committed under
`runtime/mk8/graphics/ffx/` — the same API/desc headers the runtime bridge and the FSR 3.1.7
provider build use. This avoids requiring the user's SDK download just to build the shim; the
headless test links the library and calls the entry points through those headers, so a layout
mismatch fails at build or run time. (The SDK tree used by `tools/fg-linux` carries the same
headers; the operator `#if defined(_WIN32)` versions are identical.)

## 2. ABI and descriptor mapping

The five exports are the FFX API (`ffx_api.h`):

```c
ffxReturnCode_t ffxCreateContext(ffxContext*, ffxCreateContextDescHeader*, const ffxAllocationCallbacks*);
ffxReturnCode_t ffxDestroyContext(ffxContext*, const ffxAllocationCallbacks*);
ffxReturnCode_t ffxQuery(ffxContext*, ffxQueryDescHeader*);
ffxReturnCode_t ffxConfigure(ffxContext*, const ffxConfigureDescHeader*);
ffxReturnCode_t ffxDispatch(ffxContext*, const ffxDispatchDescHeader*);
```

### 2.1 Provider identity

The signed DLL constructs provider ids as `0xF6000000 << 32 | FFX_SDK_MAKE_VERSION(major, minor,
patch)`, where `FFX_SDK_MAKE_VERSION(m, n, p) = (m << 22) | (n << 12) | p` (this is why the
captured FSR 3.1.6 entry is `0xF600000000C01006`). The MLFI provider is therefore:

| item | value |
| --- | --- |
| version id | `0xF600000001000002` = `0xF6000000 << 32 | (4 << 22 | 0 << 12 | 2)` |
| version name | `"4.0.2"` |
| effect id | `FFX_API_EFFECT_ID_FRAMEGENERATION` (`0x00020000`) |
| descriptor that selects it | `FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION` (`0x00020001`) |

`ffxQueryDescGetVersions` (`type = 4`) with `createDescType` whose effect mask is
`0x00020000` returns exactly one version (`count = 1`), the id above and the name `"4.0.2"`.
For every other `createDescType` it sets `*outputCount = 0` and returns
`FFX_API_RETURN_NO_PROVIDER` (4) — in particular it does **not** claim the 3.1.7 swapchain
provider. `ffxQueryDescGetProviderVersion` (`type = 6`) on a provider context returns the same
id/name.

### 2.2 Context creation

`ffxCreateContext` walks the `pNext` chain:

| descriptor | value | handling |
| --- | --- | --- |
| `ffxCreateContextDescFrameGeneration` | `0x00020001` | required; `displaySize`, `maxRenderSize`, `backBufferFormat`, `flags` |
| `ffxCreateBackendVKDesc` | `0x01000003` | required; `VkDeviceContext{vkDevice, vkPhysicalDevice, vkDeviceProcAddr}` |
| `ffxCreateContextDescFrameGenerationVersion` | `0x0002000e` | optional; if present must be `FFX_FRAMEGENERATION_VERSION` = `0x01000002` |
| `ffxOverrideVersion` | `5` | optional; if present must be `0xf600000001000002` |
| `ffxCreateContextDescFrameGenerationHudless` | `0x00020009` | ignored (no hudless composition in this milestone) |

Rejections are explicit and use the FFX return codes (never a fictitious success):

| condition | code |
| --- | --- |
| head desc is not the frame-generation effect | `FFX_API_RETURN_NO_PROVIDER` (4) |
| missing backend VK desc or null device/proc addr | `FFX_API_RETURN_ERROR_PARAMETER` (6) |
| API version desc ≠ 4.0.2 | `FFX_API_RETURN_ERROR_PARAMETER` (6) |
| `ffxOverrideVersion` ≠ MLFI id | `FFX_API_RETURN_NO_PROVIDER` (4) |
| size ≠ 1280×720, flags ≠ 0, backbuffer not RGBA8/BGRA8 UNORM | `FFX_API_RETURN_ERROR_PARAMETER` (6) |
| module/model files missing or model hash wrong | `FFX_API_RETURN_ERROR_RUNTIME_ERROR` (3) |
| Vulkan allocation/pipeline failure | `FFX_API_RETURN_ERROR_RUNTIME_ERROR` (3) / `FFX_API_RETURN_ERROR_MEMORY` (5) |

The `ffxAllocationCallbacks` passed to `ffxCreateContext` are used for the context object and the
long-lived module buffers (the transient SPIR-V parser strings use the C allocator); the callbacks
passed to `ffxDestroyContext` must be the same, as the FFX contract states.

The device must have been created with the features the captured modules require (the provider
queries and refuses with a clear log line if any is missing): `shaderInt64`, `shaderFloat16`,
`shaderInt8`, 8/16-bit storage, `vulkanMemoryModel`, variable pointers, cooperative matrix +
maintenance1 conversions/per-element/reductions, float8 cooperative matrices, 64-bit image
atomics, subgroup-size control, and formatless storage-image read/write. Device entry points are
resolved through the supplied `vkDeviceProcAddr`; only the physical-device queries use the loader.

### 2.3 Configure

`ffxConfigureDescFrameGeneration` (`0x00020002`) is accepted: `swapChain`, both callbacks,
`frameGenerationEnabled`, `allowAsyncWorkloads`, `HUDLessColor`, `flags`, `onlyPresentGenerated`,
`generationRect` and `frameID` are stored. The callbacks are **not invoked** in this milestone
(the application calls `ffxDispatch` itself, exactly as the capture harness did); the swapchain
hand-off is described in §9. A non-sequential `frameID` while frame generation is enabled arms a
reset for the next `PrepareV2`.

`ffxConfigureDescGlobalDebug` (`7`, and the deprecated `1`) stores the message callback and debug
level. `ffxConfigureDescFrameGenerationKeyValue` (`0x00020006`) returns
`FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE` (7); unknown types return
`FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE` (2).

### 2.4 Dispatch

| descriptor | value | action |
| --- | --- | --- |
| `PrepareV2` | `0x0002000c` | validate inputs, optional reset, write the five prepare constant blocks, copy depth/MVs, record the five preprocess dispatches |
| `FrameGeneration` | `0x00020003` | validate `numGeneratedFrames == 1`, write `CsFrameInterpolationConstants`, update the colour history, record `pass0`…`pass190`, record the composition into `outputs[0]` |
| anything else | — | `FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE` (2) |

`PrepareV2` requires `renderSize` = the context size, a 2D `depth` in `R32_FLOAT` (surface format;
the Vulkan image may be `R32_SFLOAT` or `D32_SFLOAT`, detected via
`FFX_API_RESOURCE_USAGE_DEPTHTARGET`) and 2D `motionVectors` in `R16G16_FLOAT`, both at the render
size, with a known FFX resource state. `FrameGeneration` requires a 2D `presentColor` and
`outputs[0]` at the render size, in 8-bit UNORM (RGBA8 or BGRA8), `presentColor` in a read state
and `outputs[0]` in `FFX_API_RESOURCE_STATE_UNORDERED_ACCESS` (GENERAL).

The application images must carry the usages the provider's shaders need: `SAMPLED` on
`presentColor`, depth and motion vectors (the provider reads them with `texelFetch`), and
`COLOR_ATTACHMENT` on `outputs[0]` (the composition is a render pass, because a GLSL storage image
cannot express the BGRA8 format). The 3.1.7 swapchain's interpolation output has
`TRANSFER_SRC|TRANSFER_DST|SAMPLED|STORAGE|COLOR_ATTACHMENT` and its replacement backbuffer has
`SAMPLED`, so both requirements are met by that path.

The `FfxApiResource.state` field is mapped to a Vulkan image layout exactly as the SDK Vulkan
backend does (`COMMON`/`GENERIC_READ`/`UNORDERED_ACCESS` → GENERAL, `COMPUTE_READ`/`PIXEL_READ`/
`PIXEL_COMPUTE_READ` → SHADER_READ_ONLY_OPTIMAL, `COPY_SRC`/`COPY_DEST` → TRANSFER_*, `PRESENT` →
PRESENT_SRC_KHR, `RENDER_TARGET` → COLOR_ATTACHMENT_OPTIMAL, `DEPTH_ATTACHMENT` →
DEPTH_STENCIL_ATTACHMENT_OPTIMAL). The provider does **not** transition application images; the
layout written into the descriptor is the state layout, and a global memory barrier orders the
application's earlier writes before the provider's reads. That is the FFX Vulkan contract and it
keeps the provider from guessing the format/aspect of images it does not own.

## 3. Context resources

Creation mirrors `tools/mlfi-replay/mlfi_replay.c` (the code is copied and adapted, not shared,
because the replay's globals are single-context by design):

| item | value | source |
| --- | --- | --- |
| model buffer | 10,657,152 B, device local, storage + transfer-dst | `model_blob[72 : 72+10,657,152]`, FNV-1a checked (basis `1469598103934665603`, the capture's non-canonical basis) |
| scratch | 58,397,248 B (the captured 1280×720 size) | zero-filled once per reset (`vkCmdFillBuffer`) so two processes agree |
| uniform arena | 3,276,800 B, host-visible coherent, uniform + transfer-dst | captured tensor tables at all 175 offsets, prepare blocks at 0/80/144/288/352, model blocks at 416/200,688/256,704 |
| pipelines | 195 graph pipelines (191 model in pass order + 4 preprocess created in the captured order 9,008 / 4,204 / 5,004 / 8,980 B) | the legalised modules from `prepare.py` |
| descriptor sets | 196 (5 prepare + 191 model) | one set per dispatch; the captured arena offsets are baked in |
| images | `rgb0`, `rgb1`, `depth`, `mv`, `distortion`, `dilated_mv`, `downscaled_depth`, `fwd_mv`, `splat0to05`, `splat1to05`, `flow0to05`, `flow1to05`, `hudless_ui`, `prediction` | formats/sizes exactly as `docs/MLFI-REPLAY.md` §3.2 |

The captured descriptor counts are asserted at startup (`model0=78 scratch=175 constants=3
tensors=175` over the 191 model passes). The four-gigabyte-class scratch allocation is why the
provider is not something to create per frame: `ffxCreateContext` is the expensive call.

The model bank is copied into the device buffer during the **first** `PrepareV2` (the capture does
the same), using a host-visible staging buffer that lives until the context is destroyed. There is
one uniform-arena region and therefore one frame in flight; a second frame recorded before the
first has finished would overwrite the 80-byte model blocks. The runtime's 3.1.7 path submits each
game command buffer before the swapchain records the interpolation command buffer, so this
milestone's single in-flight restriction is compatible with it; a future async-queue path would
need one arena region (258,256 B step, as the capture shows) per in-flight frame.

## 4. `PrepareV2` mapping

`ffxDispatchDescFrameGenerationPrepareV2` has no colour resource: `presentColor` only exists in
`ffxDispatchDescFrameGeneration`. The provider therefore splits the work as follows:

| stage | what it does |
| --- | --- |
| `PrepareV2` | depth and motion vectors into the provider images; five prepare constant blocks from `renderSize`/`displaySize`/jitter/camera; reset handling; 5 preprocess dispatches |
| `FrameGeneration` | colour history (`rgb0` = previous, `rgb1` = current) from `presentColor`; 80-byte frame constants; `pass0`…`pass190`; composition |

This is the only possible split with the 4.0.2 descriptor set, and the preprocess modules confirm
it: none of the five `main` modules binds `r_input_rgb0/1` (`docs/MLFI-REPLAY.md` §3.3), so the
preprocess dispatches do not need the colour resource that arrives later.

### 4.1 Depth and motion vectors

The depth and motion-vector images of the application are **sampled** by three compute copy
shaders (ours) into the provider's `depth` (R32_SFLOAT) and `mv` (R16G16_SFLOAT) images:

* a `sampler2D` + `texelFetch` copy at the same integer coordinate, with a nearest sampler and a
  global memory barrier before it;
* the depth view is `R32_SFLOAT` (colour-coded depth, the runtime's captured-scene path) or
  `D32_SFLOAT` with the depth aspect (the runtime's neutral path), selected from
  `FFX_API_RESOURCE_USAGE_DEPTHTARGET` in the resource description. The FFX surface format is
  `R32_FLOAT` for both Vulkan formats, so this flag is the only discriminator;
* sampling instead of `vkCmdCopyImage` deliberately avoids requiring `TRANSFER_SRC` on application
  images. The runtime's motion image is created with `SAMPLED|TRANSFER_DST|STORAGE` and has no
  `TRANSFER_SRC`, so a copy would be invalid.

### 4.2 Prepare constants (pinned layouts)

The five blocks are slices of the arena. Their member layouts are read from the modules'
`OpMemberDecorate` offsets, and the captured bytes confirm them:

**`CsDilate` (80 B, row 0)** — `motionVectorJitterCancellation: v2float @0`, `renderSize: v2uint
@8`, `displaySize: v2uint @16`, `dispatchSize: v2uint @24`, `motionVectorScale: v2float @32`,
`generationRectMinUV @40`, `generationRectMaxUV @48`, `depthInverted: uint @56`, `_pad1 @60`,
`colorRectMinUV @64`, `colorRectMaxUV @72`. The provider writes `-jitterOffset` as the
cancellation term (the capture's jitter was zero, so the sign is inferred), the render/display
sizes, `dispatchSize = (W/3, H/3)`, `motionVectorScale` from the descriptor, full-generation-rect
UVs, `depthInverted = 0` and full colour-rect UVs.

**`CsAMDSplatting64Constants` (64 B, rows 1/3/4)** — `dstSize: v2uint @0`, `renderSize @8`,
`projAmount: float @16`, `flowIsUv: int @20`, `generationRectMinUV @24`, `generationRectMaxUV
@32`, `depthInverted @40`. The captured values are reproduced: row 1 `projAmount=1.0`,
`flowIsUv=0`; row 3 `0.5`/`1`; row 4 `0.5`/`0`. (`projAmount`/`flowIsUv` are the capture's; the
semantics inside the module are not interpreted.)

**`CsComputeFwdMVUsingMatConstants` (144 B, row 2)** — `prevPstoPs: mat4 @0`, `prevPsToPrev2Ps:
mat4 @64`, `displaySize: v2uint @128`.

The view-projection convention is pinned by the capture's frame-0 block: Vulkan depth range
`[0,1]`, right-handed, look-at view rows `(right, up, -forward)`, column-major storage, and
`prevPstoPs = inverse(prevVP) * curVP` with `prevVP = identity` for the first frame (the second
matrix is `inverse(prev2VP) * prevVP`, also identity-initialised). Rebuilding that block from the
captured `PrepareV2` camera (position 0,0,5; up +Y; right +X; forward −Z; near 0.1, far 1000, fov
1.0, 1280×720) matches the captured 16 floats to a maximum relative error of **7.2 × 10⁻⁶** (the
vendor's first "previous" matrix carries a ~2 ppm scale that we do not reproduce). The second
frame's captured block is approximately the identity, which is what this derivation produces for
a static camera. The exact float path is not bit-identical to AMD's, and with zero motion vectors
it does not change the output at all (verified: ±37 px motion vectors change frame 1, zero does
not).

### 4.3 Reset

`reset` on `PrepareV2` (or a non-sequential `frameID` seen by `ffxConfigure`) clears every provider
image, zero-fills the scratch and uploads the model bank on the first reset. The five preprocess
dispatches then run against empty history. If only `FrameGeneration` carries `reset`, the provider
still writes the reset constant and duplicates `presentColor` into `rgb0`, but the preprocess
dispatches have already used the previous history; a log line says so. The runtime sets
`prepare.reset = !fg.prepared_any`, so this case should not occur there.

## 5. `FrameGeneration` mapping

1. `presentColor` is copied into the provider's `rgb1` by our RGBA8 copy shader (sampled →
   `rgba8` storage). `rgb0` receives the previous `rgb1` with `vkCmdCopyImage`, or, on a reset
   frame, a duplicate of the current frame (the capture's first frame binds both colour images).
   Both are provider images in GENERAL.
2. The 80-byte `CsFrameInterpolationConstants` is rewritten in all three captured offsets
   (416 / 200,688 / 256,704). Member mapping from the module: `renderSize @0`, `displaySize @8`,
   `reset @16`, `backBufferTransferFunction @20`, `hudlessAttachedFactor @24`, `dispatchFlags
   @28`, `minMaxLuminance @32`, `generationRectMinUV @40`, `generationRectMaxUV @48`, `_pad1
   @56`, `debugBarColor @64`, `_pad2 @76`. `dispatchFlags` is written as 0: the 4.0.2
   `ffxDispatchDescFrameGeneration` has no `flags` member (the capture's block is 0 too).
   `debugBarColor` is the constant `(0,1,1)` from the capture's frame 0; the capture's frame 1
   changes this cosmetic field to `(1,0.42,0)`, which the provider does not reproduce.
3. `pass0`…`pass190` are recorded in ascending order with the captured group counts and a full
   memory barrier between every dispatch (the capture did not intercept
   `vkCmdPipelineBarrier`, so global barriers are the conservative choice; the replay uses the
   same placement).
4. The composition pass writes `prediction` (RGBA32F) to `outputs[0]`. This pass is **ours**, not
   AMD's: the vendor's final 8,980-byte `main` was created but never dispatched in the capture
   (`docs/MLFI-CAPTURE.md` §4.1), so no vendor composition stage exists to replay. Our
   `compose.vert`/`compose.frag` draw one fullscreen triangle into `outputs[0]` as a colour
   attachment, `texelFetch` the prediction at the integer coordinate, clamp RGB to `[0,1]` and
   write alpha 1. Using a colour attachment (instead of a storage image) keeps the channel order
   and the format conversion driver-defined for both RGBA8 and BGRA8, which a GLSL storage image
   could not express (SPIR-V has no `Bgra8` image format; it would need `Unknown`, which glslang
   cannot emit).
5. A final global barrier publishes the colour-attachment writes to all later commands
   (`COLOR_ATTACHMENT_WRITE` → `MEMORY_READ|MEMORY_WRITE`, `COLOR_ATTACHMENT_OUTPUT` →
   `ALL_COMMANDS`). The 3.1.7 swapchain's post-interpolation barrier expects shader writes; this
   barrier makes the frame available before that barrier anyway.

The output contract is therefore: 8-bit UNORM colour, `COLOR_ATTACHMENT` usage, state
`UNORDERED_ACCESS`/GENERAL, fully overwritten every frame with alpha 1. HDR, sRGB output and
non-8-bit outputs are rejected with `FFX_API_RETURN_ERROR_PARAMETER` (clear code + log).

## 6. What is wired, what is inferred

| item | state |
| --- | --- |
| five exports, version id/name, `ffxQuery` answers, descriptor types | **pinned** by the capture and the SDK headers |
| 195-pipeline set and creation order, module bytes, binding counts | **pinned** (modules from the user's extraction; the same legalisation as the replay) |
| pass order, group counts, arena offsets, 92-entry tensor table | **pinned** (committed `tools/mlfi-capture/derived/` TSVs) |
| model upload range and hash | **pinned** (FNV-1a matches the capture before the first dispatch) |
| prepare block sizes and member layouts | **pinned** (module decorations + captured bytes) |
| `CsDilate`/splat values for the captured camera and 720p | **pinned** (the 416-byte prepare block set matches the capture; only the camera matrix differs, at 7.2 × 10⁻⁶) |
| the 144-byte camera block derivation | **inferred convention, checked** to 7.2 × 10⁻⁶ relative against the capture; the exact vendor float path is not reproduced |
| jitter-cancellation sign | **inferred** (capture jitter was zero) |
| depth/MV/image wiring inside the provider | **inferred** from the replay (`docs/MLFI-REPLAY.md` §4); the capture cannot settle image identity |
| sampler states, subgroup size 32 | **inferred**, same as the replay |
| colour history ordering (rgb0 = previous, rgb1 = current) and reset duplication | **inferred** from the API and the capture's two non-empty reset-frame images |
| barriers between dispatches, app-resource state→layout, final publish barrier | **inferred** (the capture did not record barriers) |
| composition | **ours**: no vendor stage was captured; no quality claim |
| one frame in flight, 1280×720 only | **implemented limit** of this milestone |

## 7. Run evidence

Host: AMD Radeon RX 9070 XT (RADV GFX1201), Vulkan 1.4.363, driver 109457507, Mesa 26.99.99.

```sh
# 1. prepare the legalised modules (once; writes only to the user cache)
python3 tools/mlfi-replay/prepare.py

# 2. build the provider and its headless program
tools/mlfi-provider/build.sh

# 3. run two 1280x720 frames with the validation layer
MK8_MLFI_MODULES="$HOME/.cache/mk8online/mlfi-replay/modules" \
MK8_MLFI_MODEL="$HOME/.cache/mk8online/mlfi/model/model_blob.bin" \
nice -n 19 "$HOME/.cache/mk8online/mlfi-provider/build/mlfi_provider_test"
```

Output (`$HOME/.cache/mk8online/mlfi-provider/evidence/run-a.log`):

```
[mlfi-provider] modules: 195 (191 model + 4 preprocess)
[mlfi-provider] model: model_blob[72 : 10657224] fnv1a=b88e755565aa4249 (matches capture)
[mlfi-provider] pipelines: 195 created (191 model + 4 preprocess); bindings match capture
[mlfi-provider] descriptor sets: 196 written (5 prepare + 191 model)
[mlfi-provider] context: MLFI 4.0.2 id=0xf600000001000002 display=1280x720 render=1280x720
ffxCreateContext rc=0
provider version rc=0 id=0xf600000001000002 name=4.0.2
ffxConfigure rc=0
[mlfi-provider] model uploaded (10,657,152 bytes, fnv1a matches the capture)
MLFI-PROVIDER frame 0 hash=ee9c3cb0caa8b8ef nonzero=921600/921600 motion_px=0.00
MLFI-PROVIDER frame 1 hash=0ddc97da20dd3d61 nonzero=921600/921600 motion_px=0.00
MLFI-PROVIDER stats rc=0 prepare_dispatches=5 copy_dispatches=2 model_dispatches=191 \
    compose_dispatches=1 frames=2 resets=1 last_prepare_rc=0 last_frame_rc=0
MLFI-PROVIDER summary pipelines=195 dispatches=199 compose=1 validation_errors=0 \
    validation_warnings=0 known_cm_fconvert=10 known_int_fetch_linear=8 known_cm2_spirv=20 \
    hashes=ee9c3cb0caa8b8ef/0ddc97da20dd3d61 changed=yes device=alive
MLFI-PROVIDER RESULT: OK
```

* `pipelines=195` is the graph set; the diagnostic also reports 3 input-copy and 1 compose
  pipeline. The per-dispatch counters are incremented at the `vkCmdDispatch`/draw call site:
  **5 preprocess + 191 model + 1 compose + 2 copies = 199** per frame.
* `validation_errors=0`, `validation_warnings=0`. The three `known_*` counters are the documented
  validator disagreements of `docs/MLFI-REPLAY.md` §7, counted separately so a real error is
  never hidden. They have the same values as the replay (10/8/20).
* Both frames are fully non-zero, and the reset frame is not special-cased.
* `run-a` and `run-b` (two separate processes, same inputs) produce exactly the same two hashes:
  `ee9c3cb0caa8b8ef` and `0ddc97da20dd3d61`. The raw dumps are byte-identical:

  | file | sha256 |
  | --- | --- |
  | `frame0.rgba` | `c9995533c30395e036971a794ef0e853d86ddd9d910022181dfba6b9d4939c16` |
  | `frame1.rgba` | `9d9f96d42e99d5dd0b13787033894b0c2903870fa23d2b1c89860e5168b888de` |

  A second run with its own dump directory produced the same two sha256 values (`cmp` reports both
  files identical), so the determinism claim covers the full 3.6 MB frame and not only the FNV
  hash.

* Motion vectors reach the model. `--motion 37` and `--motion -37` leave the reset frame at
  `ee9c3cb0caa8b8ef` and change frame 1 to `c3e6a314d1e5643c` and `293f3c819aa6a7e1`; zero motion
  vectors give `0ddc97da20dd3d61`. The prepare → dilated MV → splat → model path is wired.
* `--d32-depth` (the runtime's neutral depth path) produces the same hashes and the same clean
  validation as the R32 path.
* `journalctl -k --since` after the runs contains no `amdgpu`, GPUVM, ring or device-lost line.
* Run time: 2 frames + 199-pipeline context creation in **0.8 s** wall on a loaded host with
  `nice -n 19`.

## 8. Limitations of this milestone

1. **No vendor reference.** The only vendor output on this host is the no-op-substituted capture
   (`0/921,600` non-zero pixels). Nothing here shows the provider matches AMD's frame.
2. **Composition is ours.** `rw_prediction` is a plausible software frame, but the vendor's final
   `main` (watermark/UI composition, 8,980 B) was never dispatched in the capture. A future Wine
   run with a swapchain can capture it; until then the composition stage is not AMD's.
3. **No swapchain provider.** This library answers the effect id only; the 3.1.7 swapchain
   provider remains the other module and the runtime drives both (§9.2).
4. **One resolution.** Only the captured 1280×720 layout is implemented. The 4K/8K threshold and
   per-resolution tensor families are untouched; other sizes are refused at creation.
5. **8-bit UNORM only.** sRGB/HDR present colour and outputs are refused; the runtime's window is
   an SDR UNORM swapchain today (but `surface_format()` also accepts sRGB, see §9).
6. **One frame in flight**, one arena region, no async compute. A second submission before the
   first completes would corrupt the 80-byte model constants.
7. **Input images are sampled**, so they must have `SAMPLED` usage and the depth/MV images must be
   in the state implied by `FfxApiResource.state`. The runtime creates them that way.
8. **No quality measurement.** Ghosting, haloing, HUD/text edges and pacing are not measured;
   `gameplay_verified` remains false.

## 9. Integration with `MK8_FSR_FG=fsr4`

`MK8_FSR_FG=fsr3` loads one library, checks that its **swapchain** provider reports exactly 3.1.7,
creates a swapchain context and an effect context from that same library, and routes
`mk8_fsr_fg_prepare` into `ffxDispatch(PrepareV2)` and the swapchain's `frameGenerationCallback`
into `ffxDispatch(FrameGeneration)`. `MK8_FSR_FG=fsr4` keeps that swapchain and moves the effect to
this MLFI provider; the implemented topology is described in §9.2 and its evidence in §9.3.

### 9.1 The hand-off in one sentence

The 3.1.7 swapchain does not need to know which effect implementation it drives: it calls the
application's `frameGenerationCallback` with `ffxDispatchDescFrameGeneration`, and the runtime
passes that descriptor to whichever library owns the effect context. The swapchain supplies
`outputs[0] = interpolationOutput()` (the swapchain format, usage
`TRANSFER_SRC|TRANSFER_DST|SAMPLED|STORAGE|COLOR_ATTACHMENT`, state
`FFX_API_RESOURCE_STATE_UNORDERED_ACCESS`), `presentColor = backbuffer` (state
`FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ`), and a command buffer on the interpolation queue
(`FrameInterpolationSwapChainVK::dispatchInterpolationCommands`). Those are exactly the states and
usages this provider consumes; the swapchain's own pre/post barriers remain in charge of the
application images, and the provider's final barrier publishes its colour-attachment writes.

### 9.2 Implemented two-library topology (20/11/2026)

`MK8_FSR_FG=fsr4` is implemented in `runtime/mk8/graphics/fsr_fg.cpp`; `fsr3` and the FG-off path
are unchanged. The mode loads the 3.1.7 swapchain provider exactly as fsr3 does and additionally
loads the MLFI provider from `MK8_FSR_MLFI_LIB` (alias `MK8_FSR4_FG_LIB`), else
`$XDG_DATA_HOME/mk8online/fsr/libmk8_mlfi_framegeneration_vk.so`, else
`$HOME/.local/share/mk8online/fsr/…`, else `local/fg-provider/…`. An explicitly set env path is
authoritative: a wrong value disables the mode instead of silently trying another file.

Version checks: the swapchain library must report exactly **3.1.7** (unchanged), and the effect
library must answer `ffxQueryDescGetVersions` for `FRAMEGENERATION` with exactly one entry named
**4.0.2**, id `0xf600000001000002`. Both checks log every offered version and fail closed.

**Three contexts, not two.** The 3.1.7 swapchain does not accept
`ffxConfigureDescFrameGeneration` directly; it learns that interpolation is enabled when the
*3.1.7 frame-generation effect provider* forwards the config through
`fpSwapChainConfigureFrameGeneration`. The MLFI provider's Configure only stores the fields. The
implemented topology is therefore:

| context | library | role |
| --- | --- | --- |
| `sc_ctx` | 3.1.7 | swapchain replacement, pacing, present/acquire |
| `notify_ctx` | 3.1.7 | frame-generation context used **only** as the swapchain's configure router; it never dispatches |
| `fg_ctx` | MLFI 4.0.2 | `PrepareV2` + `FrameGeneration`; selected with `ffxOverrideVersion{0xf600000001000002}` |

Per frame, `mk8_fsr_fg_prepare` configures `notify_ctx` (3.1.7 → swapchain) and `fg_ctx` (MLFI,
frame-ID/reset bookkeeping), then records `ffxDispatch(PrepareV2)` into the game's command buffer.
The swapchain's `frameGenerationCallback` receives the interpolation command buffer,
`presentColor = replacement backbuffer` (PIXEL_COMPUTE_READ) and
`outputs[0] = interpolationOutput()` (UNORDERED_ACCESS/GENERAL), and dispatches `FrameGeneration`
to `fg_ctx`. This is the hand-off §9.1 describes, with the router context that makes the 3.1.7
swapchain actually call it. The router context performs no GPU work in fsr4.

**One frame in flight.** The provider has one uniform arena and one descriptor set per phase, so
the bridge serialises the MLFI command buffers itself: a `VkEvent` is set at the end of every
recorded MLFI command buffer (prepare and frame generation) and the next recording polls it before
touching the provider (Vulkan 1.2 timeline semaphores cannot be signalled from inside a command
buffer). The wait is 5 s, then the mode disables interpolation with one log line and presents real
frames. This is the only added synchronisation; the game's own pipelining is untouched.

**Device features.** Only for `MK8_FSR_FG=fsr4`, `toolchain/fsr_fg.py` adds the two extensions not
already enabled by `toolchain/fsr.py` (`VK_EXT_cooperative_matrix_maintenance1`,
`VK_EXT_shader_image_atomic_int64`) and enables `subgroupSizeControl`, `shaderFloat16`,
`vulkanMemoryModel`, `variablePointers`/`variablePointersStorageBuffer`. Cooperative matrix and
float8 are already requested unconditionally by `fsr.py` for the upscaler. The swapchain keeps the
game's 8-bit UNORM format (the runtime already prefers `B8G8R8A8_UNORM`; an sRGB swapchain is
refused by the provider with ffx 6). On resize/recreation the effect is destroyed and recreated so
the provider never keeps views of retired swapchain images.

**Failure behaviour.** A missing library, a foreign version, a refused context (wrong size/format,
missing feature) or a failing dispatch disables interpolation, logs the reason and lets the swapchain
present real frames. No path returns a fictitious success.

### 9.3 Runtime evidence (20/11/2026)

Build: `2189534` plus this change (the FG descriptor-pool fix `db4c239` is in that base),
`toolchain/build.py` with the private qualification config; executable SHA-256
`48c44007d287df4ef9e33a9b8e658af9e8d76b9fd8eb942b0e6866e46449c59d`.
Driver: Mesa 27.0.0-devel (mesa-git `f7dd61f78f8`), RADV GFX1201, Vulkan 1.4.363. Provider:
`~/.cache/mk8online/fsr-fix-install/libamd_fidelityfx_framegeneration_vk.so` (3.1.7) and
`~/.cache/mk8online/mlfi-provider/build/libmk8_mlfi_framegeneration_vk.so` (419,176 B).

A headless two-library run (a copy of the repo fixture adapted for fsr4 and saved in
`local/qualification/fsr4-fg-2026-11-20/`, 1280×720, validation layer) created
the swapchain, the 3.1.7 router and the MLFI effect (`rc=0` each), queried 4.0.2 and presented six
frames through the router-installed callback. The only validation messages were shader-module
disagreements in the same classes the provider test counts as known (linear sampler on integer
formats, the NV cooperative-matrix2 SPIR-V capabilities) plus the memory-model device-scope message
for the fixture's device; there were no hand-off, synchronisation or layout errors.

In-game `MK8_FSR_FG=fsr4` at 1280×720:

```
[fsr_fg] MLFI device extensions enabled (cooperative_matrix_maintenance1, image_int64_atomics)
[fsr_fg] swapchain provider 3.1.7 (version 0xc01007)
[fsr_fg] MLFI effect provider 4.0.2 (id 0xf600000001000002)
[mlfi-provider] context: MLFI 4.0.2 id=0xf600000001000002 display=1280x720 render=1280x720
[fsr_fg] MLFI effect context: 1280x720 format 14, override 0xf600000001000002
[fsr_fg] active: FFX frame-interpolation swapchain 1280x720 format 44 (present family 1)
[fsr_fg] MLFI prepare frame 0: 5 preprocess + 2 copy dispatches (resets 1)
[fsr_fg] first interpolated frame recorded (frameID 0, 1280x720)
[fsr_fg] MLFI frame generation: 191 model + 1 compose dispatches, frames 1, resets 1
[fsr_fg] MLFI progress: 1800 generated frames, 191 model + 1 compose dispatches
```

Exit 0 (the last screenshot is frame 2040; the input script was sized to 2,150), screenshots at
frames 840 and 2040, no GPUVM/device-lost/`FATAL` line, and the
bridge never disabled itself after activation. A separate attempt reached 2,400 frames. Both real
frames are byte-identical between the final fsr4 and fsr3 runs at the same frames/window/driver
(menu sha256 `617d9919291a5d02483360b20c959ae15fb13aa500f32e5923221dbee82f22ea`, kart select
`c8dafba17ee796e399e743d5b38c81f0003531a7f1614edde4ea8966f6e7db08`): the MLFI path does not alter
the game's real-frame output. (One earlier fsr4 run caught a different animated-menu phase under
slower pacing; the static frame was identical in every run.)

**Blocker for a race frame.** The host's Mesa 27.0.0-devel segfaults at ~frame 2100 when the game
enters the race (`vkCmdDrawIndexed`, `libvulkan_radeon.so`), with FG off, fsr3 and fsr4 alike; it is
the driver issue already recorded in `docs/STATUS.md` (the stable RADV 26.2.6 does not crash). The
stable driver lacks `VK_EXT_cooperative_matrix_maintenance1`, which the MLFI 4.0.2 provider
requires, so there is no driver on this host that is both crash-free and MLFI-capable. No race
screenshot exists; the menu/kart-select/race-transition frames above are the end of the evidence.
The generated frames are not separately captured either (the screenshot path reads the game's
replacement buffer), so image quality, ghosting, pacing and the vendor comparison remain
**unmeasured**. `gameplay_verified` stays false.

### 9.4 Packaging end state (one library)

The cleaner end state is one `libamd_fidelityfx_framegeneration_vk.so`-shaped module that
registers both providers: the existing 3.1.7 swapchain provider plus this MLFI effect provider.
The loader's provider list would then contain two providers that answer the same effect id, which
is exactly why the capture used `ffxOverrideVersion` — with the override the MLFI provider is
selected, and without it the loader would pick by version id. The composition source is ours, so
that combined build must keep the licence split: SDK-source-derived 3.1.7 code under AMD's SDK
licence, the MLFI shell/shader code under this project's MIT licence, and no AMD bytes in the
repository. Until that build exists, the two-library route above needs no new ABI and is validated.

## 10. Reproduction

```sh
# extraction (once; user's own DLL)
python3 tools/extract_mlfi.py --dll <sdk>/Kits/FidelityFX/signedbin/amd_fidelityfx_framegeneration_vk.dll

# legalise the four modules RADV cannot compile unchanged (once)
python3 tools/mlfi-replay/prepare.py

# build the provider + headless program
tools/mlfi-provider/build.sh

# two frames at 1280x720, validation on
MK8_MLFI_MODULES="$HOME/.cache/mk8online/mlfi-replay/modules" \
MK8_MLFI_MODEL="$HOME/.cache/mk8online/mlfi/model/model_blob.bin" \
nice -n 19 "$HOME/.cache/mk8online/mlfi-provider/build/mlfi_provider_test"

# the same with motion vectors or the neutral-depth format
... mlfi_provider_test --motion 37
... mlfi_provider_test --motion -37
... mlfi_provider_test --d32-depth

# tests (skip when the extraction/tools are absent)
python3 -m unittest tests.test_mlfi_provider -v

# in-game fsr4 (the full command list is in local/qualification/fsr4-fg-2026-11-20.md)
MK8_FSR_FG=fsr4 MK8_FSR_FG_LIB=$HOME/.cache/mk8online/fsr-fix-install/libamd_fidelityfx_framegeneration_vk.so MK8_FSR_MLFI_LIB=$HOME/.cache/mk8online/mlfi-provider/build/libmk8_mlfi_framegeneration_vk.so LARECOMP_WINDOW=1280x720 SDL_VIDEO_WAYLAND_SCALE_TO_DISPLAY=1 ./mk8drecomp <game-root>
```

The test prepares, builds, runs the program twice and asserts: context rc 0, `ffxQuery` 4.0.2,
a refused foreign override and a refused 1920×1080 creation, 195 graph pipelines, 5 + 2 + 191 + 1
recorded dispatches, both frames fully non-zero, a changing output, identical hashes between the
two processes, zero validation errors/warnings, and the same hashes for `--d32-depth`. It also
checks that `mlfi_shaders.h` matches the GLSL and that the generated graph matches the committed
TSVs. `MK8_MLFI_PROVIDER_REQUIRE=1` turns a missing prerequisite into a failure.

## 11. Verified vs still open

| claim | state |
| --- | --- |
| the library exports the five FFX entry points and `mk8MlfiGetStats` | **verified** (`nm -D`) |
| `ffxQuery` reports MLFI 4.0.2 and refuses other effects/overrides | **verified** (test assertions) |
| a context is created from `ffxCreateContextDescFrameGeneration` + `ffxCreateBackendVKDesc` at 1280×720 | **verified** (rc 0, 195 graph pipelines) |
| the model upload is the captured range | **verified** (FNV-1a `b88e755565aa4249` before the first dispatch) |
| the descriptor counts match the capture | **verified** (78/175/3/175 asserted at startup) |
| `PrepareV2` records 5 preprocess dispatches and `FrameGeneration` records 191 model passes | **verified** (counters at the call site: 5 + 191) |
| the composition writes every pixel of `outputs[0]` | **verified** (sentinel fill, 0 sentinel pixels left) |
| the generated frame is non-trivial and follows its input | **verified** (921,600/921,600 non-zero on both frames; hashes change with the input and with ±37 px motion vectors) |
| runs are deterministic across processes | **verified** (identical FNV hashes and byte-identical raw dumps) |
| validation is clean | **verified except** the three documented validator disagreements (same 10/8/20 as the replay) |
| no GPUVM fault / device loss | **verified** for these runs (`device=alive`, no kernel amdgpu line) |
| `MK8_FSR_FG=fsr4` in the runtime | **implemented and exercised** through title, menus, kart select and the race transition (clean exits at 2,040 frames with at least 1,800 generated and at 2,400 frames); no race frame because the host driver crashes there (see §9.3). `gameplay_verified` remains false |
| the swapchain provider is replaced | **not implemented**; the 3.1.7 provider stays |
| the provider matches AMD's frame | **not demonstrated** — no vendor reference on Linux |
| image quality / ghosting / haloing | **not measured** — synthetic inputs, no reference, our composition |
| any resolution other than 1280×720, HDR/sRGB output, async/multi-frame | **not implemented** |
| `gameplay_verified` | remains **false** for MLFI |
