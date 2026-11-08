"""Stalls seen in a manual session: the pipeline cache is saved during play, and SendSyncRequest frees the core."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from host import prepare_host_sources
from native import DEFAULT_COMPILER, compiler_env
import ipc_core_release
import pipeline_cache_save

HARNESS = r'''
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#define S 1000000000ull
static _Atomic uint64_t now, sleeps;
static uint64_t saves[16];
static _Atomic int nsaves;
uint64_t plat_now_ns(void) { return atomic_load(&now); }
void mk8_pipeline_built(void);
void plat_sleep_until_ns(uint64_t t) {
    // A fake clock: every poll is 5 s later. Pipelines are built at 100 s and every 5 s from 200 s to 300 s.
    if (atomic_load(&sleeps) >= 90) for (;;) Sleep(1000);
    atomic_store(&now, t);
    if (t == 100 * S || (t >= 200 * S && t <= 300 * S)) mk8_pipeline_built();
    atomic_fetch_add(&sleeps, 1);
}
void vk_pipeline_cache_save(void) { saves[atomic_fetch_add(&nsaves, 1)] = atomic_load(&now) / S; }
void rt_log(const char *fmt, ...) { (void)fmt; }
void mk8_pipeline_saver_start(void);
int main(void) {
    mk8_pipeline_built();  // (one pipeline at 0 s, before the thread starts)
    mk8_pipeline_saver_start();
    mk8_pipeline_saver_start();  // (a second start does nothing)
    while (atomic_load(&sleeps) < 90) Sleep(1);
    for (int i = 0; i < atomic_load(&nsaves); i++) printf("%llu ", (unsigned long long)saves[i]);
    printf("\n");
    return 0;
}
'''


class RuntimeStallTests(unittest.TestCase):
    def test_prepared_runtime_saves_pipelines_and_frees_the_core_in_ipc(self):
        with tempfile.TemporaryDirectory() as folder:
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', Path(folder))
            exec_c = (runtime/'nvn/vulkan/exec.c').read_text(encoding='utf-8')
            self.assertEqual(exec_c.count('mk8_pipeline_built();'), 3)
            device_c = (runtime/'nvn/vulkan/device.c').read_text(encoding='utf-8')
            self.assertIn('static void vk_pipeline_cache_save_locked(void) {', device_c)
            self.assertIn('mk8_pipeline_saver_start();', device_c)
            kernel = (runtime/'kernel.c').read_text(encoding='utf-8')
            dispatch = kernel[kernel.index('case 0x21:'):kernel.index('break;', kernel.index('case 0x21:'))]
            self.assertLess(dispatch.index('kernel_host_wait_begin();'), dispatch.index('ipc_handle_request'))
            self.assertLess(dispatch.index('ipc_handle_request'), dispatch.index('kernel_host_wait_end();'))
            self.assertIn('"inside" : "after"', kernel)

    def test_boundaries_are_checked(self):
        with tempfile.TemporaryDirectory() as folder:
            runtime = Path(folder)
            (runtime/'nvn/vulkan').mkdir(parents=True)
            for name in ('nvn/vulkan/exec.c', 'nvn/vulkan/device.c', 'kernel.c'):
                (runtime/name).write_text('int x;\n')
            with self.assertRaises(ValueError):
                pipeline_cache_save.prepare(runtime)
            with self.assertRaises(ValueError):
                ipc_core_release.prepare(runtime)

    @unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
    def test_cache_is_saved_after_new_pipelines_once_compiling_stops(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', out/'host')
            (out/'harness.c').write_text(HARNESS, encoding='utf-8')
            exe = out/'saver.exe'
            subprocess.run([DEFAULT_COMPILER, 'cc', '-target', 'x86_64-windows-gnu', '-O1', '-I' + str(runtime),
                            out/'harness.c', ROOT/'runtime/mk8/graphics/pipeline_saver.c', '-o', exe],
                           check=True, capture_output=True, env=compiler_env())
            result = subprocess.run([exe], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            # (the pipeline built before the thread started counts as saved: the thread starts with the cache)
            # 110 s: 10 s after the one built at 100 s. 310 s: 10 s after the burst of 200-300 s ended, never in the
            # middle of it. Nothing new afterwards, so no other save.
            self.assertEqual(result.stdout.split(), ['110', '310'])


if __name__ == '__main__':
    unittest.main()
