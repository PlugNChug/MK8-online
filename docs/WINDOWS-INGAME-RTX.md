# In-engine run on the Windows RTX 5080 — 20/11/2026

## Result

| Test | Result | Evidence |
| --- | --- | --- |
| Native Windows plain boot, RTX 5080 | **PASS** | 1280x720 race render; exit 0; `race-05000.png` |
| FSR 3.1.6 (AMD signed DLL) | **PASS** | `present 1920x1080 -> 2560x1440`, `provider 3.1.6`, `upscale active`; exit 0; `fsr-05000.png` |
| DLSS Super Resolution (`nvngx_dlss.dll` 58 MB set) | **PASS (with zero motion)** | `SuperSampling.Available = 1`, `upscale active`; exit 0; `dlss-05000.png` |
| DLSS Frame Generation (`MK8_DLSS_FG=1`) | **FAIL** | `c0000005` in `nvoglv64.dll` during swapchain creation; exit `-1073740791`; no `[dlss_fg] active` |
| Interactive session required | **No** | SSH session 0 created the Vulkan window and swapchain normally |

This is an in-engine render, not a full qualification. `gameplay_verified` stays false; audio,
controls, a complete race, restart/save and clock speed were not measured.

## Environment

- Windows 11 `10.0.26300.10066`, SSH session 0 (`Ronny@192.168.8.197`).
- NVIDIA GeForce RTX 5080, driver `32.0.16.2101`, Vulkan `1.4.357`.
- Code under test: `8eeb6ef` plus the documentation commits, HEAD `4fdd9f8`.
- Data root: `C:/Users/Ronny/mk8-rtx/game/extracted`, 12,149,826,670 bytes,
  6,597 files (`exefs`, `romfs`, `control.nacp`).
- Runtime directory: `C:/Users/Ronny/mk8-rtx/game/run-rtx`.

## Build

The Windows host and game library were cross-built with the pinned Zig 0.17.0
`x86_64-windows-gnu` target, from a fresh 4.0.1 generation of the current
recompiler (the `4318c07` sized `STT_NOTYPE` fix is included).

```sh
MK8_ZIG=$PWD/local/windows-rtx2/tools/zig-x86_64-linux-0.17.0/zig \
MK8_NATIVE_TOOLS=$PWD/local/windows-verify/native-tools \
SystemRoot=$PWD/local/windows-rtx2/fake-system \
python3 local/windows-rtx2/cross_build.py \
  local/windows-rtx2/prepared local/windows-rtx2/host 4
```

```text
Runtime x86_64-windows-gnu: 234/234 unidades
Executável do runtime ligado; gameplay ainda não verificado.
mk8drecomp.exe 4,525,568 bytes
SHA-256 1103cdbd5e928bc62e725aa75fbf2df6a04fa80aa21eb3d20f0fe4cb68f8bb7f
```

```sh
MK8_ZIG=… MK8_NATIVE_TOOLS=… MK8_JOBS=6 MK8_SKIP_OPTIONAL_GUEST_PATCHES=1 \
python3 local/windows-rtx2/cross_game.py \
  local/windows-rtx2/prepared local/windows-rtx2/generated-new \
  local/windows-rtx2/native main,sdk,subsdk0
```

```text
483 guest units, target x86_64-windows-gnu, -O1
mk8dgame.dll            535,040 bytes  SHA-256 fe818caef82bda000f3a0bbeef66d31bf9a862e0da76980a4113f82f3e5c211f
mk8d-code-main.dll   75,041,792 bytes
mk8d-code-sdk.dll    24,498,688 bytes  SHA-256 76509b620a7c3c31ba520391f77b27faa166fe431a144b4cc9b16640e431a9c5
mk8d-code-subsdk0.dll 19,187,712 bytes
```

`MK8_SKIP_OPTIONAL_GUEST_PATCHES=1` skips only the **optional ultrawide and
native-settings guest hooks**, whose pinned `main_077.c`/`main_164.c` hashes
predate the `4318c07` recompiler fix and therefore reject the fresh
generation. The race/render path, Yas0, cup relocation and no-Mii policy are
unchanged. The generated SDK table now contains `sdk_fn_count = 28234` and
`f_7004004c30`, so the old `no function at 7004004c30` FATAL does not occur.

## Boot and input setup

The data root has no Mii database. `MK8_EXPERIMENTAL_NO_MII=1` plus a
deterministic local Mii identity lets the game pass the Mii selection and reach
the main menu:

```powershell
$env:MK8_EXPERIMENTAL_NO_MII = '1'
$env:MK8_MII_ID = '00000000000000008000000000000000'
$env:MK8_MII_NAME = 'Player'
$env:MK8_MII_COLOR = '0'
$env:MK8_MII_GENDER = '0'
```

Race navigation used the presented-frame input script:

```text
200:RIGHT:6,220:A:6,300:A:6,400:A:6,700:A:6,900:A:6,1100:A:6,
1300:A:6,1500:A:6,1700:A:6,1900:A:6,2100:A:6
```

`LARECOMP_WINDOW_SHOT=4200:5000:400` and `LARECOMP_EXIT_AFTER_SHOT=1` capture
the settled race frame after the first-visit shader/pipeline work.

## Plain boot

An initial run with exactly the requested `LARECOMP_INPUT=400:A:6` and
`LARECOMP_WINDOW_SHOT=700:900:200` rendered the Mii selection screen at
1280x720 (`shot-00700.png`/`shot-00900.png`), not the race: this data root has no
Mii database, so the game asks for a Mii before the main menu. The local Mii
identity and the longer sequence below were needed to reach Grand Prix. The
same session-0 SSH process created the window and swapchain without a hidden
window or Wine.

```powershell
$env:LARECOMP_WINDOW = '1280x720'
$env:LARECOMP_INPUT = '200:RIGHT:6,...'
$env:LARECOMP_NO_USER_INPUT = '1'
$env:LARECOMP_WINDOW_SHOT = '4200:5000:400'   # first settled race frames
$env:LARECOMP_SHOT_PATH = 'C:/Users/Ronny/mk8-rtx/game/race-%05d.png'
$env:LARECOMP_EXIT_AFTER_SHOT = '1'
C:\Users\Ronny\mk8-rtx\game\run-rtx\mk8drecomp.exe --play C:\Users\Ronny\mk8-rtx\game\extracted
```

Exit code **0**. The race started at 1280x720:

```text
[vk] NVIDIA GeForce RTX 5080, swapchain 1280x720
[vk] t=4.0  frame 145  60.0 fps max 21ms level (...)
[vk] t=6.0  frame 265  60.0 fps max 18ms level (...)
...
[vk] t=80.1 frame 4678 57.3 fps max 129ms level (...)
[vk] t=82.1 frame 4798 60.0 fps max 20ms level (...)
[vk] t=84.1 frame 4917 59.3 fps max 35ms level (...)
[vk] saved C:/Users/Ronny/mk8-rtx/game/race-05000.png (1280x720)
```

`race-05000.png` shows Mario Kart Stadium, Mario on the kart, HUD coins 05,
lap 1/3 and position 12th. No FATAL, GPUVM or device-lost line. The first
frame after the loading screen was black while the first-visit pipelines
compiled; by frame 4200 the world rendered normally. This is the same
first-visit behavior documented for the other backends, not a failed render.

## FSR 3.1.6

Only the window size changed (the game image is 1920x1080, so the upscaler has
a real 1920x1080 -> 2560x1440 job):

```powershell
$env:LARECOMP_WINDOW = '2560x1440'
$env:MK8_FSR4 = '1'
$env:MK8_FSR_VERSION = '3.1.6'
$env:MK8_FSR_LIB = 'C:\Users\Ronny\mk8-rtx\amd\amd_fidelityfx_upscaler_vk.dll'
# same input, shots 4600/4800/5000
```

Exit code **0**; log:

```text
[fsr] present 1920x1080 -> 2560x1440 color 1920x1080
[fsr] selecting version 3.1.6 (0xf5a5ca1e00c01006)
[fsr] provider 3.1.6
[fsr] upscale active 1920x1080 -> 2560x1440 (depth neutral, motion neutral)
[fsr] cpu dispatch 0.02 ms
[vk] saved .../fsr-05000.png (2560x1440)
```

`fsr-05000.png` is a correct 2560x1440 race frame. Depth and motion were the
neutral inputs in this run; the FSR bridge logs that state rather than claiming
camera motion. No device loss.

## DLSS Super Resolution

`nvngx_dlss.dll` (DLSS 5 set, 58,956,912 bytes) and
`nvngx_dlssg.dll` (7,506,544 bytes) were copied next to `mk8drecomp.exe`.

```powershell
$env:LARECOMP_WINDOW = '2560x1440'
$env:MK8_DLSS = '1'
$env:MK8_DLSS_LIB = 'C:\Users\Ronny\mk8-rtx\game\run-rtx\nvngx_dlss.dll'
# same race input and captures
```

Exit code **0**; log:

```text
[dlss] VK_KHR_get_physical_device_properties2 enabled
[dlss] NGX device extensions enabled: VK_NVX_binary_import,
       VK_NVX_image_view_handle, VK_KHR_buffer_device_address,
       VK_KHR_push_descriptor
[dlss] feature requirements: supported (min GPU arch 0x160, min OS 10.0.0)
[dlss] NVSDK_NGX_VULKAN_Init_ProjectID: 0x00000001 (success)
[dlss] SuperSampling.Available = 1
[dlss] NVSDK_NGX_VULKAN_CreateFeature: 0x00000001 (success)
[dlss] NVSDK_NGX_VULKAN_EvaluateFeature: 0x00000001 (success)
[dlss] upscale active 1920x1080 -> 2560x1440
       (quality quality, depth neutral, motion zeros, jitter 0.000,0.000)
```

`dlss-05000.png` is a correct 2560x1440 race frame. The bridge warns that motion
vectors are zeros because the camera view/projection were not captured in this
run; quick-moving karts can ghost. That is an input-quality limitation, not an
NGX failure. The task's requirement — device extensions, availability,
feature creation and evaluation — passed.

## DLSS Frame Generation — crash

```powershell
$env:MK8_DLSS_FG = '1'
$env:DISABLE_RTSS_LAYER = '1'
$env:DISABLE_VULKAN_OBS_CAPTURE = '1'
$env:VK_LOADER_LAYERS_DISABLE = '~implicit~'
# `nvngx_dlssg.dll` beside the executable, same 2560x1440 race input
```

Exit code **-1073740791** (`0xC0000409`, fail-fast). Log:

```text
[dlss] VK_KHR_get_physical_device_properties2 enabled
[dlss_fg] VK_KHR_present_id enabled
[dlss_fg] VK_NV_low_latency2 enabled
[dlss] NGX device extensions enabled: ...
*** exception c0000005 accessing 0x1
    host pc ... (nvoglv64.dll+e0e900)
    host backtrace ... vulkan-1.dll+52080 ... nvoglv64.dll+dfb8e6 ...
```

No `[dlss_fg] active`, no EvaluateFeature and no screenshot: the process dies
while creating the swapchain, before the two-image present path runs. Disabling
the RTSS and OBS implicit Vulkan layers removed `RTSSVkLayer64.dll` from the
stack but did **not** change the crash; the fault remains inside the NVIDIA
Vulkan driver (`nvoglv64.dll`). This is a real blocker for DLSS-G on this
driver/machine, not a captured "no device loss" success.

## What remains

- Fix or isolate the DLSS-G swapchain crash (driver `32.0.16.2101` +
  `VK_NV_low_latency2`), then run the two-image present/evaluation.
- Qualify the race with audio, controls, clock speed, a full lap/cup and a
  clean restart/save.
- Re-enable the optional ultrawide/native-settings guest hooks after their
  pinned hashes are updated for the `4318c07` generation.
- A real Windows package (`package-launcher.py`/`release.py`) and WebView2
  window were not part of this run.

## Evidence

Local (ignored, not committed): `local/rtx-ingame/race-05000.png`,
`fsr-05000.png`, `dlss-05000.png`, `local/rtx-ingame/logs/{race,fsr,dlss,dlssfg}.err`
and the matching `.exit` files. Remote originals remain under
`C:/Users/Ronny/mk8-rtx/game/` and `C:/Users/Ronny/mk8-rtx/game/run-rtx/`.
No binaries, screenshots or game data are committed.
