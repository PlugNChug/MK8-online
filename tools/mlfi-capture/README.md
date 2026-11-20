# MLFI capture harness

Runs AMD's signed `amd_fidelityfx_framegeneration_vk.dll` (FSR SDK 2.4.0) under Wine against
the host Vulkan device and records the host-side ML frame-interpolation layout: pipeline
creation, pass order, dispatch geometry, descriptor bindings, the `CsTensorSizes` table and the
model-bank upload.

This is a measurement tool. It does not contain or redistribute AMD code or data. The DLL is
loaded from the user's own SDK download.

## Requirements

- Wine 11.x with a Vulkan-capable prefix (tested with Wine 11.22 and Wine's `vulkan-1.dll`
  forwarding to the host loader).
- mingw-w64 (`x86_64-w64-mingw32-gcc`).
- Vulkan headers (`/usr/include/vulkan`, `/usr/include/vk_video`).
- FidelityFX SDK 2.4.0 headers (`Kits/FidelityFX/api/include` and
  `Kits/FidelityFX/framegeneration/include`).

## Build

```sh
./build.sh
```

`build.sh` copies the platform-neutral Vulkan headers into a private include root (using
`/usr/include` directly with mingw picks up the host `stdint.h`), then links the harness.

Environment overrides:

| variable | default |
| --- | --- |
| `FFX_SDK` | `$HOME/.cache/mk8online/fg-sdk/Kits/FidelityFX` |
| `VULKAN_HEADERS` | `/usr/include` |
| `MLFI_CAPTURE_BUILD` | `$HOME/.cache/mk8online/mlfi-capture/build` |

## Run

```sh
FFX_DLL=/path/to/amd_fidelityfx_framegeneration_vk.dll \
  ./run.sh "$HOME/.cache/mk8online/mlfi-capture/run" \
  --width 1280 --height 720 --frames 2
```

`run.sh` creates the output directory, copies the DLL and the harness there, records the DLL
SHA-256, runs Wine from that directory and leaves every capture file there. Keep this directory
outside the repository: it contains AMD bytes.

Options:

| option | meaning |
| --- | --- |
| `--width N --height N` | display/render size; `<= 3840x2160` selects the `mlfi_4k` module set, above that `mlfi_8k` (tested at 5120x2880 and 7680x4320) |
| `--frames N` | frames to record (default 2) |
| `--readback` | copy the generated output image back and hash it; the value is not meaningful while composite passes are substituted |
| `--no-substitute` | do not replace the modules that crash RADV; reproduces the driver crash |

### RADV crash workaround

RADV on the tested host (GFX1201, Mesa 26.99.99, Vulkan 1.4.363) segfaults in
`vkCreateComputePipelines` for four modules per context: `mlfi_4k_pass151`, `_pass152`,
`_pass190` (or the `mlfi_8k` equivalents) and the 5,004-byte preprocess `main`. The harness
creates a no-op compute pipeline with the same pipeline layout for those modules so that the
rest of the host layout can be captured. The substitution is logged and appears in the derived
tables. It is a measurement workaround, not a fix and not a claim that the generated frame is
correct.

## Output

| file | contents |
| --- | --- |
| `log.txt` | human-readable API/pipeline/dispatch/buffer trace |
| `events.tsv` | machine-readable events (`module`, `pipeline`, `bind`, `dispatch`, `passdesc`, `constants`, `tensor`, `updatebuf`, `copybuf`, `memsnap`, ...) |
| `passes.tsv` | one row per recorded dispatch: stage, entry, pipeline, substituted flag, groups, binding offsets, tensor-table id |
| `tensors.tsv` + `tensor_NNN.bin` | unique `CsTensorSizes` tables (92 `uint4` each) |
| `raw/` | module bytes, buffer writes, model upload, memory snapshots (AMD content; never commit) |
| `layout.json` | produced by `analyze_capture.py` |

## Analyse

```sh
python3 analyze_capture.py --capture "$HOME/.cache/mk8online/mlfi-capture/run"
python3 analyze_capture.py --capture DIR --derived DIR
```

The analyser verifies that the runtime shader bytes are byte-identical to the extracted set
(when `raw/module_*.bin` exists), locates the model-bank upload inside the extracted model,
and writes the compact layout JSON. `--derived` writes the pass and tensor tables that are
committed under `derived/`.

## Interception

The DLL imports part of Vulkan statically and resolves the rest through the
`VkDeviceContext.vkDeviceProcAddr` passed in `ffxCreateBackendVKDesc`. The harness covers both:

- the DLL's import address table is patched after `LoadLibraryA` (buffer/memory/shader module,
  descriptor updates, command-buffer recording, queue submit, `vkGetDeviceProcAddr`);
- `VkDeviceContext.vkDeviceProcAddr` is a wrapper that returns wrappers for the compute and
  dispatch entry points and the driver pointer otherwise.

Buffer contents are recovered from `vkCmdUpdateBuffer` (data copied directly), `vkCmdCopyBuffer`
(source read from a live mapping or from a snapshot taken at `vkUnmapMemory`) and a shadow copy
of each buffer written by those commands. This is what recovers the model upload and the
per-dispatch `CsTensorSizes` slice.
