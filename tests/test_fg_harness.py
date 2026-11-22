"""End-to-end test for the native frame-generation Vulkan harness.

The harness (tools/fg-harness/fg_harness.cpp) is a standalone headless Vulkan program: it creates a
real swapchain, replaces it with the FFX one, records a prepare dispatch for every rendered frame,
presents through the provider and reads a generated frame back. It is the only test that exercises
the FFX swapchain present/interpolation path without the game.

The test skips when the provider or a C++ compiler is absent. The provider used is, in order:
MK8_FG_HARNESS_LIB, MK8_FSR_FG_LIB, local/fg-provider/, then the runtime's XDG install path.

Set MK8_FG_HARNESS_REQUIRE_PROVIDER=1 to turn a provider-side failure into a test failure instead of
a skip. The provider built at the time of writing (20/11/2026) cannot run the interpolation path:
its generated blob metadata omits constant-buffer/UAV-buffer bindings, so the FFX pipeline creation
raises validation errors and `r_counters` stays unbound, which makes every interpolated frame a copy
of the current frame. Those failures are reported by the harness and skipped by default here; the
provider needs the fix recorded in docs/FG-HARNESS.md before the summary can be asserted.
"""
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HARNESS = ROOT / 'tools/fg-harness/fg_harness.cpp'

# Signatures of the known provider defect (not a harness or environment problem).
PROVIDER_DEFECT_SIGNATURES = (
    'rw_counters',
    'cbFI',
    'descriptor [Set 0, Binding 224',
    'VkResult -4',
    'GPUVM',
    'spirv-val produced an error',
)


def provider_path():
    for variable in ('MK8_FG_HARNESS_LIB', 'MK8_FSR_FG_LIB'):
        value = os.environ.get(variable)
        if value:
            return Path(value)
    base = Path(os.environ.get('XDG_DATA_HOME') or Path.home() / '.local/share')
    installed = base / 'mk8online/fsr/libamd_fidelityfx_framegeneration_vk.so'
    if installed.is_file():
        return installed
    return ROOT / 'local/fg-provider/libamd_fidelityfx_framegeneration_vk.so'


def cache_dir():
    base = Path(os.environ.get('XDG_CACHE_HOME') or Path.home() / '.cache')
    directory = base / 'mk8online/fg-harness-test'
    directory.mkdir(parents=True, exist_ok=True)
    return directory


def first_line(text, needle):
    for line in text.splitlines():
        if needle in line:
            return line
    return text.splitlines()[-1] if text.splitlines() else '(no output)'


@unittest.skipUnless(shutil.which('g++'), 'no C++ compiler for the frame-generation harness')
class FrameGenerationHarnessTests(unittest.TestCase):
    def build(self):
        directory = Path(tempfile.mkdtemp(prefix='build-', dir=str(cache_dir())))
        self.addCleanup(shutil.rmtree, directory, ignore_errors=True)
        binary = directory / 'fg-harness'
        result = subprocess.run(
            ['g++', '-std=c++17', '-O2', '-I', str(ROOT / 'runtime/mk8/graphics/ffx'),
             str(HARNESS), '-o', str(binary), '-lvulkan', '-ldl', '-pthread'],
            capture_output=True, text=True)
        if result.returncode != 0:
            self.fail(f'harness compilation failed:\n{result.stderr}')
        return binary

    def run_harness(self, binary, provider, frames='120'):
        directory = cache_dir()
        return subprocess.run(
            [str(binary), '--frames', str(frames), '--provider', str(provider)],
            capture_output=True, text=True, timeout=150, cwd=str(directory))

    def test_runs_the_frame_generation_swapchain_end_to_end(self):
        provider = provider_path()
        if not provider.is_file():
            self.skipTest(f'frame-generation provider not installed at {provider}')
        binary = self.build()
        result = self.run_harness(binary, provider)
        output = result.stdout + result.stderr
        if result.returncode != 0:
            signature = next((value for value in PROVIDER_DEFECT_SIGNATURES if value in output), None)
            if signature and not os.environ.get('MK8_FG_HARNESS_REQUIRE_PROVIDER'):
                self.skipTest(
                    'provider cannot run the FFX interpolation path (known provider defect: '
                    f'{first_line(output, signature)}); see docs/FG-HARNESS.md')
            self.fail(f'harness exited {result.returncode}:\n{output}')

        summary = re.search(
            r'FG-HARNESS ok rendered=(\d+) presented=(\d+) last_present_count=(\d+) '
            r'generated=(\d+) real=(\d+) readback=ok validation=(\w+)',
            output)
        self.assertIsNotNone(summary, f'missing FG-HARNESS summary:\n{output}')
        rendered = int(summary.group(1))
        presented = int(summary.group(2))
        last_present_count = int(summary.group(3))
        generated = int(summary.group(4))
        real = int(summary.group(5))
        validation = summary.group(6)

        self.assertEqual(rendered, int(120))
        self.assertGreaterEqual(presented, rendered,
                                f'present callback ran {presented} times for {rendered} frames:\n{output}')
        self.assertGreaterEqual(last_present_count, rendered,
                                f'provider presents {last_present_count} < {rendered}:\n{output}')
        self.assertGreaterEqual(generated, rendered - 2,
                                f'only {generated} generated frames for {rendered} rendered:\n{output}')
        self.assertGreaterEqual(real, rendered,
                                f'only {real} real frames for {rendered} rendered:\n{output}')
        self.assertIn(validation, ('clean', 'off'), f'validation status {validation}:\n{output}')
        if validation == 'clean':
            self.assertIn('warnings=', output)


if __name__ == '__main__':
    unittest.main()
