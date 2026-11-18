"""FSR 4.1.2/3.1.6 host bridge hooks (runtime/mk8/graphics/fsr.cpp).

The provider is an optional shared library loaded by the game library. These
patches add the present call, a depth resample from the game's scene depth to
the presented color size, and the FP8/cooperative-matrix device features.
No guest code or ABI changes.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ANCHOR = ("    extern void vk_frame_suspend(void);\n"
          "    extern void vk_frame_resume(void);\n"
          "    vk_frame_suspend();\n")
CALL = ANCHOR + """    { extern VkImageView mk8_fsr4_upscale(VkCommandBuffer, TexBackend *, TexBackend *, uint32_t, uint32_t,
                                         uint32_t, uint32_t, float);
      TexBackend *fsr_depth = NULL;
      NvnTexture *fsr_dt = mk8_fsr_last_depth();
      if (fsr_dt && fsr_dt->backend) {
          TexBackend *db = (TexBackend *)fsr_dt->backend;
          if (db->image && db->width == b->width && db->height == b->height) fsr_depth = db;
          else if (db->image && db->width && db->height) {
              fsr_depth = post_image(FSR_DEPTH, b->width, b->height, VK_FORMAT_R32_SFLOAT);
              VkImageAspectFlags da = (db->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                                              : VK_IMAGE_ASPECT_COLOR_BIT;
              VkImageView dv = vk_image_view(db, VK_IMAGE_VIEW_TYPE_2D, db->format, 0, 1, 0, 1,
                                             (VkComponentMapping){0}, da);
              float dpc[8] = {0};
              post_pass(cb, "depth_copy.frag", &dv, 1, fsr_depth, dpc, false);
          }
      }
      VkImageView fsr4 = mk8_fsr4_upscale(cb, b, fsr_depth, b->width, b->height, ow, oh, 16.7f);
      if (fsr4) { vk_full_barrier(cb); vk_frame_resume(); return fsr4; } }
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
    # Replace a previous prototype block, if any, before inserting the current one.
    start = text.find('    { extern VkImageView mk8_fsr4_upscale')
    if start >= 0:
        end = text.find('\n    }\n', start)
        if end < 0:
            raise ValueError('Previous FSR block is malformed')
        text = text[:start] + text[end + len('\n    }\n'):]
    if 'mk8_fsr4_upscale' not in text:
        text = text.replace(ANCHOR, CALL, 1)
    old = 'static NvnTexture *g_pass_depth;'
    if text.count(old) != 1:
        raise ValueError('Pass depth declaration changed upstream')
    if 'NvnTexture *g_fsr_depth;' not in text:
        text = text.replace(old, old + '\nNvnTexture *g_fsr_depth;', 1)
    if 'NvnTexture *mk8_fsr_last_depth(void) { return g_pass_depth; }' in text:
        text = text.replace('NvnTexture *mk8_fsr_last_depth(void) { return g_pass_depth; }',
                            'NvnTexture *mk8_fsr_last_depth(void) { return g_fsr_depth; }', 1)
    if 'NvnTexture *mk8_fsr_last_depth(void)' not in text:
        text = text.replace('NvnTexture *g_fsr_depth;',
                            'NvnTexture *g_fsr_depth;\nNvnTexture *mk8_fsr_last_depth(void) { return g_fsr_depth; }', 1)
    old = '    g_pass_depth = st.rt.depth;'
    if text.count(old) != 1:
        raise ValueError('Pass depth assignment changed upstream')
    if 'g_fsr_depth = st.rt.depth;' not in text:
        text = text.replace(old, old + '\n    if (st.rt.depth) g_fsr_depth = st.rt.depth;', 1)
    old = 'FG_DUMMY = 20, N_POST = 21'
    if text.count(old) == 1 and 'FSR_DEPTH = 21' not in text:
        text = text.replace(old, 'FG_DUMMY = 20, FSR_DEPTH = 21, N_POST = 22', 1)
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
