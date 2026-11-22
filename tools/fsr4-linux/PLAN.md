# FSR 4.1.2 native Linux — Phase 0 plan

Canonical plan: `docs/plans/2026-11-03-fsr4-1.2-native-linux.md` (kept with
the port's planning notes; the vendored copy does not include that private tree).

Working summary:
`../PLAN-FSR4-LINUX.md`

## Phase 0 checklist

- [x] Scaffold `linux/` and vendor `fsr4-vk`.
- [x] Port shader extractor (FFXZ + zlib → SPIR-V, no hardcoded offsets).
- [x] Port model-bank extractor (dispatcher tables, no hardcoded offsets).
- [x] Port SPIR-V catalog + selector-table decoder.
- [x] Add `extract_all.py` and `verify_assets.py`.
- [x] Run the full extraction and record counts/hashes (`PHASE0.md`).
- [~] Reconstruct the 496-byte `CsTensorSizes` table (31 × i32x4) — hypothesis in
  `PHASE0.md`; resolve `pass5_post`/index 6.
- [~] Reconstruct per-variant dispatch geometry — 4.1.1 formulas as starting point.
- [ ] Build a minimal native runner for one case.
- [ ] Compare against the Windows 4.1.2 DLL under Wine.
- [ ] Gate: non-degenerate output, target ≤1 ULP FP16 / ≤1 LSB UNORM.

## M1 after the gate

- Port provider/runtime to the 4.1.2 flat descriptor ABI.
- Build loader and stage both `.so` files next to the game.
- Patch game `vulkan_device.cpp` for descriptor buffer/FP8/cooperative matrix.
- Patch `fsr4_upscaler.cpp` for history/jitter/depth/MV and 10-bit formats.
- Run native `--present_effect=fsr4` at 1440p/4K, 60 FPS, no Wine.
