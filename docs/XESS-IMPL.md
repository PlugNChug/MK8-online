# XeSS Super Resolution: implementation notes

Status 20/11/2026. This is the Windows half of the XeSS plan in
[DLSS-XESS-NEURAL.md](DLSS-XESS-NEURAL.md). The bridge is implemented and the full
Windows runtime links with it. The standalone harness runs a complete
**1280x720 -> 2560x1440 SR dispatch**, submits it, waits and reads back a non-trivial
image under Wine **and natively on Windows on an NVIDIA RTX 5080**. The runtime
itself still has no `fsr.cpp` dispatch hunk for XeSS (that file belongs to another
agent), so no in-game image exists. `gameplay_verified` stays false.

The important result: the `libxess.dll` from the official XeSS 3.0.2 ZIP loads on
this AMD RX 9070 XT (RADV GFX1201, Vulkan 1.4.363) under Wine 11.22 **and on real
Windows with a GeForce RTX 5080 (driver 621.01, Vulkan 1.4.357)**, creates a Vulkan
context, initialises SR and **executes it on the non-XMX (DP4a-class) path**. The
output is a smooth upscaled gradient/checker, not noise: 14,710,400 of 14,745,600
bytes are non-zero, all 256 byte values occur, and a shifted second frame produces a
different checksum.

## Files

| File | Role |
| --- | --- |
| `runtime/mk8/graphics/xess.cpp` | PE inspector, path/probe, Windows loader, Vulkan SR context and per-frame execute, Linux stub |
| `runtime/mk8/graphics/xess.h` | probe/status/query API and `mk8_xess_upscale` |
| `runtime/mk8/graphics/xess/mk8_xess_abi.h` | hand-written minimal XeSS Vulkan ABI (not a copy of the SDK headers) |
| `tests/test_xess.py` | Linux loader/ABI cases and the opt-in Wine query/init smoke |
| `docs/XESS-IMPL.md` | this file |

No toolchain or launcher change was needed: `toolchain/host.py` already compiles
every `runtime/mk8/**/*.cpp`, and the packaging manifest ships every
`runtime/mk8/**/*.cpp|*.h`. The one missing piece is the dispatch in `fsr.cpp`,
which is outside this change (see "Remaining step").

## Licence and why the ABI header is hand-written

The XeSS SDK is under the Intel Simplified Software License (October 2022), which
permits unmodified binary redistribution with the notice and **prohibits reverse
engineering, decompilation and disassembly**. The SDK headers themselves are Intel
copyrighted material; committing them would be vendor bundling. `mk8_xess_abi.h`
therefore declares only the facts the bridge needs: entry-point names, enum values
and field order for five structs. `tests/test_xess.py` compiles it together with the
official `xess.h`/`xess_vk.h` and `static_assert`s every size/offset and the enum
values against SDK 3.0.2. That test passes when the SDK headers are present and
skips cleanly when they are not.

`libxess.dll` is never committed. It is downloaded by
`tools/fetch_vendor_sdk.py --accept-license intel` (pinned ZIP 76,839,749 B,
SHA-256 `88b8a373…471990`), then `bin/libxess.dll` is extracted next to it or
pointed at with `MK8_XESS_LIB`. The probe reports whether `LICENSE.txt` sits beside
the DLL.

## Gate and fallback

- `MK8_XESS=1` (or any non-empty value except `0`) enables the bridge.
- Unset/`0`: `mk8_xess_upscale` returns immediately; the existing FSR/bilinear path
  is untouched. `MK8_XESS` set while `MK8_FSR_FG` is also set logs once and stays off,
  like the DLSS bridge.
- Missing DLL, corrupt/truncated PE, wrong architecture or missing exports: one
  `[xess] XeSS unavailable: <reason>` line, then `VK_NULL_HANDLE` forever. The probe
  never calls `LoadLibraryA` on a file that fails the PE checks.
- Linux: after validating the PE (if one is present), the probe reports
  `unsupported-platform`; the bridge compiles to a stub that prints exactly
  `[xess] XeSS unavailable: XeSS is Windows-only (libxess.dll); this Linux build keeps the FSR/bilinear path`.

## ABI facts used

From XeSS SDK 3.0.2 (`inc/xess/xess.h`, `inc/xess/xess_vk.h`):

- exports: `xessVKCreateContext`, `xessVKInit`, `xessVKExecute`, `xessDestroyContext`,
  `xessVKGetRequiredInstanceExtensions`, `xessVKGetRequiredDeviceExtensions`,
  `xessVKGetRequiredDeviceFeatures`, `xessGetVersion`;
- there is **no** `xessVKDestroyContext`; destruction is the generic
  `xessDestroyContext`;
- public structs use `#pragma pack(push, 8)`;
- `xess_vk_execute_params_t` starts with six `xess_vk_image_view_info` values
  (color, velocity, depth, exposure, responsive mask, output), then
  jitter/exposure/reset/input size, then six `xess_coord_t` bases;
- `xess_result_t` is an int-sized enum; success is 0, errors are negative, and
  1/2 are warnings (`warning-nonexisting-folder`, `warning-old-driver`);
- `xessGetVersion` is a context-free query and returned **2.0.2** for the
  `libxess.dll` shipped in the 3.0.2 ZIP (the SDK release and the SR library
  version are not the same number; do not label it "3.0.2" in logs).

## Exact sequence implemented (`mk8_xess_upscale`)

1. **Probe.** Resolve `MK8_XESS_LIB`, else `MK8_XESS_DIR`, else
   `$XDG_DATA_HOME/mk8online/intel` (fallback `~/.local/share/...`). Parse the PE:
   MZ, `PE\0\0`, machine `0x8664`, PE32+ magic `0x20b`, every section inside the
   file, then the export directory and the seven required names. Only then
   `LoadLibraryA` + `GetProcAddress` on Windows.
2. **Log the SDK's requirements.** `xessVKGetRequiredInstanceExtensions` (0
   extensions and min Vulkan API 1.3.0 on the Wine/RADV run) and
   `xessVKGetRequiredDeviceExtensions` (on RADV:
   `VK_EXT_mutable_descriptor_type`). The bridge logs the names; it cannot add them
   to a device that already exists.
3. **Context.** `xessVKCreateContext(instance, physicalDevice, device, &context)`
   on the runtime's existing `g_vk` device.
4. **Initialise.** `xessVKInit(context, &params)` with
   `outputResolution = display size`, a quality preset derived from
   `render/display` (0.75 → `ULTRA_QUALITY`; native → `AA`), and
   `XESS_INIT_FLAG_LDR_INPUT_COLOR` (the MK8 image is SDR). `MK8_XESS_DEPTH_INVERTED=1`
   adds `XESS_INIT_FLAG_INVERTED_DEPTH`; the default is standard Z, matching the FSR
   bridge.
5. **Resources.** A display-size `VK_FORMAT_R8G8B8A8_UNORM` output image (storage,
   sampled, transfer-src), a render-size `R16G16_SFLOAT` velocity image, and a
   neutral `R32F` depth image for frames where the game depth capture is absent.
   The colour/depth views are cached by image+format+size.
6. **Per frame.** Compute camera motion vectors with the same invVP/prevVP maths as
   the FSR/DLSS bridges (the FSR motion image is private to `fsr.cpp`), transition
   colour/depth/velocity to `SHADER_READ_ONLY_OPTIMAL`, output stays `GENERAL`, then
   fill `xess_vk_execute_params_t` (views, formats, sizes, `jitterOffsetX/Y` from
   `g_fsr_jitter_x/y`, `resetHistory`, `inputWidth/Height`) and call
   `xessVKExecute(context, cb, &params)`. Returns the output view; failures return
   `VK_NULL_HANDLE` and log once.
7. **Resize.** Destroy the context and the cached images, recreate on the next
   frame. The MV pipeline and UBO survive (same device).

Inputs match the other bridges: composed colour at render resolution (post-tonemap/UI,
the same limitation as FSR/DLSS), captured scene depth or neutral, camera-only motion
vectors, and the shared Halton jitter. Karts still have no object vectors.

## Windows build evidence

Prepared a fresh runtime from the current `feature/fsr` tree and cross-built the
host with the pinned **Zig 0.17.0** (`local/windows-verify/tools/zig-x86_64-linux-0.17.0/zig`)
and the UCRT64 dependency tree:

```text
$ MK8_ZIG=$PWD/local/windows-verify/tools/zig-x86_64-linux-0.17.0/zig \
  python3 local/windows-verify/cross_build.py \
      local/windows-verify/prepared-xess local/windows-verify/host-xess 8

Runtime x86_64-windows-gnu: 225/225 unidades
Executável do runtime ligado; gameplay ainda não verificado.
host built: local/windows-verify/host-xess/mk8drecomp.exe (4423680 bytes)
```

`xess.cpp` is in the link: the executable contains `xessVKCreateContext`,
`[xess] XeSS unavailable: %s`, the PE error strings and `libxess.dll`. The
verification driver is `local/windows-verify/cross_build.py` (a local script, not a
product path); it had to select Zig explicitly because `native.cc()` captured the
Linux clang default at import time.

The full-tree build is the compile proof; the new translation unit was also compiled
alone for `x86_64-w64-windows-gnu` with clang and with MinGW-w64 (probe-only), both
clean apart from the usual `-Wmissing-field-initializers` on Vulkan structs.

## Dispatch evidence (real DLL, AMD/RADV)

Harness: `tools/xess_wine_harness.cpp` (source only; no vendor binary committed).
It creates a Vulkan 1.3 device with `VK_EXT_mutable_descriptor_type` and the feature
chain from `xessVKGetRequiredDeviceFeatures`, then:

- 1280x720 RGBA8 colour, D32_FLOAT depth and R16G16_SFLOAT velocity inputs;
- 2560x1440 RGBA8 output, `XESS_INIT_FLAG_LDR_INPUT_COLOR`;
- `xessVKInit` with `PERFORMANCE`, then two `xessVKExecute` records: frame 0 with
  `resetHistory=1`, frame 1 with a 64-pixel horizontal colour shift,
  `velocity=(64,0)`, `resetHistory=0` and opposite jitter;
- submit, `vkQueueWaitIdle`, copy the output image to a host-visible buffer and read
  it back.

Exact result (`local/qualification/xess-wine/`, Wine 11.22, `nice -n 10`):

```text
xessVKCreateContext=0
xessGetInputResolution=0 1114x627
xessVKInit=0
xess_version=2.0.2
frame0_execute_rc=0
frame0_submit_rc=0
frame0_wait_rc=0
frame0_nonzero=14710400
frame0_unique=256
frame0_min=0
frame0_max=255
frame0_checksum=8901939667484331069
frame0_bytes=14745600
frame1_execute_rc=0
frame1_submit_rc=0
frame1_wait_rc=0
frame1_nonzero=14712982
frame1_unique=256
frame1_min=0
frame1_max=255
frame1_checksum=17835196424027649762
dispatch_ok=1
```

Artifacts: `local/qualification/xess-wine/frame0.rgba`, `frame1.rgba` and
`frame0.png` (667,307 B, 2560x1440 RGBA, visually a smooth upscaled gradient/checker).
No device-loss or validation error was observed (`execute_rc=0`, `submit_rc=0`,
`wait_rc=0`).

One nuance worth recording: for 2560x1440 `PERFORMANCE`, XeSS 3.0.2's
`xessGetInputResolution` reports **1114x627**, not 1280x720 (its performance scale is
about 2.3x, not the older 2.0x). The harness used the requested 1280x720 input and
XeSS executed it anyway, but a production integration should use the API's returned
optimal resolution, not assume the old scale factors. The runtime bridge currently
passes the game's fixed render resolution; that is the next quality/perf item to
check on real hardware.

## Native Windows evidence (real DLL, NVIDIA RTX 5080)

A real Windows machine was reached over OpenSSH (Windows OpenSSH, command through
`cmd.exe`): **NVIDIA GeForce RTX 5080** (deviceID `0x2c02`, vendorID `0x10de`),
driver **621.01**, Vulkan **1.4.357** (instance and device), 16,303 MiB,
`vulkaninfo --summary` reports one discrete GPU. The machine has the NVIDIA Vulkan
runtime only; no Intel component is installed. The DLL is the same official 3.0.2
binary used under Wine (77,795,704 B, SHA-256
`251659dd84a3e84de67c886a4186e01f3eca49b00641906fe38bb6b807e5d5b7`, confirmed again
on the Windows side with `certutil -hashfile ... SHA256`), copied to
`C:/Users/Ronny/mk8-rtx/xess/libxess.dll`. It is not committed.

The harness was cross-built on Linux with **mingw-w64 16.2.0** and the Khronos
Vulkan 1.4.363 headers, copied to an isolated include root so the Windows build
never sees the host libc headers:

```sh
VK_INC=$HOME/.cache/xess-win/include
mkdir -p "$VK_INC"
cp -r /usr/include/vulkan "$VK_INC/vulkan"
cp -r /usr/include/vk_video "$VK_INC/vk_video"
x86_64-w64-mingw32-g++ -std=c++17 -O2 -static -DMK8_XESS_PROBE_ONLY \
    -I "$VK_INC" -I runtime/mk8/graphics \
    tools/xess_wine_harness.cpp runtime/mk8/graphics/xess.cpp \
    -o "$HOME/.cache/xess-win/xess_harness.exe"
```

The EXE (299,724 B, PE32+ x86-64, SHA-256
`6df8f838dd0abbeadd33a0fc862409211c5fadb9bccfe36212c98c4cccafd66e`) and the DLL were
copied with `scp` to `C:/Users/Ronny/mk8-rtx/xess/`, then run natively:

```bat
set MK8_XESS_LIB=C:\Users\Ronny\mk8-rtx\xess\libxess.dll && ^
C:\Users\Ronny\mk8-rtx\xess\xess_harness.exe C:\Users\Ronny\mk8-rtx\xess\out
```

Exit code `0`, exact output:

```text
vkCreateInstance=0
device=NVIDIA GeForce RTX 5080
required_extensions_rc=0
count=1
ext=VK_EXT_mutable_descriptor_type
required_features_rc=0
vkCreateDevice=0
xessVKCreateContext=0
xessGetInputResolution=0 1114x627
xessVKInit=0
xess_version=2.0.2
xess_intel_xefx_rc=0 version=0.0.0
frame0_execute_rc=0
frame0_submit_rc=0
frame0_wait_rc=0
frame0_nonzero=14710400
frame0_unique=256
frame0_min=0
frame0_max=255
frame0_checksum=16097837884742765276
frame0_bytes=14745600
frame1_execute_rc=0
frame1_submit_rc=0
frame1_wait_rc=0
frame1_nonzero=14712610
frame1_unique=256
frame1_min=0
frame1_max=255
frame1_checksum=13467285208402489168
frame1_bytes=14745600
dispatch_ok=1
```

The run was repeated four times (before and after adding the XeFX diagnostic) with
identical checksums, so the dispatch is deterministic on this GPU. Artifacts:
`local/qualification/xess-windows-rtx/{dispatch.log,frame0.rgba,frame1.rgba,frame0.png,frame1.png}`.
`frame0.png` is 2560x1440, 287,008 B, and shows the expected smooth gradient with a
checker; `frame1.png` shows the 64-pixel checker shift at the top edge. No
`VK_ERROR_DEVICE_LOST` and no failing `VkResult` was observed on instance, device,
context, init, execute, submit or wait. The harness does not enable validation
layers, so this is a clean-result observation, not a validation-layer report.

### Wine/RADV vs native Windows/RTX (same harness)

| Quantity | Wine 11.22, RX 9070 XT (RADV GFX1201) | Windows, RTX 5080 (621.01) |
| --- | --- | --- |
| `xessGetVersion` | 2.0.2 | 2.0.2 |
| `xessGetIntelXeFXVersion` | 0.0.0 | 0.0.0 |
| `xessGetInputResolution` (1440p PERFORMANCE) | **1114x627** | **1114x627** |
| `xessVKCreateContext` / `xessVKInit` | 0 / 0 | 0 / 0 |
| frame0 non-zero / unique / checksum | 14,710,400 / 256 / 8901939667484331069 | 14,710,400 / 256 / 16097837884742765276 |
| frame1 non-zero / unique / checksum | 14,712,982 / 256 / 17835196424027649762 | 14,712,610 / 256 / 13467285208402489168 |
| execute/submit/wait rc, both frames | 0/0/0 | 0/0/0 |

`xessGetInputResolution` returns the same **1114x627** for a 2560x1440
`PERFORMANCE` request on both vendors, so the 2.3x scale is a property of the XeSS
3.0.2 quality preset, not of the AMD driver or Wine. The harness still feeds the
requested 1280x720 image, and XeSS accepts it (the API result is advisory, not a
hard constraint).

The two vendors do not produce bit-identical pixels, which is expected for
per-architecture kernels: frame 0 differs in 49,961 of 14,745,600 bytes (0.34%),
mean absolute difference 0.0035/255, maximum 16, RGB PSNR 70.6 dB, and the zero-byte
mask is identical. That is the same SR network with different rounding, not a
different algorithm. On one GPU the output does track the input: frame 1 differs
from frame 0 in 4,533,158 bytes (30.7%), mean absolute difference 7.21, max 243.

**XMX or DP4a?** On the RTX 5080 XeSS does **not** use XMX. `xessGetIntelXeFXVersion`
returns `0.0.0`, which the SDK header documents as the value for non-Intel platforms
(the Intel XeFX/XMX library is not loaded), and XMX is an Intel Xe-core instruction
that does not exist on NVIDIA. The DLL therefore runs its vendor-neutral
non-XMX path, which Intel documents as the DP4a-class fallback for other vendors.
The DLL's DEBUG logging callback (`xessSetLoggingCallback` returned 0) emitted no
messages, so the exact kernel name is not observable through the public API and is
not claimed here.

## Wine evidence (real DLL, AMD/RADV)

`libxess.dll` from `XeSS_SDK_3.0.2.zip` (`bin/libxess.dll`, 77,795,704 B,
SHA-256 `251659dd84a3e84de67c886a4186e01f3eca49b00641906fe38bb6b807e5d5b7`),
Wine 11.22, RADV GFX1201:

```text
probe_ok=1
status=ok
loaded=1
query_rc=0
version=2.0.2
instance_extensions=0
min_api=0x403000

vkCreateInstance=0
physical_devices=2
device=AMD Radeon RX 9070 XT (RADV GFX1201) api=1.4.363
required_device_extensions rc=0 count=1
  ext=VK_EXT_mutable_descriptor_type
required_device_features rc=0 chain=...
vkCreateDevice=0
xessVKCreateContext rc=0
xessVKInit rc=0
xessDestroyContext done
```

What this proves: the DLL loads under Wine, the export names in the ABI header are
right, the query path works without a GPU, the SR context/initialisation runs on a
non-Intel DP4a device, and `xessVKExecute` records a real upscale that reads back as
a varied image. What it does **not** prove: that the runtime's actual device enables
the required extension/features (the standalone harness added them itself), that the
image is correct for a real MK8 frame (synthetic inputs, camera-less depth/MVs), or
that performance is acceptable.

## Tests

```text
$ python3 -m unittest tests.test_xess -v
12 tests, OK (the 3 Wine tests are skipped by default)

$ MK8_XESS_WINE=1 MK8_XESS_WINE_VK=1 python3 -m unittest \
      tests.test_xess.XessTests.test_wine_loads_the_dll_and_calls_the_query_path \
      tests.test_xess.XessTests.test_wine_creates_a_vulkan_device_and_initialises_xess -v
2 tests, OK

$ MK8_XESS_WINE_EXECUTE=1 python3 -m unittest \
      tests.test_xess.XessTests.test_wine_executes_xess_sr_and_reads_back_the_output -v
1 test, OK (writes target/xess-wine-evidence/frame0.png + wine-dispatch.log)
```

The Linux cases cover: no DLL (names the fetcher and the expected directory),
corrupt/truncated PE, a valid DLL without XeSS Vulkan exports (`libxess_dx11.dll`),
the real DLL's export table, the Linux one-line stub, the exact `MK8_XESS` gate, and
the ABI static asserts. The Wine tests are opt-in because they need Wine and the
official DLL; they skip cleanly otherwise.

## Remaining step (precise)

1. **Dispatch (owner: `fsr.cpp`).** Add the mirror of the DLSS hunk in
   `mk8_fsr4_upscale`, before the FSR path:

   ```c
   if (getenv("MK8_XESS")) {
       VkImageView xess_view = mk8_xess_upscale(cb, color, depth, rw, rh, ow, oh, frame_ms);
       if (xess_view) return xess_view;
   }
   ```

   With this, the built `mk8drecomp.exe` can actually call the bridge. Until then
   the code is linked but dormant.
2. **Device creation.** The runtime must be created with
   `VK_EXT_mutable_descriptor_type` and the feature chain returned by
   `xessVKGetRequiredDeviceFeatures`, before `vkCreateDevice`. The bridge can only
   report the requirement after the device exists. This is the first thing to check
   when `xessVKCreateContext`/`xessVKInit` fail in-game.
3. **Execute in the real frame.** The standalone harness proves execute and
   readback under Wine and natively on Windows; the runtime still needs the `fsr.cpp`
   dispatch above, then a same-frame comparison against FSR 4 and a check that the
   game's device exposes the required extension/features. Use
   `xessGetInputResolution` (or `xessGetOptimalInputResolution`) instead of assuming
   the old quality scale.
4. **Hardware qualification.** NVIDIA DP4a on native Windows is now proven by the
   harness (RTX 5080, driver 621.01). Still missing: a full race with screenshots,
   resize and frame pacing in-game, AMD on native Windows (only Wine/RADV so far),
   and Intel Arc for the XMX path. Intel Arc is the only path that gets XMX;
   everything else is DP4a, as the SDK documents.
5. **FG/XeLL are not applicable.** XeSS Frame Generation and XeLL are D3D12-only
   (`xefg_swapchain_d3d12.h`, `xell_d3d12.h`); this runtime is Vulkan on both OSes.
   Do not plan them.

## Unverified / limits

- No in-game XeSS image or FPS measurement; the runtime dispatch is absent. The
  dispatch evidence is a synthetic 1280x720 -> 2560x1440 image, not a game frame.
- Correctness, ghosting and cost on a real scene are unknown; camera-less depth and
  MVs are not a quality test.
- Platform coverage is partial: native Windows on an NVIDIA RTX 5080 now accepts the
  context, initialises and executes, but AMD on native Windows is still untested
  (only Wine/RADV), and no Intel Arc XMX hardware has run this bridge.
- The runtime device's current extension list was not checked against
  `VK_EXT_mutable_descriptor_type`; the standalone harness enabled it explicitly.
- `xessGetVersion` returned 2.0.2 for the 3.0.2 SDK's `libxess.dll`; the mapping
  between SDK release, library version and model version is Intel's, not inferred
  here beyond the observed value.
- XeSS on native Linux remains impossible with the official SDK (no `libxess.so`);
  the Linux build is a deliberate stub.
