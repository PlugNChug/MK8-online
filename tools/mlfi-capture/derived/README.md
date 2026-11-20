# Derived MLFI layout metadata

These files are produced by `analyze_capture.py` from a capture made with the user's own copy
of AMD's signed frame-generation DLL. They contain pass names, dispatch geometry, buffer
offsets, tensor dimensions and a heuristic list of shader constants. They contain **no shader
bytes and no model bytes**.

Provenance:

| file | capture |
| --- | --- |
| `passes_mlfi_4k_1280x720.tsv`, `tensor_4k_1280x720.tsv` | `$HOME/.cache/mk8online/mlfi-capture/canonical-4k-1f` (1280x720, 1 frame) |
| `passes_mlfi_8k_7680x4320.tsv`, `tensor_8k_7680x4320.tsv` | `$HOME/.cache/mk8online/mlfi-capture/canonical-8k-7680` (7680x4320, 1 frame) |

## `passes_*.tsv`

One row per recorded dispatch, in recording order.

| column | meaning |
| --- | --- |
| `index` | row index (prepare rows first, then frame-generation) |
| `stage` | `prepare` (`ffxDispatch` PrepareV2) or `framegen` (`ffxDispatch` FrameGeneration) |
| `entry` | SPIR-V entry point (`mlfi_4k_passN`, `mlfi_8k_passN`, or `main`) |
| `groups` | `vkCmdDispatch` group counts |
| `substituted` | 1 if the module crashes RADV and a no-op pipeline was used for the capture |
| `weight_offset`, `weight_range` | descriptor binding 0 (`InitializerBuffer`) buffer offset/range; 0/0 when the pass does not bind it |
| `tensor_offset`, `constants_offset` | offsets inside the host's uniform arena for binding 257 (`CsTensorSizes`) and binding 256 (`CsFrameInterpolationConstants`); 0 when absent |
| `tensor_table` | id of the unique tensor table in the capture, or -1 |
| `model_range_constants` | heuristic: 32-bit `OpConstant` values in `[100000, uploaded model size)` from the extracted module, comma-separated |

The `model_range_constants` column is **not** a proven weight-offset table. The host binds the
whole model buffer (binding 0, offset 0, `VK_WHOLE_SIZE`) and supplies no per-pass weight
offsets. The modules contain integer constants; some fall inside the uploaded model size, some
do not (for example `mlfi_8k_pass0` also contains 18,909,072 and 34,632). Their roles (base,
stride, dimension, mask) require per-instruction analysis. A provider that reuses the extracted
modules unchanged does not need to interpret them.

## `tensor_*.tsv`

The 92 `uint4` entries of `CsTensorSizes`, in order. Every captured entry has `z = w = 0`; only
the first two components carry data. The table is identical across all model passes that bind
it and across frames; the 4k and 8k tables differ only in the resolution-derived dimensions.
