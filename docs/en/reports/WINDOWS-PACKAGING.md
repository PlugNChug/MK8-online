# Windows portable package — 20/11/2026

This records the Windows x86-64 package built from the current 4.0.1 sources and the
optional-guest-patch hash refresh that made the package build without
`MK8_SKIP_OPTIONAL_GUEST_PATCHES=1`.

It is a packaging and boot check, not a qualification. `gameplay_verified` stays
false: audio, controls, a complete race, restart/save and clock speed were not
measured here.

## Result

| Check | Result | Evidence |
| --- | --- | --- |
| Optional ultrawide/settings hashes match the current generated units | **PASS** | `tests.test_optional_guest_patches` (3 tests) |
| Windows game build with optional patches, no skip | **PASS** | 483 units; `mk8dgame.dll` `45ce5ffc…`; hooks present in the patched units |
| Linux game build from the same tree | **PASS** | 483 units; `mk8dgame.so` 159,341,232 bytes |
| Windows host (`x86_64-windows-gnu`) | **PASS** | 234 units; `mk8drecomp.exe` `37af7b74…` |
| Portable package produced | **PASS** | zip 110,717,508 bytes; `Launcher.exe` 25,603,047 bytes |
| Headless launcher page on RTX 5080 | **PASS** | HTTP page 40,097 bytes; `/api/fsr` served |
| FSR gate on RTX 5080 | **PASS** | `blocked:true`, `reason_code:"not-rdna4"`, `["3.1.6","2.3.4"]` |
| 3.1.6 selectable / 4.1.2 rejected | **PASS** | save `200` / save `400` |
| Packaged runtime boot, 1920x1080 | **PASS** | exit 0; race render; `package2-05000.png`; 60.0 fps lines |
| MSVC/WebView2 `setup.exe` | **Not generated** | cross packaging path only; see below |

## Source state and guest hashes

Branch `feature/fsr`, built from `1fa7ca3` plus `1a78c6d` (the gated XeSS
device hook) and the commit that adds this document, the package script, the
tests and the refreshed hashes. No guest behavior changed: the
pinned hashes and the patch anchors now describe the current 4.0.1 generated units
(the `4318c07` sized `STT_NOTYPE` recompiler fix changed their output).

| Unit | Role | New SHA-256 | Previous pinned hash |
| --- | --- | --- | --- |
| `main/main_077.c` | ultrawide camera | `d4f3917c93a685cf81570987e2640c264ef5dc98c26c391ba00919dce6d6ac7d` | `e2acde44…d5dce4` |
| `main/main_164.c` | ultrawide HUD | `6794319ad61606e5a2910676d06eb26f3ac3f8b748f61a2302b798f4016b4f00` | `3116bd45…d7a86` |
| `main/main_114.c` | settings top-menu event | `bea4142aaec02e8af9285c1de21be90d1209546c56e81a11019411b799a5b0b9` | `ace7dcd1…d960cf` |
| `main/main_113.c` | settings build | `ab615528d5a4d6ca6a734a47130e2d583c816f90d1731da9aaef780058ff9210` | `4b4cf4a1…d88f7` |
| `main/main_082.c` | settings navigation | `bcacfa659a58ddf02404e9923512e29ae324fa507b9187ba8fd7218660389f13` | `c6d46af3…07082` |
| `main/main_115.c` | settings tick | `02f16954f504ab999adcbc1a7036bd7010f72430b422e07f8f3b6b4004414b96` | `e7f979c9…ef048` |
| `main/main_100.c` | shared build hook | `3134491b6aa0511945779190419987f6650d3349aecbc78392c12455a26eeef6` | `187122fd…518cc` |
| `main/main_191.c` | shared page-frame hook | `2ae69dad8c0b61bb97d87ae4e49e96d5057a31e8645584ea054d8e063d872043` | `f61eefe2…9d133` |

`tests/test_optional_guest_patches.py` reads a private generated tree (the game
translation is not in the repository). It checks every pinned hash and applies
every anchor, writing the patched units into a temporary directory. Without a tree
it skips cleanly; point it at one with `MK8_GENERATED_MAIN` or use the builder's
`local/windows-rtx2/generated-new/main`, `local/builds/*/generated/main` or
`local/qualification/*/builds/*/generated/main`.

```sh
python3 -m unittest tests.test_optional_guest_patches -v
```

```text
test_patch_anchors_apply_to_the_generated_units ... ok
test_pinned_settings_hashes_match_generated_units ... ok
test_pinned_ultrawide_hashes_match_generated_units ... ok
Ran 3 tests in 2.562s
OK
```

The Windows game build below ran without `MK8_SKIP_OPTIONAL_GUEST_PATCHES=1`. The
generated patched units contain the hooks:

```text
local/windows-rtx2/native-all/ultrawide/main_077.c:25812: void mk8_ultrawide_camera(uint64_t manager);
local/windows-rtx2/native-all/ultrawide/main_077.c:26004:     mk8_ultrawide_camera(c->x[20]);  // MK8 (toolchain/ultrawide.py)
local/windows-rtx2/native-all/ultrawide_hud/main_164.c:18632: void mk8_ultrawide_hud(Cpu *c, uint64_t layout);
local/windows-rtx2/native-all/ultrawide_hud/main_164.c:18661:     ... mk8_ultrawide_hud(c, c->x[21]);
local/windows-rtx2/native-all/experimental/main_114.c:3: extern bool mk8_settings_top_event(uint64_t, uint64_t);
local/windows-rtx2/native-all/experimental/main_114.c:37:     if (mk8_settings_top_event(c->x[0], c->x[1])) return;
```

## Build

The Windows host and game library used the pinned Zig 0.17.0
`x86_64-windows-gnu` target and the current 4.0.1 generation under
`local/windows-rtx2/prepared` / `local/windows-rtx2/generated-new`. The
generated tree and generated code are private build data; they are not committed.

Host:

```sh
MK8_ZIG="$PWD/local/windows-rtx2/tools/zig-x86_64-linux-0.17.0/zig" \
MK8_NATIVE_TOOLS="$PWD/local/windows-verify/native-tools" \
SystemRoot="$PWD/local/windows-rtx2/fake-system" \
python3 local/windows-rtx2/cross_build.py \
  local/windows-rtx2/prepared local/windows-rtx2/host 4
```

```text
Runtime x86_64-windows-gnu: 234/234 unidades
Executável do runtime ligado; gameplay ainda não verificado.
mk8drecomp.exe 4,527,104 bytes
SHA-256 37af7b744a09e36dc5e55025e3cdd0a508afa0ea91517f28817fec14a6cc8479
```

Game library, with the optional patches applied:

```sh
MK8_ZIG="$PWD/local/windows-rtx2/tools/zig-x86_64-linux-0.17.0/zig" \
MK8_NATIVE_TOOLS="$PWD/local/windows-verify/native-tools" \
MK8_JOBS=6 \
python3 local/windows-rtx2/cross_game.py \
  local/windows-rtx2/prepared local/windows-rtx2/generated-new \
  local/windows-rtx2/native-all main,sdk,subsdk0
```

```text
{"state": "native_objects", "objects": 483, "target": "x86_64-windows-gnu",
 "modules": ["main", "sdk", "subsdk0"]}
game built: …/local/windows-rtx2/native-all/mk8dgame.dll (535040 bytes)
```

The same generated tree was compiled natively on Linux with
`toolchain.native.compile_modules` and `toolchain.host.link_game` for
`main,sdk,subsdk0` (483 units, `-O1`), proving the refreshed anchors do not only
build on Windows:

```sh
python3 - <<'PY'
import json, sys
from pathlib import Path
root = Path('.').resolve()
sys.path.insert(0, str(root/'toolchain'))
import native, host
source = root/'local/windows-rtx2/prepared'
generated = root/'local/windows-rtx2/generated-new'
output = root/'local/linux-check/native'
output.mkdir(parents=True, exist_ok=True)
result = native.compile_modules(source, generated, output, ['main', 'sdk', 'subsdk0'],
                                optimization='-O1', workers=6,
                                cache=root/'local/linux-check/object-cache')
print(json.dumps({key: result[key] for key in ('state', 'objects', 'target', 'modules')}))
print('game built:', host.link_game(Path(result['archive']), output, source, ['main', 'sdk', 'subsdk0']))
PY
```

```text
local/linux-check/native/mk8dgame.so  159,341,232 bytes
SHA-256 16831630509cf0cece978908a98ca8b34e2f249bdcac6cf2459d3b4a14aa5bef
```

## Portable package

`scripts/package-windows-portable.py` is the cross-packaging path. It builds the
launcher with the project's `x86_64-pc-windows-gnu` target, embeds a source payload
in `Launcher.exe`, copies the host, game and code libraries plus the runtime DLLs,
and writes both the tree and a zip. It deliberately does **not** run
`scripts/package-launcher.py` or create the MSVC/WebView2 `setup.exe`; that remains
a Windows CI/release step and was not generated here.

```sh
python3 scripts/package-windows-portable.py \
  --output local/windows-portable \
  --target-dir local/windows-rtx-target \
  --deps-dir /home/mitchell/Projects/MK8-online-windows-verify/local/run-win
```

`--deps-dir` defaults to `local/windows-verify/run-win`; it must contain the
runtime DLL set (SDL3, shaderc, FFmpeg and their dependencies). The command also
accepts `--skip-build` to reuse an existing `Launcher.exe`.

Layout:

```text
MK8-online-0.4.0-windows-x86_64-portable/
  Launcher.exe
  WebView2Loader.dll
  launcher-source.zip          # the same bytes embedded in Launcher.exe
  LICENSE
  README.txt
  runtime/                     # 110 files
    mk8drecomp.exe
    mk8dgame.dll
    mk8dgame.dll.id
    mk8d-code-main.dll
    mk8d-code-sdk.dll
    mk8d-code-subsdk0.dll
    SDL3.dll  vulkan-1.dll  libshaderc_shared.dll
    avcodec-63.dll  avformat-63.dll  avutil-61.dll  swresample-7.dll  swscale-10.dll
    … (FFmpeg, font, image and compression dependencies)
```

Hashes:

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `MK8-online-0.4.0-windows-x86_64-portable.zip` | 110,717,508 | `5e02d0d14bd1e4019f9144998d1686121c31d70e69175f76628a01a1800240a2` |
| `Launcher.exe` | 25,603,047 | `62cadbaa57173411eb7bce6687918ad60d8792f22445d55602ac23516dfb6386` |
| `WebView2Loader.dll` | 160,880 | `86545b66cdb0603bc26b626fb9ad610cb6e71f28d468f5ea66df23b03dda96d5` |
| `launcher-source.zip` | 710,072 | `fe61c2b115e3d00651a9b2490e7629365f814fa7e71b81a5b0bd5fa9546d5cb1` |
| `runtime/mk8drecomp.exe` | 4,527,104 | `37af7b744a09e36dc5e55025e3cdd0a508afa0ea91517f28817fec14a6cc8479` |
| `runtime/mk8dgame.dll` | 535,040 | `45ce5ffc42c5ccc51ac607cc7e3ba27db9bdfdf02b27c01881c58dea3aa70f53` |
| `runtime/mk8d-code-main.dll` | 75,041,792 | `b611246c01dd15a3a4103a46b7f100f2432b113919d1ffb4bff62b4c92c7ecb5` |
| `runtime/mk8d-code-sdk.dll` | 24,498,688 | `245d678c1a6677ce05ff088a22b7f91497e0ad5c7d271face1f4dc2a86bc0f8d` |
| `runtime/mk8d-code-subsdk0.dll` | 19,187,712 | `565f1edd754a95c611b8d70cb1f66a512d9f480bb7928c27db1c0b32bf157208` |

The payload contains the FSR/DLSS/XeSS/Vulkan runtime sources under
`runtime/`, the toolchain scripts (`toolchain/fsr.py`, `toolchain/dlss.py`,
`toolchain/dlss_fg.py`, `toolchain/xess.py`, `toolchain/ultrawide*.py`,
`toolchain/settings_menu.py`, …), the FSR/DLSS generators and harnesses under
`tools/`, `games/mk8d/`, `scripts/`, `docs/licenses/` and the project `LICENSE`.
No game data, keys, saves or vendor DLLs are included. The host also carries the
gated XeSS device-extension hook (`toolchain/xess.py`); it is inert unless
`MK8_XESS` is set.

## RTX 5080 validation

Machine and key are the ones in `docs/WINDOWS-INGAME-RTX.md`:
`Ronny@192.168.8.197`, key `~/.ssh/mkdx_windows_ed25519`, Windows 11, NVIDIA
GeForce RTX 5080, driver `32.0.16.2101`, Vulkan `1.4.357`.

Copy and extract:

```sh
scp -i ~/.ssh/mkdx_windows_ed25519 MK8-online-0.4.0-windows-x86_64-portable.zip \
  Ronny@192.168.8.197:C:/Users/Ronny/mk8-rtx/package/
```

```powershell
Set-Location 'C:/Users/Ronny/mk8-rtx/package'
& 'C:/Windows/System32/tar.exe' -xf 'MK8-online-0.4.0-windows-x86_64-portable.zip'
```

Headless launcher and FSR gate:

```powershell
$root = 'C:\Users\Ronny\mk8-rtx\package\MK8-online-0.4.0-windows-x86_64-portable'
$data = 'C:\Users\Ronny\mk8-rtx\package\data'
$env:MK8_FSR_DIR = 'C:\Users\Ronny\mk8-rtx\amd'
$p = Start-Process -FilePath "$root\Launcher.exe" `
  -ArgumentList @('--headless', '--port', '38090', '--data-dir', $data) `
  -PassThru -WindowStyle Hidden
# fetch http://127.0.0.1:38090/, read the mk8-token meta tag, then:
#   GET  /api/fsr
#   POST /api/fsr/settings  {"upscaling":"3.1.6",...}
#   POST /api/fsr/settings  {"upscaling":"4.1.2",...}
```

Observed:

```text
PAGE_OK=40097
FSR4: {"allowed":false,"blocked":true,"framegeneration_versions":["3.1.7"],
       "gpu":"NVIDIA GeForce RTX 5080","override":false,
       "reason":"FSR 4.1.2 exige AMD RDNA 4; …",
       "reason_code":"not-rdna4","vendor":"NVIDIA","versions":["3.1.6","2.3.4"]}
SETTINGS={"dxc":"","frame_generation":"off","legacy_abi":false,"provider_lib":"","upscaling":"off"}
SAVE_316_CODE=200
SAVE_412_CODE=400
```

The 3.1.6 save returned the updated settings with `upscaling:"3.1.6"`; the 4.1.2
save was refused with the `not-rdna4` message. The page is the launcher's HTTP
page served headless; it does not exercise WebView2 painting in an interactive
desktop session.

Packaged runtime boot:

```powershell
$run = 'C:\Users\Ronny\mk8-rtx\package\MK8-online-0.4.0-windows-x86_64-portable\runtime'
$data = 'C:\Users\Ronny\mk8-rtx\game\extracted'
$env:LARECOMP_WINDOW = '1920x1080'
$env:LARECOMP_INPUT = '200:RIGHT:6,220:A:6,300:A:6,400:A:6,700:A:6,900:A:6,1100:A:6,1300:A:6,1500:A:6,1700:A:6,1900:A:6,2100:A:6'
$env:LARECOMP_NO_USER_INPUT = '1'
$env:LARECOMP_WINDOW_SHOT = '4600:5000:400'
$env:LARECOMP_SHOT_PATH = 'C:/Users/Ronny/mk8-rtx/package/package2-%05d.png'
$env:LARECOMP_EXIT_AFTER_SHOT = '1'
$env:MK8_EXPERIMENTAL_NO_MII = '1'
$env:MK8_MII_ID = '00000000000000008000000000000000'
$env:MK8_MII_NAME = 'Player'
$env:MK8_MII_COLOR = '0'
$env:MK8_MII_GENDER = '0'
Set-Location $run
& "$run\mk8drecomp.exe" --play $data
```

Observed: exit `0`; the log reported `60.0 fps` around the capture and no
`GPUVM`, `DEVICE_LOST` or `FATAL`; `package2-05000.png` is 4,251,995 bytes at
1920x1080 and shows the Mario Kart Stadium race with the kart, world and HUD.
The 12 GB data root is the existing `C:/Users/Ronny/mk8-rtx/game/extracted`; it
was not copied into the package.

A 1280x720 capture from the fresh package tree (which has no `mk8d.ini`) showed
the race HUD over a black world. The same package renderer at 1920x1080 rendered
normally, and the existing `run-rtx` tree rendered at 1280x720 with its
`mk8d.ini` (`resolution = 75%`, `upscaler = fsr1`). The validated packaged boot
is therefore 1920x1080; 1280x720 is not claimed.

## Post-package in-engine checks

These checks used the existing `run-rtx` data/runtime and did not change the
package zip, `Launcher.exe` or the hashes above. Full logs and screenshots are
under `local/windows-rtx-check/` and the remote `run-rtx/` directory.

| Check | Result | Notes |
| --- | --- | --- |
| FSR 3.1.6 frame generation | **PASS** | `MK8_FSR_FG=fsr3` + signed `amd_fidelityfx_framegeneration_vk.dll`; swapchain 3.1.7, effect 3.1.6, first interpolated frame; exit 0; `fsrfg316-05000.png` |
| XeSS Super Resolution | **PARTIAL** | device extension/feature enabled, `SR context 1920x1080 -> 2560x1440`, `upscale active`, then `xessVKExecute -4`; exit 0; fallback; `xess2560-05000.png` |
| DLSS-G at 2560x1440 | **PASS** | depth resample 1152x648 to 2560x1440, `pair present`; exit 0; `dlssfg1152-05000.png` |

The XeSS run used the package host
`37af7b744a09e36dc5e55025e3cdd0a508afa0ea91517f28817fec14a6cc8479`;
FSR FG passed on that host and was repeated successfully on a depth-resample
host. At
1920x1080 with `LARECOMP_SCALE=1.0` the render is 1:1, so the actual XeSS
upscale was run at 2560x1440 with the same scale. The provider's exact failure
is `xessVKExecute failed -4 (invalid-argument)`; its diagnostic string names
`velocityTexture`, and a gated diagnostic build showed valid non-null
view/image, aspect 1, level 1, layer 1, 1920x1080, format 83.

The 2560x1440 DLSS-G pass uses `LARECOMP_SCALE=0.6` (1152x648 render) and the host built from
`runtime/mk8/graphics/fsr.cpp` with the display-size depth resample:
`99c88afdf8740af7bed9404d1c68bcf8cc3274d3328eeabff8f07e688bdfcfe0`
(4,529,152 bytes). **That host is not in the package zip above**; the package
still carries `37af7b74…`. To include the fix, rebuild the tree with
`--host-dir local/windows-rtx2/host-depth` (and regenerate the launcher payload)
and repeat the launcher/gate and boot checks. The source change is committed;
the shipped package hash must not be quoted for the 2560x1440 result.

## Files added or changed

- `toolchain/ultrawide.py`, `toolchain/ultrawide_hud.py`,
  `toolchain/settings_menu.py`: refreshed pinned hashes.
- `toolchain/host.py`, `toolchain/xess.py`: gated XeSS device-extension hook
  registered after the DLSS preparation; inert without `MK8_XESS`.
- `runtime/mk8/graphics/fsr.cpp`: display-size depth resample for the
  frame-generation inputs (2560x1440 DLSS-G).
- `tests/test_optional_guest_patches.py`: hash and anchor checks.
- `scripts/package-windows-portable.py`: cross-built portable tree and zip.
- `docs/WINDOWS-PACKAGING.md`, `docs/WINDOWS-INGAME-RTX.md`: this record.

`docs/pt-BR/log/` was not edited for this package.
