# mlfi-provider: FFX entry points for AMD's FSR 4 ML frame interpolation

A native Linux shared library that exports `ffxCreateContext`, `ffxDestroyContext`, `ffxQuery`,
`ffxConfigure` and `ffxDispatch` for the **MLFI 4.0.2** provider
(`0xF600000001000002`) and runs the pass graph recovered from the signed DLL inside the
application's `VkCommandBuffer`. It is the provider counterpart of `tools/mlfi-replay` and the
first half of `MK8_FSR_FG=fsr4`; the swapchain provider is not part of this library.

`docs/en/reports/MLFI-PROVIDER.md` has the exact ABI/descriptor mapping, what is pinned vs inferred, the run
evidence and the precise `runtime/mk8/graphics/fsr_fg.cpp` integration steps.

## Requirements

- a Vulkan 1.3 device with the feature set the captured modules need (cooperative matrix +
  maintenance1, float8 cooperative matrices, 64-bit image atomics, the Vulkan memory model,
  variable pointers, subgroup-size control, formatless storage-image read/write; the tested host
  is RADV GFX1201 / RX 9070 XT);
- a C compiler and the Vulkan headers/loader;
- python3 and `spirv-dis`/`spirv-as` for `tools/mlfi-replay/prepare.py`;
- the user's own extraction of the signed DLL (`tools/extract_mlfi.py`), legalised with
  `tools/mlfi-replay/prepare.py`. Nothing AMD-owned is stored here.

## Build

```sh
tools/mlfi-provider/build.sh
# -> $XDG_CACHE_HOME/mk8online/mlfi-provider/build/libmk8_mlfi_framegeneration_vk.so
#    $XDG_CACHE_HOME/mk8online/mlfi-provider/build/mlfi_provider_test
```

The build generates `mlfi_graph.h` from the committed capture tables
(`tools/mlfi-capture/derived/`) and compiles `mlfi_provider.c`, which includes the committed
`mlfi_shaders.h` (the SPIR-V of our four shaders; regenerate with
`python3 tools/mlfi-provider/build_shaders.py`).

## Run

```sh
MK8_MLFI_MODULES="$HOME/.cache/mk8online/mlfi-replay/modules" \
MK8_MLFI_MODEL="$HOME/.cache/mk8online/mlfi/model/model_blob.bin" \
nice -n 19 "$HOME/.cache/mk8online/mlfi-provider/build/mlfi_provider_test"
```

Options: `--motion PX` (constant horizontal motion vector in pixels per frame), `--d32-depth`
(the neutral-depth image format), `--verbose`. The program creates a Vulkan device, creates the
provider context at 1280×720, records two frames (`PrepareV2` + `FrameGeneration`) into one
command buffer, submits, reads back `outputs[0]` and prints one `MLFI-PROVIDER frame` line per
frame plus a `MLFI-PROVIDER RESULT: OK|FAIL` summary.

## Files

| file | purpose |
| --- | --- |
| `mlfi_provider.c` | provider implementation |
| `mlfi_provider_ext.h` | `mk8MlfiGetStats`, a diagnostic export with the measured dispatch counters |
| `mlfi_provider_test.c` | headless validation program |
| `shaders/` | our input-copy and composition shaders (not AMD's) |
| `mlfi_shaders.h` | committed SPIR-V of those shaders |
| `build_shaders.py`, `gen_graph_header.py`, `build.sh` | build steps |

## Scope and limits

1280×720 only; 8-bit UNORM inputs/outputs; one frame in flight; no swapchain replacement; no HDR;
the final `rw_prediction` → `outputs[0]` composition is ours because the vendor's final `main` was
never dispatched in the capture. `gameplay_verified` remains false. See
`docs/en/reports/MLFI-PROVIDER.md` §8 and §11.
