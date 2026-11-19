"""The pipeline catalog records each successful pipeline once and replays it without touching guest code.

The prepared exec.c/device.c are checked for the injected record/prewarm hooks; the record function is taken from the
prepared runtime and run in a native harness over synthetic keys (dedup, the off switch, the append-only file)."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from host import prepare_host_sources
import pipeline_warm

HARNESS = r'''
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
typedef struct {
    uint8_t attr_kind[32];
    uint8_t rt_kind[8];
    bool depth_minus_one, flip_y, flip_x;
    uint32_t vs_out_mask;
    uint8_t glsl, vs_fn;
    uint32_t gs_out_mask;
} ShaderKey;
typedef struct { uint64_t code[3], cmax[3]; ShaderKey vkey, fkey; } Prog;
typedef struct { Prog *prog; int marker; } PipeKey;
static uint64_t hash_words(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001B3ull; }
    return h;
}
/*RECORD*/
static long records(void) {
    FILE *f = fopen("cache/pipeline_recipes2.bin", "rb");
    if (!f) return 0;
    long n = 0;
    PwHeader h;
    while (fread(&h, sizeof h, 1, f) == 1) {
        n++;
        if (fseek(f, (long)(h.len[0] + h.len[1] + h.len[2]), SEEK_CUR)) break;
    }
    fclose(f);
    return n;
}
int main(void) {
    uint8_t code0[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t code1[8] = {9, 10, 11, 12, 13, 14, 15, 16};
    Prog p;
    memset(&p, 0, sizeof p);
    p.code[0] = (uint64_t)(uintptr_t)code0;
    p.code[1] = (uint64_t)(uintptr_t)code1;
    p.cmax[0] = p.cmax[1] = 8;
    PipeKey k;
    memset(&k, 0, sizeof k);
    k.prog = &p;
    k.marker = 7;
    setenv("MK8_PIPELINE_WARM", "1", 1);
    mk8_pipe_record(&k);
    mk8_pipe_record(&k);              // (the same key twice: one record)
    code0[0] = 99;                    // (another program: a second record)
    mk8_pipe_record(&k);
    if (pw_fp) fflush(pw_fp);
    if (records() != 2) return 1;
    setenv("MK8_PIPELINE_WARM", "0", 1);
    pw_off = -1;
    code0[0] = 42;
    mk8_pipe_record(&k);              // (off: nothing appended)
    if (pw_fp) fflush(pw_fp);
    if (records() != 2) return 2;
    return 0;
}
'''


class PipelineWarmTests(unittest.TestCase):
    def test_boundaries_are_checked(self):
        with tempfile.TemporaryDirectory() as folder:
            vulkan = Path(folder)/'nvn/vulkan'
            vulkan.mkdir(parents=True)
            for name in ('exec.c', 'device.c'):
                (vulkan/name).write_text('int x;\n', encoding='utf-8')
            with self.assertRaises(ValueError):
                pipeline_warm.prepare(Path(folder))

    @unittest.skipUnless(shutil.which('cc'), 'Native C compiler required')
    def test_prepared_runtime_records_once_and_replays(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', out/'host')
            exec_c = (runtime/'nvn/vulkan/exec.c').read_text(encoding='utf-8')
            device_c = (runtime/'nvn/vulkan/device.c').read_text(encoding='utf-8')
            self.assertEqual(exec_c.count('mk8_pipe_record(&pp->k)'), 1)
            self.assertEqual(exec_c.count('mk8_pipe_record(const PipeKey *k);'), 1)
            self.assertIn('PW_MAGIC 0x4d4b3850u', exec_c)
            self.assertIn('PW_VERSION 2u', exec_c)
            self.assertIn('mk8_pipeline_warm_start(); frame_end(shot_path, 0);', device_c)
            self.assertIn('mk8_pipeline_warm_start(); frame_end(shot_path, present_at_ns', device_c)
            # A second preparation of the same tree must not duplicate anything.
            pipeline_warm.prepare(runtime)
            pipeline_warm.prepare(runtime)
            self.assertEqual((runtime/'nvn/vulkan/exec.c').read_text(encoding='utf-8').count('#define PW_MAGIC'), 1)
            self.assertEqual((runtime/'nvn/vulkan/device.c').read_text(encoding='utf-8').count('mk8_pipeline_warm_start'), 4)
            # The record function itself: duplicates collapse, a new key appends, the switch stops it.
            start = exec_c.index('#define PW_MAGIC')
            end = exec_c.index('static volatile int pw_stop;')
            block = exec_c[start:end]
            self.assertIn('static void mk8_pipe_record(const PipeKey *k)', block)
            harness = out/'harness.c'
            harness.write_text(HARNESS.replace('/*RECORD*/', block), encoding='utf-8')
            exe = out/'harness'
            subprocess.run(['cc', '-std=gnu11', '-O1', str(harness), '-o', str(exe), '-pthread'], check=True,
                           capture_output=True)
            result = subprocess.run([str(exe)], cwd=out, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
