# In-engine run on the Windows RTX 5080 — 20/11/2026

## Result

| Test | Result | Evidence |
| --- | --- | --- |
| Native Windows plain boot, RTX 5080 | **PASS** | 1280x720 race render; exit 0; `race-05000.png` |
| FSR 3.1.6 (AMD signed DLL) | **PASS** | `present 1920x1080 -> 2560x1440`, `provider 3.1.6`, `upscale active`; exit 0; `fsr-05000.png` |
| DLSS Super Resolution (`nvngx_dlss.dll` 58 MB set) | **PASS (captured depth/motion)** | `SuperSampling.Available = 1`, `depth source: captured scene depth`, `motion vectors: camera-derived`; exit 0; `dlssmove-05000.png` |
| DLSS Frame Generation (`MK8_DLSS_FG=1`) | **PASS with workaround** | latency chain skipped; `[dlss_fg] active`, `EvaluateFeature` success, `pair present` real+interpolated; exit 0; `dlssfgalone-05000.png` |
| XeSS Super Resolution (`MK8_XESS=1`) | **PARTIAL** | device extension/feature enabled and `upscale active`, then `xessVKExecute failed -4 (invalid-argument)`; exit 0; `xess2560-05000.png` |
| FSR 3.1.6 frame generation (`MK8_FSR_FG=fsr3`) | **PASS** | swapchain provider 3.1.7, effect provider 3.1.6, first interpolated frame; exit 0; `fsrfg316-05000.png` |
| DLSS-G at 2560x1440 with depth resample | **PASS** | `depth resampled 1152x648 -> 2560x1440`; `pair present` real+interpolated; exit 0; `dlssfg1152-05000.png` |
| Interactive session required | **No** | SSH session 0 created the Vulkan window and swapchain normally |

This is an in-engine render, not a full qualification. `gameplay_verified` stays false; audio,
controls, a complete race, restart/save and clock speed were not measured.

## Environment

- Windows 11 `10.0.26300.10066`, SSH session 0 (`Ronny@192.168.8.197`).
- NVIDIA GeForce RTX 5080, driver `32.0.16.2101`, Vulkan `1.4.357`.
- Code under test: `8eeb6ef` plus the documentation commits; this round starts at
  `1fa7ca3` with `1a78c6d` (XeSS device hook) and `5014ed7` (hash/package
  refresh), plus the DLSS-G depth-resample change described below.
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
SHA-256 11b9f002c110468308f7651d794194b95902986d2880207b014041a6bb82f07b
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

## DLSS Super Resolution with real depth/motion

`nvngx_dlss.dll` (DLSS 5 set, 58,956,912 bytes) and
`nvngx_dlssg.dll` (7,506,544 bytes) were copied next to `mk8drecomp.exe`.
`MK8_FSR4=1` enables the shared camera/depth capture; `MK8_FSR4_CAPTURE=1`
records the scene-depth copy. `MK8_DLSS=1` then selects the NGX bridge.

```powershell
$env:LARECOMP_WINDOW = '2560x1440'
$env:MK8_DLSS = '1'
$env:MK8_DLSS_LIB = 'C:\Users\Ronny\mk8-rtx\game\run-rtx\nvngx_dlss.dll'
$env:MK8_FSR4 = '1'
$env:MK8_FSR4_CAPTURE = '1'
# same race input and captures
```

Exit code **0**:

```text
[dlss] NGX device extensions enabled: VK_NVX_binary_import,
       VK_NVX_image_view_handle, VK_KHR_buffer_device_address,
       VK_KHR_push_descriptor
[dlss] SuperSampling.Available = 1
[dlss] NVSDK_NGX_VULKAN_CreateFeature: 0x00000001 (success)
[dlss] NVSDK_NGX_VULKAN_EvaluateFeature: 0x00000001 (success)
[dlss] upscale active 1920x1080 -> 2560x1440
       (quality quality, depth neutral, motion zeros, jitter 0.000,0.000)
...
[dlss] depth source: captured scene depth 1920x1080 fmt 100
[dlss] motion vectors: camera-derived
```

The one-time `upscale active` line is emitted at the first evaluation, before
the main-draw capture exists; it is stale for this run. The state-change lines
after the first captured frame prove the real inputs. A temporary diagnostic
(removed again) logged the actual Halton jitter passed to NGX:

```text
[dlss] jitter 0.1250,2.6667
[dlss] jitter 1.0000,1.0000
[dlss] jitter 0.5000,2.0000
[dlss] jitter 1.5000,0.3333
...
```

`dlssmove-05000.png` is a correct 2560x1440 race frame with captured depth and
camera-derived motion. The start-grid frame is nearly static, so it does not
show a dramatic ghosting difference from the earlier zero-motion screenshot;
the player kart and track are sharp in both. A moving-camera A/B remains the
right visual test; the inputs are now real rather than the neutral/zero
fallback.

## DLSS Frame Generation — latency-chain crash and workaround

The failure was isolated with a minimal Windows Vulkan swapchain harness
(`local/rtx-harness/swapchain_probe.c`, 13.5 KB source; not committed). It
creates a hidden Win32 window, a surface, a device and a swapchain in the same
session-0 SSH desktop that runs the game.

| Variant | Device extensions / create chain | Result |
| --- | --- | --- |
| 1 plain | `VK_KHR_swapchain` | PASS |
| 2 | + `VK_KHR_present_id` | PASS |
| 3 | + `VK_NV_low_latency2`, `VkSwapchainLatencyCreateInfoNV` TRUE | PASS |
| 4 | + `VK_NV_low_latency2` only | PASS |
| 5 | + `VK_KHR_present_id`, `VkPresentIdKHR` at present | PASS |
| 6 | present_id + low_latency, no latency struct, `vkSetLatencySleepModeNV` | PASS, sleep rc=0 |
| 7/8 | as 3/6 with `VkPhysicalDevicePresentIdFeaturesKHR` | PASS |
| 9 | latency struct + `SAMPLED|TRANSFER_SRC|TRANSFER_DST` usage | PASS |
| 10 | latency struct + non-null `oldSwapchain` | PASS |
| 11 | sampled usage + `oldSwapchain` + latency struct | PASS |
| 12 | low_latency only + latency struct + sampled usage | PASS |
| 13 | present_id + low_latency + sampled + old + latency | PASS |
| 14 | variant 13 at a larger window (session-0 client 1028x732) | PASS |
| 15/17 | latency struct with `latencyModeEnable = VK_FALSE` | PASS |
| 16 | latency struct TRUE with `pNext = NULL` | PASS |
| 18 | `VK_KHR_present_id` only + latency struct | PASS |
| 21 | `VK_NV_low_latency2` only, no present_id, no struct, sleep mode | PASS, sleep rc=0 |

The minimal chain and every tested variant are stable, including low-latency
without `VK_KHR_present_id` and `latencyModeEnable` FALSE. The in-engine crash
therefore needs the runtime's full device feature set/context, but the trigger
is specifically the **`VkSwapchainLatencyCreateInfoNV` chain**:

- with the chain in the game runtime: `c0000005` in `nvoglv64.dll`, exit
  `-1073740791`, no `[dlss_fg] active` (the original failure);
- with the chain removed in the same runtime: swapchain creates, DLSS-G
  initializes and evaluates normally.

The workaround is in `toolchain/dlss_fg.py`: the latency chain is **off by
default**, `MK8_DLSS_FG_LATENCY_CHAIN=1` opts back in for testing. The
`VK_NV_low_latency2` extension itself stays enabled and
`vkSetLatencySleepModeNV` is still called, so low-latency pacing remains. The
runtime logs the chosen combination:

```text
[dlss_fg] VK_KHR_present_id enabled
[dlss_fg] VK_NV_low_latency2 enabled
[dlss_fg] swapchain latency chain skipped (default; MK8_DLSS_FG_LATENCY_CHAIN=1 enables it)
```

In-engine run at 1920x1080 (`LARECOMP_SCALE=1.0`, `MK8_DLSS_FG=1` only,
no `MK8_FSR4`), exit **0**:

```text
[dlss_fg] active: NGX DLSS-G 2x 1920x1080 (render 1920x1080, format 44),
          VK_NV_low_latency2 pacing
[fsr_fg] depth source: captured scene depth 1920x1080 fmt 100
[fsr_fg] motion vectors: camera-derived
[dlss_fg] NVSDK_NGX_VULKAN_EvaluateFeature(FrameGeneration): 0x00000001 (success)
[dlss_fg] first evaluated frame 29: 1920x1080 backbuffer, second image 2
[dlss_fg] pair present: real image 1 + interpolated image 2
[vk] saved .../dlssfgalone-05000.png (1920x1080)
```

A second run with `MK8_FSR4=1` and `MK8_FSR4_CAPTURE=1` (the shared
camera/depth hooks) produced the same `depth source: captured scene depth`,
`motion vectors: camera-derived`, pair-present and a valid screenshot
(`dlssfg7-05000.png`). The DLSS-G-alone run proves the extended scan condition
in `toolchain/dlss_fg.py` works without `MK8_FSR4`; no extra gating fix was
needed.

`dlssfgalone-05000.png` is a correct race frame. The early
`no captured view/projection; presenting the real frame only` line is the
first prepare before the main-draw scan; it is one-time. The later
`[fsr_fg] motion vectors: camera-derived`, `first evaluated frame 29` and
`pair present: real image 1 + interpolated image 2` prove the path switched to
the two-image present. Both images are submitted; no separate hook captures the
interpolated image, so visible interpolation is not independently proven by the
still. Setting `MK8_DLSS_FG_LATENCY_CHAIN=1` reproduces the original crash with
the same `-1073740791` exit, confirming the workaround is the chain.

At 2560x1440 without `LARECOMP_SCALE=1.0`, the game renders below the display;
the original DLSS-G prepare required matching depth/display sizes and fell back
to real frames. The depth-resample section below removes that limitation.

## XeSS Super Resolution

The official `libxess.dll` (77,795,704 bytes, SHA-256
`251659dd84a3e84de67c886a4186e01f3eca49b00641906fe38bb6b807e5d5b7`) was
copied next to `mk8drecomp.exe` in `run-rtx`. At 1920x1080 with
`LARECOMP_SCALE=1.0`, `output_scale` forces a 1920x1080 render, so the output is
1:1 and the XeSS upscale call is not reached; that run only exercises the device
hook. The real upscale run therefore uses `2560x1440` with
`LARECOMP_SCALE=1.0` (render 1920x1080):

```powershell
$env:LARECOMP_WINDOW = '2560x1440'
$env:LARECOMP_SCALE = '1.0'
$env:MK8_XESS = '1'
$env:MK8_XESS_LIB = "$run\libxess.dll"
$env:MK8_FSR4 = '1'
$env:MK8_FSR4_CAPTURE = '1'
# same race input and 4600/4800/5000 captures
```

The device hook (commit `1a78c6d`) now runs before `vkCreateDevice` and enables
what XeSS asks for:

```text
[xess] VK_EXT_mutable_descriptor_type enabled
[xess] mutableDescriptorType feature enabled
[xess] shaderStorageImageWriteWithoutFormat: 1
[xess] provider 2.0.2 from C:\Users\Ronny\mk8-rtx\game\run-rtx\libxess.dll
[xess] required instance API 0x403000, 0 extension(s)
[xess] 1 required device extension(s)
[xess]   device extension: VK_EXT_mutable_descriptor_type
[xess] SR context 1920x1080 -> 2560x1440, quality ultra-quality, flags 0x40
[xess] depth source: neutral (no captured scene depth) 1920x1080 fmt 100
[xess] upscale active 1920x1080 -> 2560x1440 (ultra-quality)
[xess] depth source: captured scene depth 1920x1080 fmt 100
[xess] motion vectors: camera-derived
[xess] xessVKExecute failed -4 (invalid-argument)
[xess] XeSS unavailable: xessVKExecute failed -4 (invalid-argument)
```

The process exits 0 and the screenshot `xess2560-05000.png` (9,208,022 bytes)
is a valid race frame; the runtime falls back after the rejected execute. The
DLL's own diagnostic string for this rejection is
`xessVKExecute(): xess_vk_execute_params_t::velocityTexture is invalid.` A
gated diagnostic build printed the rejected descriptor as a non-null view and
image, `aspect=1`, `level=1`, `layer=1`, `1920x1080`, `fmt=83`
(`VK_FORMAT_R16G16_SFLOAT`), so this is not a missing/null descriptor. XeSS
does not complete the upscale on this NVIDIA driver; the remaining work is to
find which additional condition the 2.0.2 provider applies to the velocity
image.

## FSR 3.1.6 frame generation

The AMD signed frame-generation provider
(`amd_fidelityfx_framegeneration_vk.dll`, 36,806,928 bytes, SHA-256
`d002dabff8c5e71eaa715fc99438cd4dc81230262a7897b16bc402efec3498b4`) was
selected explicitly. At 1920x1080 with `LARECOMP_SCALE=1.0`:

```powershell
$env:MK8_FSR_FG = 'fsr3'
$env:MK8_FSR_FG_LIB = 'C:\Users\Ronny\mk8-rtx\amd\amd_fidelityfx_framegeneration_vk.dll'
$env:MK8_FSR4 = '1'
$env:MK8_FSR4_CAPTURE = '1'
```

Exit **0**. The swapchain and effect versions are distinct:

```text
[fsr_fg] provider loaded: C:\Users\Ronny\mk8-rtx\amd\amd_fidelityfx_framegeneration_vk.dll
[fsr_fg] swapchain provider offers 3.1.7
[fsr_fg] swapchain provider 3.1.7 (version 0xc01007)
[fsr_fg] frame-generation effect provider 3.1.6
[fsr_fg] active: FFX frame-interpolation swapchain 1920x1080 format 44 (present family 2)
[fsr_fg] depth source: captured scene depth 1920x1080 fmt 100
[fsr_fg] motion vectors: camera-derived
[fsr_fg] first interpolated frame recorded (frameID 0, 1920x1080)
```

The FSR FG path names its two-image result `first interpolated frame recorded`;
there is no separate `pair present` line on this path (that string belongs to
the DLSS-G bridge). There was no `GPUVM`, `DEVICE_LOST` or `FATAL`.
`fsrfg316-05000.png` (4,256,111 bytes) is a correct race frame. The same
run was repeated with a depth-resample host and kept the same providers and
exit 0.

## DLSS-G at 2560x1440 with depth resample

`runtime/mk8/graphics/fsr.cpp` now resamples the captured colour-R32F depth to
the display size when the captured scene is smaller but has the same aspect
ratio. The resample is a `vkCmdBlitImage` with nearest filtering, a display-sized
`VK_FORMAT_R32_SFLOAT` image, and is used only by the frame-generation inputs;
the equal-size 1920x1080 path is unchanged. The host built from this source is
`99c88afdf8740af7bed9404d1c68bcf8cc3274d3328eeabff8f07e688bdfcfe0`
(4,529,152 bytes).

At 2560x1440, with `LARECOMP_SCALE=0.6` to force the 1152x648 render,
`MK8_DLSS_FG=1`, `MK8_FSR4=1` and `MK8_FSR4_CAPTURE=1`:

```text
[dlss_fg] active: NGX DLSS-G 2x 2560x1440 (render 2560x1440, format 44), VK_NV_low_latency2 pacing
[fsr_fg] depth resampled 1152x648 -> 2560x1440
[fsr_fg] depth source: captured scene depth 2560x1440 fmt 100
[fsr_fg] motion vectors: camera-derived
[dlss_fg] NVSDK_NGX_VULKAN_EvaluateFeature(FrameGeneration): 0x00000001 (success)
[dlss_fg] first evaluated frame 28: 2560x1440 backbuffer, second image 1
[dlss_fg] pair present: real image 0 + interpolated image 1
```

Exit **0**; `dlssfg1152-05000.png` (8,833,972 bytes) is the 2560x1440 race
frame. A second run without `LARECOMP_SCALE` (1920x1080 capture) also logged
`depth resampled 1920x1080 -> 2560x1440` and `pair present`. This removes the
previous `2560x1440 DLSS-G remains unverified` limit.

## What remains

- Verify the latency-chain workaround on other NVIDIA driver versions; the
  `MK8_DLSS_FG_LATENCY_CHAIN=1` opt-in currently reproduces the driver bug.
- Find the remaining XeSS 2.0.2 velocity-image condition behind
  `xessVKExecute -4`; the descriptor fields are valid and the device hook is
  already enabled.
- Capture a moving-camera DLSS SR A/B to quantify the remaining ghosting with
  real motion vectors; the start-grid frame is nearly static.
- Qualify the race with audio, controls, clock speed, a full lap/cup and a
  clean restart/save.
- Rebuild the portable package with the depth-resample host (`99c88afd…`) if
  the package should carry the 2560x1440 DLSS-G fix; the validated package
  still has host `37af7b74…`.
- A real Windows package (`package-launcher.py`/`release.py`) and WebView2
  window were not part of this run.

## Evidence

Local (ignored, not committed): `local/rtx-ingame/race-05000.png`,
`fsr-05000.png`, `dlssmove-05000.png`, `dlssfgalone-05000.png`,
`local/windows-rtx-check/xess2560-05000.png`,
`local/windows-rtx-check/fsrfg316-05000.png`,
`local/windows-rtx-check/dlssfg1152-05000.png`,
`local/windows-rtx-check/dlssfg2560-05000.png`,
`local/rtx-ingame/logs/{race,fsr,dlss,dlsscap,dlssfg}.err`
and the matching `.exit` files. The new logs are remote
`run-rtx/{xess2560,fsrfg316,dlssfg1152}.err`/`.exit`. The harness source is
`local/rtx-harness/swapchain_probe.c` with `probe-*.out` logs. Remote originals
remain under `C:/Users/Ronny/mk8-rtx/game/` and
`C:/Users/Ronny/mk8-rtx/game/run-rtx/`. No binaries, screenshots or game
data are committed.
