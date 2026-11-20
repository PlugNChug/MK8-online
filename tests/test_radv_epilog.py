"""The Mesa 27 RADV PS-epilog workaround: extension/feature insertion and idempotency."""
import tempfile
import unittest
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'toolchain'))

import radv_epilog


DEVICE = '''\
static void create_device(void) {
    const char *ext[16];
    uint32_t n_ext = 0;
    ext[n_ext++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    bool fault = has_ext(ep, ne, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
    if (fault) ext[n_ext++] = VK_EXT_DEVICE_FAULT_EXTENSION_NAME;
    g_fault_ext = fault;
    VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                   .pNext = rob2 ? (void *)&r2 : fault ? (void *)&ff : (void *)&f11};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f};
}
'''


class RadvEpilogTests(unittest.TestCase):
    def runtime(self):
        temp = tempfile.TemporaryDirectory(prefix='mk8-radv-epilog-')
        self.addCleanup(temp.cleanup)
        runtime = Path(temp.name) / 'runtime'
        (runtime / 'nvn/vulkan').mkdir(parents=True)
        (runtime / 'nvn/vulkan/device.c').write_text(DEVICE, encoding='utf-8')
        return runtime

    def test_adds_the_gated_extension_and_feature(self):
        runtime = self.runtime()
        radv_epilog.prepare(runtime)
        text = (runtime / 'nvn/vulkan/device.c').read_text(encoding='utf-8')
        self.assertIn('mk8_ed3_workaround', text)
        self.assertIn('bool mk8_ed3 = has_ext(ep, ne, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);', text)
        self.assertIn('ext[n_ext++] = VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME;', text)
        self.assertIn('VkPhysicalDeviceExtendedDynamicState3FeaturesEXT mk8_ed3_feat', text)
        self.assertIn('extendedDynamicState3ColorBlendEquation', text)
        self.assertIn('mk8_ed3_feat.pNext = f.pNext;', text)
        self.assertIn('f.pNext = &mk8_ed3_feat;', text)

    def test_is_idempotent(self):
        runtime = self.runtime()
        radv_epilog.prepare(runtime)
        once = (runtime / 'nvn/vulkan/device.c').read_text(encoding='utf-8')
        radv_epilog.prepare(runtime)
        self.assertEqual(once, (runtime / 'nvn/vulkan/device.c').read_text(encoding='utf-8'))

    def test_refuses_a_changed_anchor(self):
        runtime = self.runtime()
        path = runtime / 'nvn/vulkan/device.c'
        path.write_text(DEVICE.replace('    ext[n_ext++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;\n', ''),
                        encoding='utf-8')
        with self.assertRaises(ValueError):
            radv_epilog.prepare(runtime)


if __name__ == '__main__':
    unittest.main()
