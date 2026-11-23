# RADV GFX1201 segfaults compiling AMD's MLFI composite passes: root cause and workaround

Status 20/11/2026. Companion to `docs/FSR4-MLFI.md` §6 and `docs/MLFI-CAPTURE.md` §4.1/§13, which
record that four modules of AMD's FSR 4 MLFI provider kill RADV inside
`vkCreateComputePipelines` and that a native provider therefore cannot be finished. This document
identifies the defect, separates the two different constructs behind it, and gives a SPIR-V rewrite
that makes all 195 pipelines of a 4K (and 8K) MLFI context compile and run.

Short answer: **the crash is a RADV error-handling bug, not a shader-compiler limitation.** Mesa's
SPIR-V front end rejects the modules with a diagnostic and returns `NULL`; RADV dereferences that
`NULL` at `src/amd/vulkan/radv_shader.c:549` and segfaults instead of failing pipeline creation with
`VK_ERROR_UNKNOWN`. The same modules, on the same Mesa commit, make lavapipe return
`VK_ERROR_UNKNOWN` (-13) and nothing worse. Two different constructs cause the front-end rejection:
a 16-bit image *sampled type* in `pass151/152/190` (invalid SPIR-V: AMD's compiler bug), and a
64-bit integer sampled type in the 5,004-byte preprocess `main` (valid SPIR-V, but Mesa cannot
represent a 64-bit integer texture). Both can be rewritten into equivalent legal modules, and the
rewritten `pass151` reproduces the shader's arithmetic exactly on hardware.

## 1. Environment

| item | value |
| --- | --- |
| GPU | AMD Radeon RX 9070 XT, `deviceID 0x7550`, RADV GFX1201 |
| Vulkan | loader 1.4.363, device API 1.4.363 |
| driver | `mesa-git 27.0.0_devel.232438.f7dd61f78f8-1`, commit `f7dd61f78f83c9ffb3dc9c980cd9b303a5ce58b1` (2026-10-26), `driverVersion 0x06863063` (109457507) |
| kernel | 7.4.0-rc1-2-cachyos-rc |
| spirv-tools | 1.4.363 |
| CPU driver for contrast | llvmpipe/lavapipe, same Mesa commit, same loader |

The debug backtraces below come from a local build of the *same* Mesa commit
(`-Dbuildtype=debug -Dvulkan-drivers=amd`, selected with `VK_ICD_FILENAMES`); the crash also
reproduces with the distribution's release build of `libvulkan_radeon.so`.

## 2. The crash and its exact site

`vkCreateComputePipelines` segfaults for `mlfi_4k_pass151`, `mlfi_4k_pass152`, `mlfi_4k_pass190`
(and the 8K copies), and for the 5,004-byte preprocess `main`. With the release driver and a
`VK_EXT_debug_report` callback, the driver explains itself and then dies:

```
$ tools/radv-mlfi/repro --module .../0135_mlfi_4k_pass151.spv --debug-report
device: AMD Radeon RX 9070 XT (RADV GFX1201)
image[0]: sampled_type=float16 dim=2D sampled=2 format=Unknown
bindings: 4
radv-report: SPIR-V offset 1060: SPIR-V parsing FAILED:
    Sampled type of OpTypeImage must be a 32 or 64-bit scalar
    1060 bytes into the SPIR-V binary
timeout: the monitored command dumped core       # shell reports Segmentation fault, $? = 139
```

`MESA_SPIRV_FAIL_DUMP_PATH=<dir>` can be set to dump the offending module at that point. Without a
debug callback the message is not printed at all (Mesa only writes it to `stderr` in `!NDEBUG`
builds), so a release driver gives a silent segfault.

With the debug build of the same commit, gdb shows the instruction that faults:

```
SPIR-V parsing FAILED:
    In file ../src/compiler/spirv/spirv_to_nir.c:2388
    Sampled type of OpTypeImage must be a 32 or 64-bit scalar
Thread 1 received signal SIGSEGV, Segmentation fault.
0x00007ffff64a03e1 in radv_shader_spirv_to_nir (compiler_info=..., stage=..., is_internal=false)
    at ../src/amd/vulkan/radv_shader.c:549
549       nir->info.internal |= is_internal;
```

`spirv_to_nir()` returned `NULL` and the return value is used before any check. The surrounding
code, unchanged on Mesa `main` as of 20/11/2026:

```c
      nir = spirv_to_nir(spirv, stage->spirv.size / 4, spec, stage->stage, stage->entrypoint,
                         &spirv_options, &compiler_info->nir_options[stage->stage]);
      nir->info.internal |= is_internal;      /* radv_shader.c:549, nir == NULL here */
      assert(nir->info.stage == stage->stage);
```

The same unchecked result is used by three callers — `radv_compile_cs`
(`radv_pipeline_compute.c:96`, which immediately runs `NIR_PASS(_, cs_stage->nir, …)`), the graphics
path (`radv_pipeline_graphics.c:2460`) and the ray tracing path (`radv_pipeline_rt.c:660`) — so any
SPIR-V parse failure crashes RADV for every shader stage, not just compute.

The correct behaviour is shown by lavapipe on the same Mesa commit and the same loader:

```
$ tools/radv-mlfi/repro --device 1 --module .../sampled64_unknown.spv
device: llvmpipe (LLVM 23.1.4, 256 bits)
pipeline: FAIL VkResult=-13 (VK_ERROR_UNKNOWN (shader rejected))
```

## 3. Two distinct constructs make the front end fail

### 3.1 `pass151/152/190`: a 16-bit image sampled type (invalid SPIR-V)

`mlfi_4k_pass151` (2,648 B) declares

```
%type_2d_image = OpTypeImage %half 2D 2 0 0 2 Unknown
...
OpImageWrite %104 %50 %103 None          ; v2half into an R16G16_SFLOAT view
```

and `pass152/190` declare the same sampled type for a *sampled* image (bindings 3/4, the
`rw_output_flow_*` buffers) whose `OpImageSampleExplicitLod` result is `v4half`. `spirv-val
--target-env vulkan1.2` rejects both:

```
error: line 59: [VUID-StandaloneSpirv-OpTypeImage-04656] Expected Sampled Type to be a 32-bit int,
64-bit int or 32-bit float scalar type for Vulkan environment
```

so the modules are not valid Vulkan SPIR-V. The Vulkan-correct spelling of the same operation is a
32-bit sampled type plus the 16-bit *format* (`%float … Rg16f`, `OpImageWrite` of `v2float`) — which
is what glslang emits for `layout(rg16f) uniform image2D`, and which compiles on the same driver
(case `storage16_rg16f`). The NIR front end reports the invalid form as `spirv_to_nir.c:2388`,
"Sampled type of OpTypeImage must be a 32 or 64-bit scalar".

The extractor's `--validate-spirv` (which produced the "326 of 416 accepted" figure in
FSR4-MLFI.md §2) runs `spirv-val` **without** `--target-env vulkan1.x`, so it does not apply the
Vulkan VUIDs; that is why `pass151` was counted as valid there and fails the Vulkan-environment
validator here.

The 16-bit type is the *only* common factor of the six composite modules. `pass152` binds **no
storage image at all** (10 bindings: 6 sampled images, 1 sampler, 2 UBOs, 1 SSBO), so the earlier
description of the failing set as "the modules that bind storage images" (FSR4-MLFI.md §6) is a
misattribution; the storage write in `pass151` compiles fine once the sampled type is 32-bit
(the `storage16_rg16f` case is exactly that spelling and passes).

### 3.2 the 5,004-byte `main`: a 64-bit integer sampled type (valid SPIR-V)

`0389_main` (the prepare/splat pass, binding 2 `r_packed_splatted1to0`) declares

```
%type_2d_image_0 = OpTypeImage %ulong 2D 2 0 0 1 Unknown
%168 = OpImageFetch %v4ulong %167 %64 Lod %uint_0
```

This module **passes** `spirv-val --target-env vulkan1.2` (VUID 04656 explicitly allows a 64-bit
integer sampled type). Mesa fails it anyway, at `spirv_to_nir.c:1519`:

```
SPIR-V parsing FAILED:
    glsl_type_is_texture(type->glsl_image)
```

Mesa's `glsl_type` has no 64-bit integer texture type, so `vtn_handle_image` rejects the module.
Whether RADV should implement 64-bit integer sampled images is a separate question; the point for
this report is that the module is valid and its rejection must not crash the driver.

### 3.3 Minimal reproducers committed with this report

`tools/radv-mlfi/cases/` contains five hand-written modules (no AMD bytes) that isolate the
constructs. `repro` prints the construct summary and then creates the pipeline.

| case | construct | `spirv-val` vulkan1.2 | RADV release | lavapipe |
| --- | --- | --- | --- | --- |
| `storage16_unknown` | `%half` storage image, Unknown, `v2half` store | **VUID-04656** | **SIGSEGV (139)** | `VK_ERROR_UNKNOWN` |
| `sampled16_unknown` | `%half` sampled image, Unknown, `v4half` sample | **VUID-04656** | **SIGSEGV (139)** | `VK_ERROR_UNKNOWN` |
| `sampled64_unknown` | `%ulong` sampled image, Unknown, `v4ulong` fetch | valid | **SIGSEGV (139)** | `VK_ERROR_UNKNOWN` |
| `storage16_rg16f` | `%float` storage image, `Rg16f`, `v2float` store | valid | PASS | PASS |
| `storage64_r64ui` | `%ulong` storage image, `R64ui`, `v4ulong` read | valid | PASS | PASS |

`sampled64_unknown.spvasm` is 38 instructions and is the cleanest crash: valid SPIR-V, no images
bound, no dispatch, no AMD content. Any RADV bug report should use it.

## 4. Workaround: legalise the image types before compiling

`tools/radv-mlfi/fix_image_types.py` rewrites a module into the legal, equivalent form. It works on
`spirv-dis` text and re-assembles with `spirv-as`, so `spirv-tools` is the only dependency.

**16-bit images** (`--in`/`--out`, default): the `OpTypeImage` sampled type becomes 32-bit float in
place (ids are preserved, so pointers and sampled-image types stay valid). At each use:

* `OpImageWrite %img %coord %v` gains `%v32 = OpFConvert %v2float %v` and writes `%v32`;
* `OpImageSample*/OpImageFetch/OpImageRead` keep their old result id: the fetch is re-typed to
  `v4float`/`v2float` and an `OpFConvert` back to the 16-bit type produces the old id, so no
  consumer changes.

The rewrite is exact for a 16-bit float view (R16G16_SFLOAT / R16G16B16A16_SFLOAT): f16 → f32 → f16
is lossless, and the final f32 → f16 conversion the shader asked for is now done by the image store
instead of `OpFConvert`; on this hardware the two agree bit-for-bit (§5). The number of components is
unchanged, which matters because a storage image with `format Unknown` takes its byte width from the
view at bind time — also measured below.

**64-bit sampled images** (`--i64-sampled-to-storage`): a sampled `%ulong … Unknown` image used with
`OpImageFetch` becomes a storage image `%ulong … 2 R64ui` (or `R64i`) read with `OpImageRead`, with
`Int64ImageEXT` + `SPV_EXT_shader_image_int64` added when missing. This is exact for a raw integer
buffer: a fetch at an explicit Lod with no sampler returns the texel, and so does a storage read. The
provider must create that view with `STORAGE` usage and format `R64_UINT`.

Transform sizes (4K set; the 8K modules transform identically):

| module | in | transform | out |
| --- | --- | --- | --- |
| `mlfi_4k_pass151` | 2,648 B | 1 image type, 2 `OpFConvert` | 2,680 B |
| `mlfi_4k_pass152` | 18,440 B | 1 image type, 2 fetches + 2 `OpFConvert`, 3 ids merged | 18,356 B |
| `mlfi_4k_pass190` | 18,264 B | same as `pass152` | 18,180 B |
| `main` (5,004 B) | 5,004 B | 1 sampled→storage image, 1 `OpImageRead` | 4,996 B |

After the rewrite, `spirv-val --target-env vulkan1.2` accepts every module and the release driver
creates pipelines for the **complete captured pipeline sets**:

```
$ tools/radv-mlfi/repro --module fixed/0135_mlfi_4k_pass151.spv        # pipeline: OK
$ cc -O2 -o /tmp/mlfi_probe tools/mlfi_probe.c -lvulkan
$ /tmp/mlfi_probe fixed-all/*mlfi_4k*.spv fixed-all/0386_main.spv fixed-all/0389_main.spv \
      fixed-all/0390_main.spv fixed-all/0392_main.spv
195/195 modules accepted
$ /tmp/mlfi_probe fixed-all/*mlfi_8k*.spv fixed-all/0386_main.spv fixed-all/0389_main.spv \
      fixed-all/0390_main.spv fixed-all/0392_main.spv
195/195 modules accepted
```

(191 model passes + the four preprocess `main` modules the 4K context creates — 9,008, 5,004, 4,204
and 8,980 bytes, per MLFI-CAPTURE.md §4; the 8K set is the same count.)

## 5. Dispatch evidence

`tools/radv-mlfi/repro --pass151` drives the *real* `pass151` shape: it allocates the module's
bindings (128 scratch SSBO, 257 tensor UBO, 129/130 `R16G16_SFLOAT` storage images), fills the
scratch buffer through the address arithmetic taken from the module
(`6150320 + 8·(x+1) + 3856·(y+1)`, read as `uint32`), dispatches the captured 20×12 groups (160×96
threads) and compares every pixel of both images against the exact shader computation
`f16(f32(v) / f32(160 or 90))`:

```
pipeline: OK
dispatch: 20x12 groups
readback image[0] (format=83 256x256): nonzero=14400/65536 fnv1a=4814609ddd3600f3
readback image[1] (format=83 256x256): nonzero=14400/65536 fnv1a=d15e3c60310d0757
device: alive (vkDeviceWaitIdle=0)
pass151 verify: matched=131072 mismatched=0
```

14,400 = 160×90 written pixels per image (the shader's `tensorSizes[70]` guard trims the 160×96
grid), 131,072 = both images × 65,536 pixels compared, all exact. This also proves the
`format Unknown` interpretation: RADV wrote 4 bytes per pixel into an `R16G16_SFLOAT` view from a
`v2float` store, i.e. the rewrite does not silently widen the texel.

For `pass190` and the 5,004-byte `main`, correctness cannot be compared against a reference (the
only capture with a readback is the no-op-substituted one, 0/921,600 nonzero pixels), so the claim
is limited to execution. The surfaces are cleared to 0.5 (1 for integer formats), the module is
dispatched, and the output image hash is compared with the no-dispatch run:

| module | output binding | no dispatch | 20×12 groups | result |
| --- | --- | --- | --- | --- |
| `pass190` | 129 `rw_prediction` (Rgba32f) | `886fbee78c740383` | `fd727d0a37760383` | written, device alive |
| `main` 5,004 | 128 `rw_output_mv_0_to_1` (Rg16f) | `7cd7573fe29d0383` | `27063ff137a0c383` | written, device alive |

Both dispatch, both change the output, neither loses the device nor trips a validation error.
`pass152` was verified at pipeline level only; it is the same transform as `pass190` and its 10
bindings need no storage images.

## 6. What this does and does not establish

| claim | state |
| --- | --- |
| `vkCreateComputePipelines` segfaults on `pass151/152/190` and the 5,004-byte `main` | **verified** (release driver, 4 modules) |
| the fault is `nir->info.internal` on a `NULL` `spirv_to_nir` result (`radv_shader.c:549`) | **verified** (debug build of the same commit, gdb) |
| lavapipe returns `VK_ERROR_UNKNOWN` for the same modules | **verified** (same Mesa commit, same loader) |
| the four modules fail the NIR front end for two different reasons | **verified** (`spirv_to_nir.c:2388` vs `:1519`) |
| `pass151/152/190` are invalid SPIR-V (VUID-StandaloneSpirv-OpTypeImage-04656) | **verified** (`spirv-val --target-env vulkan1.2`) |
| the 5,004-byte `main` is valid SPIR-V and still rejected | **verified** (`spirv-val` rc=0, then the `:1519` failure) |
| the rewrite makes all 195 4K and 195 8K pipelines compile | **verified** (release driver, per-module probe) |
| rewritten `pass151` computes the captured arithmetic exactly | **verified** on hardware, 131,072/131,072 pixel checks |
| rewritten `pass190`/`main` dispatch without device loss and write their outputs | **verified** (hash change, `vkDeviceWaitIdle` = 0) |
| rewritten `pass152/190`/`main` produce *correct* images | **not demonstrated** — no vendor reference exists |
| an MLFI frame can now be generated | **not demonstrated** — the provider wiring and the composite semantics are still open |
| `gameplay_verified` | remains **false** for MLFI |

## 7. Upstream bug reports

Two reports; the first is the crash and is the one that matters.

### 7.1 radv: `vkCreateComputePipelines` segfaults when the SPIR-V front end rejects a module

```
radv dereferences a NULL NIR shader when spirv_to_nir() fails, so every SPIR-V
parse error is a SIGSEGV instead of a VkResult.

Mesa 27.0.0-devel (git-f7dd61f78f8) and current main (2026-11-20).
GPU: AMD Radeon RX 9070 XT (RADV GFX1201), kernel 7.4.0-rc1.
Lavapipe on the same commit returns VK_ERROR_UNKNOWN for the same modules.

Reproduce (no AMD content, 40 instructions of SPIR-V):
  spirv-as --target-env vulkan1.2 sampled64_unknown.spvasm -o sampled64_unknown.spv
  repro --module sampled64_unknown.spv
The module is valid (spirv-val --target-env vulkan1.2 accepts it); it contains a
64-bit integer sampled image (OpTypeImage %ulong ... Unknown) fetched with
OpImageFetch %v4ulong.  radv_compile_cs -> radv_shader_spirv_to_nir:

  nir = spirv_to_nir(...);
  nir->info.internal |= is_internal;      /* src/amd/vulkan/radv_shader.c:549 */

spirv_to_nir() fails at src/compiler/spirv/spirv_to_nir.c:1519
("glsl_type_is_texture(type->glsl_image)") and returns NULL.

Expected: vkCreateComputePipelines returns VK_ERROR_UNKNOWN (or
VK_ERROR_INVALID_SHADER_NV) and the parse message reaches the debug report
callback (it already does, right before the crash).
Actual: SIGSEGV in radv_shader_spirv_to_nir.

A second, independent trigger is a 16-bit image sampled type
(OpTypeImage %half ...), which is invalid per VUID-StandaloneSpirv-OpTypeImage-04656
but must still fail cleanly; that is what AMD's MLFI composite passes do.

Suggested fix: check the return value in radv_shader_spirv_to_nir, return NULL,
and propagate it — radv_compile_cs() -> radv_compute_pipeline_compile() already
has a VkResult path (return vk_error(device, VK_ERROR_UNKNOWN)).  The graphics
(radv_pipeline_graphics.c:2460) and RT (radv_pipeline_rt.c:660) callers need the
same treatment; today all three dereference the result unconditionally.
```

### 7.2 nir/spirv: a valid 64-bit integer sampled image is rejected by `vtn_handle_image`

```
spirv_to_nir() fails with "glsl_type_is_texture(type->glsl_image)" for

  %ulong = OpTypeInt 64 0
  %img = OpTypeImage %ulong 2D 0 0 0 1 Unknown
  %v = OpImageFetch %v4ulong %img %coord Lod %uint_0

The module passes spirv-val --target-env vulkan1.2 (VUID 04656 allows a 64-bit
integer sampled type), but glsl_type has no 64-bit integer texture type, so the
front end rejects it.  If 64-bit integer sampled images are not going to be
supported, the rejection should at least say so; currently it is an internal
glsl_type assertion message, and on radv it becomes a segfault (see 7.1).
```

## 8. Reproduction

Build the harness (needs `vulkan-headers` and a C compiler; `spirv-tools` for the fixer):

```sh
tools/radv-mlfi/build.sh
```

Crash, minimal and self-contained:

```sh
spirv-as --target-env vulkan1.2 tools/radv-mlfi/cases/sampled64_unknown.spvasm -o /tmp/s64.spv
nice -n 19 tools/radv-mlfi/repro --module /tmp/s64.spv --debug-report   # SIGSEGV, exit 139
nice -n 19 tools/radv-mlfi/repro --device 1 --module /tmp/s64.spv      # lavapipe: VkResult=-13
```

Crash on the real AMD modules (extraction paths in `docs/FSR4-MLFI.md`):

```sh
M=$HOME/.cache/mk8online/mlfi/modules
nice -n 19 tools/radv-mlfi/repro --module $M/0135_mlfi_4k_pass151.spv --debug-report
```

Fix and verify:

```sh
nice -n 19 tools/radv-mlfi/repro --module $M/0135_mlfi_4k_pass151.spv   # SIGSEGV
tools/radv-mlfi/fix_image_types.py --in $M/0135_mlfi_4k_pass151.spv --out /tmp/p151.spv
nice -n 19 tools/radv-mlfi/repro --module /tmp/p151.spv --pass151       # exact pixel check

tools/radv-mlfi/fix_image_types.py --in $M/0389_main.spv --out /tmp/main.spv --i64-sampled-to-storage
nice -n 19 tools/radv-mlfi/repro --module /tmp/main.spv --dispatch 20 12 --clear-hi
```

The debug-driver backtrace used a local Mesa build at the same commit:

```sh
meson setup build -Dbuildtype=debug -Dvulkan-drivers=amd -Dgallium-drivers=
ninja -C build src/amd/vulkan/libvulkan_radeon.so
VK_ICD_FILENAMES=<build>/radeon_debug_icd.json gdb --args tools/radv-mlfi/repro --module ...
# debug builds raise SIGTRAP in vtn_fail; in gdb: handle SIGTRAP stop noprint nopass,
# run, continue -> the SIGSEGV lands in radv_shader_spirv_to_nir at radv_shader.c:549
```

## 9. Data handling

Nothing from AMD's DLL is committed. The five `cases/*.spvasm` files are hand-written for this
report; the modules under `$HOME/.cache/mk8online/mlfi/` stay where `tools/extract_mlfi.py` put
them, and `fix_image_types.py` reads and writes them outside the repository. The hashes, sizes and
binding numbers in this document are metadata, as in the companion reports.

Mesa source excerpts above are quoted for diagnosis from
`gitlab.freedesktop.org/mesa/mesa` at commit `f7dd61f78f8` (MIT licence); they are not vendored here.
