"""Fila real e jobs reais; tradução/driver substituídos por trabalhos determinísticos, sem GPU."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from host import DEPS, prepare_host_sources
from native import DEFAULT_COMPILER, compiler_env


@unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
class ComputePrepareTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.folder = tempfile.TemporaryDirectory()
        cls.out = Path(cls.folder.name)
        runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', cls.out/'prepared')
        objects = (runtime/'nvn/objects.c').read_text(encoding='utf-8')
        start = objects.index('// MK8 (toolchain/async_submit.py): submissions, presents')
        queue = objects[start:objects.index('void renderer_wait_seq(uint64_t seq);', start)]
        (cls.out/'queue.inc').write_text(queue, encoding='utf-8')
        execute = (runtime/'nvn/vulkan/exec.c').read_text(encoding='utf-8')
        start = execute.index('static struct CsEnt')
        end = execute.index('\n    return cp;\n}', start) + len('\n    return cp;\n}')
        body = execute[start:end] + '\n' + (ROOT/'runtime/mk8/graphics/compute_prepare.inc').read_text(encoding='utf-8')
        (cls.out/'compute.inc').write_text(body, encoding='utf-8')
        cls.exe = cls.out/'compute.exe'
        cls.env = compiler_env()
        for key in ('MK8_COMPUTE_PREPARE', 'MK8_ASYNC_SUBMIT', 'LARECOMP_ASYNC_SHADERS', 'LARECOMP_NO_SHADER_PREFETCH'):
            cls.env.pop(key, None)
        result = subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu', '-O1', '-UNDEBUG',
                                 '-I'+str(runtime), '-I'+str(runtime/'nvn'), '-I'+str(DEPS/'include'), '-I'+str(cls.out),
                                 str(ROOT/'tests/native/compute_prepare.c'), '-o', str(cls.exe)],
                                env=cls.env, capture_output=True, text=True, timeout=120)
        if result.returncode:
            cls.folder.cleanup()
            raise AssertionError(result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.folder.cleanup()

    def run_case(self, case, **env):
        result = subprocess.run([str(self.exe), case], env=dict(self.env, **env),
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_default_off_and_modes_preserve_demand_path(self):
        for env in ({}, {'MK8_COMPUTE_PREPARE': 'garbage'}, {'MK8_COMPUTE_PREPARE': '0'},
                    {'MK8_COMPUTE_PREPARE': '1', 'MK8_ASYNC_SUBMIT': '0'},
                    {'MK8_COMPUTE_PREPARE': '1', 'LARECOMP_NO_SHADER_PREFETCH': '1'},
                    {'MK8_COMPUTE_PREPARE': '1', 'LARECOMP_ASYNC_SHADERS': '1'}):
            with self.subTest(env=env):
                self.run_case('off', **env)

    def test_queue_snapshot_order_duplicates_and_capacity(self):
        self.run_case('queue', MK8_COMPUTE_PREPARE='1')

    def test_first_use_validates_code_dimensions_and_bound(self):
        self.run_case('invalidate', MK8_COMPUTE_PREPARE='1')

    def test_ready_and_failed_jobs_remain_distinct(self):
        self.run_case('states', MK8_COMPUTE_PREPARE='1')

    def test_reboot_drains_and_frees_only_unused_preparations(self):
        self.run_case('reset', MK8_COMPUTE_PREPARE='1')


if __name__ == '__main__':
    unittest.main()
