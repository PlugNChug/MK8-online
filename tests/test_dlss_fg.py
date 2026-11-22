"""DLSS toolchain gating and fsr_fg integration compile checks.

These tests do not need an NVIDIA GPU. They apply toolchain/dlss.py and toolchain/dlss_fg.py
to a prepared runtime copy that already has toolchain/fsr_fg.py: the NGX instance/device
extensions must be added only under MK8_DLSS/MK8_DLSS_FG, the low-latency/swapchain changes
must stay gated on MK8_DLSS_FG, and the patched device.c/exec.c plus the fsr_fg.cpp branch
must compile with -fsyntax-only. When no prepared runtime exists they skip.
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

from host import linux_pkg_config, prepare_host_sources  # noqa: E402
import dlss  # noqa: E402
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
        cls.temp = tempfile.TemporaryDirectory(prefix='mk8-dlssfg-')
        cls.addClassCleanup(cls.temp.cleanup)
        stage = Path(cls.temp.name)
        cls.runtime = stage / 'runtime'
        if source is None:
            if not (ROOT / 'upstream/smm2recomp/recomp/runtime').is_dir():
                raise unittest.SkipTest('fetch the pinned upstream sources first')
            prepared = stage / 'prepared'
            subprocess.run([sys.executable, ROOT / 'runtime/prepare.py',
                            '--upstream', ROOT / 'upstream/smm2recomp', '--output', prepared,
                            '--profile', ROOT / 'games/mk8d/profiles/4.0.1.json'],
                           check=True, stdout=subprocess.DEVNULL)
            prepare_host_sources(prepared, stage)
        else:
            shutil.copytree(source, cls.runtime)
        dlss.prepare(cls.runtime)
        dlss_fg.prepare(cls.runtime)
        cls.device = (cls.runtime / 'nvn/vulkan/device.c').read_text(encoding='utf-8')
        cls.exec_c = (cls.runtime / 'nvn/vulkan/exec.c').read_text(encoding='utf-8')

    def test_patch_adds_only_gated_low_latency_and_sampled_swapchain(self):
        self.assertIn('static bool mk8_dlss_fg_requested(void)', self.device)
        self.assertIn('VK_KHR_PRESENT_ID_EXTENSION_NAME', self.device)
        self.assertIn('VK_NV_LOW_LATENCY_2_EXTENSION_NAME', self.device)
        self.assertIn('const char *ext[20];', self.device)
        self.assertIn('mk8_dlss_fg_low_latency = true', self.device)
        self.assertIn('VkSwapchainLatencyCreateInfoNV', self.device)
        self.assertIn('VK_IMAGE_USAGE_SAMPLED_BIT', self.device)
        self.assertIn('mk8_present_image_index', self.device)
        self.assertIn('!mk8_dlss_fg_active()', self.device)
        self.assertIn('.waitSemaphoreInfoCount = pair ? 2', self.exec_c)
        self.assertIn('.signalSemaphoreInfoCount = pair ? 3', self.exec_c)
        self.assertLess(self.device.index('VkSwapchainLatencyCreateInfoNV dlss_fg_latency'),
                        self.device.index('if (mk8_dlss_fg_requested())', self.device.index('.oldSwapchain = old}')))
        self.assertGreaterEqual(self.device.count('if (mk8_dlss_fg_requested())'), 2)
        # The helper is false for MK8_DLSS_FG unset/0 and for an active MK8_FSR_FG.
        helper = self.device.split('static bool mk8_dlss_fg_requested(void)', 1)[1].split('}', 1)[0]
        self.assertIn('MK8_DLSS_FG', helper)
        self.assertIn('strcmp(value, "0")', helper)
        self.assertIn('MK8_FSR_FG', helper)
        # The main-draw matrix scan and depth capture must also run for DLSS-G alone.
        self.assertIn('MK8_DLSS_FG', self.exec_c)
        self.assertEqual(self.exec_c.count('MK8_DLSS_FG'), 5)

    def test_ngx_extensions_are_only_in_the_gated_blocks(self):
        device = self.device
        self.assertIn('static bool mk8_dlss_requested(void)', device)
        self.assertEqual(device.count('if (mk8_dlss_requested())'), 2)

        first = device.index('if (mk8_dlss_requested())')
        second = device.index('if (mk8_dlss_requested())', first + 1)
        instance_end = device.index('    bool validate = getenv("LARECOMP_VK_VALIDATION")', first)
        device_end = device.index('    free(ep);', second)
        instance_block = device[first:instance_end]
        device_block = device[second:device_end]

        self.assertIn('VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME', instance_block)
        for name in ('VK_NVX_BINARY_IMPORT_EXTENSION_NAME',
                     'VK_NVX_IMAGE_VIEW_HANDLE_EXTENSION_NAME',
                     'VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME',
                     'VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME'):
            self.assertIn(name, device_block)
        self.assertIn('is not advertised; DLSS will fail closed', device_block)

        outside = device[:first] + device[instance_end:second] + device[device_end:]
        for name in ('VK_NVX_BINARY_IMPORT_EXTENSION_NAME',
                     'VK_NVX_IMAGE_VIEW_HANDLE_EXTENSION_NAME',
                     'VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME',
                     'VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME',
                     'VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME'):
            self.assertNotIn(name, outside)

    def test_ngx_request_helper_truth_table(self):
        if not COMPILER:
            self.skipTest('clang is not available')
        start = self.device.index('static bool mk8_dlss_requested(void)')
        end = self.device.index('\n}\n', start) + 3
        helper = self.device[start:end]
        source = ('#define _POSIX_C_SOURCE 200809L\n'
                  '#include <stdbool.h>\n'
                  '#include <stdlib.h>\n'
                  '#include <string.h>\n'
                  + helper +
                  'int main(int argc, char **argv) {\n'
                  '    for (int i = 1; i < argc; i++) {\n'
                  '        char *eq = strchr(argv[i], \'=\');\n'
                  '        if (!eq) return 2;\n'
                  '        *eq = 0;\n'
                  '        setenv(argv[i], eq + 1, 1);\n'
                  '    }\n'
                  '    return mk8_dlss_requested() ? 0 : 1;\n'
                  '}\n')
        with tempfile.TemporaryDirectory(prefix='mk8-dlss-gate-') as temp:
            src = Path(temp) / 'gate.c'
            exe = Path(temp) / 'gate'
            src.write_text(source, encoding='utf-8')
            build = subprocess.run([COMPILER, '-std=c11', '-O0', str(src), '-o', str(exe)],
                                   capture_output=True, text=True, timeout=120)
            self.assertEqual(build.returncode, 0, build.stderr)
            env = {'PATH': os.environ.get('PATH', '/usr/bin:/bin')}
            cases = [
                ([], 1),
                (['MK8_DLSS=1'], 0),
                (['MK8_DLSS_FG=1'], 0),
                (['MK8_DLSS=0'], 1),
                (['MK8_DLSS_FG=0'], 1),
                (['MK8_FSR_FG=1'], 1),
                (['MK8_DLSS=1', 'MK8_FSR_FG=1'], 1),
            ]
            for args, expected in cases:
                result = subprocess.run([str(exe), *args], env=env, timeout=30)
                self.assertEqual(result.returncode, expected, f'{args} -> {result.returncode}')

    def test_host_registers_the_ngx_hook_before_dlss_fg(self):
        host = (ROOT / 'toolchain/host.py').read_text(encoding='utf-8')
        self.assertLess(host.index('from dlss import prepare as prepare_dlss'),
                        host.index('from dlss_fg import prepare as prepare_dlss_fg'))

    def test_patch_is_idempotent(self):
        dev_before = (self.runtime / 'nvn/vulkan/device.c').read_bytes()
        exec_before = (self.runtime / 'nvn/vulkan/exec.c').read_bytes()
        dlss.prepare(self.runtime)
        dlss_fg.prepare(self.runtime)
        self.assertEqual(dev_before, (self.runtime / 'nvn/vulkan/device.c').read_bytes())
        self.assertEqual(exec_before, (self.runtime / 'nvn/vulkan/exec.c').read_bytes())

    def test_patched_device_and_exec_compile(self):
        if not COMPILER:
            self.skipTest('clang is not available')
        vma = ROOT / 'local/native-tools/deps/linux/include/vk_mem_alloc.h'
        if not vma.is_file():
            self.skipTest('vk_mem_alloc.h is not available')
        try:
            cflags = linux_pkg_config('--cflags')
        except subprocess.CalledProcessError:
            self.skipTest('Linux development libraries are not available')
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
