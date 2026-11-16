"""Vertex cache entries no draw used for a minute are dropped; recent ones and the pending submission's stay.

The prepared exec.c's own cache table, vc_drop, vc_free_dead and the sweep run in a native harness; only Vulkan/VMA
and the submission and frame clocks are stand-ins."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from host import prepare_host_sources
from native import DEFAULT_COMPILER, compiler_env
import vertex_cache_age

HARNESS = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define LOG(...) printf(__VA_ARGS__)
typedef struct Buf *VkBuffer;
typedef struct Alloc *VmaAllocation;
static struct { int vma; } g_vk;
static uint64_t pending = 1, completed = 0, frame = 0, destroyed = 0;
static uint64_t vk_pending_seq(void) { return pending; }
static uint64_t vk_completed_seq(void) { return completed; }
static void vk_wait_seq(uint64_t s) { completed = s; }
static void vmaDestroyBuffer(int vma, VkBuffer b, VmaAllocation m) { (void)vma; (void)b; (void)m; destroyed++; }
uint64_t vk_frame_counter(void) { return frame; }
/*CACHE*/
/*SWEEP*/
static VcEnt *add(int i, uint64_t mb, uint64_t used) {
    VcEnt *e = &vc_tab[i];
    e->buf = (VkBuffer)(uintptr_t)(0x1000 + i), e->mem = (VmaAllocation)(uintptr_t)(0x2000 + i);
    e->lo = (uint64_t)i << 32, e->hi = e->lo + (mb << 20), e->used_seq = used;
    vc_bytes += mb << 20;
    return e;
}
// one presented frame: 8 submissions, all completed later; entries in `use` drawn from in it
static void run_frames(int n, int *use, int nuse) {
    for (int f = 0; f < n; f++) {
        for (int k = 0; k < nuse; k++) vc_tab[use[k]].used_seq = pending;
        mk8_vc_sweep();
        pending += 8, completed = pending - 8, frame++;
    }
}
int main(void) {
    add(1, 10, 1);   // a range of the last track: never drawn from again
    add(2, 20, 1);   // drawn from all along
    add(3, 5, 1);    // drawn from for 30 s, then not
    int both[] = {2, 3}, one[] = {2};
    run_frames(1800, both, 2);   // 30 s
    if (!vc_tab[1].buf || !vc_tab[3].buf) return 1;  // (nothing idle for a minute yet)
    // (a sweep every 10 s against the mark of a minute earlier: an entry goes 60-70 s after its last use)
    run_frames(2500, one, 1);    // ~42 s more: entry 1 idle for ~72 s, entry 3 for ~42 s
    if (vc_tab[1].buf || !vc_tab[2].buf || !vc_tab[3].buf) { printf("70 s: %d %d %d\n", !!vc_tab[1].buf, !!vc_tab[2].buf, !!vc_tab[3].buf); return 2; }
    run_frames(2400, one, 1);    // 40 s more: entry 3 idle for 80 s
    if (vc_tab[3].buf || !vc_tab[2].buf) return 3;
    if (vc_bytes != (20ull << 20)) return 4;
    run_frames(600, one, 1);     // (their buffers go once the GPU is past their last use)
    if (destroyed != 2) { printf("destroyed %llu\n", (unsigned long long)destroyed); return 5; }
    // an entry the pending submission draws from is never dropped, whatever its age
    add(5, 1, 1);
    vc_tab[5].used_seq = pending;
    run_frames(1, NULL, 0);
    if (!vc_tab[5].buf) return 6;
    return 0;
}
'''


class VertexCacheAgeTests(unittest.TestCase):
    def test_boundaries_are_checked(self):
        with tempfile.TemporaryDirectory() as folder:
            execute = Path(folder)/'nvn/vulkan/exec.c'
            execute.parent.mkdir(parents=True)
            execute.write_text('void vc_flush(void) {\n}\n', encoding='utf-8')
            with self.assertRaises(ValueError):
                vertex_cache_age.prepare(Path(folder))

    @unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
    def test_idle_entries_go_and_used_ones_stay(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', out/'host')
            text = (runtime/'nvn/vulkan/exec.c').read_text(encoding='utf-8')
            self.assertIn('void vc_flush(void) {\n    mk8_vc_sweep();\n', text)
            cache = text[text.index('#define VC_PAGE'):text.index('// The page tables (vc_page_gen')]
            sweep = text[text.index('// MK8 (toolchain/vertex_cache_age.py)'):text.index('void vc_flush(void) {')]
            harness = out/'harness.c'
            harness.write_text(HARNESS.replace('/*CACHE*/', cache).replace('/*SWEEP*/', sweep), encoding='utf-8')
            exe = out/'harness.exe'
            subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu', '-O1', '-Wno-unused-function',
                            str(harness), '-o', str(exe)], env=compiler_env(), check=True, capture_output=True)
            env = {k: v for k, v in os.environ.items() if not k.startswith('MK8_')}
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60, env=env)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn('[mk8:vcache] 1 idle ranges dropped (10.0 MB)', result.stdout)
            # off: nothing dropped, the first check fails at 70 s
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60,
                                    env={**env, 'MK8_VCACHE_IDLE_S': '0'})
            self.assertEqual(result.returncode, 2, result.stdout)


if __name__ == '__main__':
    unittest.main()
