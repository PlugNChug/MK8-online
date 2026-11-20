# FSR 3.1.6 upscaler, native Linux from source

Status 20/11/2026. Extends [FSR.md](FSR.md) and [FSR3-4-FG.md](FSR3-4-FG.md).

This is a native Linux `libamd_fidelityfx_upscaler_vk.so` built from the user's own MIT-licensed
FidelityFX SDK 2.4.0 source: the FFX API object, the Vulkan backend, the FSR 3.1.6 upscaler effect, the
SDK's upscaler provider and shader blobs generated from the SDK's HLSL with DXC. It is an optional
host-side component loaded by `runtime/mk8/graphics/fsr.cpp` through `MK8_FSR_LIB`; there is no guest
code, ABI or toolchain change. Nothing from AMD's signed Windows DLLs is used, and no AMD binary,
SPIR-V or model is committed.

This is the FSR **3.1.6 upscaler** path. It is separate from the source-built 3.1.7 frame-generation
provider (`tools/build_fg_provider.py`) and from the GPL 4.1.2 port. The launcher does not offer it yet;
the commands below are the supported diagnostic path.

## Files

- `tools/gen_upscaler_blobs.py` — compiles every FSR 3.1 upscaler pass from the SDK HLSL with DXC,
  reflects the SPIR-V descriptors, rewrites the bindings and storage-image formats, and emits one
  translation unit per module plus the permutation switch the effect calls.
- `tools/fg-linux-upscaler/` — the provider build scaffolding: `CMakeLists.txt`, the provider registry
  (`upscaler_provider_list.cpp`), the Linux compatibility header, `win32compat/` and `miniz/`.
  `test_query.cpp` and `test_context.cpp` are the headless validations.
- `tools/build_upscaler_provider.py` — verifies the pinned SDK archive, applies the explicit portability
  edits, generates the blobs, builds `libamd_fidelityfx_upscaler_vk.so` and optionally runs the self-test.
- `tests/test_fg_upscaler.py` — native tests; skips when the provider or SDK headers are absent.

## Build

From the pinned official SDK archive (253,592,010 bytes, SHA-256
`111f9b19c0c9b8a1f24d2a5104095b8e626f12deb4eca53cd0c29921aa1fd43a`) and DXC (13,297,763 bytes,
SHA-256 `96faadc7f5c282d2ffda49804beb4c3ee38127bc252b723234e3c5cdf7aa39a1`):

```sh
python3 tools/build_upscaler_provider.py \
  --sdk-archive FidelityFX-SDK-v2.4.0.tar.gz \
  --sdk-size 253592010 \
  --sdk-sha256 111f9b19c0c9b8a1f24d2a5104095b8e626f12deb4eca53cd0c29921aa1fd43a \
  --dxc-archive linux_dxc_2026_09_28.x86_x64.tar.gz \
  --dxc-size 13297763 \
  --dxc-sha256 96faadc7f5c282d2ffda49804beb4c3ee38127bc252b723234e3c5cdf7aa39a1 \
  --self-test
```

`--dxc /path/to/dxc` and `--sdk-src <extracted tree>` work when the archives are already unpacked. The
working directory defaults to `~/.cache/mk8online/fsr-upscaler-build`; the provider and AMD's licence
text are installed to `$XDG_DATA_HOME/mk8online/fsr/` (default
`~/.local/share/mk8online/fsr/`), the same directory the launcher uses for the frame-generation provider:

```
libamd_fidelityfx_upscaler_vk.so
LICENSE-AMD-FSR-SDK.md
```

A from-archive build with the pinned hash and `--self-test` completed in this checkout. Two independent
builds (one from the pinned archive, one from the prepared SDK tree) produced the same 573,200-byte
library, SHA-256 `beb77bfd2ceb19a03e48f2216505aa67742fca46fe9ad8b8eae00e701a9570cb`.

## What the provider contains

- The 11 FSR 3.1 pass enum values, with the accumulate pass compiled twice for
  `REPROJECT_USE_LANCZOS_TYPE` 0 (reference) and 1 (LUT): 13 modules in total.
- The permutation contract the Linux bridge can request: `LOW_RESOLUTION_MOTION_VECTORS=1`,
  `HDR_COLOR_INPUT=0`, `JITTERED_MOTION_VECTORS=0`, `INVERTED_DEPTH=0`; `APPLY_SHARPENING` selects the
  separately compiled accumulate-sharpen module. `FFX_HALF=0`.
- The Vulkan backend reports wave 32/64 on RDNA and 32/32 on some devices, so both Lanczos variants
  exist. fp16 and wave64 lookups return the same fp32/wave32 modules: the output is correct, only the
  half-float throughput optimisation is absent. This is the same choice AMD's signed provider makes.
- Bindings use the shared scheme (samplers 320/321, CBVs 128+, SRV textures 0+, UAV textures 64+,
  SRV buffers 192+, UAV buffers 224+). The blob metadata and the SPIR-V decorations come from one
  reflection pass.
- Every non-atomic storage image is declared format `Unknown` with
  `StorageImageRead/WriteWithoutFormat`, because the SDK creates R8_UNORM, R16_FLOAT, RGBA16_FLOAT and
  presentation-format images while DXC infers R32f/Rgba32f. The SPD atomic counter keeps `R32ui`
  (`OpImageTexelPointer` requires an explicit format and the SDK view is `R32_UINT`).
- Linux portability edits (in the build copy, not in AMD's source): `FFX_API_ENTRY` visibility, the
  loader-internal `ffxProviderExternal` stub, UTF-8/UTF-16 conversion without `<codecvt>`, the doubled
  `FFX_FSR3UPSCALER_CONTEXT_SIZE` for 4-byte `wchar_t`, a generated `git_hash_branch.h` placeholder and
  the MSVC `ui64` literal. The build fails closed if the expected SDK text is absent.

Known limitation: a context created with HDR, jitter-cancelled motion vectors, inverted depth or
display-resolution motion vectors is not in the generated set. `fsr3UpscalerGetPermutationBlobByIndex`
returns `FFX_ERROR_INVALID_ARGUMENT` for those keys, so context creation fails honestly; it never
returns a wrong blob. Extending the set means adding the option to `BASE_OPTIONS`/the variant list in
`tools/gen_upscaler_blobs.py` and rebuilding.

### Device features

The `Unknown` storage images and two SPD pyramid shaders (`ComputeDerivativeGroupLinearKHR`) are
normally used with `shaderStorageImageWriteWithoutFormat`, `shaderStorageImageReadWithoutFormat` and
`VK_KHR_compute_shader_derivatives` enabled at `vkCreateDevice`. The game's device does not enable them;
on RADV GFX1201 the provider creates its pipelines and dispatches anyway (headless A/B: identical
output with and without the features). A validation-enabled process or a stricter driver may reject
them. If the FSR path is made permanent, `toolchain/fsr_fg.py`'s existing gated feature block should
also run for `MK8_FSR4` (one condition), the same way frame generation enables them for
`MK8_FSR_FG`. That file is owned by the runtime/toolchain work; this provider does not modify it.

## Verification

### Headless, real hardware (RADV GFX1201, RX 9070 XT)

`ffxQuery` for `FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE` reports one version:
`id=0xf5a5ca1e00c01006 name=3.1.6` (the same id as AMD's signed 3.1.6 provider).

`ffxCreateContext(FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, 1920x1080 -> 2560x1440, flags
FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE)` returns `FFX_API_RETURN_OK`; the provider builds every
pipeline from the generated blobs, `ffxQueryGetProviderVersion` returns 3.1.6, one dispatch returns OK,
the readback has 14,711,840 of 14,745,600 bytes non-zero and varied, and `ffxDestroyContext` returns OK.
The same run also succeeds with a device created exactly like the game's (no formatless-image or
derivatives features).

### Byte-for-byte parity with AMD's 3.1.6

Using the private captured inputs in `local/qualification/fsr316-native/` (960x540 -> 1920x1080,
sharpening off, sharpness 0.9), the source-built provider writes the same bytes as both references:

| output | SHA-256 |
|---|---|
| this provider | `df8c4cf223fd824ae2a8f8860f342b4f71a451d1029537c9ba6ab6dd748049ae` |
| native provider from AMD's signed-DLL 3.1.6 blobs | `df8c4cf2…49ae` |
| AMD's signed Windows 3.1.6 DLL under Wine | `df8c4cf2…49ae` |

MAE 0, max 0, every byte identical. This validates the shader compilation, permutation selection,
binding rewrite, cbuffer layout and format handling together. The inputs and reference images are
private evidence (`local/qualification/fsr316-native/`); nothing is committed.

### In-game (title screen and race start)

The runtime was not rebuilt: `local/qualification/builds/4e03c7a3…/host-optimized/mk8drecomp` (19/11)
already has the FSR bridge. Commands:

```sh
MK8_FSR4=1 MK8_FSR_VERSION=3.1.6 \
MK8_FSR_LIB=$XDG_DATA_HOME/mk8online/fsr/libamd_fidelityfx_upscaler_vk.so \
  ./mk8drecomp <data-root>/extracted
```

Log (title screen, frame 700; the provider is confirmed in `/proc/<pid>/maps`):

```
[fsr] present 1920x1080 -> 2560x1440 color 1920x1080
[fsr] selecting version 3.1.6 (0xf5a5ca1e00c01006)
[fsr] provider 3.1.6
[fsr] depth now neutral (1920x1080 fmt 126)
[fsr] motion now neutral
[fsr] upscale active 1920x1080 -> 2560x1440 (depth neutral, motion neutral)
```

With the scripted input sequence and `MK8_FSR4_CAPTURE=1` the race-start frame at 2400 logs the full
input path as well (the `upscale active` line is logged once, when the context starts):

```
[fsr] depth now game (1920x1080 fmt 100)
[fsr] motion now camera
[vk] saved .../fsr316-source-race-02400.png (2560x1440)
```

Evidence (local, not committed):

- `local/qualification/fsr316-source-provider-00700.png` (title, SHA-256 `156afc98…e35fda`)
- `local/qualification/fsr316-source-title-00813.png` (title, `bdc81b79…798fbb`)
- `local/qualification/fsr316-source-race-02400.png` (race start, `00e1e26b…2decb8f`)
- `local/qualification/fsr316-source-provider.log`, `fsr316-source-race.log`,
  `fsr316-source-provider-maps.txt`, `fsr316-source-parity.rgba8`

Notes:

- The race run used the stable RADV package in `local/qualification/stable-radv/radv.json`, like the
  earlier FSR runs. Two attempts with the system RADV (Mesa 27.0.0-devel) crashed inside
  `libvulkan_radeon.so` at frame ~2160 while the runtime was compiling race shaders (`compile 754`,
  136 MB upload). The stable driver completed the same script; the crash is in driver pipeline
  compilation, not in the provider, and offers no evidence either way.
- No timing is claimed: the host was loaded throughout. The logged `[fsr] cpu dispatch` values are CPU
  record time only (0.01–0.07 ms) and are not GPU cost.
- This is menu and race-start visual evidence. A full race, restart/save, audio and controls were not
  qualified; `gameplay_verified` stays false. No performance comparison against FSR off was made.

## Tests

```sh
MK8_UPSCALER_LIB=$XDG_DATA_HOME/mk8online/fsr/libamd_fidelityfx_upscaler_vk.so \
MK8_UPSCALER_SDK=$HOME/.cache/mk8online/fsr-upscaler-build/sdk-copy \
  python3 -m unittest tests.test_fg_upscaler -v
```

Six tests pass: four generator tests (descriptor classification, storage-image format/atomic handling,
empty binding arrays, permutation contract) and two native tests (version query; 1920x1080 ->
2560x1440 context, dispatch, readback and destroy). Without the provider or SDK headers the native
tests skip; `MK8_UPSCALER_LIB` points at a non-default installation.

## Launcher integration (for the launcher work)

The provider is intentionally not bundled. A launcher step can mirror the frame-generation download:

1. show AMD's FidelityFX SDK licence from the pinned tag and require acceptance;
2. download/extract only the pinned official SDK source archive and the pinned DXC release (size and
   SHA-256 above), never a mirror;
3. run `tools/build_upscaler_provider.py` with those inputs and `--install-dir` inside the user's data
   directory (`local/`), keeping `LICENSE-AMD-FSR-SDK.md` beside the `.so`;
4. offer FSR 3.1.6 as the upscaler version and set `MK8_FSR4=1`, `MK8_FSR_VERSION=3.1.6` and
   `MK8_FSR_LIB=<install>/libamd_fidelityfx_upscaler_vk.so` for the game process.

The runtime's fallback path is unchanged when the library is absent: it logs
`[fsr] provider not loaded: <path>` and keeps FSR 1/bilinear. `MK8_FSR_LEGACY_ABI` must not be set for
this provider (it uses the current 2.4 backend descriptor), and `FSR4_ASSET_DIR` is not needed (there
are no external ML models).

## Licensing

The provider is compiled from AMD's FidelityFX SDK 2.4.0, MIT, Copyright (C) 2026 Advanced Micro
Devices, Inc. The generated SPIR-V is build output from AMD's HLSL and is not committed. The installed
library sits next to `LICENSE-AMD-FSR-SDK.md`; attribution is also recorded in
[THIRD_PARTY.md](THIRD_PARTY.md). AMD's signed DLLs, extracted shaders and reference inputs remain
private under `local/`.
