"""Native frame generation: the FFX swapchain hooks, their anchors, and provider smoke tests.

The patch tests run against the pinned upstream runtime (the real device.c/exec.c anchors), not a
synthetic excerpt. The smoke test compiles tests/fixtures/fsr_fg_swapchain.cpp and exercises the
3.1.7 swapchain provider on a headless Vulkan surface: version check, swapchain context, replacement
functions, 1080p frame-generation context, configure, one prepare dispatch and one present. The
MLFI 4.0.2 device-feature gate (MK8_FSR_FG=fsr4) is checked in the patched source; the two-library
hand-off itself is exercised in-game (local/qualification/fsr4-fg-2026-11-20.md).
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
        self.assertIn('fg_prio[2] = {1.0f, 1.0f}', device)
        self.assertIn('.queueCreateInfoCount = fg_queues ? 2 : 1', device)

    def test_enables_formatless_storage_images_only_for_frame_generation(self):
        # The generated FG blobs declare storage images with format Unknown (tools/gen_fg_blobs.py),
        # so the device needs shaderStorageImageWrite/ReadWithoutFormat. The gate keeps the
        # FG-disabled device creation identical.
        runtime = self.patched()
        device = (runtime / 'nvn/vulkan/device.c').read_text(encoding='utf-8')
        gate = ('    if (getenv("MK8_FSR_FG")) {\n'
                '        b->shaderStorageImageWriteWithoutFormat = h->shaderStorageImageWriteWithoutFormat;\n'
                '        b->shaderStorageImageReadWithoutFormat = h->shaderStorageImageReadWithoutFormat;')
        self.assertIn(gate, device)

    def test_enables_compute_shader_derivatives_for_frame_generation(self):
        # The FG inpainting-pyramid and the FSR 3.1 SPD pyramid shaders declare
        # ComputeDerivativeGroupLinearKHR, so the device needs VK_KHR_compute_shader_derivatives and
        # the feature when either FSR or FG is requested; the no-FSR path is unchanged.
        runtime = self.patched()
        device = (runtime / 'nvn/vulkan/device.c').read_text(encoding='utf-8')
        self.assertIn(
            'if ((getenv("MK8_FSR_FG") || getenv("MK8_FSR4")) &&\n'
            '        has_ext(ep, ne, VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME)) {\n'
            '        fsr_derivatives = true;\n'
            '        ext[n_ext++] = VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME;',
            device)
        self.assertIn('VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR fsr_deriv_feat', device)
        self.assertIn('f.pNext = &fsr_deriv_feat;', device)

    def test_enables_the_depth_capture_and_camera_scan_for_frame_generation(self):
        runtime = self.patched()
        exec_c = (runtime / 'nvn/vulkan/exec.c').read_text(encoding='utf-8')
        self.assertIn('if ((getenv("MK8_FSR4_CAPTURE") || getenv("MK8_FSR_FG")) && g_pass_depth', exec_c)
        self.assertIn('if (getenv("MK8_FSR4") || getenv("MK8_DLSS") || getenv("MK8_FSR_FG")) {', exec_c)
        # The FG bridge prepares at the swapchain extent, while the game's depth pass is at its own
        # render size; the capture is resampled to the presented size only for FG.
        self.assertIn('if (getenv("MK8_FSR_FG") && g_vk.sc_extent.width && g_vk.sc_extent.height &&\n'
                      '                db->width * g_vk.sc_extent.height == db->height * g_vk.sc_extent.width)', exec_c)
        self.assertIn('cw = g_vk.sc_extent.width, ch = g_vk.sc_extent.height;', exec_c)
        self.assertIn('post_image(21, cw, ch, VK_FORMAT_R32_SFLOAT)', exec_c)
        self.assertIn('float pc[8] = {(float)cw, (float)ch', exec_c)
        shaders = (runtime / 'nvn/vulkan/shaders.c').read_text(encoding='utf-8')
        self.assertIn('gl_FragCoord.xy / pc.a.xy', shaders)
        self.assertIn('resampled to the presented size', exec_c)
        # The motion-vector compute's storage-image descriptor needs a matching pool size.
        self.assertIn('getenv("MK8_DLSS") || getenv("MK8_DLSS_FG");', exec_c)
        self.assertIn('sizes[5] = (VkDescriptorPoolSize){VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1024};', exec_c)

    def test_enables_the_mlfi_device_features_only_for_fsr4(self):
        # The MLFI 4.0.2 provider's captured modules need cooperative-matrix maintenance1,
        # 64-bit image atomics, the Vulkan memory model, subgroup-size control and variable
        # pointers. Cooperative matrix/float8 are already requested by toolchain/fsr.py; this
        # patch adds only the missing pieces and gates them on MK8_FSR_FG=fsr4.
        runtime = self.patched()
        device = (runtime / 'nvn/vulkan/device.c').read_text(encoding='utf-8')
        self.assertIn('bool mlfi = getenv("MK8_FSR_FG") && !strcmp(getenv("MK8_FSR_FG"), "fsr4");', device)
        self.assertIn('ext[n_ext++] = VK_EXT_COOPERATIVE_MATRIX_MAINTENANCE_1_EXTENSION_NAME;', device)
        self.assertIn('ext[n_ext++] = VK_EXT_SHADER_IMAGE_ATOMIC_INT64_EXTENSION_NAME;', device)
        self.assertIn('mlfi_img64.pNext = f13.pNext;\n        f13.pNext = &mlfi_img64;\n'
                      '        mlfi_cm2.pNext = f13.pNext;\n        f13.pNext = &mlfi_cm2;', device)
        self.assertIn('f13.subgroupSizeControl = have13.subgroupSizeControl;', device)
        self.assertIn('f12.vulkanMemoryModel = have12.vulkanMemoryModel;', device)
        self.assertIn('f11.variablePointers = mlfi_varptr.variablePointers;', device)
        self.assertIn('// room for the swapchain/WSI extensions plus the fsr4 MLFI set\n'
                      '    const char *ext[16];', device)
        self.assertIn('if (!have_cm.cooperativeMatrix || !mlfi_cm2.cooperativeMatrixConversions ||', device)

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
            self.assertIn('presented 6 frames', output)
            self.assertIn('frame 5:', output)
            self.assertIn('RESULT: OK', output)
            # The fixture enables the validation layer whenever it is installed, and fails the run
            # itself on any validation error. Assert the clean result when it ran.
            if 'validation: VK_LAYER_KHRONOS_validation enabled' in output:
                self.assertIn('validation errors: 0', output)
                self.assertIn('features: formatless storage image write=1 read=1', output)


if __name__ == '__main__':
    unittest.main()
