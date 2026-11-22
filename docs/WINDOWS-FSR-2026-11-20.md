# Windows FSR path under Wine — 20/11/2026

This report answers one question: does the Windows FSR path that the launcher configures work on
this Linux/AMD host under Wine, using AMD's signed Vulkan DLLs from the pinned FidelityFX SDK
2.4.0 archive? The runtime is the Windows `mk8drecomp.exe` cross-built from `feature/fsr`; the
environment is exactly the one `apps/launcher/src/fsr/mod.rs` sets for Windows.

## Result in one table

| Check | Result | Evidence |
| --- | --- | --- |
| Windows runtime host builds from this tree (Zig `x86_64-windows-gnu`) | **PASS** | 225/225 units; `mk8drecomp.exe` 4,430,848 B, SHA-256 `c3f12c45…81e0d` |
| Signed SDK 2.4.0 DLLs match the launcher pins | **PASS** | upscaler `4a7e895a…dab7`, framegeneration `d002dabf…98b4`, loader `38a56a01…bab9` |
| Wine 11.22 reaches the first frame with this host build | **PASS** | `[vk] t=9.1 frame 7 …` (baseline log) |
| FSR 4.1.2 upscaling under Wine (launcher env) | **PASS** | `[fsr] selecting version 4.1.2`, `[fsr] provider 4.1.2`, `[fsr] upscale active 1920x1080 -> 2560x1440`; `win-fsr412-wine.png` |
| FSR 3.1.6 upscaling under Wine (launcher env) | **PASS** | `[fsr] provider 3.1.6`, `upscale active 1920x1080 -> 2560x1440`; `win-fsr316-wine.png` |
| FSR 4.1.2 through the title → Mii menu transition, 900 frames | **PASS** | `[input] id 0 buttons 0x40`; `win-fsr412-menu-00450.png`, `-00900.png`; no GPUVM/device-lost/FATAL |
| FSR 4.1.2 under Proton-GE 11-12 | **PASS** | `[fsr] provider 4.1.2`, `upscale active 1920x1080 -> 2560x1440`; `win-fsr412-proton2.png` |
| The signed framegeneration DLL loads and reports 3.1.7 / 4.0.2 | **PASS** | `[fsr_fg] swapchain provider offers 3.1.7`; `MLFI effect provider offers 4.0.2` (with a device) |
| FSR 3 frame generation reaches a first interpolated frame under Wine | **FAIL (host driver)** | process dies in `vkCreateComputePipelines` while the provider compiles `mlfi_4k_pass151`; no `[fsr_fg] first interpolated frame` line |
| FSR 4 MLFI (`MK8_FSR_FG=fsr4`) under Wine | **FAIL (same host driver fault)** | the DLL offers 4.0.2, selects it, then hits the same `mlfi_4k_pass151` crash |
| Real Windows (AMD driver, MSVC, WebView2) | **NOT RUN** | no Windows host here; this is Wine/RADV only |
| Launcher download/terms flow and WebView2 window | **NOT RUN** | only the DLL hashes were checked against the launcher's pins |

`gameplay_verified` stays **false**. No race, restart/save, audio output, physical controller or
clock-speed comparison was attempted. Frame generation is **not** claimed working: no interpolated
frame was produced on this host.

## Environment

| item | value |
| --- | --- |
| Verified code commit | `72c5e5babad7f75245042becab83a6e7752a8e2e` (`feature/fsr`, the HEAD named for this task) |
| Repository state after the session | a concurrent upstream merge moved the worktree from `72c5e5b` to `fc9efb2`; `git diff 72c5e5b..fc9efb2` over `runtime/mk8/graphics/fsr*`, `runtime/mk8/graphics/ffx`, `toolchain/fsr*.py` and `apps/launcher/src/fsr` is empty |
| Report/script commit | `2441ffcfb15c837c1140727aa9283ee0d1c18e68` (adds this file and `scripts/verify-windows-fsr.sh`) |
| Host | Linux x86-64, kernel `7.4.0-rc1-2-cachyos-rc` |
| GPU / driver | AMD Radeon RX 9070 XT (RADV GFX1201), Vulkan 1.4.363, Mesa `27.0.0-devel` (git `f7dd61f78f`) |
| Wine | `wine-11.22` |
| Proton | GE-Proton11-12 (`Proton-GE Latest`) |
| Cross compiler | Zig `0.17.0` for Linux, target `x86_64-windows-gnu`, `-mcpu=x86_64_v2` |
| Windows deps | the pinned UCRT64 set fetched by `scripts/setup-runtime-deps.py` (SDL3 3.4.18, Vulkan 1.4.363, shaderc 2026.4, FFmpeg 9.0.2-2) |

Mesa `27.0.0-devel` at `f7dd61f78f` is the exact commit in
[`RADV-MLFI-CRASH.md`](RADV-MLFI-CRASH.md); that document's RADV defect is what blocks frame
generation below.

## Build

The worktree was exported cleanly (`git archive 72c5e5b`) to `$HOME/mk8-win-fsr/src-head`, so the
in-progress merge in the shared checkout could not leak into the binary. The prepared runtime and
the host build use the project's own toolchain patches:

```sh
python3 runtime/prepare.py --upstream upstream/smm2recomp \
  --output $HOME/mk8-win-fsr/job2/runtime --profile games/mk8d/profiles/4.0.1.json

MK8_ZIG=<zig-0.17.0>/zig python3 $HOME/mk8-win-fsr/cross_win.py \
  $HOME/mk8-win-fsr/job2/runtime $HOME/mk8-win-fsr/job2/host-win 8
```

`cross_win.py` is the same verification driver the earlier Windows agent used
(`local/cross_build.py` in the `MK8-online-windows-verify` worktree): it sets `native.WINDOWS`,
`TARGET = x86_64-windows-gnu`, the Zig compiler and then calls `host.build_host`. It lives under
`$HOME`, not in the repository.

Build tail:

```text
Runtime x86_64-windows-gnu: 225/225 unidades
Executável do runtime ligado; gameplay ainda não verificado.
host built: /home/mitchell/mk8-win-fsr/job2/host-win/mk8drecomp.exe (4430848 bytes)
```

```text
PE32+ executable for MS Windows 6.00 (console), x86-64, 10 sections
SHA-256 c3f12c45c6e09e0938fbca4365842cbfc03e63296d75d692fd14eb6bcb681e0d
```

The build includes the FSR bridge (`[fsr] upscale active`, `[fsr] selecting version`), the FSR
frame-generation bridge (`swapchain provider`, `first interpolated frame`) and the gated
`shaderStorageImageWriteWithoutFormat` / `VK_KHR_compute_shader_derivatives` device features
(`runtime/nvn/vulkan/device.c`). The guest DLLs are the fixed Windows build from
[`WINDOWS-BOOT-2026-11-20.md`](WINDOWS-BOOT-2026-11-20.md); FSR is host-side, so no guest rebuild
is required:

| file | bytes | SHA-256 |
| --- | ---: | --- |
| `mk8dgame.dll` | 535,040 | `a56b2263277c5f78df84f908cd244a5dbd09f487b87b6b81c7b170146a6e0468` |
| `mk8d-code-main.dll` | 75,041,792 | `84a1592b154041a55dd411eb081d749eb74ab4f9e0ce44474f53e38798d08fdc` |
| `mk8d-code-sdk.dll` | 24,498,688 | `0d80585d17c978423ca69aac10082e15b8e8f025bd2ed8ae783cf29b77153cb0` |
| `mk8d-code-subsdk0.dll` | 19,187,712 | `cceea3cc74910b779f92fcd858cc8d5477bc16353ce785f1e4cec203078b02a9` |

## AMD's signed DLLs

Extracted from the pinned archive
`~/.cache/mk8online/sdk-cache/FidelityFX-SDK-v2.4.0.tar.gz` (253,592,010 B, SHA-256
`111f9b19c0c9b8a1f24d2a5104095b8e626f12deb4eca53cd0c29921aa1fd43a`), member
`Kits/FidelityFX/signedbin/`. They stay under `$HOME/mk8-win-fsr/fsr/`, never in the repository.

| file | bytes | SHA-256 | launcher pin |
| --- | ---: | --- | --- |
| `amd_fidelityfx_loader_vk.dll` | 39,184 | `38a56a01fb3565c0757d9f5c1e4e69de4b65e5f2e5769b323b200dc1f142bab9` | match |
| `amd_fidelityfx_upscaler_vk.dll` | 36,135,696 | `4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7` | match |
| `amd_fidelityfx_framegeneration_vk.dll` | 36,806,928 | `d002dabff8c5e71eaa715fc99438cd4dc81230262a7897b16bc402efec3498b4` | match |

The launcher's `SIGNED_DLLS` table in `apps/launcher/src/fsr/install.rs` is the source of those
sizes and hashes. The loader DLL is not loaded by the runtime; both provider DLLs export the five
FFX entry points directly (`ffxCreateContext`, `ffxDestroyContext`, `ffxConfigure`, `ffxQuery`,
`ffxDispatch`).

## Upscaling runs (Wine 11.22)

The launcher's Windows upscaling environment is `MK8_FSR4=1`, `MK8_FSR_VERSION=<3.1.6|4.1.2>`,
`MK8_FSR_LIB=<amd_fidelityfx_upscaler_vk.dll>`. No `MK8_FSR_LEGACY_ABI` is set on Windows.
A 2560x1440 swapchain is required because the game's window image is 1920x1080: only then is
`ow > b->width + 1` true and the FSR bridge is entered. The host display is 1440x900, so the runs
use Wine's virtual desktop (`wine explorer /desktop=…,2560x1440`); the runtime still creates a
2560x1440 swapchain and writes its own screenshot at that size.

### 4.1.2

```sh
cd $HOME/mk8-win-fsr/run-win
DISPLAY=:0 WINEPREFIX=$HOME/mk8-win-boot/wineprefix \
  MK8_EXPERIMENTAL_NO_MII=1 WINEDEBUG=-all \
  MK8_FSR4=1 MK8_FSR_LIB='Z:\home\mitchell\mk8-win-fsr\fsr\amd_fidelityfx_upscaler_vk.dll' \
  MK8_FSR_VERSION=4.1.2 LARECOMP_WINDOW=2560x1440 \
  LARECOMP_WINDOW_SHOT=240 \
  LARECOMP_SHOT_PATH='Z:/home/mitchell/mk8-win-fsr/win-fsr412-wine.png' \
  LARECOMP_EXIT_AFTER_SHOT=1 timeout 300 \
  wine explorer /desktop=fsr,2560x1440 ./mk8drecomp.exe --play
```

Exit 0. The same environment run by `scripts/verify-windows-fsr.sh` kept the runtime log at
`script-run3/runtime.log`; the lines are:

```text
[vk] AMD Radeon RX 9070 XT (RADV GFX1201), swapchain 2560x1440
[fsr] present 1920x1080 -> 2560x1440 color 1920x1080
[fsr] selecting version 4.1.2 (0xf5a5ca1e01001002)
[fsr] provider 4.1.2
[fsr] depth now neutral (1920x1080 fmt 126)
[fsr] depth source neutral 1920x1080 fmt 126
[fsr] motion now neutral
[fsr] upscale active 1920x1080 -> 2560x1440 (depth neutral, motion neutral) color 00007F08400232C0 output 00007F0840113900
[fsr] view/proj captured in main draw
[fsr] cpu dispatch 0.15 ms
```

Screenshot: `win-fsr412-wine.png` (manual run) and `script-run3/fsr-4.1.2.png` (script run), 2560x1440, the Mario Kart 8 Deluxe title with
`Press L R to start` and `Ver. 4.0.1`. No `GPUVM fault`, `VK_ERROR_DEVICE_LOST`, `SyncWait` or
`FATAL` line in the log.

### 3.1.6

Same command with `MK8_FSR_VERSION=3.1.6`:

```text
[fsr] selecting version 3.1.6 (0xf5a5ca1e00c01006)
[fsr] provider 3.1.6
[fsr] upscale active 1920x1080 -> 2560x1440 (depth neutral, motion neutral) color 00007F4C7C0232C0 output 00007F4C7C113900
[fsr] cpu dispatch 0.07 ms
```

Screenshot: `win-fsr316-wine.png` (manual run), same title frame; the log above is the
`script-run316c` copy. Exit 0. The script run at frame 240
(`script-run316c/runtime.log`) shows the title with its light-ray transition; one earlier 3.1.6
capture at frame 240 caught an almost white title transition, and the frame-480 capture
(`script-run316b`) is the clean Mii menu. No run logged a fault.

### Title → Mii menu, 900 frames

```sh
LARECOMP_INPUT='300:L:6' LARECOMP_WINDOW_SHOT=450:900:450 \
LARECOMP_SHOT_PATH='Z:/home/mitchell/mk8-win-fsr/win-fsr412-menu-%05d.png' \
LARECOMP_EXIT_AFTER_SHOT=1 timeout 420 \
wine explorer /desktop=fsr412menu,2560x1440 ./mk8drecomp.exe --play
```

```text
[input] id 0 buttons 0x40 (sampling 310, lr 70005fc0f0)
[fsr] upscale active 1920x1080 -> 2560x1440 (depth neutral, motion neutral) …
[fsr] cpu dispatch 0.21 ms
[vk] saved …/win-fsr412-menu-00450.png (2560x1440)
[fsr] cpu dispatch 0.18 ms
[vk] saved …/win-fsr412-menu-00900.png (2560x1440)
```

Both screenshots show **Select a Mii** over the upscaled title background; this data root has no
Mii resource, so the game asks for a Mii before the main menu. The upscaler stayed active across
the scene change. No fault line in the 900-frame log.

### Proton-GE 11-12

The same 4.1.2 environment under `Proton-GE Latest` with a 2560x1440 Wine desktop (set in the
Proton prefix's `HKCU\Software\Wine\Explorer` keys):

```text
[vk] AMD Radeon RX 9070 XT (RADV GFX1201), swapchain 2560x1440
[fsr] selecting version 4.1.2 (0xf5a5ca1e01001002)
[fsr] provider 4.1.2
[fsr] upscale active 1920x1080 -> 2560x1440 …
[vk] saved Z:/home/mitchell/mk8-win-fsr/win-fsr412-proton2.png (2560x1440)
```

Screenshot: `win-fsr412-proton2.png`, the same title. Proton's process exited 0 after the capture.

## Frame generation (the signed DLL loads, then the RADV shader defect stops it)

The launcher's Windows frame-generation environment is `MK8_FSR_FG=fsr3` and
`MK8_FSR_FG_LIB=<amd_fidelityfx_framegeneration_vk.dll>`; for `fsr4` it would also need
`MK8_FSR_MLFI_LIB`. The runtime requires the swapchain provider to answer exactly **3.1.7**.

### The DLL is reachable

```text
[fsr_fg] present/acquire queue family 1 (4 queues)
[fsr_fg] formatless storage images: write 1 read 1
[fsr_fg] provider loaded: Z:\home\mitchell\mk8-win-fsr\fsr\amd_fidelityfx_framegeneration_vk.dll
[fsr_fg] swapchain provider offers 3.1.7
[fsr_fg] swapchain provider 3.1.7 (version 0xc01007)
[fsr_fg] present mode 1 replaced by FIFO so the frame-interpolation swapchain paces itself
```

With `MK8_FSR_FG=fsr4` and the same DLL as the MLFI effect library, the runtime selects 4.0.2:

```text
[fsr_fg] mode fsr4: 3.1.7 frame-interpolation swapchain + MLFI 4.0.2 effect
[fsr_fg] MLFI effect provider loaded: …amd_fidelityfx_framegeneration_vk.dll (stats export absent)
[fsr_fg] MLFI effect provider offers 4.0.2 (id 0xf600000001000002)
[fsr_fg] MLFI effect provider offers 3.1.6 (id 0xf600000000c01006)
[fsr_fg] MLFI effect provider 4.0.2 (id 0xf600000001000002)
```

This is the same `0xF600000001000002` id the native MLFI provider reports in
[`MLFI-PROVIDER.md`](MLFI-PROVIDER.md). The signed DLL therefore contains a selectable 4.0.2 MLFI
provider; the launcher's `Settings::validate` still rejects the `fsr4` choice unconditionally (it
does not offer the MLFI provider), even though this signed DLL exposes the 4.0.2 version.

### The failure

Immediately after `present mode … replaced`, the process dies. `WINEDEBUG=-all,+vulkan` records:

```text
[fsr_fg] present mode 1 replaced by FIFO so the frame-interpolation swapchain paces itself
0024:err:vulkan:vkCreateComputePipelines Exception 0xc0000005 in Unix call.
```

+seh locates the fault in `winevulkan.dll` (return address `winevulkan+0x108d9`), i.e. the provider
called a Vulkan entry and the Unix-side driver faulted. `MESA_SPIRV_FAIL_DUMP_PATH` dumps the exact
module RADV rejected:

```text
$ sha256sum $HOME/mk8-win-fsr/spv-fail/0xfail.comp
19f393a5c4a4724f9cef56a0e6c71a6f6477267f7fd782eac660da342f951ace
$ spirv-dis 0xfail.comp | grep OpEntryPoint
OpEntryPoint GLCompute %mlfi_4k_pass151 "mlfi_4k_pass151" ...
```

That is `mlfi_4k_pass151`, 2,648 bytes at file offset `0x727830` in the DLL — the exact module
documented in [`RADV-MLFI-CRASH.md`](RADV-MLFI-CRASH.md) §2: a 16-bit image sampled type that
Mesa's SPIR-V front end rejects, after which RADV dereferences the NULL and segfaults instead of
returning `VK_ERROR_UNKNOWN`. The FFX 3.1.7 swapchain provider compiles this MLFI pass during
context creation even for `MK8_FSR_FG=fsr3`, so both `fsr3` and `fsr4` stop at the same place.

Proton-GE 11-12 reproduces it identically (`exit=3`, same last line). No `[fsr_fg] first
interpolated frame recorded` line exists in any run.

### Isolation experiment (local patched DLL, not the shipped path)

To separate the runtime from the driver, three offending modules were rewritten with the project's
`tools/radv-mlfi/fix_image_types.py`, stripped of debug names, padded with `OpNop` to their original
size and patched into a **copy** of AMD's DLL under `$HOME` (the shipped file was not modified):

| module | original | rewritten+stripped | result |
| --- | ---: | ---: | --- |
| `mlfi_4k_pass151` | 2,648 | 2,420 | accepted |
| `mlfi_4k_pass152` | 18,440 | 17,420 | accepted |
| `mlfi_4k_pass190` | 18,264 | 17,172 | accepted |
| `main` (preprocess) | 5,004 | — | still rejected: a valid 64-bit (`%ulong`, `R64ui`) sampled image, which Mesa cannot represent |

With the first three patched, the crash moved from `pass151` to `pass152`, then `pass190`, then the
5,004-byte preprocess `main`. The preprocess workaround in `RADV-MLFI-CRASH.md` changes the sampled
image to a **storage** image and requires the provider to create the view with `STORAGE` usage and
`R64_UINT`; that cannot be done by replacing the shader blob alone. The patched-DLL run therefore
also ends in `vkCreateComputePipelines`, and is reported only as evidence that the fault is the
documented RADV/Mesa limitation, not a runtime-side FFX usage error.

The patched copy's SHA-256 was `b2d944730301d5e2e99469efe22ca4eae169d6a0cc98426350f58293b6e07b65` for the three-module version; it is not shipped,
not installed and not used for the upscaling results.

### Fail-closed on a device without the second queue family

Forcing llvmpipe (`LARECOMP_GPU=1`) gives a clean refusal instead of a crash:

```text
[fsr_fg] disabled: no separate present queue family with two queues
[fsr_fg] disabled: no separate present/acquire queue family
[vk] saved Z:/home/mitchell/mk8-win-fsr/win-fg-llvmpipe.png (1280x720)
```

The process exits 0 after the capture. The runtime does not fake an active swapchain when the
provider cannot run.

## Standalone FFX probes (isolation, not part of the product)

Two small Windows programs were cross-compiled with Zig and run under Wine. They are under
`$HOME/mk8-win-fsr/probe/`, not in the repository.

- `ffx_versions.exe <dll>` calls `ffxQuery(GET_VERSIONS)` with **no device** (`device = NULL`).
  The upscaler answers only `3.1.6` and `2.3.4`; the frame-generation DLL answers `3.1.6` for
  `FRAMEGENERATION` and `3.1.7` for `FGSWAPCHAIN_VK`.
- `ffx_versions_vk.exe <dll>` creates a bare Vulkan device on the Wine RADV device and passes
  `VkDeviceContext` in the query. The upscaler now answers **`4.1.2` (`0xf5a5ca1e01001002`),
  `3.1.6`, `2.3.4`**; the frame-generation DLL answers **`4.0.2` (`0xf600000001000002`), `3.1.6`**
  and **`3.1.7`** for the swapchain. This explains why the runtime (which passes the device) selects
  4.1.2 while a device-less query does not.

```text
gpu 0: AMD Radeon RX 9070 XT (RADV GFX1201) (api 1.4.363)
UPSCALE        with device: rc=0 count=3 [0] 4.1.2 (0xf5a5ca1e01001002) [1] 3.1.6 … [2] 2.3.4 …
FRAMEGEN       with device: rc=0 count=2 [0] 4.0.2 (0xf600000001000002) [1] 3.1.6 …
FGSWAPCHAIN_VK with device: rc=0 count=1 [0] 3.1.7 (0xf65d564b00c01007)
```

The probes only query versions; the actual 4.1.2/3.1.6 dispatch, readback and screenshot come from
the in-game runs above.

## Reproduction

- `scripts/verify-windows-fsr.sh` runs the launcher's Windows environment and checks the expected
  log lines and screenshot. It verifies the signed DLL sizes/hashes first, uses a 2560x1440 Wine
  desktop, and exits 0 only when the upscale line and a non-empty screenshot exist. In `--fg` mode
  it exits 2 when the DLL was loaded and queried but no interpolated frame was produced.
- The script was run on this host: FSR 4.1.2 → exit 0 (`script-run3/fsr-4.1.2.png`, 9,759,905 B);
  `--fg fsr3` → exit 2 with the `[fsr_fg]` lines above.
- The clean-export build driver and the probes are scratch files under `$HOME/mk8-win-fsr/`; they
  are not committed because they depend on the private SDK archive and the Windows data root.

```sh
# upscaling (from the repository)
DISPLAY=:0 WINEPREFIX=<prefix> scripts/verify-windows-fsr.sh \
  --runtime <run dir with mk8drecomp.exe and the game DLLs> \
  --data <data root containing extracted/> \
  --fsr-dir <dir with AMD's signed DLLs> --version 4.1.2 --out <evidence dir>

# frame generation (expected to exit 2 on this RADV host)
DISPLAY=:0 WINEPREFIX=<prefix> scripts/verify-windows-fsr.sh \
  --runtime <run dir> --data <data root> --fsr-dir <DLL dir> \
  --fg fsr3 --frame 60 --timeout 150 --out <evidence dir>
```

Evidence files referenced here live under `$HOME/mk8-win-fsr/`:
`host-build2.log`, `baseline-wine3.log`, `script-run3/runtime.log`, `script-run316c/runtime.log`,
`script-run316b/runtime.log`, `fsr412-menu900-runtime.log`, `fsr412-proton-runtime.log`, `fgfsr3-vk.log`,
`fgfsr3-mod.log`, `spv-fail/0xfail.comp`, `fgfsr4-fallback.log`, `fg-llvmpipe.log`,
`probe-*-versions.log`, `win-fsr412-wine.png`, `win-fsr316-wine.png`,
`win-fsr412-menu-00450.png`, `win-fsr412-menu-00900.png`, `win-fsr412-proton2.png`,
`win-fg-llvmpipe.png`.

## What is verified, what is not

Verified on this host:

- The Windows host build from `72c5e5b` includes the FSR bridge and runs under Wine 11.22 and
  Proton-GE 11-12.
- The launcher's signed DLLs have the pinned sizes/hashes.
- The runtime selects and dispatches FSR 4.1.2 and FSR 3.1.6 for a 1920x1080 → 2560x1440 upscale,
  with the launcher's exact environment, in the title and in the Mii menu, under Wine (both
  versions) and Proton-GE (4.1.2).
- The runtime logs the provider version, the upscale dimensions and the frame screenshot; the
  screenshot dimensions (2560x1440) match the swapchain, not the game image (1920x1080).
- The frame-generation DLL is loadable under Wine, reports 3.1.7 / 4.0.2, and the runtime selects
  4.0.2 for `MK8_FSR_FG=fsr4`.
- Frame generation does **not** reach a first interpolated frame under RADV; the failure is the
  documented `mlfi_4k_pass151` RADV crash (Mesa `f7dd61f78f`), reproduced with Wine and Proton.

Not verified / not claimed:

- **Real Windows.** No AMD Windows driver, MSVC build, WebView2 window or Windows installer was
  run. The launcher's DLL download/terms flow was only hash-checked.
- **Frame generation on real Windows.** AMD's driver may compile the MLFI shaders; that is not
  established here. Do not describe `MK8_FSR_FG=fsr3` as working on Windows from this report.
- **Gameplay qualification.** No race, restart/save, audio output, physical controller, clock
  speed or two-PC multiplayer. `gameplay_verified` remains false.
- **Quality/performance.** The upscaled screenshots are visual evidence of the path, not a quality
  comparison; the Wine desktop and the loaded host are not an FPS or latency measurement.
- **Depth/motion.** The runs above use the default neutral depth and motion vectors. The
  diagnostic `MK8_FSR4_CAPTURE=1` path was not exercised under Windows.
- **The patched DLL experiment** is not a product artifact and does not make the shipped FG path
  work on this host.
