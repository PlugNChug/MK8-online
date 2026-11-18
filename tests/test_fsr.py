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
static void render_pass(void) {
    g_pass_depth = st.rt.depth;
}
static VkImageView post_process(VkCommandBuffer cb, TexBackend *b, double ow_d, double oh_d) {
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
        return root

    def test_inserts_the_present_call_and_depth_getter(self):
        runtime = self.runtime()
        fsr.prepare(runtime)
        exec_c = (runtime / 'nvn/vulkan/exec.c').read_text(encoding='utf-8')
        self.assertIn('mk8_fsr4_upscale', exec_c)
        self.assertIn('mk8_fsr_last_depth', exec_c)
        self.assertIn('g_fsr_depth = st.rt.depth;', exec_c)
        self.assertLess(exec_c.index('mk8_fsr4_upscale'), exec_c.index('vk_frame_suspend();'))

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
