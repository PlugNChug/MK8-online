# mlfi-replay: native replay of AMD's FSR 4 MLFI pass graph

Standalone Linux/Vulkan program that runs the **complete captured MLFI pass graph** — five
preprocess dispatches and `mlfi_4k_pass0` … `mlfi_4k_pass190` — on real inputs and reads the
generated frame back. It is the first milestone towards a native FSR 4 ML frame-generation
provider; it is **not** a provider and makes no image-quality claim.

The pass order, dispatch geometry, descriptor offsets and the 92-entry tensor table come from the
Wine capture committed under `tools/mlfi-capture/derived/`. The shader modules and the model bank
come from the user's own extraction (`tools/extract_mlfi.py`), legalised for RADV with
`tools/radv-mlfi/fix_image_types.py`. Nothing from AMD is stored in this repository.

## Requirements

- a Vulkan 1.3 device with `VK_KHR_cooperative_matrix`, `VK_EXT_cooperative_matrix_maintenance1`,
  `VK_EXT_shader_float8`, `VK_EXT_shader_image_atomic_int64`, the Vulkan memory model and variable
  pointers (the tested host is RADV GFX1201 / RX 9070 XT);
- `spirv-dis`, `spirv-as` (spirv-tools) for the preparation step;
- a C compiler and the Vulkan headers for the executable;
- the extraction from `tools/extract_mlfi.py` and the capture from `tools/mlfi-capture/`.

## Usage

```sh
# 1. legalise the four modules RADV cannot compile unchanged into the user cache
python3 tools/mlfi-replay/prepare.py

# 2. build
tools/mlfi-replay/build.sh

# 3. run two 1280x720 frames with the validation layer
nice -n 19 "$HOME/.cache/mk8online/mlfi-replay/build/mlfi_replay" \
    --modules "$HOME/.cache/mk8online/mlfi-replay/modules" \
    --model "$HOME/.cache/mk8online/mlfi/model/model_blob.bin" \
    --capture "$HOME/.cache/mk8online/mlfi-capture/canonical-4k-1f" \
    --out "$HOME/.cache/mk8online/mlfi-replay/out" --frames 2
```

Options: `--frames N`, `--static-input` (same input every frame; the reset frame 0 differs, later
frames are identical — a determinism check), `--motion PX` (constant horizontal motion vector in
pixels per frame; 0 by default), `--no-validation`, `--verbose`, `--out DIR`, `--passes`/`--tensors`
to point at another captured resolution.

The program prints one `MLFI-REPLAY frame` line per frame and a final `MLFI-REPLAY summary` /
`MLFI-REPLAY RESULT: OK`. Exit status is non-zero on a device-lost submit, a validation error other
than the documented validator disagreements, or an output that does not change between frames.

## Output

`prediction_fN.png` / `.raw` — the generated frame (`rw_prediction`, RGBA32F, 1280×720); the PNG is a
plain clamp + 1/2.2 preview, not a colour-managed image. `rgb0_fN.png` / `rgb1_fN.png` are the
synthetic inputs, for comparison.

## What is pinned and what is inferred

See `docs/MLFI-REPLAY.md`. In short: the pass order, group counts, buffer offsets, tensor table,
model upload range and descriptor binding *numbers* are the capture's; the image sizes, image
wiring, sampler states, subgroup size and the final `rw_prediction` output are inferred from the
modules and documented there.
