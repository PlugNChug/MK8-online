"""The shader budget's optional course-entry burst grace (toolchain/shader_budget.py).

The prepared exec.c is checked for the injected budget; the budget function is taken from it and run in a native
harness with stubbed in-flight/frame counters: the default stays 4 ms, and an explicit MK8_SHADER_BURST_WAIT_MS covers
a >64-build burst plus its 120-frame grace."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from host import prepare_host_sources
import shader_budget

HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
static int g_inflight;
static uint64_t g_frame;
int build_inflight(void) { return g_inflight; }
uint64_t vk_frame_counter(void) { return g_frame; }
/*BUDGET*/
int main(int argc, char **argv) {
    g_inflight = atoi(argv[1]);
    g_frame = (uint64_t)atoll(argv[2]);
    double a = mk8_shader_budget_ms();
    g_inflight = atoi(argv[3]);
    g_frame = (uint64_t)atoll(argv[4]);
    printf("%.0f %.0f\n", a, mk8_shader_budget_ms());
    return 0;
}
'''


class ShaderBudgetBurstTests(unittest.TestCase):
    def test_boundaries_are_checked(self):
        with tempfile.TemporaryDirectory() as folder:
            vulkan = Path(folder)/'nvn/vulkan'
            vulkan.mkdir(parents=True)
            (vulkan.parent/'buildsync.c').write_text('int x;\n', encoding='utf-8')
            for name in ('exec.c', 'device.c'):
                (vulkan/name).write_text('int x;\n', encoding='utf-8')
            with self.assertRaises(ValueError):
                shader_budget.prepare(Path(folder))

    @unittest.skipUnless(shutil.which('cc'), 'Native C compiler required')
    def test_burst_grace_is_opt_in_and_bounded(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', out/'host')
            exec_c = (runtime/'nvn/vulkan/exec.c').read_text(encoding='utf-8')
            self.assertEqual(exec_c.count('mk8_job_ready(g_tq, &p->tjob)'), 1)
            self.assertEqual(exec_c.count('mk8_job_ready(g_wq, &pp->job)'), 1)
            start = exec_c.index('static double mk8_shader_budget_ms(void) {')
            end = exec_c.index('\n}\n', start) + 3
            block = exec_c[start:end]
            self.assertIn('MK8_SHADER_BURST_WAIT_MS', block)
            self.assertIn(': 0;', block)
            shader_budget.prepare(runtime)
            self.assertEqual((runtime/'nvn/vulkan/exec.c').read_text(encoding='utf-8').count('mk8_job_ready'), 4)
            harness = out/'harness.c'
            harness.write_text(HARNESS.replace('/*BUDGET*/', block), encoding='utf-8')
            exe = out/'harness'
            subprocess.run(['cc', '-std=gnu11', '-O1', str(harness), '-o', str(exe)], check=True, capture_output=True)

            def budget(i1, f1, i2, f2, **env):
                e = {k: v for k, v in os.environ.items()}
                for key in ('MK8_SHADER_WAIT_MS', 'MK8_SHADER_BURST_WAIT_MS', 'MK8_SHADER_BURST_FRAMES'):
                    e.pop(key, None)
                e.update(env)
                r = subprocess.run([str(exe), str(i1), str(f1), str(i2), str(f2)], capture_output=True, text=True, env=e)
                self.assertEqual(r.returncode, 0, r.stderr)
                return tuple(float(x) for x in r.stdout.split())

            self.assertEqual(budget(0, 0, 0, 0), (4, 4))
            self.assertEqual(budget(100, 0, 100, 0), (4, 4))  # (default: the budget's original behaviour)
            self.assertEqual(budget(100, 0, 0, 121, MK8_SHADER_BURST_WAIT_MS='-1'), (-1, 4))  # (grace ends at 120)
            self.assertEqual(budget(100, 0, 0, 100, MK8_SHADER_BURST_WAIT_MS='-1'), (-1, -1))  # (inside the grace)
            self.assertEqual(budget(100, 0, 0, 0, MK8_SHADER_BURST_WAIT_MS='250'), (250, 250))
            self.assertEqual(budget(100, 0, 0, 0, MK8_SHADER_BURST_WAIT_MS='0'), (4, 4))


if __name__ == '__main__':
    unittest.main()
