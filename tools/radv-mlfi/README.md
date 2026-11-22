# radv-mlfi: reproduce and work around the RADV MLFI pipeline crash

Small, self-contained tools for the defect described in `docs/RADV-MLFI-CRASH.md`: RADV GFX1201
segfaults in `vkCreateComputePipelines` when Mesa's SPIR-V front end rejects a module, and four of
AMD's FSR 4 MLFI modules trigger that path. Nothing here contains AMD bytes: the modules are read
from the user's own extraction (`tools/extract_mlfi.py`).

## Files

| file | purpose |
| --- | --- |
| `repro.c` | creates a compute pipeline for one `.spv`, prints the module's image constructs, optionally dispatches it |
| `build.sh` | `cc -O2 … -lvulkan` wrapper (`vulkan-headers` only) |
| `fix_image_types.py` | rewrites the invalid 16-bit image sampled types (and the 64-bit sampled image) into legal, equivalent SPIR-V |
| `cases/*.spvasm` | five hand-written minimal modules that isolate the constructs (three crash, two are controls) |
| `selftest.sh` | builds, assembles the cases, checks the controls and the fixer, and reports whether the raw cases still crash the driver |

## Build

```sh
tools/radv-mlfi/build.sh
```

## Self-test

```sh
nice -n 19 tools/radv-mlfi/selftest.sh
```

It expects the controls to compile, the fixed modules to validate and compile, and each raw crash
case to either segfault (`KNOWN-BUG`, the current RADV) or be rejected cleanly (`FIXED`). No AMD
module is needed.

## Crash cases

```sh
spirv-as --target-env vulkan1.2 tools/radv-mlfi/cases/sampled64_unknown.spvasm -o /tmp/s64.spv
nice -n 19 tools/radv-mlfi/repro --module /tmp/s64.spv --debug-report   # SIGSEGV
nice -n 19 tools/radv-mlfi/repro --device 1 --module /tmp/s64.spv      # lavapipe: VkResult=-13
```

`--device 1` selects the second Vulkan physical device (lavapipe when it is installed), which is the
control: same Mesa commit, same loader, clean `VK_ERROR_UNKNOWN` instead of a crash.

Options:

| option | meaning |
| --- | --- |
| `--module PATH` | `.spv` to compile (positional path also accepted) |
| `--entry NAME` | override the entry point (default: from `OpEntryPoint`) |
| `--device N` | physical device index |
| `--empty-layout` | create the pipeline with an empty descriptor set layout |
| `--no-cm`, `--no-cm2`, `--no-f8`, `--no-i64img`, `--no-f16` | drop one feature at device creation (attribution runs) |
| `--debug-report` | install a `VK_EXT_debug_report` callback so RADV's SPIR-V diagnostic is visible |
| `--dispatch GX GY` | dispatch after creating the pipeline; allocates bindings from the module's own decorations and prints per-image readback hashes |
| `--clear-hi` | clear sampled/storage images to 0.5 (1 for integer formats) so a write is visible in the hash |
| `--pass151` | the `mlfi_4k_pass151` self-test: fills `ScratchBuffer` with a deterministic pattern, dispatches 20×12 groups and checks every pixel of both RG16F outputs against the shader's arithmetic |

## Workaround

```sh
python3 tools/radv-mlfi/fix_image_types.py --in pass151.spv --out pass151.fixed.spv
python3 tools/radv-mlfi/fix_image_types.py --in 0389_main.spv --out main.fixed.spv --i64-sampled-to-storage
```

The first form rewrites `OpTypeImage %half …` to a 32-bit sampled type and inserts the matching
`OpFConvert`s (exact for 16-bit float views). `--i64-sampled-to-storage` turns a 64-bit sampled
image (`%ulong … Unknown`, `OpImageFetch`) into an `R64ui` storage image read with `OpImageRead`
(the view must then be created with `STORAGE` usage and format `VK_FORMAT_R64_UINT`).

Both rewrites preserve ids where possible, merge duplicate type declarations and re-assemble with
`spirv-as`; run `spirv-val --target-env vulkan1.2` afterwards to confirm the result.

## Notes

* `repro` only creates the pipeline unless a dispatch mode is given; the crash happens inside
  `vkCreateComputePipelines`, before any resource is needed.
* The harness never reads AMD's DLL. Point it at the modules extracted by `tools/extract_mlfi.py`.
* Keep GPU runs short (`nice -n 19`, one module per process) on a loaded host.
