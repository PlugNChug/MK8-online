"""Native frame generation: the FFX swapchain hooks, their anchors, and a provider smoke test.

The patch tests run against the pinned upstream runtime (the real device.c/exec.c anchors), not a
synthetic excerpt. The smoke test compiles tests/fixtures/fsr_fg_swapchain.cpp and exercises the
provider on a headless Vulkan surface: 3.1.7 check, swapchain context, replacement functions,
1080p frame-generation context, configure, one prepare dispatch and one present.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
UPSTREAM = ROOT / 'upstream/smm2recomp/recomp/runtime/nvn/vulkan'
sys.path.insert(0, str(ROOT / 'toolchain'))

import fsr
import fsr_fg
import submit_profile
import acquire_bound


class FsrFgPatchTests(unittest.TestCase):
    def runtime(self):
        temp = tempfile.TemporaryDirectory(prefix='mk8-fsr-fg-')
        self.addCleanup(temp.cleanup)
        runtime = Path(temp.name) / 'runtime'
        shutil.copytree(UPSTREAM, runtime / 'nvn/vulkan')
        return runtime

    def patched(self):
        runtime = self.runtime()
        # The real order in toolchain/host.py: the upscaler, the WSI instrumentation, then this
        # bridge last (it wraps the acquire/present calls those patches rewrite).
        fsr.prepare(runtime)
        submit_profile.prepare(runtime)
        acquire_bound.prepare(runtime)
        fsr_fg.prepare(runtime)
        return runtime

    def test_routes_the_swapchain_and_present_through_the_bridge(self):
        runtime = self.patched()
        device = (runtime / 'nvn/vulkan/device.c').read_text(encoding='utf-8')
        self.assertIn('mk8_fsr_fg_create_swapchain(&ci, &g_vk.swapchain)', device)
        self.assertEqual(device.count('mk8_fsr_fg_acquire(g_vk.dev, g_vk.swapchain, mk8_acquire_timeout_ns()'), 2)
        self.assertEqual(device.count('        VkResult r = mk8_fsr_fg_present(g_vk.queue, &pi);'), 2)
        self.assertEqual(device.count('if (!mk8_fsr_fg_active()) vk_queue_lock();'), 2)
        self.assertEqual(device.count('if (!mk8_fsr_fg_active()) vk_queue_unlock();'), 2)
        self.assertIn('mk8_fsr_fg_prepare(cb, g_vk.sc_extent.width, g_vk.sc_extent.height)', device)
        self.assertIn('mk8_fsr_fg_destroy_swapchain(g_vk.dev, g_vk.swapchain, NULL)', device)
        self.assertIn('VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR', device)
        self.assertIn('if (old && !fg_sc && !fg_was) vkDestroySwapchainKHR', device)
        self.assertIn('void mk8_fsr_fg_queue_lock(void) { vk_queue_lock(); }', device)

    def test_adds_the_extra_present_queue_family(self):
        runtime = self.patched()
        device = (runtime / 'nvn/vulkan/device.c').read_text(encoding='utf-8')
        self.assertIn('uint32_t g_vk_fg_present_family = UINT32_MAX;', device)
        self.assertIn('g_vk_fg_present_family = i;', device)
        self.assertIn('.queueCount = 2,', device)
        self.assertIn('.queueCreateInfoCount = fg_queues ? 2 : 1', device)

    def test_enables_the_depth_capture_and_camera_scan_for_frame_generation(self):
        runtime = self.patched()
        exec_c = (runtime / 'nvn/vulkan/exec.c').read_text(encoding='utf-8')
        self.assertIn('if ((getenv("MK8_FSR4_CAPTURE") || getenv("MK8_FSR_FG")) && g_pass_depth', exec_c)
        self.assertIn('if (getenv("MK8_FSR4") || getenv("MK8_FSR_FG")) {', exec_c)

    def test_is_idempotent(self):
        runtime = self.patched()
        device = (runtime / 'nvn/vulkan/device.c').read_text(encoding='utf-8')
        exec_c = (runtime / 'nvn/vulkan/exec.c').read_text(encoding='utf-8')
        fsr_fg.prepare(runtime)
        self.assertEqual(device, (runtime / 'nvn/vulkan/device.c').read_text(encoding='utf-8'))
        self.assertEqual(exec_c, (runtime / 'nvn/vulkan/exec.c').read_text(encoding='utf-8'))

    def test_rejects_a_changed_present_call(self):
        runtime = self.runtime()
        fsr.prepare(runtime)
        path = runtime / 'nvn/vulkan/device.c'
        text = path.read_text(encoding='utf-8').replace(
            '        VkResult r = vkQueuePresentKHR(g_vk.queue, &pi);',
            '        VkResult r = vkQueuePresentKHR(g_vk.queue, NULL);', 1)
        path.write_text(text, encoding='utf-8')
        with self.assertRaises(ValueError):
            fsr_fg.prepare(runtime)


def default_provider():
    if os.environ.get('MK8_FSR_FG_LIB'):
        return Path(os.environ['MK8_FSR_FG_LIB'])
    base = Path(os.environ.get('XDG_DATA_HOME') or Path.home() / '.local/share')
    return base / 'mk8online/fsr/libamd_fidelityfx_framegeneration_vk.so'


@unittest.skipUnless(shutil.which('g++'), 'no C++ compiler for the native smoke test')
class FsrFgNativeTests(unittest.TestCase):
    def test_provider_creates_a_1080p_swapchain_context_and_presents(self):
        provider = default_provider()
        if not provider.is_file():
            message = f'provider not installed at {provider}'
            if os.environ.get('MK8_FSR_FG_REQUIRE_NATIVE'):
                self.fail(message)
            self.skipTest(message)
        with tempfile.TemporaryDirectory(prefix='mk8-fsr-fg-native-') as temp:
            binary = Path(temp) / 'fsr_fg_swapchain'
            compile_result = subprocess.run(
                ['g++', '-std=c++17', '-O1', '-I', str(ROOT / 'runtime/mk8/graphics/ffx'),
                 str(ROOT / 'tests/fixtures/fsr_fg_swapchain.cpp'), '-o', str(binary), '-lvulkan', '-ldl'],
                capture_output=True, text=True)
            if compile_result.returncode != 0:
                self.fail(f'compilation failed:\n{compile_result.stderr}')
            run = subprocess.run([str(binary), str(provider)], capture_output=True, text=True, timeout=120)
            output = run.stdout + run.stderr
            self.assertEqual(run.returncode, 0, output)
            self.assertIn('swapchain provider offers 3.1.7', output)
            self.assertIn('swapchain context: rc=0 (OK)', output)
            self.assertIn('frame-generation context: rc=0 (OK)', output)
            self.assertIn('RESULT: OK', output)


if __name__ == '__main__':
    unittest.main()
