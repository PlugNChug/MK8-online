# FSR 4.1.2 upscaler, native Linux from the project's GPL port

Status 20/11/2026. Extends [FSR.md](FSR.md), [FSR3-UPSCALER.md](FSR3-UPSCALER.md) and
[FSR3-4-FG.md](FSR3-4-FG.md).

This is a one-click native Linux `libamd_fidelityfx_upscaler_vk.so` built from the project's own
GPL-3.0-only FSR 4.1.2 port (`tools/fsr4-linux/`, derived from
[fsr4-vk](https://github.com/Astyyyyy/fsr4-vk) commit `b0125ea9798633750f83ebabbc00279bcb49d8dd`). The
provider reads the FSR 4.1.2 SPIR-V and ML model banks extracted at build time from the user's own signed
AMD DLL, which ships in AMD's FidelityFX SDK 2.4.0 download. It is an optional host-side component loaded by
`runtime/mk8/graphics/fsr.cpp` through `MK8_FSR_LIB`; there is no guest code, ABI or toolchain change.

Nothing AMD ships as a binary, and no generated SPIR-V or model bank, is committed, bundled in the AppImage
or tarball, or copied into the source tree. The launcher's FSR 4.1.2 option calls the build script after the
user accepts AMD's licence; the extracted assets remain in the user's data directory.

## Files

- `tools/fsr4-linux/` — the vendored port: `src/abi` (the recovered 4.1.2 tensor/constant ABI and selector
  table), `src/provider` (FFX provider), `src/loader`, `src/runner`, `tests/`, the extraction tools and their
  selector map, and `third_party/fsr4-vk/` (the GPL-3.0-only base snapshot, no submodules).
- `tools/fsr4-linux/CMakeLists.txt` — builds the provider and loader. The FFX headers come from the SDK:
  `-DFSRSDK_ROOT=<sdk>` or the `FSR4_SDK_ROOT` environment variable. Inside the SDK the default
  (`<port>/..`) still works.
- `tools/build_fsr4_provider.py` — verifies the pinned inputs, extracts the assets with the port's tools,
  builds and installs the provider and its licence/source-offer files, and optionally runs the self-test.
- `tools/fsr4-linux/tests/provider/selftest.cpp` — headless version query and a legacy-ABI 1920x1080 ->
  2560x1440 context, dispatch and readback.
- `tests/test_fsr4_provider.py` — pinned-input and extractor contract tests, plus native query/context tests
  when the provider and SDK headers are installed.

## Build and install

Pinned inputs:

| input | size | SHA-256 |
|---|---:|---|
| `FidelityFX-SDK-v2.4.0.tar.gz` | 253,592,010 | `111f9b19c0c9b8a1f24d2a5104095b8e626f12deb4eca53cd0c29921aa1fd43a` |
| `Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_vk.dll` | 36,135,696 | `4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7` |

`--sdk-size`/`--sdk-sha256` default to those values; a different archive, or a DLL whose size or hash does
not match, fails before anything is extracted. The port's `PHASE0.md` records the DLL hash and
`tools/fsr4-linux/tools/verify_assets.py` pins the model-bank hashes derived from it.

```sh
# from the pinned official SDK archive (contains the signed Windows DLL)
python3 tools/build_fsr4_provider.py \
  --sdk-archive FidelityFX-SDK-v2.4.0.tar.gz \
  --self-test

# or from an SDK tree that is already extracted
python3 tools/build_fsr4_provider.py \
  --sdk-src /path/to/FidelityFX-SDK-2.4.0 \
  --self-test
```

`--reuse-assets` reuses an extraction in the work directory only when its recorded source hash matches the
verified DLL. The work directory defaults to `~/.cache/mk8online/fsr4-build`; the script stops if less than
4 GiB is free there. Python 3.10+ and zlib are required (stdlib only, no network). CMake, Ninja, g++ and the
Vulkan loader/headers are the build prerequisites.

The default installation is `$XDG_DATA_HOME/mk8online/fsr/` (`~/.local/share/mk8online/fsr/`), the same
place as the other FSR providers:

```
libamd_fidelityfx_upscaler_vk.so
fsr4-assets/
  shaders/           191 SPIR-V modules selected by the port's trim_assets.py
  models/FP8_standard.bin
LICENSE-GPL-3.0.txt
LICENSE-AMD-FidelityFX-SDK.md
LICENSE-AMD-FidelityFX-SDK-MIT.md
THIRD_PARTY_NOTICES.md
SOURCE-OFFER.md
```

The provider reads `fsr4-assets` next to the library by default, or the directory named by `FSR4_ASSET_DIR`.
No `FSR4_ASSET_DIR` is needed when the library and assets are installed together.

## What is vendored, what is generated

Vendored source (committed, GPL-3.0-only): `tools/fsr4-linux/src`, `tools`, `tests`, the recovery data in
`tools/data/fsr4_412_selector_map.json`, and the GPL-3.0-only `third_party/fsr4-vk` snapshot. The vendored
tree does not include the SDK, AMD binaries, a `build/` directory or generated assets.

Two vendoring edits keep it usable outside the SDK: `CMakeLists.txt` takes `FSR4_SDK_ROOT` from the cache or
environment, and `ffxQuery(GET_VERSIONS)` lists the version as `4.1.2` (the string the runtime's
`MK8_FSR_VERSION` compares) while `GET_PROVIDER_VERSION` still reports the display name `FSR 4.1.2`.

Generated at build time from the signed DLL, kept only under the user's work/install directories:

- 1,778 SPIR-V modules (102,181,900 uncompressed bytes), four model dispatchers with eight banks, the pass
  catalog and the decoded selector tables in the work directory;
- the runtime subset actually installed: 191 modules plus `models/FP8_standard.bin`
  (SHA-256 `ce1bd5f19d4fc14f857c5fc810def7f45d5a935a62aa2ca5b5a59829a1d6c868`).

The installed assets are never committed or packaged. `SOURCE-OFFER.md` beside them records the DLL hash,
the extraction counts and how to rebuild.

## Verification

Headless, RADV GFX1201 (RX 9070 XT), from the pinned archive:

```sh
python3 tools/build_fsr4_provider.py \
  --sdk-archive FidelityFX-SDK-v2.4.0.tar.gz \
  --self-test
```

The self-test prints the version list (`name=4.1.2 id=0x1001002`; `GET_PROVIDER_VERSION` still
reports `FSR 4.1.2` for display), creates a context through the
deprecated 1.x Vulkan backend descriptor (the runtime's `MK8_FSR_LEGACY_ABI=1` path), dispatches one
1920x1080 -> 2560x1440 frame, checks the readback is not degenerate, and destroys the context. The build
script also loads the produced library and resolves all five FFX entry points.

In-game (title/menu, short run):

```sh
MK8_FSR4=1 \
MK8_FSR_VERSION=4.1.2 \
MK8_FSR_LEGACY_ABI=1 \
MK8_FSR_LIB=$XDG_DATA_HOME/mk8online/fsr/libamd_fidelityfx_upscaler_vk.so \
  ./mk8drecomp <data-root>/extracted
```

Expected lines: `[fsr] selecting version 4.1.2 (0x1001002)`, `[fsr] provider FSR 4.1.2`,
`[fsr] upscale active 1920x1080 -> 2560x1440`. The runtime requires `MK8_FSR4=1`; the other three variables
select the legacy ABI used by this port and the installed library.

Tests:

```sh
MK8_FSR4_LIB=$XDG_DATA_HOME/mk8online/fsr/libamd_fidelityfx_upscaler_vk.so \
MK8_FSR4_SDK=$HOME/.cache/mk8online/fsr4-build/sdk-extract/FidelityFX-SDK-2.4.0 \
  python3 -m unittest tests.test_fsr4_provider -v
```

The native tests skip when the provider, assets or SDK headers are absent. The generator tests exercise the
port's FFXZ scanner and the trim/verifier contract without the DLL.

### Evidence, 20/11/2026

From-archive build with the pinned SDK archive (`253,592,010` bytes, SHA-256 `111f9b19…`):

```
SDK archive ok: 253592010 bytes sha256=111f9b19c0c9b8a1f24d2a5104095b8e626f12deb4eca53cd0c29921aa1fd43a
signed upscaler DLL ok: 36135696 bytes sha256=4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7
assets ok: 1778 modules, 102181900 bytes, 4 model classes / 8 banks
exports ok: ffxCreateContext, ffxDestroyContext, ffxConfigure, ffxQuery, ffxDispatch
installed .../libamd_fidelityfx_upscaler_vk.so (183512 bytes, sha256=ae0cd7fdd04f77202e73337dc4a13ff5f8cf5127d14acd843fb5e3370beb441b)
installed runtime assets: 191 shader modules + FP8_standard.bin, 14.9 MiB
version[0]: name=4.1.2 id=0x1001002
ffxCreateContext(UPSCALE 1920x1080 -> 2560x1440 legacy ABI flags=0x100) -> 0x0 (OK)
ffxQuery(provider) -> 0x0 id=0x1001002 name=FSR 4.1.2
ffxDispatch(UPSCALE) -> 0x0 (OK)
output: 14745600 bytes, nonzero=14696000, unique=256, min=0, max=255
ffxDestroyContext -> 0x0 (OK)
RESULT: OK
```

In-game menu and race-start runs used `local/qualification/builds/4e03c7a3…/host-optimized/mk8drecomp`
(SHA-256 `e6f37670…5d6f90`), the extracted 4.0.1 data root, `MK8_EXPERIMENTAL_NO_MII=1`, the environment
above, and `MK8_FSR4_CAPTURE` unset (neutral depth/motion; the capture path is separate research). The menu
frame used the system RADV (Vulkan API 1.4.363) and exited 0. The race-start frame used the stable RADV package in
`local/qualification/stable-radv/radv.json` (API 1.4.354) and exited 0:

```
[fsr] present 1920x1080 -> 2560x1440 color 1920x1080
[fsr] selecting version 4.1.2 (0x1001002)
[fsr] provider FSR 4.1.2
[fsr] depth now neutral (1920x1080 fmt 126)
[fsr] motion now neutral
[fsr] upscale active 1920x1080 -> 2560x1440 (depth neutral, motion neutral)
[fsr] view/proj captured in main draw
[vk] saved .../ingame-stable-02400.png (2560x1440)
```

Screenshots (private): `local/qualification/fsr4-linux/ingame-01000.png` (engine-class menu, SHA-256
`ac921540…a520ac7b2b`) and `ingame-stable-02400.png` (Mario Kart Stadium start, `e3ba0c9a…1a44c84`). A
longer attempt on the system RADV aborted at frame 2161 inside `libvulkan_radeon.so` while compiling race
shaders, the same driver crash documented for the 3.1.6 provider; the stable RADV completed the run. The
crash is not in the provider (no `[fsr]` failure line, and the dispatch stays 0.01–0.08 ms CPU record time).

`python3 -m unittest tests.test_fsr4_provider -v` passes 11/11 with the installed provider, assets and SDK
headers. Logs are in `local/qualification/fsr4-linux/` (`build-from-archive.log`, `build-committed.log`,
`tests.log`, `ingame-2.log`, `ingame-race-stable.log`); they stay private with the screenshots.

No timing is claimed: the host was loaded by another build. This is API, menu and race-start visual
evidence, not a full-race/restart/save or performance qualification; `gameplay_verified` remains false. The
screenshots show the game's real rendered image through the provider, but the run used neutral depth/motion
(no `MK8_FSR4_CAPTURE`), so they do not demonstrate the real-depth motion-vector path.

## Launcher hand-off

The launcher should treat FSR 4.1.2 exactly like the other terms-gated providers:

1. show AMD's FidelityFX SDK 2.4.0 licence (and this port's GPL-3.0 notice) and require acceptance;
2. obtain only the pinned official SDK archive (size and SHA-256 above), never a mirror or a repack;
3. run `tools/build_fsr4_provider.py --sdk-archive <archive>` with a staging `--install-dir` inside the
   user's data directory, and keep the default `--self-test` or run it once after installation;
4. verify the staged layout (`libamd_fidelityfx_upscaler_vk.so`, `fsr4-assets/`,
   `LICENSE-GPL-3.0.txt`, an AMD `LICENSE-AMD-*`, `SOURCE-OFFER.md`), read the version through
   `ffxQuery(GET_VERSIONS)` (it must be exactly `4.1.2`), then publish the staging directory atomically;
5. when FSR 4.1.2 upscaling is selected, set the game process environment:

   ```
   MK8_FSR4=1
   MK8_FSR_VERSION=4.1.2
   MK8_FSR_LEGACY_ABI=1
   MK8_FSR_LIB=<published>/libamd_fidelityfx_upscaler_vk.so
   ```

   The provider finds `fsr4-assets` beside the library by default. The launcher may also set
   `FSR4_ASSET_DIR=<published>/fsr4-assets`; both forms are equivalent. If the assets are moved, set
   `FSR4_ASSET_DIR` to the directory that contains `shaders/` and `models/`.

The current launcher implementation uses `--work-dir <fsr>/fsr4-build --install-dir <fsr>/fsr4.next
--reuse-assets`, then publishes to `<fsr>/provider/fsr4.1.2/`. That layout matches the paths above.

The runtime's fallback is unchanged: without `MK8_FSR4` or with a missing library it logs
`[fsr] provider not loaded: <path>` or keeps FSR 1/bilinear. The provider only advertises 4.1.2; asking for
another version logs that it is not offered and continues on 4.1.2. `MK8_FSR_LEGACY_ABI` is required for this
port; it is not required for the SDK-built 3.1.6 provider.

## Licensing

The port and `third_party/fsr4-vk` are GPL-3.0-only; this project is GPL-3.0, so the vendored source is
compatible. The installed library is accompanied by the full GPL-3.0 text, AMD's FidelityFX SDK licence and
MIT notice, the vendored third-party notices, and `SOURCE-OFFER.md`, which identifies the corresponding
source in `tools/fsr4-linux/`. AMD's signed DLL and the extracted SPIR-V/model assets are not distributed by
the project. Attribution is also recorded in [THIRD_PARTY.md](../THIRD_PARTY.md).
