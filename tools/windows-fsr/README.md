# Windows FSR probes (mingw-w64)

Cross-compiles the FidelityFX upscaler and frame-generation probes for native Windows x86-64 and runs them
against AMD's signed Vulkan DLLs from the FSR SDK 2.4.0 archive. Nothing from AMD is stored here: point the
probes at the user's own copies.

This is the native-Windows check of the path the launcher takes (`amd_fidelityfx_upscaler_vk.dll` /
`amd_fidelityfx_framegeneration_vk.dll` through the FFX API), complementing the Wine runs in
`scripts/verify-windows-fsr.sh`.

## Build

```sh
FFX_SDK=$HOME/.cache/mk8online/fsr-build/sdk-copy/Kits/FidelityFX \
  tools/windows-fsr/build.sh
```

Outputs (default `$HOME/.cache/mk8online/win-rtx/build`):

| file | source |
| --- | --- |
| `test_query.exe` | `tools/fg-linux-upscaler/test_query.cpp` |
| `test_context.exe` | `tools/fg-linux-upscaler/test_context.cpp` |
| `mlfi_capture.exe` | `tools/mlfi-capture/mlfi_capture.c` |

The upscaler sources are shared with the Linux tests. `tools/fg-linux-upscaler/platform.h` selects
`LoadLibraryA`/`GetProcAddress` on Windows and `dlopen`/`dlsym` elsewhere; `build.sh` generates the
`vulkan-1` import library mingw lacks.

The binaries are statically linked (`-static`); they still need the Vulkan loader (`vulkan-1.dll`) and the
MSVC runtime the signed DLLs import (`MSVCP140`, `VCRUNTIME140`, `VCRUNTIME140_1`). Those ship with Windows
and the NVIDIA driver.

## Run

Copy the two signed DLLs and the probes to the target machine. From that directory:

```bat
test_query.exe amd_fidelityfx_upscaler_vk.dll
test_context.exe amd_fidelityfx_upscaler_vk.dll 1280 720 2560 1440 --provider 3.1.6 --out upscale-316-1280x720
test_context.exe amd_fidelityfx_upscaler_vk.dll 1920 1080 2560 1440 --provider 3.1.6 --out upscale-316-1920x1080

mlfi_capture.exe --dll amd_fidelityfx_framegeneration_vk.dll --out mlfi-402 ^
    --width 1280 --height 720 --frames 3 --no-substitute
```

`--provider NAME` mirrors `runtime/mk8/graphics/fsr.cpp`: query the provider ids, then select with
`ffxOverrideVersion`. `--out PREFIX` writes `PREFIX.rgba` and `PREFIX.ppm`; convert the PPM to PNG for
inspection. `--override-id HEX` forces a version id the provider did not advertise, to capture the exact
rejection.

`mlfi_capture.exe` selects 4.0.2 by default. `--version-name 3.1.6` selects the analytic FSR 3 FG provider;
`--version-id HEX` forces an unadvertised id. On a non-AMD GPU `--no-substitute` is the real path (the
substitution exists only for the RADV crash).

## Results (RTX 5080, driver 621.01, Vulkan 1.4.357, 20/11/2026)

- Upscaler: `ffxQuery` returns `3.1.6` (`0xf5a5ca1e00c01006`) and `2.3.4` (`0xf5a5ca1e00803004`).
  `4.1.2` is **not offered** on this GPU. Both offered providers create, dispatch and destroy at
  1280x720 -> 2560x1440 and 1920x1080 -> 2560x1440 with non-zero varied output.
- Forcing the FSR 4.1.2 id `0xf5a5ca1e01001002` (present in the DLL) returns `NO_PROVIDER` (rc 4);
  no diagnostic message is emitted even with `ffxConfigure(GLOBALDEBUG)` verbose.
- Frame generation: the DLL offers only `3.1.6` (`0xf600000000c01006`); `4.0.2` (MLFI) is not offered.
  FSR 3.1.6 FG at 1280x720 runs three frames: 18 pipelines, 97 dispatches (3 prepare + 94 framegen),
  0 substituted, `ffxCreateContext=0`, all `configure/prepare/framegen/submit=0`, no device loss.
- Forcing the MLFI 4.0.2 id `0xf600000001000002` returns `ffxCreateContext=4` (`NO_PROVIDER`), context
  null, 0 pipelines and 0 dispatches.
- AMD's SDK documentation says the version list "depend[s] on the installed driver", and the MLFI
  technique documentation requires RX 9000 or later. The omission is therefore the expected
  driver/device gate, not an API or probe failure.

Keep raw captures outside the repository: `mlfi_capture` writes AMD shader and model bytes under `raw/`.
