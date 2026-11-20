# FSR 4.1.2 native Linux port

**Status: Phase 0 PASSED.** A native Linux Vulkan runner reproduces AMD's signed
Windows FSR 4.1.2 implementation **byte-for-byte** for 1080p and 1440p output
classes on RADV (RX 9070 XT). Runtime is Wine-free; Wine is only used to capture
the reference.

## Vendored in MK8-online

This is the project's own GPL-3.0-only native Linux FSR 4.1.2 port, vendored at
`tools/fsr4-linux`. The user builds it locally with
`tools/build_fsr4_provider.py`; AMD's signed DLL and the extracted SPIR-V/models
are never committed. The FFX headers come from the user's FidelityFX SDK 2.4.0
tree: pass `-DFSRSDK_ROOT=<sdk>` (or set `FSR4_SDK_ROOT`) when configuring
outside the SDK. See `docs/FSR4-UPSCALER.md`. The layout below is the original
SDK-relative view; the vendored copy has no `build/` directory.

## Layout

```
linux/
  tools/                 asset extraction + catalog + validation
  src/runner/            Phase 0 native single-case runner
  tests/reference/       Wine reference tracer + case preparation
  tests/compare.py       reference-vs-native image comparison
  third_party/fsr4-vk/   GPL-3.0 base (vendored, no submodules)
  build/                 generated assets and build output (ignored)
```

## Extract assets

Requirements: Python 3.10+, zlib (stdlib), and the signed DLL at
`Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_vk.dll`.

```sh
cd tools/fsr4-linux
FSR4_SDK_ROOT=/path/to/FidelityFX-SDK-2.4.0 python3 tools/extract_all.py
python3 tools/verify_assets.py build/assets
```

This writes 1,778 SPIR-V modules, four model banks, the pass catalog and the
decoded selector tables under `tools/fsr4-linux/build/assets/`. In this checkout
the supported path is `tools/build_fsr4_provider.py`, which passes the SDK root
and stages only the runtime subset.

## Phase 0 runner

Build the native runner and the reference tracer:

```sh
cd linux
cmake -S . -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DFSRSDK_ROOT=/path/to/FidelityFX-SDK-2.4.0
cmake --build build/native
sh tests/reference/build.sh          # mingw PE; copies the signed DLLs
```

Capture a reference case (Wine), then run the native case and compare:

```sh
mkdir -p build/tests/reference/caseA
cd build/tests/reference/caseA
cp ../fsr4_trace.exe ../amd_fidelityfx_*.dll .
WINEPREFIX=<wine-prefix> WINEDEBUG=-all wine fsr4_trace.exe 960 540 1920 1080
cd ../../../..
sh tests/run_case.sh build/tests/reference/caseA build/assets build/run/caseA
sh tests/run_case.sh build/tests/reference/caseA build/assets build/run/caseA
```

Expected: `IDENTICAL (byte-for-byte)`.

See `PHASE0.md` for the recovered tensor table, dispatch geometry, resource
layout and remaining M1 work.

## ABI module (M1.1)

`src/abi/` computes the 272-byte tensor table, the 104-byte optimized constants,
the 32-byte SPD and RCAS blocks, and the shader-selection table. It is validated
against every captured reference case:

```sh
cmake --build build/native
sh tests/run_abi_tests.sh
```

The selector table is generated from `tools/data/fsr4_412_selector_map.json`:

```sh
python3 tools/gen_selector_table.py
```

## Native FFX provider (M1.1b)

`cmake --build build/native` also produces the FFX effect and loader:

- `libamd_fidelityfx_upscaler_vk.so` — exports the five FFX entry points, selects
  pipelines from `src/abi`, records the FSR 4.1.2 chain into the caller's command
  buffer.
- `libamd_fidelityfx_loader_vk.so` — forwards to the effect next to itself or on
  `LD_LIBRARY_PATH`.

The provider reads SPIR-V and model banks from `FSR4_ASSET_DIR` (default `.`).
Run the end-to-end matrix:

```sh
sh tests/run_provider_tests.sh
sh tests/run_bench.sh        # timestamped GPU benchmark
```

## Licensing

- The port derives from [fsr4-vk](https://github.com/Astyyyyy/fsr4-vk), GPL-3.0-only.
- AMD SDK 2.4.0 `Kits/FidelityFX/docs/license.md` lists the signedbin DLLs among the
  MIT-licensed exceptions. The signed DLLs themselves are not redistributed; assets are
  extracted from the user's own copy at build time.
- Include GPL-3.0 and AMD MIT notices in any distribution.
