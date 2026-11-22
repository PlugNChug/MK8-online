# Reference tracer (Wine + AMD signed DLL)

`fsr4_trace.c` runs AMD's signed FSR 4.1.2 Vulkan DLL under Wine and records the
information the native port needs. It is a development harness; it is not part of
the shipped library and the AMD DLLs are never copied into the repository.

## What it captures

- `vkCreateShaderModule` code hashes (match the extracted SPIR-V by SHA-256)
- pipeline -> shader mapping and every `vkCmdDispatch` group count
- descriptor set layouts and the descriptor writes the backend performs
- created images (format/extent/usage) and buffers (size/usage)
- the 272-byte `CsTensorSizes` and 104-byte `MLSR_Optimized_Constants` contents
  (found by scanning host-visible mapped memory at dispatch time; also captured
  from `vkCmdUpdateBuffer` if the backend uses it)
- deterministic input images and the upscaled output as raw files

## Build

```sh
sh build.sh
```

Requires `x86_64-w64-mingw32-g++` and the Khronos Vulkan headers in
`/usr/include/vulkan` (they are copied into an isolated include root so mingw
never sees the host libc headers). The signed DLLs are taken from
`$FSR4_SDK_ROOT/Kits/FidelityFX/signedbin`.

## Run

```sh
cd ../../build/tests/reference
WINEPREFIX=/path/to/wine-prefix WINEDEBUG=-all wine fsr4_trace.exe [RW RH OW OH]
```

Defaults to 960x540 -> 1920x1080. Configuration knobs (environment):

| variable | meaning |
|---|---|
| `FSR4_CREATE_FLAGS` | `ffxCreateContextDescUpscale.flags` (0x21 = HDR + AUTO_EXPOSURE) |
| `FSR4_DISPATCH_FLAGS` | per-dispatch flags |
| `FSR4_SHARPEN` | `enableSharpening` (0/1) |
| `FSR4_SHARPNESS` | sharpness value (default 0.9) |
| `FSR4_CTX_MAX_R` | context `maxRenderSize` as `WxH` (default render size) |
| `FSR4_CTX_MAX_U` | context `maxUpscaleSize` as `WxH` (default output size) |
| `FSR4_TRACE_DUMP_MAPS` | dump host-visible mappings at the first dispatch |
| `FSR4_TRACE_DUMP_MAPS_ALL` | dump mappings at every dispatch (last state wins) |

Outputs in the working directory:

| file | contents |
|---|---|
| `trace.txt` | full trace |
| `in_color.rgba8` | input color, RGBA8 |
| `in_depth.f32` | input depth, float32 |
| `in_mv.rg16f` | input motion vectors, RG16F (zeros) |
| `out_color.rgba8` | reference upscale result |
| `tensor.bin` | 17 x uvec4 `CsTensorSizes` |
| `constants.bin` | 104-byte optimized constants |
| `spd.bin` | 32-byte auto-exposure SPD constants (when AE is on) |
| `rcas.bin` | 32-byte RCAS constants (when sharpening is on) |

The native runner must upload the exact `in_*.raw` bytes and aim to reproduce
`out_color.rgba8`.
