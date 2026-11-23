# Mesa 27 RADV race-entry segfault: null PS-epilog cache

Status 20/11/2026. Companion to `docs/FSR4-MLFI.md`, `docs/MLFI-REPLAY.md` and
`docs/RADV-MLFI-CRASH.md`. That document covers a different RADV defect (invalid SPIR-V in
`vkCreateComputePipelines`); this one is a **draw-time** null dereference that blocks every race on
Mesa 27, including FG-off.

Short answer: Mesa 27's RADV initialises `device->ps_epilogs` only when one of the
`VK_EXT_extended_dynamic_state3` features is enabled. This runtime uses the **core** dynamic
`VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE` and its race fragment shaders write depth, so
`radv_shader_info` sets `ps.has_epilog` through `depth_output_needs_epilog`. The first draw that needs
that epilog calls `radv_bind_ps_epilog` -> `lookup_ps_epilog` -> `radv_shader_part_cache_get` on the
uninitialised cache, whose `ops` table is NULL, and segfaults inside `vkCmdDrawIndexed`. Mesa 26.2.6
does not hit it. `toolchain/radv_epilog.py` enables one extended-dynamic-state-3 feature when the
driver advertises it; with that, races run on Mesa 27. Stable 26.2.6 lacks
`VK_EXT_cooperative_matrix_maintenance1`, which the MLFI provider needs, so the workaround is what
makes MLFI race validation possible at all.

## 1. Environment and evidence

| item | value |
| --- | --- |
| GPU | AMD Radeon RX 9070 XT (RADV GFX1201) |
| driver | `mesa-git 27.0.0_devel.232438.f7dd61f78f8-1`, commit `f7dd61f78f83c9ffb3dc9c980cd9b303a5ce58b1` (2026-10-26) |
| stable comparison | RADV 26.2.6 (`local/qualification/stable-radv/radv.json`), which has no race crash |
| runtime | `local/qualification/fsr4-fg-2026-11-20/builds/3046f863b9cf8592/.../mk8drecomp` |
| crash logs | `local/qualification/fsr4-fg-2026-11-20/fgoff.log` (FG off), `fsr3q.log` (FSR 3), `fsr4c.log` (MLFI) |
| gdb logs | `local/qualification/mesa-race-2026-11-20/gdb-release.log` (release driver), `gdb-debug.log` (same Mesa commit, debug build) |

All three logs crash at the same point: after the track load, at about frame 2110-2130, immediately
after the first race-entry `[nvn] ReportCounter type 1` / `[mk8] samples-passed reset recorded`:

```
*** signal 11 accessing 0
    host pc ... libvulkan_radeon.so+19d4f3
  #5 libvulkan_radeon.so+19d4f3
  #6 libvulkan_radeon.so+100ce0
  #7 libvulkan_radeon.so+1086a2
  #8 libvulkan_radeon.so+11e8d2
  #9 mk8drecomp+2b0a44   (draw_)
 #10 mk8drecomp+29d9a2   (exec_list)
 #11 mk8drecomp+29cead   (exec_list)
 #12 mk8drecomp+29c2f3   (backend_execute)
 #13 mk8drecomp+2690cd   (mk8_q_worker)
```

## 2. Root cause

A gdb run with the same Mesa commit (debug build, `-Dbuildtype=debug -Dvulkan-drivers=amd
-Dplatforms=wayland`) gives the exact site:

```
#0 radv_shader_part_cache_get (device=..., cache=..., local_entries=..., key=...)
    at ../src/amd/vulkan/radv_shader.c:3426
#1 lookup_ps_epilog (cmd_buffer=...) at ../src/amd/vulkan/radv_cmd_buffer.c:6824
#2 radv_bind_ps_epilog (cmd_buffer=...) at ../src/amd/vulkan/radv_cmd_buffer.c:9421
#3 radv_emit_all_graphics_states (..., indexed_draw=true, draw_vertex_count=3, ...)
    at ../src/amd/vulkan/radv_cmd_buffer.c:14086
#4 radv_emit_graphics_states (...) at ../src/amd/vulkan/radv_cmd_buffer.c:14269
#5 radv_before_draw (...) at ../src/amd/vulkan/radv_cmd_buffer.c:14421
#6 radv_CmdDrawIndexed (...) at ../src/amd/vulkan/radv_cmd_buffer.c:14651
#7 draw_ ()
#8 exec_list ()
#9 exec_list ()
#10 backend_execute ()
#11 mk8_q_worker ()
```

The faulting instruction is `mov (%rax),%rdx; call *%rdx` with `rax = 0` (from the release-driver gdb
run: `call *(%rax)`, `rax = 0`). In `radv_shader_part_cache_get` (radv_shader.c:3415-3426):

```c
struct radv_shader_part *
radv_shader_part_cache_get(struct radv_device *device, struct radv_shader_part_cache *cache, ...)
{
   uint32_t hash = cache->ops->hash(key);     /* cache->ops == NULL */
```

`lookup_ps_epilog` passes `&device->ps_epilogs` (radv_cmd_buffer.c:6824). That cache is initialised in
`radv_device.c:1744-1749` only when at least one of these is enabled:

```c
device->vk.enabled_features.extendedDynamicState3AlphaToCoverageEnable ||
device->vk.enabled_features.extendedDynamicState3ColorBlendEquation ||
device->vk.enabled_features.extendedDynamicState3RasterizationSamples
```

This runtime does **not** use `VK_EXT_extended_dynamic_state3`; it uses the core dynamic states
(`VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE`, etc.). A pipeline with dynamic depth-test enable and a
fragment shader that writes depth gets `has_epilog` from
`radv_shader_info.c:886-889` (`depth_output_needs_epilog && writes_z`), but the cache guard above is
false, so `ps_epilogs.ops` remains NULL. Mesa 26.2.6 does not crash on the same draw (the stable
driver completes the identical script to the race), so this is a Mesa 27 regression in the epilog
cache initialisation/guard.

The game's pass at that point also logs `[vk] a pass with an empty color slot between 5 targets`; that
is a property of the same race pass, not the trigger. The trigger is any PS epilog, in this case the
depth output with dynamic depth-test state.

### Minimal reproducer (recipe)

A standalone Vulkan program that isolates the bug:

1. Create a Mesa 27 RADV device **without** enabling `VK_EXT_extended_dynamic_state3` (or any of its
   features).
2. Create a render pass / dynamic-rendering pipeline with one colour attachment and one depth
   attachment, and a fragment shader that writes `gl_FragDepth` (for example
   `void main(){ gl_FragDepth = 0.5; o = vec4(1); }`).
3. Put `VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE` in the pipeline's dynamic-state list and enable depth
   testing.
4. Bind a pipeline, begin a render pass, and call `vkCmdDrawIndexed(cb, 3, 1, 0, 0, 0)`.

`radv_shader_info` sets `ps.has_epilog`; `radv_bind_ps_epilog` calls `lookup_ps_epilog`;
`device->ps_epilogs.ops` is NULL and RADV segfaults. Enabling
`extendedDynamicState3ColorBlendEquation` in the device features makes the same program draw
normally.

## 3. Workaround

`toolchain/radv_epilog.py` (called from `toolchain/host.py` after the FSR/FG patches) adds
`VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME` to the device extensions when the driver advertises
it, queries `VkPhysicalDeviceExtendedDynamicState3FeaturesEXT`, and enables
`extendedDynamicState3ColorBlendEquation` when supported. This runtime does not use dynamic colour
blend, so the pipeline state is unchanged; the feature only makes RADV initialise the PS-epilog
cache. If the extension or feature is missing, the device is created exactly as before and one
warning is logged.

Verified in-engine on Mesa 27, 1280x720 or 2560x1440 as noted:

```
[vk] Mesa 27 PS-epilog workaround: extendedDynamicState3ColorBlendEquation enabled
[fsr_fg] active: FFX frame-interpolation swapchain ...        (FSR 3 / MLFI runs)
...
[vk] saved .../fgoff-fix-02400.png (2560x1440)                (race reached, exit 0)
```

- FG off: `local/qualification/mesa-race-2026-11-20/fgoff-fix.log`, `fgoff-fix-02400.png` (race,
  1st Race), exit 0.
- FSR 3 FG at 1280×720: `fsr3-720b.log`, `fsr3-720b-02400.png` (race, 1st Race), exit 0.
- FSR 3 FG at 1920×1080: `fsr3-1080.log`, `fsr3-1080-02400.png` (course entry at frame 2400), exit 0.
- MLFI 4.0.2 at 1280×720: `mlfi-720b.log`, `mlfi-720b-02400.png` (race, 1st Race), 1,800 generated
  frames, exit 0. See `MLFI-QUALITY.md`.

The workaround is a runtime device-feature change, not a claim that the driver is fixed. A control
run of the unpatched build with `RADV_DEBUG=nocache` still crashes at the same
`libvulkan_radeon.so+19d4f3` (`local/qualification/fsr4-fg-2026-11-20/nocache27.log`), so the driver
pipeline cache is not the cause and disabling it is not a workaround. The upstream fix should
initialise `ps_epilogs` whenever a PS epilog can be required (or unconditionally), not only for the
extended-dynamic-state-3 features; `radv_shader_part_cache_get`/`lookup_ps_epilog` should also fail
pipeline creation cleanly instead of dereferencing a NULL ops table.

## 4. Stable RADV and MLFI

Stable RADV 26.2.6 does not have this crash, but it also does not expose
`VK_EXT_cooperative_matrix_maintenance1`, `shader_float8` and the other MLFI device features. The
MLFI provider refuses context creation there (`the MLFI extensions are unavailable`), so MLFI race
validation requires Mesa 27 plus this workaround. The 3.1.7 FSR 3 FG provider and the 3.1.6
upscaler do not need the MLFI features and can run on either driver; the workaround is harmless on
26.2.6 when the extension is advertised.

## 5. What remains

- The workaround is a GPU/device-feature path; it was validated on RADV GFX1201/Mesa 27 only. Other
  Vulkan drivers that expose the extension but not the feature are left unchanged (warning).
- The race screenshots are the game's real render images; generated-frame quality is addressed in
  `docs/MLFI-QUALITY.md`, including what could and could not be captured.
- A standalone reproducer binary is a recipe here; the in-engine gdb stack is the empirical
  reproducer. A dedicated minimal program would be the right attachment to a Mesa issue.
