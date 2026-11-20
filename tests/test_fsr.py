"""FSR host hooks: the patch inserts the present call and device features, and refuses changed anchors."""
import tempfile
import unittest
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'toolchain'))

import fsr


EXEC = '''\
static NvnTexture *g_pass_depth;  // (its depth target)
enum { FG_DUMMY = 20, N_POST = 21 };
static void render_pass(void) {
    g_pass_depth = st.rt.depth;
    if (g_debug_frame || mk8_rtt_pass) debug_draw(pr, r, indexed);  // (toolchain/texture_pool.py)
}
static void end_pass(void) {
            VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 32768}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096},
                                            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32768},
                                            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1024}, {VK_DESCRIPTOR_TYPE_SAMPLER, 1024}};
            VkDescriptorPoolCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 8192,
                                             .poolSizeCount = 5, .pPoolSizes = sizes};
    vkCmdEndRendering(vk_cmd());
    in_pass = false;
}
static VkImageView post_process(VkCommandBuffer cb, TexBackend *b, double ow_d, double oh_d) {
    if (!smaa && !fsr) return v;
    extern void vk_frame_suspend(void);
    extern void vk_frame_resume(void);
    vk_frame_suspend();
    return vk;
}
'''

DEVICE = '''\
    bool rob2 = robust && has_ext(ep, ne, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
    if (rob2) ext[n_ext++] = VK_EXT_ROBUSTNESS_2_EXTENSION_NAME;
    free(ep);
    VkPhysicalDeviceRobustness2FeaturesEXT have_r2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    VkPhysicalDeviceVulkan13Features have13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                               .pNext = rob2 ? &have_r2 : NULL};
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                            .dynamicRendering = VK_TRUE,
                                            .pNext = g_vk.custom_border ? (void *)&cbc : g_vk.divisor ? (void *)&div : NULL};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                            .pNext = &f13};
'''


class FsrPatchTests(unittest.TestCase):
    def runtime(self):
        temp = tempfile.TemporaryDirectory(prefix='mk8-fsr-')
        self.addCleanup(temp.cleanup)
        root = Path(temp.name)
        (root / 'nvn/vulkan').mkdir(parents=True)
        (root / 'nvn/vulkan/exec.c').write_text(EXEC, encoding='utf-8')
        (root / 'nvn/vulkan/device.c').write_text(DEVICE, encoding='utf-8')
        (root / 'nvn/vulkan/shaders.c').write_text(
            'static const char *shader(const char *name, int *stage) {\n'
            '    if (!strcmp(name, "easu.frag")) {\n'
            '        *stage = 0; return "";\n'
            '    }\n'
            '    return NULL;\n'
            '}\n', encoding='utf-8')
        return root

    def test_inserts_the_present_call_and_depth_getter(self):
        runtime = self.runtime()
        fsr.prepare(runtime)
        exec_c = (runtime / 'nvn/vulkan/exec.c').read_text(encoding='utf-8')
        self.assertIn('mk8_fsr4_upscale', exec_c)
        self.assertIn('if (!smaa && !fsr && !getenv("MK8_FSR4") && !getenv("MK8_DLSS")) return v;', exec_c)
        self.assertIn('mk8_fsr_last_depth', exec_c)
        self.assertIn('g_fsr_depth = st.rt.depth;', exec_c)
        self.assertGreater(exec_c.index('mk8_fsr4_upscale'), exec_c.index('vk_frame_suspend();'))

    def test_defines_the_matrix_globals_and_scans_the_main_draw(self):
        runtime = self.runtime()
        fsr.prepare(runtime)
        exec_c = (runtime / 'nvn/vulkan/exec.c').read_text(encoding='utf-8')
        self.assertIn('float g_fsr_view[12], g_fsr_proj[16];', exec_c)
        self.assertNotIn('g_fsr_jitter_', exec_c)
        # The log format must survive as an escape, not as a real newline in the C string.
        self.assertIn('view/proj captured in main draw\\n")', exec_c)
        self.assertLess(exec_c.index('if (getenv("MK8_FSR4") || getenv("MK8_DLSS"))'),
                        exec_c.index('debug_draw(pr, r, indexed);'))

    def test_rejects_a_missing_main_draw(self):
        runtime = self.runtime()
        path = runtime / 'nvn/vulkan/exec.c'
        path.write_text(EXEC.replace('debug_draw(pr, r, indexed);', 'debug_draw(pr, r, 0);'), encoding='utf-8')
        with self.assertRaises(ValueError):
            fsr.prepare(runtime)

    def test_enables_the_fp8_device_features(self):
        runtime = self.runtime()
        fsr.prepare(runtime)
        device = (runtime / 'nvn/vulkan/device.c').read_text(encoding='utf-8')
        self.assertIn('VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME', device)
        self.assertIn('VK_EXT_SHADER_FLOAT8_EXTENSION_NAME', device)
        self.assertIn('FSR features', device)
        self.assertIn('cooperativeMatrix = coopmat', device)

    def test_rejects_a_changed_depth_assignment(self):
        runtime = self.runtime()
        path = runtime / 'nvn/vulkan/exec.c'
        path.write_text(EXEC.replace('g_pass_depth = st.rt.depth;', 'g_pass_depth = NULL;'), encoding='utf-8')
        with self.assertRaises(ValueError):
            fsr.prepare(runtime)


if __name__ == '__main__':
    unittest.main()
