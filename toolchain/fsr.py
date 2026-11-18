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
CALL = ANCHOR + """    { extern VkImageView mk8_fsr4_upscale(VkCommandBuffer, TexBackend *, TexBackend *, uint32_t, uint32_t,
                                         uint32_t, uint32_t, float);
      TexBackend *fsr_depth = g_fsr_captured;
      if (fsr_depth && (fsr_depth->width != b->width || fsr_depth->height != b->height)) {
          TexBackend *copy = post_image(22, b->width, b->height, VK_FORMAT_R32_SFLOAT);
          VkImageView dv = vk_image_view(fsr_depth, VK_IMAGE_VIEW_TYPE_2D, fsr_depth->format, 0, 1, 0, 1,
                                         (VkComponentMapping){0}, VK_IMAGE_ASPECT_COLOR_BIT);
          float dpc[8] = {0};
          post_pass(cb, "depth_copy.frag", &dv, 1, copy, dpc, false);
          fsr_depth = copy;
      }
      VkImageView fsr4 = mk8_fsr4_upscale(cb, b, fsr_depth, b->width, b->height, ow, oh, 16.7f);
      if (fsr4) { vk_full_barrier(cb); vk_frame_resume(); return fsr4; } }
"""

CAPTURE = """    if (getenv("MK8_FSR4_CAPTURE") && g_pass_depth && g_pass_depth->backend) {
        TexBackend *db = (TexBackend *)g_pass_depth->backend;
        if (db->image && db->width >= 640 && db->height >= 360 && db->width * 9 == db->height * 16) {
            static int cap_log;
            if (cap_log++ < 3) LOG("[fsr] capture scene depth %ux%u fmt %d\\n", db->width, db->height, db->format);
            TexBackend *cap = post_image(21, db->width, db->height, VK_FORMAT_R32_SFLOAT);
            VkImageAspectFlags da = (db->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                                             : VK_IMAGE_ASPECT_COLOR_BIT;
            VkImageView dv = vk_image_view(db, VK_IMAGE_VIEW_TYPE_2D, db->format, 0, 1, 0, 1,
                                           (VkComponentMapping){0}, da);
            float pc[8] = {0};
            post_pass(vk_cmd(), "depth_copy.frag", &dv, 1, cap, pc, false);
            g_fsr_captured = cap;
        }
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
               "layout(location = 0) out vec4 o;\\n"
               "void main() { o = vec4(texture(t, gl_FragCoord.xy / vec2(textureSize(t, 0))).r, 0.0, 0.0, 1.0); }\\n";
    }
'''


def prepare_exec(runtime):
    exec_c = runtime / 'nvn/vulkan/exec.c'
    text = exec_c.read_text(encoding='utf-8')
    if text.count(ANCHOR) != 1:
        raise ValueError('Present post-process boundary changed upstream')
    start = text.find('    { extern VkImageView mk8_fsr4_upscale')
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
    old = 'static NvnTexture *g_pass_depth;'
    if text.count(old) != 1:
        raise ValueError('Pass depth declaration changed upstream')
    if 'TexBackend *g_fsr_captured;' not in text:
        text = text.replace(old, old + '\nNvnTexture *g_fsr_depth;\nTexBackend *g_fsr_captured;', 1)
    if 'NvnTexture *mk8_fsr_last_depth(void)' not in text:
        text = text.replace('TexBackend *g_fsr_captured;',
                            'TexBackend *g_fsr_captured;\nNvnTexture *mk8_fsr_last_depth(void) { return g_fsr_depth; }', 1)
    old = '    g_pass_depth = st.rt.depth;'
    if text.count(old) != 1:
        raise ValueError('Pass depth assignment changed upstream')
    if 'g_fsr_depth = st.rt.depth;' not in text:
        text = text.replace(old, old + '\n    if (st.rt.depth) g_fsr_depth = st.rt.depth;', 1)
    if 'FG_DUMMY = 20, N_POST = 21' in text:
        text = text.replace('FG_DUMMY = 20, N_POST = 21', 'FG_DUMMY = 20, N_POST = 23', 1)
    elif 'FG_DUMMY = 20, FSR_DEPTH = 21, N_POST = 22' in text:
        text = text.replace('FG_DUMMY = 20, FSR_DEPTH = 21, N_POST = 22', 'FG_DUMMY = 20, N_POST = 23', 1)
    anchor = 'static void vk_end_pass_at(int line);'
    at = text.find(anchor)
    if at >= 0 and 'static void post_pass(VkCommandBuffer cb' not in text[:at]:
        text = text[:at] + PROTOTYPES + text[at:]
    old = '    vkCmdEndRendering(vk_cmd());\n    in_pass = false;\n'
    if text.count(old) != 1:
        raise ValueError('Pass end boundary changed upstream')
    if 'g_fsr_captured = cap;' not in text:
        text = text.replace(old, old + CAPTURE, 1)
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
           '    bool coopmat = has_ext(ep, ne, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);\n'
           '    if (coopmat) ext[n_ext++] = VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME;\n'
           '    bool float8 = has_ext(ep, ne, VK_EXT_SHADER_FLOAT8_EXTENSION_NAME);\n'
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
           '                                                       .cooperativeMatrix = coopmat && have_cm.cooperativeMatrix, .pNext = &f8};\n'
           '    f13.pNext = &cm;\n'
           '    LOG("[vk] FSR features: cooperative matrix %d, shader float8 %d\\n", (int)coopmat, (int)float8);\n'
           '    VkPhysicalDeviceVulkan12Features f12')
    if text.count(old) != 1:
        raise ValueError('Device creation boundary changed upstream')
    text = text.replace(old, new, 1)
    device.write_text(text, encoding='utf-8')


def prepare(runtime):
    prepare_exec(runtime)
    prepare_shaders(runtime)
    prepare_device(runtime)
