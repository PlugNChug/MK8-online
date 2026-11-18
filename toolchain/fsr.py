"""FSR 4.1.2/3.1.6 host bridge hooks (runtime/mk8/graphics/fsr.cpp).

The provider is an optional shared library loaded by the game library; this
patch only adds the call in the present path when the diagnostic switch is
set. No guest code or ABI changes.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ANCHOR = ("    extern void vk_frame_resume(void);\n"
          "    vk_frame_suspend();\n")
CALL = ("    { extern VkImageView mk8_fsr4_upscale(VkCommandBuffer, TexBackend *, uint32_t, uint32_t,\n"
        "                                             uint32_t, uint32_t, float);\n"
        "      VkImageView fsr4 = mk8_fsr4_upscale(cb, b, b->width, b->height, ow, oh, 16.7f);\n"
        "      if (fsr4) { vk_full_barrier(cb); vk_frame_resume(); return fsr4; } }\n")


def prepare_exec(runtime):
    exec_c = runtime / 'nvn/vulkan/exec.c'
    text = exec_c.read_text(encoding='utf-8')
    if text.count(ANCHOR) != 1:
        raise ValueError('Present post-process boundary changed upstream')
    if 'mk8_fsr4_upscale' not in text:
        text = text.replace(ANCHOR, ANCHOR.splitlines()[0] + '\n' + CALL + "    vk_frame_suspend();\n", 1)
    old = 'static NvnTexture *g_pass_depth;'
    if text.count(old) != 1:
        raise ValueError('Pass depth declaration changed upstream')
    if 'mk8_fsr_last_depth' not in text:
        text = text.replace(old, old + '\nNvnTexture *g_fsr_depth;\nNvnTexture *mk8_fsr_last_depth(void) { return g_fsr_depth; }', 1)
    old = '    g_pass_depth = st.rt.depth;'
    if text.count(old) != 1:
        raise ValueError('Pass depth assignment changed upstream')
    if 'g_fsr_depth = st.rt.depth;' not in text:
        text = text.replace(old, old + '\n    if (st.rt.depth) g_fsr_depth = st.rt.depth;', 1)
    exec_c.write_text(text, encoding='utf-8')


def prepare_device(runtime):
    device = runtime / 'nvn/vulkan/device.c'
    text = device.read_text(encoding='utf-8')
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
    prepare_device(runtime)
