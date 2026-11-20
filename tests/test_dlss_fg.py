"""DLSS-G toolchain gating and fsr_fg integration compile checks.

These tests do not need an NVIDIA GPU. They apply toolchain/dlss_fg.py to a prepared runtime
copy that already has toolchain/fsr_fg.py, check that the low-latency/swapchain changes are
present and idempotent, and compile the patched device.c/exec.c plus the new fsr_fg.cpp branch
with -fsyntax-only. When no prepared runtime exists in this checkout they skip.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GRAPHICS = ROOT / 'runtime/mk8/graphics'
FSR_FG_CPP = GRAPHICS / 'fsr_fg.cpp'
COMPILER = shutil.which('clang')
COMPILERXX = shutil.which('clang++')
sys.path.insert(0, str(ROOT / 'toolchain'))

import dlss_fg  # noqa: E402


def prepared_runtime() -> Path | None:
    candidates = [ROOT / 'local/dlss-fg/host/runtime']
    builds = ROOT / 'local/qualification/builds'
    if builds.is_dir():
        candidates += sorted(builds.glob('*/host-*/runtime'))
    for candidate in candidates:
        device = candidate / 'nvn/vulkan/device.c'
        if not device.is_file():
            continue
        try:
            if 'mk8_fsr_fg_create_swapchain' in device.read_text(encoding='utf-8'):
                return candidate
        except OSError:
            continue
    return None


class DlssFgPatchTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if sys.platform != 'linux':
            raise unittest.SkipTest('the DLSS-G patch test runs on Linux')
        source = prepared_runtime()
        if source is None:
            raise unittest.SkipTest('no prepared runtime with toolchain/fsr_fg.py in this checkout')
        cls.temp = tempfile.TemporaryDirectory(prefix='mk8-dlssfg-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.runtime = Path(cls.temp.name) / 'runtime'
        shutil.copytree(source, cls.runtime)
        dlss_fg.prepare(cls.runtime)
        cls.device = (cls.runtime / 'nvn/vulkan/device.c').read_text(encoding='utf-8')
        cls.exec_c = (cls.runtime / 'nvn/vulkan/exec.c').read_text(encoding='utf-8')

    def test_patch_adds_only_gated_low_latency_and_sampled_swapchain(self):
        self.assertIn('static bool mk8_dlss_fg_requested(void)', self.device)
        self.assertIn('VK_NV_LOW_LATENCY_2_EXTENSION_NAME', self.device)
        self.assertIn('mk8_dlss_fg_low_latency = true', self.device)
        self.assertIn('VkSwapchainLatencyCreateInfoNV', self.device)
        self.assertIn('VK_IMAGE_USAGE_SAMPLED_BIT', self.device)
        self.assertIn('mk8_present_image_index', self.device)
        self.assertIn('!mk8_dlss_fg_active()', self.device)
        self.assertGreaterEqual(self.device.count('if (mk8_dlss_fg_requested())'), 2)
        # The helper is false for MK8_DLSS_FG unset/0 and for an active MK8_FSR_FG.
        helper = self.device.split('static bool mk8_dlss_fg_requested(void)', 1)[1].split('}', 1)[0]
        self.assertIn('MK8_DLSS_FG', helper)
        self.assertIn('strcmp(value, "0")', helper)
        self.assertIn('MK8_FSR_FG', helper)
        # The main-draw matrix scan and depth capture must also run for DLSS-G alone.
        self.assertIn('MK8_DLSS_FG', self.exec_c)
        self.assertEqual(self.exec_c.count('MK8_DLSS_FG'), 4)

    def test_patch_is_idempotent(self):
        dev_before = (self.runtime / 'nvn/vulkan/device.c').read_bytes()
        exec_before = (self.runtime / 'nvn/vulkan/exec.c').read_bytes()
        dlss_fg.prepare(self.runtime)
        self.assertEqual(dev_before, (self.runtime / 'nvn/vulkan/device.c').read_bytes())
        self.assertEqual(exec_before, (self.runtime / 'nvn/vulkan/exec.c').read_bytes())

    def test_patched_device_and_exec_compile(self):
        if not COMPILER:
            self.skipTest('clang is not available')
        vma = ROOT / 'local/native-tools/deps/linux/include/vk_mem_alloc.h'
        if not vma.is_file():
            self.skipTest('vk_mem_alloc.h is not available')
        pkg = subprocess.run(['pkg-config', '--cflags', 'sdl3'], capture_output=True, text=True)
        if pkg.returncode:
            self.skipTest('SDL3 pkg-config is not available')
        cflags = pkg.stdout.split()
        for name in ('device.c', 'exec.c'):
            result = subprocess.run(
                [COMPILER, '-std=gnu11', '-fsyntax-only', '-D_GNU_SOURCE', '-I', str(self.runtime),
                 '-I', str(self.runtime / 'nvn'), '-I', str(vma.parent),
                 *cflags, str(self.runtime / 'nvn/vulkan' / name)],
                capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, f'{name}:\n' + result.stderr[-6000:])

    def test_fsr_fg_integration_compiles(self):
        if not COMPILERXX:
            self.skipTest('clang++ is not available')
        vma = ROOT / 'local/native-tools/deps/linux/include/vk_mem_alloc.h'
        if not vma.is_file():
            self.skipTest('vk_mem_alloc.h is not available')
        pkg = subprocess.run(['pkg-config', '--cflags', 'sdl3'], capture_output=True, text=True)
        if pkg.returncode:
            self.skipTest('SDL3 pkg-config is not available')
        result = subprocess.run(
            [COMPILERXX, '-std=c++17', '-fsyntax-only', '-I', str(GRAPHICS), '-I', str(self.runtime),
             '-I', str(self.runtime / 'nvn'), '-I', str(vma.parent), *pkg.stdout.split(),
             str(FSR_FG_CPP)],
            capture_output=True, text=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stderr[-8000:])


if __name__ == '__main__':
    unittest.main()
