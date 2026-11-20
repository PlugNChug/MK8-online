"""FSR 4.1.2/3.1.6 host bridge hooks (runtime/mk8/graphics/fsr.cpp).

The provider is optional and loaded by the game library. These hooks capture
the scene depth at its pass boundary (the texture is retired by present time),
resample it to the presented color size, and run the upscaler outside the
window render pass. No guest code or ABI changes.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ANCHOR = ("    extern void vk_frame_suspend(void);\n"
          "    extern void vk_frame_resume(void);\n"
          "    vk_frame_suspend();\n")
CALL = ANCHOR + """    if (mk8_temporal_requested()) { extern VkImageView mk8_fsr4_upscale(VkCommandBuffer, TexBackend *, TexBackend *, uint32_t, uint32_t,
                                         uint32_t, uint32_t, float);
      TexBackend *fsr_depth = g_fsr_captured;
      if (fsr_depth && (fsr_depth->width != b->width || fsr_depth->height != b->height)) {
          TexBackend *copy = post_image(22, b->width, b->height, VK_FORMAT_R32_SFLOAT);
          VkImageView dv = vk_image_view(fsr_depth, VK_IMAGE_VIEW_TYPE_2D, fsr_depth->format, 0, 1, 0, 1,
                                         (VkComponentMapping){0}, VK_IMAGE_ASPECT_COLOR_BIT);
          float dpc[8] = {(float)copy->width, (float)copy->height, 0, 0, 0, 0, 0, 0};
          post_pass(cb, "depth_copy.frag", &dv, 1, copy, dpc, false);
          fsr_depth = copy;
      }
      VkImageView fsr4 = mk8_fsr4_upscale(cb, b, fsr_depth, b->width, b->height, ow, oh, 16.7f);
      if (fsr4) mk8_graphics_result(getenv("MK8_DLSS") ? "dlss" : getenv("MK8_FSR_VERSION"), b->width, b->height, ow, oh);
      if (fsr4) { vk_full_barrier(cb); vk_frame_resume(); return fsr4; } }
"""

CAPTURE = """    if (mk8_temporal_requested() && g_pass_depth && g_pass_depth->backend) {
        TexBackend *db = (TexBackend *)g_pass_depth->backend;
        if (db->image && db->width >= 640 && db->height >= 360 && db->width * 9 == db->height * 16) {
            uint32_t cw = db->width, ch = db->height;
            static int cap_log;
            if (cap_log++ < 3) LOG("[fsr] capture scene depth %ux%u fmt %d%s\\n", db->width, db->height, db->format,
                                   cw != db->width || ch != db->height ? " (resampled to the presented size)" : "");
            TexBackend *cap = post_image(21, cw, ch, VK_FORMAT_R32_SFLOAT);
            VkImageAspectFlags da = (db->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                                             : VK_IMAGE_ASPECT_COLOR_BIT;
            VkImageView dv = vk_image_view(db, VK_IMAGE_VIEW_TYPE_2D, db->format, 0, 1, 0, 1,
                                           (VkComponentMapping){0}, da);
            float pc[8] = {(float)cw, (float)ch, 0, 0, 0, 0, 0, 0};
            post_pass(vk_cmd(), "depth_copy.frag", &dv, 1, cap, pc, false);
            g_fsr_captured = cap;
        }
    }
"""

MATRIX_SCAN = """    if (mk8_temporal_requested()) {
        for (int c = 3; c < 21; c++) {
            Range ub = st.ubuf[0][c - 3];
            if (!((pr->vi->cbuf_mask >> c) & 1) || !ub.addr || ub.size < 44 * 4) continue;
            const float *u = (const float *)(uintptr_t)ub.addr;
            if (fabsf(u[38] + 1.0f) > 0.02f || fabsf(u[42] + 1.0f) > 0.02f ||
                fabsf(u[43]) > 0.001f || fabsf(u[31]) > 0.001f || fabsf(u[35]) > 0.001f)
                continue;
            memcpy(g_fsr_view, u, sizeof g_fsr_view);
            memcpy(g_fsr_proj, u + 28, sizeof g_fsr_proj);
            g_fsr_vp_valid = 1;
        }
        { static int once; if (g_fsr_vp_valid && !once++) LOG("[fsr] view/proj captured in main draw\\n"); }
    }
"""
PROTOTYPES = """static void post_pass(VkCommandBuffer cb, const char *fs, const VkImageView *srcs, int n, TexBackend *dst,
                      const float pc[8], bool clear);
static TexBackend *post_image(int i, uint32_t w, uint32_t h, VkFormat f);
"""

DEPTH_SHADER = '''    if (!strcmp(name, "depth_copy.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\\n"
               "layout(location = 0) out vec4 o;\\n"
               // pc.a.xy is the destination size: the source may be the game's render size while the
               // destination is the window or the upscaler's render size.
               "void main() { o = vec4(texture(t, gl_FragCoord.xy / pc.a.xy).r, 0.0, 0.0, 1.0); }\\n";
    }
'''


def prepare_exec(runtime):
    exec_c = runtime / 'nvn/vulkan/exec.c'
    text = exec_c.read_text(encoding='utf-8')
    if text.count(ANCHOR) != 1:
        raise ValueError('Present post-process boundary changed upstream')
    start = text.find('    if (mk8_temporal_requested()) { extern VkImageView mk8_fsr4_upscale')
    if start >= 0:
        marker = 'if (fsr4) { vk_full_barrier(cb); vk_frame_resume(); return fsr4; } }'
        end = text.find(marker, start)
        if end < 0:
            raise ValueError('Previous FSR block is malformed')
        end = text.find('\n', end)
        if end < 0:
            raise ValueError('Previous FSR block has no terminator')
        text = text[:start] + text[end + 1:]
    if 'mk8_fsr4_upscale' not in text:
        text = text.replace(ANCHOR, CALL, 1)
    include = '#include "vk.h"'
    if text.count(include) != 1:
        raise ValueError('Vulkan header changed upstream')
    if 'static bool mk8_temporal_requested(void)' not in text:
        text = text.replace(include, include + '''
static void mk8_graphics_result(const char *method, unsigned iw, unsigned ih, unsigned ow, unsigned oh) {
    if (!method) method = "fsr";\n    static const char *last;
    static unsigned w, h, x, y, frames;
    if (last == NULL || strcmp(method, last) || iw != w || ih != h || ow != x || oh != y || ++frames >= 300) {
        LOG("[graphics] %s %ux%u -> %ux%u\\n", method, iw, ih, ow, oh);
        last = method; w = iw; h = ih; x = ow; y = oh; frames = 0;
    }
}
static bool mk8_temporal_requested(void) {
    static int requested = -1;
    if (requested < 0) requested = getenv("MK8_FSR4") != NULL || getenv("MK8_DLSS") != NULL;
    return requested != 0;
}
''', 1)
    old = 'static NvnTexture *g_pass_depth;'
    if text.count(old) != 1:
        raise ValueError('Pass depth declaration changed upstream')
    if 'TexBackend *g_fsr_captured;' not in text:
        text = text.replace(old, old + '\nTexBackend *g_fsr_captured;'
                            '\nfloat g_fsr_view[12], g_fsr_proj[16];'
                            '\nint g_fsr_vp_valid;'
                            '\nfloat g_fsr_jitter_x, g_fsr_jitter_y;', 1)
    if 'FG_DUMMY = 20, N_POST = 21' in text:
        text = text.replace('FG_DUMMY = 20, N_POST = 21', 'FG_DUMMY = 20, N_POST = 23', 1)
    elif 'FG_DUMMY = 20, FSR_DEPTH = 21, N_POST = 22' in text:
        text = text.replace('FG_DUMMY = 20, FSR_DEPTH = 21, N_POST = 22', 'FG_DUMMY = 20, N_POST = 23', 1)
    # The FSR/FG motion-vector compute allocates a storage-image descriptor from the runtime's
    # descriptor pool, which was created without that type. Add it only when FSR/FG runs.
    old = ('            VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 32768}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096},\n'
           '                                            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32768},\n'
           '                                            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1024}, {VK_DESCRIPTOR_TYPE_SAMPLER, 1024}};\n'
           '            VkDescriptorPoolCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 8192,\n'
           '                                             .poolSizeCount = 5, .pPoolSizes = sizes};')
    new = ('            VkDescriptorPoolSize sizes[6] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 32768}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096},\n'
           '                                            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32768},\n'
           '                                            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1024}, {VK_DESCRIPTOR_TYPE_SAMPLER, 1024}};\n'
           '            bool fsr_mv_pool = mk8_temporal_requested();\n'
           '            if (fsr_mv_pool) sizes[5] = (VkDescriptorPoolSize){VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1024};\n'
           '            VkDescriptorPoolCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 8192,\n'
           '                                             .poolSizeCount = fsr_mv_pool ? 6 : 5, .pPoolSizes = sizes};')
    if 'fsr_mv_pool' not in text:
        if text.count(old) != 1:
            raise ValueError('Descriptor pool sizes changed upstream')
        text = text.replace(old, new, 1)

    anchor = 'static void vk_end_pass_at(int line);'
    at = text.find(anchor)
    if at >= 0 and 'static void post_pass(VkCommandBuffer cb' not in text[:at]:
        text = text[:at] + PROTOTYPES + text[at:]
    old = '    vkCmdEndRendering(vk_cmd());\n    in_pass = false;\n'
    if text.count(old) != 1:
        raise ValueError('Pass end boundary changed upstream')
    if 'g_fsr_captured = cap;' not in text:
        text = text.replace(old, old + CAPTURE, 1)
    # The draw call line carries a trailing marker from another adapter, so anchor
    # on the call itself and fail loudly if it disappears.
    draw_anchor = 'debug_draw(pr, r, indexed);'
    if text.count(draw_anchor) != 1:
        raise ValueError('Main draw boundary changed upstream')
    if 'view/proj captured' not in text:
        line = text.rfind('\n', 0, text.find(draw_anchor)) + 1
        text = text[:line] + MATRIX_SCAN + text[line:]
    exec_c.write_text(text, encoding='utf-8')


def prepare_shaders(runtime):
    shaders = runtime / 'nvn/vulkan/shaders.c'
    text = shaders.read_text(encoding='utf-8')
    if 'depth_copy.frag' in text:
        return
    anchor = '    if (!strcmp(name, "easu.frag")) {'
    if text.count(anchor) != 1:
        raise ValueError('Shader table boundary changed upstream')
    shaders.write_text(text.replace(anchor, DEPTH_SHADER + anchor, 1), encoding='utf-8')


def prepare_device(runtime):
    device = runtime / 'nvn/vulkan/device.c'
    text = device.read_text(encoding='utf-8')
    if 'FSR features' in text and 'VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME' in text:
        return
    old = ('    bool rob2 = robust && has_ext(ep, ne, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);\n'
           '    if (rob2) ext[n_ext++] = VK_EXT_ROBUSTNESS_2_EXTENSION_NAME;\n'
           '    free(ep);')
    new = ('    bool rob2 = robust && has_ext(ep, ne, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);\n'
           '    if (rob2) ext[n_ext++] = VK_EXT_ROBUSTNESS_2_EXTENSION_NAME;\n'
           '    bool ml = getenv("MK8_FSR_VERSION") && !strcmp(getenv("MK8_FSR_VERSION"), "4.1.2");\n'
           '    bool coopmat = ml && has_ext(ep, ne, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);\n'
           '    if (coopmat) ext[n_ext++] = VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME;\n'
           '    bool float8 = ml && has_ext(ep, ne, VK_EXT_SHADER_FLOAT8_EXTENSION_NAME);\n'
           '    if (float8) ext[n_ext++] = VK_EXT_SHADER_FLOAT8_EXTENSION_NAME;\n'
           '    free(ep);')
    if text.count(old) != 1:
        raise ValueError('Device extension boundary changed upstream')
    text = text.replace(old, new, 1)
    old = ('    VkPhysicalDeviceRobustness2FeaturesEXT have_r2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};\n'
           '    VkPhysicalDeviceVulkan13Features have13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,\n'
           '                                               .pNext = rob2 ? &have_r2 : NULL};')
    new = ('    VkPhysicalDeviceShaderFloat8FeaturesEXT have_f8 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT};\n'
           '    VkPhysicalDeviceCooperativeMatrixFeaturesKHR have_cm = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR, .pNext = &have_f8};\n'
           '    VkPhysicalDeviceRobustness2FeaturesEXT have_r2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, .pNext = &have_cm};\n'
           '    VkPhysicalDeviceVulkan13Features have13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,\n'
           '                                               .pNext = rob2 ? &have_r2 : (void *)&have_cm};')
    if text.count(old) != 1:
        raise ValueError('Device feature query boundary changed upstream')
    text = text.replace(old, new, 1)
    old = ('                                            .pNext = g_vk.custom_border ? (void *)&cbc : g_vk.divisor ? (void *)&div : NULL};\n'
           '    VkPhysicalDeviceVulkan12Features f12')
    new = ('                                            .pNext = g_vk.custom_border ? (void *)&cbc : g_vk.divisor ? (void *)&div : NULL};\n'
           '    VkPhysicalDeviceShaderFloat8FeaturesEXT f8 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT,\n'
           '                                                  .shaderFloat8 = float8 && have_f8.shaderFloat8,\n'
           '                                                  .shaderFloat8CooperativeMatrix = float8 && have_f8.shaderFloat8CooperativeMatrix,\n'
           '                                                  .pNext = f13.pNext};\n'
           '    VkPhysicalDeviceCooperativeMatrixFeaturesKHR cm = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR,\n'
           '                                                       .cooperativeMatrix = coopmat && have_cm.cooperativeMatrix, .pNext = float8 ? (void *)&f8 : f13.pNext};\n'
           '    if (coopmat) f13.pNext = &cm; else if (float8) f13.pNext = &f8;\n'
           '    LOG("[vk] FSR features: cooperative matrix %d, shader float8 %d\\n", (int)coopmat, (int)float8);\n'
           '    VkPhysicalDeviceVulkan12Features f12')
    if text.count(old) != 1:
        raise ValueError('Device creation boundary changed upstream')
    text = text.replace(old, new, 1)
    text = text.replace('const char *ext[8];', 'const char *ext[16];')
    device.write_text(text, encoding='utf-8')


def prepare_settings(runtime):
    def replace(file, old, new):
        path = runtime / file
        source = path.read_text(encoding='utf-8')
        if new in source:
            return
        if source.count(old) != 1:
            raise ValueError(f'Graphics setting boundary changed in {file}')
        path.write_text(source.replace(old, new), encoding='utf-8')

    replace('settings.h', '/* Windows: AMD FSR 1 */ };',
            '/* Windows: AMD FSR 1 */, UPSCALE_FSR3, UPSCALE_FSR4, UPSCALE_DLSS };')
    replace('settings.c', '? UPSCALE_BILINEAR : UPSCALE_SHARP;',
            '? UPSCALE_BILINEAR : !strcmp(val, "fsr3") ? UPSCALE_FSR3 : !strcmp(val, "fsr4") ? UPSCALE_FSR4 : !strcmp(val, "dlss") ? UPSCALE_DLSS : UPSCALE_SHARP;')
    replace('settings.c', 'g_settings.upscaler == UPSCALE_METALFX ? "metalfx"',
            'g_settings.upscaler == UPSCALE_FSR3 ? "fsr3" : g_settings.upscaler == UPSCALE_FSR4 ? "fsr4" : g_settings.upscaler == UPSCALE_DLSS ? "dlss" : g_settings.upscaler == UPSCALE_METALFX ? "metalfx"')
    # Once an actual display buffer has been identified, let the existing renderer rescale it on
    # its next use. The guest can present a plain RGBA texture without the render-target flag.
    replace('nvn/vulkan/device.c', '    b->presented = true;', """    b->presented = true;
    if (!b->scalable && (getenv("MK8_FSR4") || getenv("MK8_DLSS")) && !getenv("LARECOMP_NO_SCALE") &&
        t->target == 1 && t->samples <= 1 && t->levels == 1 && t->width == 1920 && t->height == 1080)
        b->scalable = true;""")
    replace('nvn/vulkan/exec.c', 'g_settings.upscaler == UPSCALE_FSR1 &&', 'g_settings.upscaler >= UPSCALE_FSR1 &&')
    replace('nvn/vulkan/exec.c', 'if (!smaa && !fsr) return v;', '''if (!smaa && !fsr) {
        mk8_graphics_result("native", b->width, b->height, ow, oh);
        return v;
    }''')
    replace('nvn/vulkan/exec.c', '    if (smaa) {  // SMAA 1x:', '    mk8_graphics_result(fsr ? "fsr1" : "native", b->width, b->height, ow, oh);\n    if (smaa) {  // SMAA 1x:')
    # Count based on the same wall-clock window. ImGui is also drawn on interpolated frames.
    replace('ui/menu.mm', 'static double t0, out_ratio;', 'static double t0, game_fps, present_fps;')
    replace('ui/menu.mm', 'out_ratio = dr ? 1.0 + (double)dg / dr : 1.0;',
            'if (t0 && now > t0) { game_fps = dr / (now - t0); present_fps = (dr + dg) / (now - t0); }')
    replace('ui/menu.mm', 'if (out_ratio > 1.05) ImGui::Text("%.0f fps (%.0f with frame generation)", io.Framerate, io.Framerate * out_ratio);\n            else ImGui::Text("%.0f fps", io.Framerate);',
            'if (game_fps > 0) ImGui::Text("Game %.0f FPS | Output %.0f FPS | %.1f ms", game_fps, present_fps, 1000.0 / game_fps);')
    # Count real and repeated window submissions separately. A held frame can refresh
    # the display without the game rendering a new frame.
    replace('nvn/framegen.h', 'extern uint64_t g_fg_real, g_fg_generated;', 'extern uint64_t g_fg_real, g_fg_generated, g_mk8_game_frames, g_mk8_output_frames;')
    replace('nvn/framegen.c', 'uint64_t g_fg_real, g_fg_generated;', 'uint64_t g_fg_real, g_fg_generated, g_mk8_game_frames, g_mk8_output_frames;')
    replace('ui/menu.mm', 'uint64_t dr = g_fg_real - real0, dg = g_fg_generated - gen0;', 'uint64_t dr = g_mk8_game_frames - real0, output = g_mk8_output_frames - gen0;')
    replace('ui/menu.mm', 'present_fps = (dr + dg) / (now - t0);', 'present_fps = output / (now - t0);')
    replace('ui/menu.mm', 'real0 = g_fg_real, gen0 = g_fg_generated, t0 = now;', 'real0 = g_mk8_game_frames, gen0 = g_mk8_output_frames, t0 = now;')
    replace('nvn/vulkan/device.c', 'else vk_frame_end(shot || req ? shot_path : NULL);',
            'else vk_frame_end(shot || req ? shot_path : NULL);\n        g_mk8_output_frames++;\n        if (!g_held) g_mk8_game_frames++;')
    # Increment after the intermediate frame is submitted.
    replace('nvn/framegen.c', '    if (gen) g_fg_generated++;', '    // The Vulkan presenter counts the generated image after submission.')
    replace('nvn/vulkan/device.c', 'vk_frame_end(fg_base[0] ? gen_path : NULL);', 'vk_frame_end(fg_base[0] ? gen_path : NULL);\n                g_fg_generated++; g_mk8_output_frames++;')


def prepare(runtime):
    prepare_exec(runtime)
    prepare_settings(runtime)
    prepare_shaders(runtime)
    prepare_device(runtime)
