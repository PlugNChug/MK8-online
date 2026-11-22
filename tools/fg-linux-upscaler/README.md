# Native Linux FSR 3.1.6 upscaler provider build support

This directory holds the host build scaffolding for `libamd_fidelityfx_upscaler_vk.so`, the native Linux
FidelityFX upscaler provider used by `tools/build_upscaler_provider.py`.

Nothing here is an AMD binary or a prebuilt shader. The provider is compiled locally from the user's own
MIT-licensed FidelityFX SDK 2.4.0 download; the SPIR-V is produced from the SDK's HLSL by DXC at build time.

## Files

- `CMakeLists.txt` — provider build (FidelityFX API object, Vulkan backend, FSR 3.1.6 upscaler effect,
  provider, generated blobs). `FFX_SDK` points at the prepared SDK tree.
- `upscaler_compat.h` — Linux definitions the SDK assumes from its Windows build: `FFX_API_ENTRY`,
  `FFX_UNUSED`, `_countof`, secure-CRT shims, UTF-8/UTF-16 conversion without `<codecvt>` (removed in
  GCC 16).
- `upscaler_provider_list.cpp` — the provider registry the SDK generates at build time but does not
  publish: registers `ffxProvider_FSR3Upscale` (3.1.6).
- `win32compat/` — minimal Win32 surface the shared Vulkan backend includes (events, threads, QPC,
  critical sections).
- `miniz/miniz.h` — maps the single `mz_uncompress` call onto zlib.
- `test_query.cpp` — `ffxQuery` version check (expects only `3.1.6`).
- `test_context.cpp` — headless 1920x1080 -> 2560x1440 context creation, one dispatch, readback and
  destroy. `--provider NAME` selects a version through `ffxQuery`/`ffxOverrideVersion` exactly like the
  runtime, `--out PREFIX` writes the output as raw RGBA and PPM, and `--no-device-features` creates the
  device like the unpatched game (diagnostic). `platform.h` loads the provider on Linux or Windows.

## Build

```sh
python3 tools/build_upscaler_provider.py \
  --sdk-archive FidelityFX-SDK-v2.4.0.tar.gz --sdk-size <bytes> --sdk-sha256 <hex> \
  --dxc /path/to/dxc --self-test
```

The install directory defaults to `$XDG_DATA_HOME/mk8online/fsr` and receives the provider plus
`LICENSE-AMD-FSR-SDK.md` copied from the SDK. The launcher enables the provider after the user accepts
AMD's terms and the download/patch step succeeds; see `docs/FSR3-UPSCALER.md`.

## Tests

`tests/test_fg_upscaler.py` compiles and runs both programs against an installed provider. It skips when
the provider or the SDK headers are absent:

```sh
MK8_UPSCALER_LIB=.../libamd_fidelityfx_upscaler_vk.so MK8_UPSCALER_SDK=.../sdk-copy \
  python3 -m unittest tests.test_fg_upscaler -v
```
