# FSR 3.1.6 / 4.1.2 — branch `feature/fsr`

Separate from the Linux port. The provider is optional and host-side; no guest
code or ABI changes.

## Working prototype (2026-11-18)

- `runtime/mk8/graphics/fsr.cpp` loads an FFX upscaler provider, creates a
  context for the game image and the window, and records one upscale in the
  present command buffer.
- `toolchain/fsr.py` adds the present call, tracks the last scene depth target,
  and enables `VK_KHR_cooperative_matrix` + `VK_EXT_shader_float8` when the
  driver exposes them.
- The FSR 4.1.2 provider from the custom SDK passes its 11 provider cases
  byte-identically on the RX 9070 XT.
- In-game evidence: `[fsr] provider FSR 4.1.2`, `[fsr] upscale active
  1920x1080 -> 2560x1440`, correct colors with
  `FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB`; screenshots `fsr4-srgb-03600.png`
  and `fsr4-3600` in `local/qualification`. The dispatch now runs after
  `vk_frame_suspend`, outside the window render pass.

The switch is diagnostic only: `MK8_FSR4=1`, `MK8_FSR_LIB=<provider.so>`,
`FSR4_ASSET_DIR=<assets>`. It is not exposed in the launcher yet.

## Not qualified / open

- **Real inputs:** a scene depth target was observed at 960x540 while the
  presented color is 1920x1080. The hook records the last scene depth and
  contains a depth-copy pass to the color size, but the pointer is not usable at
  present time (the texture has been retired by then), so the prototype still
  falls back to a neutral depth image and zero motion vectors, resets history
  every frame, and uses zero jitter. The next step is a pass-end copy into a
  persistent depth image; real scene color/depth pairing, camera motion vectors,
  jitter and UI separation remain the gate.
- **FSR 3.1.6:** the SDK source build currently produces empty shader
  permutation tables with the Linux/Wine shader generator, so its provider
  prototype is not integrated. A Windows-generated SDK or a fixed generator is
  required.
- **Frame generation:** FSR FG 3.1.6 source is available; native Linux FG 4.x
  is a research spike (the signed DLL contains 487 SPIR-V candidates, 359 of
  which validate, but the pass/model ABI is not reconstructed).
- **Windows/Proton:** not wired yet. The Windows path should load AMD's signed
  upscaler/FG DLLs from the user's SDK; no signed binaries or extracted assets
  are committed.

## Licensing

- The FFX API headers under `runtime/mk8/graphics/ffx` are MIT (AMD notice in
  `LICENSE`).
- The FSR 4.1.2 native provider is the GPL-3.0 port from the custom SDK; it is
  loaded as an optional component and is not linked into the game library.
- AMD signed DLLs and extracted shaders/models are never committed.
