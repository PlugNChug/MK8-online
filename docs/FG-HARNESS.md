# Frame-generation harness: the FFX Vulkan swapchain and present path

Status 20/11/2026. Companion to `docs/FSR3-4-FG.md`. The harness is
`tools/fg-harness/fg_harness.cpp`; the test is `tests/test_fg_harness.py`.

This document records what the *actual* frame-generation present path requires, which sequence works,
and which provider defects the harness found. It is not a claim that frame generation is integrated in
the game: `gameplay_verified` remains false.

## What the harness does

A standalone, headless Vulkan program with no game, launcher or runtime patch:

1. creates `VK_EXT_headless_surface` (or `VK_KHR_display`) and a 1280x720 BGRA8 FIFO swapchain;
2. replaces that swapchain with the FFX frame-generation swapchain;
3. renders a moving pattern with a real graphics pipeline (dynamic rendering, embedded SPIR-V from
   `tools/fg-harness/shaders/`);
4. records a `ffxDispatchDescFrameGenerationPrepareV2` for every rendered frame with static depth,
   constant-shift motion vectors, Halton jitter and camera data;
5. presents through `pOutQueuePresentKHR`, records the provider's frame-generation callback and
   composes through a real present callback;
6. reads back one previous real frame, one generated frame and the current real frame, and verifies
   that the generated image differs from both neighbours;
7. checks `pOutGetLastPresentCountFFXAPI`, all return codes and (by default) the Khronos validation
   layer.

Build and options:

```sh
g++ -std=c++17 -O2 -I runtime/mk8/graphics/ffx tools/fg-harness/fg_harness.cpp \
    -o fg-harness -lvulkan -ldl -pthread
./fg-harness --frames 120 --width 1280 --height 720 --shift 8 \
    --provider /path/libamd_fidelityfx_framegeneration_vk.so
```

`--surface auto|headless|display`, `--validation`/`--no-validation`, and the provider environment
overrides are listed by `--help`. The library is resolved as `--provider`, `MK8_FG_HARNESS_LIB`,
`MK8_FSR_FG_LIB`, then `$XDG_DATA_HOME/mk8online/fsr/`, then
`$HOME/.local/share/mk8online/fsr/`, matching the runtime.

`shaders.h` is committed build output from `gen_shaders.py` (glslangValidator, SPIR-V 1.2); the
harness itself does not need a shader compiler.

## Host and surface (evidence)

The run used RADV GFX1201 on an RX 9070 XT, Vulkan 1.4.363, Mesa 27.0.0-devel.
`VK_EXT_headless_surface` was accepted and used directly:

```
[fg-harness] surface=VK_EXT_headless_surface advertised=VK_EXT_headless_surface extent-request=1280x720
[fg-harness] device=AMD Radeon RX 9070 XT (RADV GFX1201) family-game=0 family-present=1 present-has-graphics=no
[fg-harness] queues: game flags=0xf count=1, present flags=0xe count=4
[fg-harness] surface extent=1280x720 transform=0x1 min-images=4
[fg-harness] swapchain format=44 colorspace=0
```

No Xvfb/XCB fallback was needed. `VK_KHR_display` is implemented as the fallback when the headless
extension is advertised but `vkCreateHeadlessSurfaceEXT` fails; RADV on this host does not export the
display-plane entry points, so that fallback cannot be exercised here. If both fail, the harness
reports that Xvfb plus an xcb surface is the remaining option rather than inventing a surface.

## Exact API sequence that works

1. **Instance**: `VK_KHR_surface` + `VK_EXT_headless_surface`, optionally
   `VK_EXT_debug_utils` and `VK_LAYER_KHRONOS_validation`.
2. **Surface and device**: require Vulkan 1.3, `VK_KHR_swapchain`, graphics+compute+present on the
   game family, and a second present-capable family with at least two queues. Enable
   `VkPhysicalDeviceVulkan12Features::timelineSemaphore` (the provider creates timeline semaphores),
   `VkPhysicalDeviceVulkan13Features::dynamicRendering` (the harness pipeline), and
   `VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR::computeDerivativeGroupLinear` with
   `VK_KHR_compute_shader_derivatives` (the generated FG shaders declare
   `ComputeDerivativeGroupLinearKHR`). The runtime device creation must enable these too.
3. **Normal swapchain**: `vkCreateSwapchainKHR` with BGRA8/RGBA8, FIFO, `minImageCount` at least 3
   and `COLOR_ATTACHMENT | TRANSFER_SRC | TRANSFER_DST`. Keep this handle: the provider retires it.
4. **Swapchain ABI query**: `ffxQuery(NULL, ffxQueryDescGetVersions)` with
   `createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK` and a `VkDeviceContext`.
   The provider offers `3.1.7`; pack it as `(3 << 22) | (1 << 12) | 7 = 0x00c01007`.
5. **Swapchain context**: `ffxCreateContext` with `ffxCreateContextDescFrameGenerationSwapChainVK`,
   `header.pNext` = `ffxCreateContextDescFrameGenerationSwapChainVersionVK{version = 0x00c01007}`.
   - `swapchain` points at the normal handle; `createInfo.oldSwapchain` is the same handle.
   - The provider destroys the normal swapchain and writes the proxy handle back into the same
     variable. The proxy is not a real `VkSwapchainKHR` driver object.
   - `gameQueue` must be graphics+compute; `presentQueue` and `imageAcquireQueue` are two queues of
     the second family. Give each a `vkQueueSubmit` callback that serializes with the render thread
     (the provider submits from two extra threads).
   - `asyncComputeQueue` may be left zero; this harness and the runtime run interpolation on the game
     queue (`allowAsyncWorkloads = false`).
6. **Replacement functions**: `ffxQuery(&swapchain_context,
   ffxQueryDescSwapchainReplacementFunctionsVK)`; route image enumeration, acquire, present and
   destroy through it. `pOutGetLastPresentCountFFXAPI` is present in this provider build.
7. **Effect context**: `ffxCreateContext(FRAMEGENERATION)` with
   `ffxCreateContextDescFrameGenerationVersion` (version = `FFX_FRAMEGENERATION_VERSION`), an
   `ffxCreateBackendVKDesc` in `header.pNext`, `displaySize == maxRenderSize`, BGRA8, and
   `FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS`.
8. **Inputs**: a static `R32_SFLOAT` depth image (gradient plus a near square is enough) and a
   constant-shift `R16G16_SFLOAT` motion-vector image. Both are sampled, uploaded once and left in
   `SHADER_READ_ONLY_OPTIMAL`. The depth resource description is passed with
   `FFX_API_RESOURCE_USAGE_READ_ONLY` and a color aspect; passing `DEPTHTARGET` on an `R32_SFLOAT`
   image creates a `D32_SFLOAT` depth-aspect view and fails validation.
9. **Configure each frame**: `ffxConfigureDescFrameGeneration` with the proxy as `swapChain`, the
   custom present callback, the frame-generation callback, `frameGenerationEnabled = true`,
   `allowAsyncWorkloads = false`, and `frameID = N`.
10. **Prepare each frame**: record `ffxDispatchDescFrameGenerationPrepareV2` into the game command
    buffer before submission. Fill `frameID`, `commandList`, `renderSize`, `jitterOffset`,
    `motionVectorScale`, `frameTimeDelta`, `reset` (true only on the first frame/discontinuity),
    `cameraNear/Far/FovAngleVertical`, `viewSpaceToMetersFactor`, `depth`, `motionVectors`, and the
    four camera vectors. The V2 struct embeds camera info; a separate
    `ffxDispatchDescFrameGenerationPrepareCameraInfo` is the deprecated V1 path.
11. **Present**: submit the game command buffer waiting on the acquire semaphore and signalling the
    render semaphore, then call `pOutQueuePresentKHR(gameQueue, &present)` with the proxy and the
    acquired image index. The provider calls `frameGenerationCallback` on the game thread; that
    callback calls `ffxDispatch(&effect, &params->header)` with `FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION`
    and the provider-supplied command buffer and output resource.
12. **Verify**: `pOutGetLastPresentCountFFXAPI(proxy)` must be at least the rendered-frame count; the
    present callback must have seen generated and real frames; the readback must differ from both the
    previous and current real frames.
13. **Shutdown**: `ffxConfigure` with `frameGenerationEnabled = false`, dispatch
    `FFX_API_DISPATCH_DESC_TYPE_FGSWAPCHAIN_WAIT_FOR_PRESENTS_VK` on the swapchain context, destroy
    the frame-generation effect, destroy the swapchain context, then call
    `pOutDestroySwapchainFFXAPI(device, proxy, nullptr, proxy)`. Do not call `vkDestroySwapchainKHR`
    on the proxy.

## Minimal per-frame order

```
wait frame fence
acquire replacement image (pOutAcquireNextImageKHR)
begin command buffer
  barrier replacement image UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL
  draw moving pattern
  barrier COLOR_ATTACHMENT_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL
  ffxConfigure(FRAMEGENERATION, frameID = N, enabled, callbacks)
  ffxDispatch(FRAMEGENERATION_PREPARE_V2, frameID = N, depth, motion vectors, jitter, camera)
end
submit game queue: wait acquire semaphore, signal render semaphore
pOutQueuePresentKHR(proxy, imageIndex): provider invokes frameGenerationCallback
  -> ffxDispatch(FRAMEGENERATION) records interpolation into the provider's command buffer
  -> provider calls presentCallback for the generated and the real frame
destroy/retire nothing; the image comes back through acquire
```

The `frameID` in the configure, the prepare and the provider's interpolation dispatch must be the
same value, and it must increase by exactly one. The frame-generation callback is stored per
`frameID % FFX_MAX_QUEUED_FRAMES`, so reusing or skipping an ID silently selects a stale callback.

## What the runtime must provide

- **Depth**: the scene depth for the current frame, at render/display resolution when upscaling is not
  combined. Non-inverted and inverted depth are separate shader permutation sets; the context flag
  must match the data. `FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE` changes the reconstruction.
- **Motion vectors**: display-resolution (or low-res) per-pixel motion vectors. With
  `FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS`, `motionVectorScale` is
  `displaySize` when the texture stores UV deltas, or `{1,1}` when it stores pixel deltas. The harness
  stores `(8/width, 0)` and uses `displaySize`; this is logged as `motion=constant-shift(8px,...)`.
- **Jitter and camera**: the same jitter used for the frame, plus near/far, vertical FOV and the
  camera basis. The V2 prepare requires the camera vectors for future MLFI providers even if the
  FSR 3 path does not use them.
- **UI**: either a UI surface (`ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceVK`, with
  optional internal double buffering) or a present callback that draws/composes UI. If the present
  callback is `nullptr`, the provider uses its internal UI blit and simply copies the backbuffer when
  no UI resource is registered. A custom present callback **must** perform the copy/composition into
  `outputSwapChainBuffer`; a no-op leaves the real swapchain image stale.
- **HUD-less color** is optional; without it the interpolation uses the full backbuffer and can smear
  UI.
- **Frame pacing**: the provider's presenter thread paces presents. Keep the render thread's
  submissions short and do not hold the queue lock across `pOutQueuePresentKHR` (the provider may wait
  for its own thread, which submits through the same lock).

## Provider quirks observed

- **Old swapchain retirement**: `createInfo.oldSwapchain` must equal the normal handle. The provider
  destroys the normal swapchain and returns the proxy. Passing a proxy from an earlier context as
  `oldSwapchain` is invalid.
- **Queue families**: the game family must have graphics+compute; the present/acquire family needs two
  queues and present support. If the present family also has graphics+compute, the provider composes
  on the present queue; otherwise it composes on the game queue (`composition=game-queue` on this
  host). Both modes work with a state-aware present callback.
- **Timeline semaphores**: the provider creates timeline semaphores unconditionally. The device must
  enable `timelineSemaphore`; otherwise the swapchain context creation fails or validation reports
  invalid semaphore creation.
- **Present index**: `queuePresent` asserts that `pImageIndices[0]` equals the provider's internal
  replacement-buffer index. Always present the image just acquired, and present every acquired image.
  Missing a present desynchronizes the index.
- **Present count**: `pOutGetLastPresentCountFFXAPI` increments in `presentToSwapChain`, i.e. per real
  swapchain present. With interpolation enabled it is about `2 * rendered`, but a failed real-image
  acquire can skip one; one 120-frame run reported `presented=240` callbacks and
  `last_present_count=238`, while a rerun reported `240`. Assert `>= rendered`, not equality.
- **`vkAcquireNextImageKHR` replacement**: it returns `VK_NOT_READY` when the application runs ahead
  of the present count, even with an infinite timeout. Treat it as retryable; the harness presents one
  image per acquired image and never sees it at 1280x720.
- **Signals and teardown**: disable frame generation and dispatch `WAIT_FOR_PRESENTS` before destroying
  the effect; otherwise the presenter thread can outlive the resources. `pOutDestroySwapchainFFXAPI`
  must be called after the swapchain context is destroyed.
- **Present callback state mapping**: the callback receives `FfxApiResourceState` values, not Vulkan
  layouts. Map `PIXEL_COMPUTE_READ` → `SHADER_READ_ONLY_OPTIMAL`, `UNORDERED_ACCESS` → `GENERAL`,
  `COPY_SRC` → `TRANSFER_SRC_OPTIMAL`, `PRESENT` → `PRESENT_SRC_KHR`, and restore the incoming layout
  after the copy. The provider submits the callback command buffer itself.
- **Double creation of `frameRenderedSemaphores`**: `FrameInterpolationSwapchainVK::init` creates the
  per-image semaphores in two places (once before the real swapchain image count is known, once after).
  The first four are overwritten and leak; validation reports
  `VkDevice ... has 4 leaked objects ... AMD FSR FrameRenderedSemaphore 0..3` at `vkDestroyDevice`.
  Removing the first loop fixes it.

## Blocking provider defects found by the harness

These are outside the harness (owned by the provider/toolchain work) but the harness cannot be green
until they are fixed. The evidence is in the provider build at
`~/.cache/mk8online/fsr-build3/build/libamd_fidelityfx_framegeneration_vk.so` and in
`local/fg-provider/libamd_fidelityfx_framegeneration_vk.so`.

1. **Generated blob metadata omits constant buffers and buffer SRV/UAVs.**
   `tools/gen_fg_blobs.py::classify` only walks `OpVariable ... UniformConstant`. Vulkan puts
   constant buffers in `Uniform` and storage buffers in `StorageBuffer`, so `cbFI`, `rw_counters`,
   `r_counters` and every other buffer binding were absent from the `FfxShaderBlob` tables. The
   pipeline layout therefore lacks those bindings:
   `vkCreateComputePipelines(): ... variable "rw_counters" ... binding was not declared in
   VkPipelineLayoutCreateInfo::pSetLayouts[0]`, with `cbFI` type mismatches.
   Fix: walk `UniformConstant`, `Uniform` and `StorageBuffer`; look up the pointee through
   `OpTypePointer`; classify `OpTypeImage`/`OpTypeSampler`/`OpTypeStruct`; treat a struct with
   `OpTypeRuntimeArray` or `StorageBuffer` as a buffer, and a `Uniform` struct with `Block` as a CBV.
   Read-only detection must include `OpMemberDecorate %type 0 NonWritable`, not only
   `OpDecorate %variable NonWritable` (HLSL `StructuredBuffer<T>` emits the former).
   Without this, the first real interpolation dispatch reads/writes unbound descriptors: RADV reports
   `GPUVM fault detected at address 0x800106c000` and `vkQueueSubmit` returns `VK_ERROR_DEVICE_LOST`
   (-4) at frame 0.
2. **`r_counters` is bound as a UAV buffer but the shader reads it as an SRV buffer.** With the
   metadata fix, `r_counters` (interpolation pass) is a `StructuredBuffer<uint>`; the SDK's pipeline
   layout adds it, but the descriptor set is never updated (the internal counters resource is a UAV).
   The interpolation shader's `FrameIndexSinceLastReset()` then reads zero and takes the reset path
   (`ffx_frameinterpolation.h`: "if we just reset copy the current back buffer and don't
   interpolate"), so every generated frame is a copy of the current frame. The harness reports
   `generated-vs-current=0.0000` until the binding is fixed. The scratch fix classifies read-only
   structured buffers from the member `NonWritable` decoration, which binds
   `context->srvResources[COUNTERS]`; after that the generated frame differs from both neighbours
   (`0.1899` vs previous, `0.1894` vs current).
3. **The interpolation output is written as three components into a BGRA8 view.**
   `ffx_frameinterpolation_callbacks_hlsl.h` uses `RWTexture2D<FfxFloat32x3> rw_output` when the
   inpainting mask is a separate UAV and writes `val.rgb`. Vulkan validation rejects the
   `OpImageWrite`:
   `storage image descriptor ... VK_FORMAT_B8G8R8A8_UNORM ... has 4 components, but the OpImageWrite
   Texel operand only contains 3 components`. Write all four components (keep the separate mask
   store) or use a 3-component output resource. The scratch build writes `val` and keeps
   `rw_inpainting_mask = val.a`.
4. **The `frameRenderedSemaphores` double creation leaks four semaphores** (see the quirk above).

Storage-image format warnings remain after the above: DXC stamps `Rg32f` on the dilated-motion-vector
UAV while the SDK allocates `R16G16_SFLOAT`, and `R32ui` on the optical-flow pyramid while the SDK
allocates `R8_UINT`. The harness reports them as validation warnings, not errors; the readback shows
interpolation output that differs from both neighbours, but the values are technically undefined by
the Vulkan spec. Matching the SDK resource formats (or using `Unknown` storage-image formats with
`shaderStorageImageReadWithoutFormat`/`WriteWithoutFormat`, except for `OpImageTexelPointer` resources)
should be the next provider fix.

## Evidence

Scratch provider built from the same sources with the four fixes above:

```
[fg-harness] provider=/home/mitchell/.cache/mk8online/fg-harness-scratch/build/libamd_fidelityfx_framegeneration_vk.so
[fg-harness] surface=VK_EXT_headless_surface advertised=VK_EXT_headless_surface extent-request=1280x720
[fg-harness] device=AMD Radeon RX 9070 XT (RADV GFX1201) family-game=0 family-present=1 present-has-graphics=no
[fg-harness] queues: game flags=0xf count=1, present flags=0xe count=4
[fg-harness] surface extent=1280x720 transform=0x1 min-images=4
[fg-harness] swapchain format=44 colorspace=0
[fg-harness] swapchain provider offers 3.1.7
[fg-harness] swapchain ABI=3.1.7 (0xc01007)
[fg-harness] swapchain context=ok proxy-handle=0x5565bb7ccc00
[fg-harness] replacement images=4 composition=game-queue
[fg-harness] frame-generation effect provider=3.1.6
[fg-harness] readback capture frame=60 buffers=3686400 bytes each
[fg-harness] inputs: color=moving-bars(step=8px) depth=gradient+square(static,R32F) motion=constant-shift(8px,R16G16F,uv-scale) jitter=halton(2,3)
[fg-harness] frame 30/120 presented-by-provider=58 generated-callbacks=30 real-callbacks=30
[fg-harness] frame 60/120 presented-by-provider=118 generated-callbacks=60 real-callbacks=60
[fg-harness] frame 90/120 presented-by-provider=178 generated-callbacks=90 real-callbacks=90
[fg-harness] frame 120/120 presented-by-provider=238 generated-callbacks=120 real-callbacks=120
[fg-harness] readback source: frame=59 generated=0 image=... checksum=...
[fg-harness] readback source: frame=60 generated=1 image=... checksum=...
[fg-harness] readback source: frame=60 generated=0 image=... checksum=...
[fg-harness] readback: generated-vs-previous=0.1899 generated-vs-current=0.1894 (threshold 0.005)
FG-HARNESS ok rendered=120 presented=240 last_present_count=240 generated=120 real=120 readback=ok validation=clean warnings=10 surface=headless extent=1280x720 format=44 generated_dispatch=120
```

The run takes about 5 seconds at 1280x720. The validation layer is enabled and reports zero errors;
the storage-image format warnings listed above remain. `tests/test_fg_harness.py` builds the harness
and asserts this summary when the provider can run the path.

Against the provider in the tree today (`fsr-build3`), the harness stops at the first interpolation
dispatch with the `rw_counters`/`cbFI` binding errors, and with validation disabled the same path
faults the GPU (`RADV GPUVM fault ... vkQueueSubmit returned VkResult -4`). `local/fg-provider` segfaults
in the same place. The test skips these runs with a `known provider defect` reason by default; set
`MK8_FG_HARNESS_REQUIRE_PROVIDER=1` to make the test fail instead.

## What remains unverified

- Windowed (X11/Wayland) presentation and `VK_KHR_display`; only the headless surface was exercised.
- Real UI composition (the callback path is exercised, but with no UI image and a plain copy).
- In-game activation, visual quality, latency, pacing, VRR and a real 120 Hz display.
- Resize/recreate (`pOutCreateSwapchainFFXAPI`), HDR, async compute workloads, and combining FG with
  the FSR upscaler.
- MLFI/FSR 4 frame generation: the source provider is FSR 3.1.7 only.
