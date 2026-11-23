# Frame-generation Vulkan harness

`fg_harness.cpp` is a standalone headless Vulkan program that drives the native FidelityFX
frame-generation swapchain end to end: it creates a real swapchain, replaces it through
`ffxCreateContext`, records a prepare dispatch for every rendered frame, presents through the
provider and reads a generated frame back. It has no dependency on the game or the launcher.

Build and run:

```sh
g++ -std=c++17 -O2 -I runtime/mk8/graphics/ffx tools/fg-harness/fg_harness.cpp \
    -o fg-harness -lvulkan -ldl -pthread
./fg-harness --frames 120 --width 1280 --height 720 \
    --provider ~/.local/share/mk8online/fsr/libamd_fidelityfx_framegeneration_vk.so
```

`--frames`, `--width`, `--height`, `--shift`, `--provider`, `--surface` and
`--validation`/`--no-validation` are documented by `--help`. Provider resolution otherwise matches
the runtime: `MK8_FG_HARNESS_LIB`, `MK8_FSR_FG_LIB`, then the XDG/HOME install path.

`shaders.h` is generated from `shaders/pattern.vert` and `shaders/pattern.frag` by
`gen_shaders.py`; the harness has no shader compiler dependency at build time. Re-run the generator
after editing a shader.

The API sequence, the per-frame order, the provider quirks and the current validation evidence are in
[`docs/en/reports/FG-HARNESS.md`](../../docs/en/reports/FG-HARNESS.md). The test is `tests/test_fg_harness.py`.
