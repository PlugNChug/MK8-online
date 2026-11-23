# MLFI host layout: running AMD's signed frame-generation DLL under Wine

Status 20/11/2026. Companion to `docs/FSR4-MLFI.md`, which established what is inside
`amd_fidelityfx_framegeneration_vk.dll` and why the 191-pass host sequence, the
`CsTensorSizes` table, the weight addressing and the dispatch geometry were the missing pieces.
This document reports the empirical recovery of that host layout: the signed DLL was loaded by
a small Windows test program under Wine, pointed at the host RADV device, and driven through
`ffxQuery` → `ffxCreateContext` → `ffxConfigure` → `ffxDispatch(PrepareV2)` →
`ffxDispatch(FrameGeneration)`. The Vulkan calls were intercepted and the data the host writes
was captured.

Short answer: **the host layout is recoverable, and most of it is now data.** The API sequence,
195-pipeline set, pass order, dispatch geometry, descriptor bindings, the 92-entry
`CsTensorSizes` table, the model-bank upload and the per-frame constants are captured for 4K
and 8K. The model does not have a host-side weight-offset table: the whole 10,657,152-byte
model slice is bound at offset 0 and each pass addresses it with constants inside its own
SPIR-V module. A native provider can therefore replay the extracted modules unchanged.

A correct native provider is nevertheless **not deliverable on this host today**, and the
blocker has moved: four modules per context (the 5,004-byte preprocess `main` and the three
composite passes `151/152/190`) segfault RADV inside `vkCreateComputePipelines`. They were
replaced with no-op pipelines to finish the capture, so no valid frame was produced. The
remaining work is a Mesa/RADV fix or a SPIR-V workaround, not more host reverse engineering.

## 1. What was run

| item | value |
| --- | --- |
| DLL | `amd_fidelityfx_framegeneration_vk.dll`, FSR SDK 2.4.0 |
| DLL size / SHA-256 | 36,806,928 B / `d002dabff8c5e71eaa715fc99438cd4dc81230262a7897b16bc402efec3498b4` |
| Wine | 11.22, prefix `$HOME/.cache/mk8online/mlfi-wine` |
| Vulkan | 1.4.363 |
| GPU / driver | AMD Radeon RX 9070 XT (RADV GFX1201), Mesa 26.99.99 (driver 109457507) |
| OS | Linux 7.4.0-rc1 (CachyOS) |
| compiler | mingw-w64 GCC 16.2.0 |
| harness | `tools/mlfi-capture/mlfi_capture.c`, built by `tools/mlfi-capture/build.sh` |
| capture driver | `tools/mlfi-capture/run.sh` |
| analyser | `tools/mlfi-capture/analyze_capture.py` |

Wine forwards `vulkan-1.dll` to the host loader, so the DLL sees the same RADV device the native
`tools/mlfi_probe.c` sees: vendor `0x1002`, device `0x7550`, Vulkan 1.4.363, with
`VK_KHR_cooperative_matrix`, `VK_EXT_cooperative_matrix_maintenance1`, `VK_EXT_shader_float8`,
`VK_EXT_shader_image_atomic_int64`, the Vulkan memory model and variable pointers.

The device was created with one compute queue and the feature set the extracted modules require
(`shaderFloat16`, `shaderInt8`, 8/16-bit storage, `vulkanMemoryModel` + device scope,
`variablePointers`, cooperative matrix + maintenance1 conversions/per-element/reductions,
`shaderFloat8` + cooperative matrix, image int64 atomics). No surface or swapchain is used.

## 2. Harness and interception

The DLL imports part of Vulkan statically and resolves the rest through the
`VkDeviceContext.vkDeviceProcAddr` supplied in `ffxCreateBackendVKDesc`. Both paths are covered:

- **IAT hook.** After `LoadLibraryA`, the DLL's import directory is parsed and the entries for
  `vkCreateShaderModule`, `vkCreateBuffer`, `vkBindBufferMemory`, `vkAllocateMemory`,
  `vkMapMemory`, `vkUnmapMemory`, `vkUpdateDescriptorSets`, `vkCreateDescriptorSetLayout`,
  `vkAllocateDescriptorSets`, `vkCmdBindPipeline`, `vkCmdBindDescriptorSets`,
  `vkBeginCommandBuffer`, `vkEndCommandBuffer`, `vkQueueSubmit` and `vkGetDeviceProcAddr`
  are replaced. The wrappers call the real driver pointers recorded at startup.
- **`vkDeviceProcAddr` wrapper.** The `VkDeviceContext` passed to the DLL points at our
  function, which returns wrappers for `vkCreateComputePipelines`, `vkCmdDispatch`,
  `vkCmdDispatchIndirect`, `vkCmdUpdateBuffer`, `vkCmdCopyBuffer`, `vkCmdFillBuffer`,
  `vkCmdPushConstants` and `vkCreateShaderModule`, and the driver pointer otherwise.

Data recovery:

- every `vkCreateShaderModule` is hashed and dumped under `raw/` (SHA-256 identity is checked
  against the extracted set);
- every `vkCreateComputePipelines` records entry point, module size/hash and the returned
  pipeline handle, so a dispatch maps to a pass name;
- `vkCmdBindPipeline` / `vkCmdBindDescriptorSets` / `vkUpdateDescriptorSets` recover the
  descriptor for each dispatch, including the buffer offsets inside the host's uniform arena;
- `vkCmdUpdateBuffer` data and `vkCmdCopyBuffer` sources are dumped; the source is read from a
  live mapping or from a snapshot taken in `vkUnmapMemory`, which is how the model upload is
  recovered even though the DLL unmaps the staging buffer before recording the copy;
- a shadow copy of each small buffer mirrors the writes, so the 1,472-byte `CsTensorSizes`
  slice can be dumped at `vkCmdDispatch` time.

## 3. API sequence that works

1. `ffxQuery(NULL, ffxQueryDescGetVersions{createDescType=0x00020001, device=&VkDeviceContext})`
   returns two providers:

   | id | name |
   | --- | --- |
   | `f600000001000002` | `4.0.2` (MLFI; selected) |
   | `f600000000c01006` | `3.1.6` (FSR 3.1.6 frame interpolation) |

2. `ffxCreateContext` with a descriptor chain:

   - `ffxCreateContextDescFrameGeneration` (`0x00020001`), `flags=0`,
     `displaySize=maxRenderSize=1280x720` (or the requested size),
     `backBufferFormat=FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM`;
   - `ffxCreateBackendVKDesc` (`0x01000003`) with `VkDeviceContext{device, physicalDevice,
     vkDeviceProcAddr}`;
   - `ffxOverrideVersion` (`type=5`) with `versionId=f600000001000002`;
   - `ffxCreateContextDescFrameGenerationVersion` (`0x0002000e`) with
     `version=FFX_FRAMEGENERATION_VERSION` (`0x01000002`).

   Without the version override the loader may choose either provider; the override makes the
   ML provider explicit. Context creation returns 0. It creates the 191 model pipelines and
   four preprocess `main` pipelines synchronously (see §4). The model upload is **not** done
   here; it is recorded during the first `PrepareV2`.

3. `ffxConfigure` with `ffxConfigureDescFrameGeneration` (`0x00020002`):
   `frameGenerationEnabled=true`, `allowAsyncWorkloads=false`,
   `flags=FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY` (1<<3),
   `onlyPresentGenerated=false`, `frameID=f`. The no-swapchain flag is what allows a context
   without a `VkSwapchainKHR`; without it the provider tries the swapchain ABI checks.

4. `ffxDispatch` with `ffxDispatchDescFrameGenerationPrepareV2` (`0x0002000c`):
   `commandList` = the application's recording `VkCommandBuffer`, `renderSize`, zero jitter,
   motion vector scale 1, `frameTimeDelta=16.6`, `reset=(frame==0)`, near/far/FOV, depth and
   motion-vector `FfxApiResource`s, and the four camera basis vectors.

5. `ffxDispatch` with `ffxDispatchDescFrameGeneration` (`0x00020003`): same command buffer,
   `presentColor`, `outputs[0]`, `numGeneratedFrames=1`, `reset=(frame==0)`, sRGB transfer,
   `minMaxLuminance[1]=1000`, `frameID=f`.

6. The application ends the command buffer, submits it and waits. The provider records all
   work into the supplied command buffer; no `vkQueueSubmit` from the DLL was observed.

All calls returned 0 in the canonical runs. Two frames were recorded at 1280x720 and the
sequence repeated identically (10 prepare + 382 frame-generation dispatches).

## 4. Pipeline set and creation order

`ffxCreateContext` creates **195 compute pipelines** in this order:

1. `mlfi_4k_pass0` … `mlfi_4k_pass190` (191), or the `mlfi_8k_*` set when the output is above
   4K. Creation order is pass order.
2. Four preprocess `main` pipelines, in the order 9,008 B, 4,204 B, 5,004 B, 8,980 B.

The four `main` modules come from the extractor's `ui` group because their entry point is
`main`; they are MLFI preprocess, not swapchain UI. Three other `main` modules in that group
(3,432 B, 4,232 B, 9,064 B) are created by no path in this no-swapchain capture and are
probably swapchain/UI variants. The 8,980-byte main is created but not dispatched here.

Every runtime module was dumped at `vkCreateShaderModule` and compared with the extracted set:
**195/195 modules are byte-identical** for both the 4K (`canonical-4k-1f`) and 8K
(`canonical-8k-7680`) captures. The DLL does not patch SPIR-V at runtime for these paths; the
extracted modules are exactly what it compiles.

### 4.1 The RADV crash and the substitution

Without a workaround, context creation dies in `vkCreateComputePipelines` on
`mlfi_4k_pass151` (2,648 B). With `WINEDEBUG=+seh`:

```
0024:err:vulkan:vkCreateComputePipelines Exception 0xc0000005 in Unix call.
```

`log.txt` ends after the `vkCreateShaderModule` for `mlfi_4k_pass151`; the process exits 3.
The per-module probe in `docs/FSR4-MLFI.md` reports the same compile crash for `pass152` and
`pass190`; in this harness, after substituting the three model passes, context creation
continued and crashed on the 5,004-byte preprocess `main`. This is the RADV compile defect
already recorded there; the capture now pins the exact first failure and the four modules a
4K/8K context needs.

To finish the capture, those four pipelines are replaced with a no-op compute pipeline created
with the same `VkPipelineLayout` (logging marks them `substituted`). This preserves descriptor
compatibility and the host call sequence but produces no image: `pass190` is the final
composite. The `canonical-4k-readback` run confirms the consequence: the 1280x720 output image
reads back with `0/921,600` nonzero pixels and FNV-1a hash `a58e4c7456800383`. **No correctness
claim is made for the output.**

| context | module | size | SHA-256 (extracted) |
| --- | --- | --- | --- |
| 4K | `mlfi_4k_pass151` | 2,648 | `19f393a5c4a4724f9cef56a0e6c71a6f6477267f7fd782eac660da342f951ace` |
| 4K | `mlfi_4k_pass152` | 18,440 | `08c2c1e614140d6758820150e184ead044c83ab8ea61f333796d1a6e5076b7ee` |
| 4K | `mlfi_4k_pass190` | 18,264 | `8aed0215127ae5ddab25647df91c531820dccb3dca64c899b8c99d2f987b96f5` |
| 4K | `main` (splat) | 5,004 | `2a2a5b6951177669aca87999f86e37186ebd25522f7eccc1b0266869817e32b8` |
| 8K | `mlfi_8k_pass151` | 2,648 | `0ffa811e30836da8a0a8ca60d67f4187fdd67997309acfd751521a85b7a9d140` |
| 8K | `mlfi_8k_pass152` | 18,440 | `b592b19b759ad0c075c26624658cc55eff9b97b026a44762df0e7609e7283b82` |
| 8K | `mlfi_8k_pass190` | 18,264 | `8a7e91150c62ec05675e07ca8afa40ad08edcb052446b7adb711da18f48d4dda` |
| 8K | `main` (splat) | 5,004 | same as above |

The four substituted pipelines are marked with `substituted=1` in `passes.tsv` and in
`derived/passes_*.tsv`.

## 5. Pass order

The provider uses two stages:

- `PrepareV2` records **five** preprocess dispatches, all with the `main` entry point, all with
  `ceil(W/24) × ceil(H/24) × 1` groups (1280x720 → 54×30, 7680x4320 → 320×180).
- `FrameGeneration` records **191** model dispatches, `mlfi_4k_pass0` … `pass190` (or
  `mlfi_8k_*`), one each, in ascending order.

The five prepare dispatches at 1280x720:

| # | module size | module SHA-256 (first 16) | groups | constants arena offset | substituted |
| --- | --- | --- | --- | --- | --- |
| 0 | 9,008 | `6884a1de9d4150ea` | 54×30 | 0 | no |
| 1 | 4,204 | `c5c6f8bc0950ad7b` | 54×30 | 80 | no |
| 2 | 5,004 | `2a2a5b6951177669` | 54×30 | 144 | yes |
| 3 | 4,204 | `c5c6f8bc0950ad7b` | 54×30 | 288 | no |
| 4 | 4,204 | `c5c6f8bc0950ad7b` | 54×30 | 352 | no |

The 4,204-byte module is dispatched three times, with different constants; the 9,008-byte module
once. The 8,980-byte module is compiled but not dispatched in this path (probably the swapchain/UI
variant). The module bytes are the extractor's `main` modules (SHA-256 above).

At 8K the same five dispatches occur with 320×180 groups and the same module set.

## 6. Dispatch geometry

The full 191-row geometry for 1280x720 and 7680x4320 is in:

- `tools/mlfi-capture/derived/passes_mlfi_4k_1280x720.tsv`
- `tools/mlfi-capture/derived/passes_mlfi_8k_7680x4320.tsv`

Both files have the five prepare rows followed by pass0…pass190. The columns include the exact
`vkCmdDispatch` group counts, whether the pass was substituted, the binding-0/256/257 offsets and
the tensor-table id.

Representative rows:

| pass | 1280x720 | 1920x1080 | 3840x2160 | 5120x2880 | 7680x4320 |
| --- | --- | --- | --- | --- | --- |
| prepare `main` | 54×30 | 80×45 | 160×90 | 214×120 | 320×180 |
| `pass0` | 20×12 | 32×20 | 60×36 | 80×48 | 120×68 |
| `pass1` | 115×1 | — | — | — | 95×1 |
| `pass2` | 3×48 | — | — | — | 15×272 |
| `pass151` | 20×12 | — | — | — | 120×68 |
| `pass152` | 40×23 | — | — | — | 240×135 |
| `pass190` | 160×90 | 240×135 | 480×270 | 640×360 | 960×540 |

`pass0` groups are the first tensor entry divided by 8 (workgroup 8×8); `pass190` groups are
exactly `W/8 × H/8`. The prepare groups are `ceil(W/24) × ceil(H/24)`. The 37 distinct group
counts and the full per-pass sequence are in the TSVs; the pattern is not a single formula, so
the captured table is the authoritative source.

## 7. Descriptor bindings

The model passes use one set (set 0). Captured binding counts at 1280x720 (one frame):

| binding | descriptor type | passes binding it | contents |
| --- | --- | --- | --- |
| 0 | storage buffer | 78 / 191 | the whole model buffer (offset 0, `VK_WHOLE_SIZE`) |
| 128 | storage buffer | 175 / 191 | scratch buffer |
| 256 | uniform buffer | 3 / 191 (pass0, pass152, pass190) | 80-byte `CsFrameInterpolationConstants` |
| 257 | uniform buffer | 175 / 191 | 1,472-byte `CsTensorSizes` |

The 16 passes without binding 257 are the eight `5190×1×1` and eight `326×1×1` passes; they do
not bind the tensor table. The composite passes additionally bind storage images: in the captured sets `pass151` uses
129/130, while `pass152`/`pass190` use 1–7 and 129. The exact per-set image writes are in
`events.tsv`; the model/table/scratch pattern above is the part a replay needs.

The uniform data is packed into one **3,276,800-byte** uniform arena (usage
`TRANSFER_DST|UNIFORM_BUFFER`). For one 1280x720 frame the last table starts at 256,784 and ends at
258,256, so one frame uses about 252 KiB of the arena. Selected offsets:

| item | offset |
| --- | --- |
| prepare constants (5 blocks) | 0, 80, 144, 288, 352 |
| `pass0` constants / tensor table | 416 / 496 |
| `pass1` tensor table | 1,968 |
| `pass2` tensor table | 3,440 |
| `pass151` tensor table | 199,216 |
| `pass152` constants / table | 200,688 / 200,768 |
| `pass190` constants / table | 256,704 / 256,784 |

The tensor tables are spaced 1,472 bytes apart; the 80-byte constants are inserted before
`pass0`, `pass152` and `pass190`, which is why the offsets are not simply `496 + 1472·pass`.
A second frame uses the next region of the arena (prepare constants at 258,256; same pattern),
so the provider can allocate one region per in-flight frame. The full per-pass offsets are in
`passes.tsv`.

Allocations observed at 1280x720:

| allocation | size | usage |
| --- | --- | --- |
| model buffer (device) | 10,657,152 | `0x33` (transfer + uniform + storage) |
| model staging buffer | 10,657,152 | `0x33` |
| uniform arena | 3,276,800 | `0x12` (transfer-dst + uniform) |
| scratch | 58,397,248 | `0x23` (transfer + storage) |

At 7680x4320 the scratch buffer grows to 232,915,648 bytes; the model, staging buffer and
uniform arena keep the same sizes.

## 8. `CsTensorSizes`: 92 `uint4` entries

Every model pass that binds binding 257 receives the **same** table for a given resolution. The
table is also identical across frames: the 2-frame 1280x720 capture produced exactly one unique
table (175 dispatch bindings per frame, one hash). Only the first two components are nonzero;
`z = w = 0` in every captured entry.

1280x720 (`canonical-4k-1f`, SHA-256
`13f5688896225ed78326d05f370020a242ae1650d0340180217cb277ec782a2b`), four entries per line:

```
 0: 160x96    1: 160x96    2:  80x48    3:  80x48
 4:  80x48    5: 160x96    6:  80x48    7:  80x48
 8:  80x48    9: 160x96   10:  40x24   11:  20x12
12:  20x12   13:  10x6    14:  10x6    15:  10x6
16:  10x6    17:  10x6    18:  10x6    19:  10x6
20:  10x6    21:  10x6    22:  20x12   23:  40x24
24:  80x48   25: 160x96   26:  40x24   27:  40x24
28:  20x12   29:  20x12   30:  20x12   31:  20x12
32:  20x12   33:  20x12   34:  20x12   35:  20x12
36:  20x12   37:  40x24   38:  80x48   39: 160x96
40: 160x96   41:  80x48   42:  80x48   43:  40x24
44:  40x24   45:  40x24   46:  40x24   47:  40x24
48:  40x24   49:  40x24   50:  40x24   51:  40x24
52:  80x48   53: 160x96   54: 160x96   55: 160x96
56:  80x48   57:  80x48   58:  40x24   59:  40x24
60:  40x24   61:  40x24   62:  40x24   63:  40x24
64:  40x24   65:  40x24   66:  40x24   67:  80x48
68: 160x96   69: 160x96   70: 160x90   71: 160x90
72: 320x180  73: 320x180  74: 160x90   75: 160x90
76: 160x90   77: 160x90   78: 160x90   79:  80x45
80:  80x45   81:  80x45   82:  80x45   83:  80x45
84: 160x90   85: 160x90   86: 160x90   87: 160x90
88: 160x90   89: 320x180  90: 320x180  91: 1280x720
```

7680x4320 (`canonical-8k-7680`, SHA-256
`5871366c2e0f859105ec37830336f66a3ebe2940292dfa47934547468b0d37b1`), four entries per line:

```
 0: 960x544   1: 960x544   2: 480x272   3: 480x272
 4: 480x272   5: 960x544   6: 480x272   7: 480x272
 8: 480x272   9: 960x544  10: 240x136  11: 120x68
12: 120x68   13:  60x34   14:  60x34   15:  60x34
16:  60x34   17:  60x34   18:  60x34   19:  60x34
20:  60x34   21:  60x34   22: 120x68   23: 240x136
24: 480x272  25: 960x544  26: 240x136  27: 240x136
28: 120x68   29: 120x68   30: 120x68   31: 120x68
32: 120x68   33: 120x68   34: 120x68   35: 120x68
36: 120x68   37: 240x136  38: 480x272  39: 960x544
40: 960x544  41: 480x272  42: 480x272  43: 240x136
44: 240x136  45: 240x136  46: 240x136  47: 240x136
48: 240x136  49: 240x136  50: 240x136  51: 240x136
52: 480x272  53: 960x544  54: 960x544  55: 960x544
56: 480x272  57: 480x272  58: 240x136  59: 240x136
60: 240x136  61: 240x136  62: 240x136  63: 240x136
64: 240x136  65: 240x136  66: 240x136  67: 480x272
68: 960x544  69: 960x544  70: 960x540  71: 960x540
72:1920x1080 73:1920x1080 74: 960x540  75: 960x540
76: 960x540  77: 960x540  78: 960x540  79: 480x270
80: 480x270  81: 480x270  82: 480x270  83: 480x270
84: 960x540  85: 960x540  86: 960x540  87: 960x540
88: 960x540  89:1920x1080 90:1920x1080 91: 7680x4320
```

The 92-entry sequence is a fixed architecture list. The shapes are derived from the display
size, mostly at 1/8, 1/16, 1/32, 1/64 and 1/128, with two families: exact divisions (entries
70–91, e.g. `W/8 × H/8`, `W/4 × H/4`, `W × H`) and aligned divisions (entries 0–69, e.g.
`ceil(W/8/32)·32 × ceil(H/8/32)·32`). The 8K table is the same family sequence at the larger
base size. The alignment is per-family and not one global rule: at 1920x1080 the `/4` family is
`480×272` (not `480×270`), consistent with the `/8` height being rounded to even first, while
at 3840x2160 it is `960×540`. A provider should either capture the table at its target resolution or derive
each family's tile alignment from the modules; do not assume a single scale.

Captured tables for the five resolutions are in the capture directories and can be reproduced
with `analyze_capture.py`. The two committed TSVs are the 1280x720 and 7680x4320 cases.

## 9. Model bank upload

The model is uploaded **once**, during the first `PrepareV2`, not at context creation:

```
[copy] src=<staging> dst=<model> size=10657152 srcOff=0 dstOff=0 hash=b88e755565aa4249
```

The `hash=` field in the log is FNV-1a 64; the SHA-256 below is computed over the same bytes.
The source bytes are byte-identical to the extracted model bank at offset 72:

| property | value |
| --- | --- |
| extracted bank | 10,720,312 B, SHA-256 `43a77c641f8ba3cd6ee08939661578097bbf0d9f43007939ccb64c161349da90` |
| uploaded range | `model_blob[72 : 72+10,657,152]`, i.e. bytes 72…10,657,223 |
| uploaded SHA-256 | `e356c70ad53bca39b27751154ddd71546e0a7c4c84003232ec43b9799bc4c748` |
| match | exact, verified by `analyze_capture.py` (`bank offset=72`, `match=True`) |
| not uploaded | first 72 bytes and the last 63,088 bytes of the bank |

The first 72 bytes and the 63,088-byte tail are consistent with CPU-side data
(descriptor/scales tables or padding), not model input; that is a hypothesis, but the byte-exact
upload boundary is verified.

The uploaded buffer is the one bound at descriptor binding 0 for the 78 passes that read
weights, always at offset 0 with range `VK_WHOLE_SIZE`. **The host supplies no per-pass weight
offset table.** Each pass addresses the buffer with constants inside its own SPIR-V module. A
heuristic scan of 32-bit `OpConstant` values in `[100000, 10,657,152)` finds such values in
171/191 4K passes and 161/191 8K passes; examples:

| pass | candidate constants |
| --- | --- |
| `mlfi_4k_pass0` | 5,032,080 |
| `mlfi_4k_pass2` | 131,072; 1,205,376; 2,410,752; 5,032,080 |
| `mlfi_4k_pass100` | 131,072; 307,840; 1,847,040; 7,355,696; 9,202,736; 9,256,192; 9,422,080 |
| `mlfi_4k_pass190` | 6,256,848 |

They are 16-byte aligned in the 4K set, but this is not a proven weight-offset table: the 8K
modules also contain larger constants (e.g. `mlfi_8k_pass0` has 18,909,072 and 34,632, both
above the upload size), and a constant used as a tensor stride would look identical to one used
as a base. The exact role needs per-instruction analysis. A provider that reuses the extracted
modules unchanged gets the correct constants for free and does not need to interpret them.

## 10. Per-frame constants

The 80-byte `CsFrameInterpolationConstants` block written for `pass0`, `pass152` and `pass190`
at 1280x720 frame 0:

```
u32: 1280 720 1280 720 1 0 0 0 0 1000.0 0 0 1.0 1.0 0 0 0 1.0 1.0 0
```

Frame 1 differs in the fields at indices 4, 16, 17 and 18 (`1` → `0`, `0` → `1.0`,
`1.0` → `0.42`, `1.0` → `0`), i.e. the provider writes per-frame state
(reset/parity/jitter-style flags), not a constant table. The
prepare `main` passes receive 80-, 64-, 144-, 64- and 64-byte blocks containing the
render/display size, the `426,240` pair (likely a prepare grid), and the camera basis/matrix
from `PrepareV2`
(`cameraPosition`/`up`/`right`/`forward`); the 144-byte block ends with `1280 720`. The exact
bytes for both frames are in `raw/*const_*.bin` under the capture directory, and the block
offsets are in `passes.tsv`.

## 11. What a native provider needs

A replay provider can be assembled from the captured data in this order:

1. create one Vulkan compute device with the feature set in §1;
2. load the 195 extracted modules (191 `mlfi_4k_*` or `mlfi_8k_*` plus four `main`), create the
   descriptor set layouts from the modules' own decorations, and create the pipelines in the
   captured order;
3. allocate the model buffer (10,657,152 B), the scratch buffer (resolution-dependent; 58.4 MB
   at 1280x720, 232.9 MB at 7680x4320) and the uniform arena (3,276,800 B);
4. upload `model_blob[72:72+10,657,152]` to the model buffer during the first prepare;
5. for each frame, write the 80-byte constants for `pass0/152/190` and the five prepare
   constant blocks at the captured arena offsets, and write the 92-entry tensor table to the
   captured table offsets (the table contents depend only on the resolution);
6. record the five prepare dispatches (`9008`, `4204`, `5004`, `4204`, `4204`) with the captured
   groups and image bindings;
7. record `pass0` … `pass190` in ascending order with the captured groups and descriptor
   offsets;
8. submit and wait.

The pass order, geometry, bindings, table and upload are now data. What is still code is the
preprocess image wiring (which captured image view feeds which `main` binding) and the
composite semantics inside `pass151/152/190`; both are in the extracted modules, but the
capture only proves the binding numbers and the data, not a validated end-to-end image.

## 12. Verified vs hypothesised

| claim | state |
| --- | --- |
| Wine 11.22 with RADV runs the signed DLL and reaches the MLFI provider | **verified** (versions query, context creation, 196 dispatches per frame, destroy) |
| the ML provider is selected by version id `f600000001000002` | **verified** (query result and context creation) |
| `ffxCreateContext` creates 195 pipelines: 191 model + 4 preprocess `main` | **verified** (pipeline log, 195 module dumps) |
| runtime modules are byte-identical to the extracted set | **verified** 195/195 for 4K and 8K (SHA-256) |
| prepare is 5 dispatches; frame generation is pass0…190 in ascending order | **verified** (1- and 2-frame captures) |
| per-pass dispatch geometry is captured | **verified** for 1280x720, 1920x1080, 3840x2160, 5120x2880, 7680x4320 (TSVs) |
| the model upload is `model_blob[72:72+10,657,152]`, during the first PrepareV2 | **verified** (byte-exact SHA-256 match) |
| the whole model buffer is bound at binding 0 offset 0 for 78 passes | **verified** (descriptor capture) |
| there is no host-side per-pass weight-offset table | **verified** (all binding-0 descriptors are offset 0/WHOLE_SIZE; per-pass values are shader constants) |
| `CsTensorSizes` is one 92-entry table shared by the model passes that bind it, and across frames | **verified** (1 unique table for 175 bindings × 2 frames) |
| the tensor table is resolution-derived with per-family alignment | **verified** for five resolutions; the exact rule for arbitrary sizes is **not** fully pinned (1920x1080 `/4` = 480×272) |
| 4K vs 8K module selection | **verified** only at 3840x2160 (4K) and 5120x2880/7680x4320 (8K); the exact threshold is **not** pinned |
| the 63,088-byte bank tail is CPU-side metadata | **hypothesised** (not uploaded; no 4K constant in the heuristic model-range addresses it, but constants above the upload size were not interpreted) |
| the 5,004-byte `main` is the splat prepass | **hypothesised** (size and known storage-image atomics; it is substituted) |
| a native provider can produce a correct frame today | **not demonstrated**; four modules crash RADV and the composite was substituted |
| the output image is valid | **no**; `canonical-4k-readback` reads back 0/921,600 nonzero pixels because `pass190` is a no-op substitute |

`gameplay_verified` remains false for MLFI.

## 13. Blockers and the next concrete step

The host layout is no longer the blocker. The blockers are:

1. **RADV compile crash.** `vkCreateComputePipelines` segfaults on `pass151`, `pass152`,
   `pass190` and the 5,004-byte `main` (the same defect recorded in `docs/FSR4-MLFI.md`).
   A minimal reproducer is `pass151` (2,648 B, `19f393a5…`): create a compute pipeline with the
   module's own descriptor layout and the feature set in §1. The harness can reproduce it with
   `--no-substitute`; the error is `Exception 0xc0000005 in Unix call`.
2. **Preprocess wiring.** The five prepare dispatches' image bindings and constants are
   captured, but the semantic mapping to `r_input_rgb0/1`, depth/MV and the packed splatted
   outputs is inferred. The `main` modules' `OpName`/decorations and the captured image-view
   handles should be matched next.
3. **Output validation.** With `pass190` substituted there is no image to compare. Once the
   four modules compile, run the vendor DLL under Wine without substitution, capture
   `outputs[0]`, then compare the native replay. Until then, no parity claim.

The concrete next step is therefore a Mesa/RADV bug report with the `pass151` reproducer, not
another host capture. In parallel, the replay provider can be written against the captured
layout and validated on the 188 model passes that do compile, using intermediate-buffer
comparisons against the vendor DLL (which is the only reference implementation).

## 14. Reproduction

Build and run:

```sh
cd /path/to/MK8-online-linux
FFX_DLL=$HOME/.cache/mk8online/fg-sdk/Kits/FidelityFX/signedbin/amd_fidelityfx_framegeneration_vk.dll \
  tools/mlfi-capture/run.sh "$HOME/.cache/mk8online/mlfi-capture/canonical-4k-1f" \
  --width 1280 --height 720 --frames 1

FFX_DLL=... tools/mlfi-capture/run.sh "$HOME/.cache/mk8online/mlfi-capture/canonical-8k-7680" \
  --width 7680 --height 4320 --frames 1
```

Analyse:

```sh
python3 tools/mlfi-capture/analyze_capture.py \
  --capture "$HOME/.cache/mk8online/mlfi-capture/canonical-4k-1f"
python3 tools/mlfi-capture/analyze_capture.py \
  --capture "$HOME/.cache/mk8online/mlfi-capture/canonical-8k-7680" \
  --derived tools/mlfi-capture/derived
```

Reproduce the crash:

```sh
FFX_DLL=... tools/mlfi-capture/run.sh "$HOME/.cache/mk8online/mlfi-capture/no-substitute" \
  --width 1280 --height 720 --frames 1 --no-substitute
# log.txt ends at mlfi_4k_pass151
```

To see the Wine Vulkan exception line, run the copied executable directly:

```sh
cd "$HOME/.cache/mk8online/mlfi-capture/no-substitute"
WINEPREFIX="$HOME/.cache/mk8online/mlfi-wine" WINEDEBUG=+seh \
  wine ./mlfi_capture.exe --dll amd_fidelityfx_framegeneration_vk.dll --out . \
  --width 1280 --height 720 --frames 1 --no-substitute
# ... err:vulkan:vkCreateComputePipelines Exception 0xc0000005 in Unix call.
```

Capture directories used for this document (all under `$HOME/.cache/mk8online/mlfi-capture/`):

| directory | content |
| --- | --- |
| `canonical-4k-1f` | 1280x720, 1 frame, primary 4K evidence, module dumps |
| `canonical-4k` | 1280x720, 2 frames, frame-repeat stability |
| `canonical-4k-readback` | 1280x720, 1 frame, output readback (`0/921,600` nonzero pixels) |
| `canonical-1080` | 1920x1080, table scaling data point |
| `canonical-4k3840` | 3840x2160, 4K class upper end |
| `canonical-8k` | 5120x2880, first 8K-class data point |
| `canonical-8k-7680` | 7680x4320, primary 8K evidence |
| `module-check-4k`, `module-check-8k` | module byte-identity checks |
| `no-substitute` | the RADV crash reproduction |

Each directory contains `dll.sha256`, `log.txt`, `events.tsv`, `passes.tsv`, `tensors.tsv`,
`tensor_*.bin`, `layout.json` and `raw/`. `raw/` contains the DLL, shader modules, model upload
and buffer snapshots; none of it is committed.

## 15. Data handling and licence

The DLL and all bytes derived from it remain on the user's machine under
`$HOME/.cache/mk8online/mlfi-capture/`. This repository carries only:

- the capture/analysis tools (`tools/mlfi-capture/`);
- derived pass names, sizes, offsets, dimensions and hashes
  (`tools/mlfi-capture/derived/`), which do not contain shader or model bytes;
- this report.

The FSR SDK 2.4.0 licence MIT-lists `amd_fidelityfx_framegeneration_vk.dll` and permits
extraction/derived work for that file; the base SDK licence still forbids reverse engineering
the rest of the SDK. Nothing was downloaded and no AMD content is redistributed.
