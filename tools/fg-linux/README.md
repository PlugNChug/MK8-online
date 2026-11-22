# Native Linux FSR frame-generation provider build support

This directory holds the host build scaffolding for `libamd_fidelityfx_framegeneration_vk.so`, the native Linux
FidelityFX frame-generation provider used by `tools/build_fg_provider.py`.

Nothing in this directory is an AMD binary or a prebuilt shader. The provider is compiled locally from the user's own
MIT-licensed FidelityFX SDK 2.4.0 download; the SPIR-V is produced from the SDK's HLSL by DXC at build time.

## Files

- `CMakeLists.txt` — provider build (FidelityFX API object, Vulkan backend, frame interpolation, optical flow, VK
  swapchain, generated blobs). `FG_SDK` points at the prepared SDK tree.
- `fg_compat.h` — Linux definitions the SDK assumes from its Windows build: `FFX_API_ENTRY`, `FFX_UNUSED`, `_countof`,
  secure-CRT shims, UTF-8/UTF-16 conversion without `<codecvt>` (removed in GCC 16).
- `fg_provider_list.cpp` — the provider registry the SDK generates at build time but does not publish: registers the
  frame-generation effect and the Vulkan swapchain provider.
- `win32compat/` — minimal Win32 surface used by the Vulkan FG code (events, threads, QPC, critical sections,
  `INFINITE`, `WINAPI`, `LPVOID`).
- `miniz/miniz.h` — maps the single `mz_uncompress` call onto zlib.
- `test_query.cpp` — `ffxQuery` version check (expects the swapchain provider `3.1.7`).
- `test_context.cpp` — headless Vulkan context creation (all pipelines from the generated blobs).

## Build

```sh
python3 tools/build_fg_provider.py \
  --sdk-archive FidelityFX-SDK-v2.4.0.tar.gz --sdk-size <bytes> --sdk-sha256 <hex> \
  --dxc /path/to/dxc --self-test
```

The install directory defaults to `$XDG_DATA_HOME/mk8online/fsr` and receives the provider plus
`LICENSE-AMD-FSR-SDK.md` copied from the SDK. The launcher only enables the provider after the user accepts AMD's
terms and the download/patch step succeeds.

## Tests

`tests/test_fg_provider.py` compiles and runs both programs against an installed provider. It skips when the provider
or the SDK headers are absent:

```sh
MK8_FG_LIB=.../libamd_fidelityfx_framegeneration_vk.so MK8_FG_SDK=.../sdk-copy \
  python3 -m unittest tests.test_fg_provider -v
```
