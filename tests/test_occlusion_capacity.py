"""Execute the prepared report recorder across query-pool boundaries."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'toolchain'))
from host import prepare_host_sources
from native import DEFAULT_COMPILER, compiler_env

HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#define MK8_OQ_MAX 256
#define LOG(...) ((void)0)
static jmp_buf fatal;
#define FATAL(...) longjmp(fatal, 1)
typedef int VkCommandBuffer;
typedef struct {
    int pool; unsigned n, nrep, dirty; bool active;
    uint64_t *rep_out[MK8_OQ_MAX]; unsigned rep_after[MK8_OQ_MAX];
} Mk8Oq;
static Mk8Oq mk8_oq[1] = {{.pool=1}};
static int cur_sub, cq_lock, locked, mk8_occlusion_flags;
static unsigned flushes;
static uint64_t samples[MK8_OQ_MAX], total;
static bool carry;
static void pthread_mutex_lock(int *p) { (void)p; assert(!locked); locked=1; }
static void pthread_mutex_unlock(int *p) { (void)p; assert(locked); locked=0; }
static uint64_t plat_now_ns(void) { return 123; }
static uint64_t ns_to_gpu_ticks(uint64_t n) { return n; }
static void vk_end_pass(void) {}
static void vkCmdBeginQuery(int cb,int pool,unsigned q,int flags) {
    (void)cb; (void)pool; (void)flags; assert(q<MK8_OQ_MAX); samples[q]=0;
}
static void vkCmdEndQuery(int cb,int pool,unsigned q) {
    (void)cb; (void)pool; assert(q<MK8_OQ_MAX);
}
static int vk_cmd(void) {
    if (carry) {
        mk8_oq[0].n=1; mk8_oq[0].active=true; samples[0]=0; carry=false;
    }
    return 1;
}
static void renderer_flush(void) {
    assert(!locked); flushes++;
    Mk8Oq *o=&mk8_oq[0]; unsigned q=0;
    for (unsigned k=0;k<o->nrep;k++) {
        while (q<o->rep_after[k]) total+=samples[q++];
        if (o->rep_out[k]) { o->rep_out[k][0]=total; o->rep_out[k][1]=123; }
        else total=0;
    }
    while (q<o->n) total+=samples[q++];
    carry=o->active;
    o->active=false; o->n=o->nrep=0;
}
typedef struct { uint64_t x[2]; } CmdRec;
static void report(uint64_t *out,bool reset) {
    CmdRec cmd={{reset ? 0x100000001ull : 1,0}}, *r=&cmd;
    unsigned type=1;
    do {
#include "report.inc"
    } while (0);
}
int main(void) {
    uint64_t out[800][2]; memset(out,0xff,sizeof out);
    for (unsigned i=0;i<800;i++) {
        if (i==400) report(NULL,true);
        report(out[i],false);
        assert(mk8_oq[0].active);
        samples[mk8_oq[0].n-1]+=3; // a real draw's samples, between reports
    }
    renderer_flush();
    for (unsigned i=0;i<800;i++) {
        assert(out[i][0]==(uint64_t)(i<400 ? i : i-400)*3);
        assert(out[i][1]==123);
    }
    assert(flushes==4);
    carry=false; mk8_oq[0].pool=0;
    if (!setjmp(fatal)) { report(out[0],false); return 9; }
    return 0;
}
'''


class OcclusionCapacityTests(unittest.TestCase):
    @unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
    def test_reports_survive_pool_rollover_and_counter_reset(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            runtime = prepare_host_sources(ROOT / 'upstream/smm2recomp/recomp', out / 'host')
            text = (runtime / 'nvn/vulkan/exec.c').read_text(encoding='utf-8')
            start = text.index('            bool reset_samples =')
            end = text.index('            out[0] = 0;', start)
            (out / 'report.inc').write_text(text[start:end], encoding='utf-8')
            (out / 'test.c').write_text(HARNESS, encoding='utf-8')
            exe = out / 'test.exe'
            result = subprocess.run([DEFAULT_COMPILER, 'cc', '-target', 'x86_64-windows-gnu',
                                     '-O1', '-UNDEBUG', out / 'test.c', '-o', exe],
                                    capture_output=True, text=True, env=compiler_env())
            self.assertEqual(result.returncode, 0, result.stderr)
            env = {k: v for k, v in os.environ.items() if not k.startswith('MK8_OCCLUSION_')}
            run = subprocess.run([exe], env=env, capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stderr)


if __name__ == '__main__':
    unittest.main()
