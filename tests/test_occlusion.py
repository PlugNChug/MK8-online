"""SAMPLES_PASSED reports get the cumulative samples at their point, resolved in submission order."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from host import DEPS, prepare_host_sources
from native import DEFAULT_COMPILER
import occlusion


@unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
class OcclusionTests(unittest.TestCase):
    def test_reports_and_conditional_dependencies(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', out)
            text = (runtime/'nvn/vulkan/exec.c').read_text(encoding='utf-8')
            for needle in ('mk8_oq_resolve_done(done);', 'VK_QUERY_TYPE_OCCLUSION',
                           'vkCmdBeginQuery(qcb, o->pool, o->n++, mk8_occlusion_flags);',
                           'vkCmdBeginQuery(s->main, o->pool, 0, mk8_occlusion_flags);', 'mk8_oq_carry = true;'):
                self.assertIn(needle, text)
            state = text[text.index('// MK8 (toolchain/occlusion.py): SAMPLES_PASSED reports'):text.index('static void cq_resolve_done(uint64_t done) {  // every slot')]
            condition = text[text.index('        case CMD_SET_RENDER_ENABLE:', text.index('static void exec_list(')):]
            condition = condition[:condition.index('        case CMD_REPORT_VALUE:')]
            source = out/'occl.c'
            source.write_text(r'''
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <setjmp.h>
#include <vulkan/vulkan.h>
#define SUBS 8
#define LOG(...) ((void)0)
static jmp_buf failure;
static void fail(void) { longjmp(failure, 1); }
#define FATAL(...) fail()
static int cq_lock,locked;
static void pthread_mutex_lock(int *lock) { assert(!locked); locked=1; }
static void pthread_mutex_unlock(int *lock) { assert(locked); locked=0; }
static const char *wait_why;
static uint64_t pending_seq=200,waited_seq;
static unsigned flushes,waits;
static bool timeout;
static uint64_t vk_pending_seq(void) { return pending_seq; }
static void vk_wait_seq(uint64_t seq);
static struct { VkDevice dev; } g_vk;
static uint64_t results[SUBS][256];
static uint32_t reset_count, reset_calls;
void VKAPI_CALL vkCmdResetQueryPool(VkCommandBuffer cb, VkQueryPool pool, uint32_t first, uint32_t count) {
    (void)cb; (void)pool; (void)first;
    reset_count = count; reset_calls++;
}
static uint64_t plat_now_ns(void) { return 1000; }
static uint64_t ns_to_gpu_ticks(uint64_t ns) { return ns; }
static unsigned query_reads;
static bool query_ok=true;
VkResult VKAPI_CALL vkGetQueryPoolResults(VkDevice d, VkQueryPool pool, uint32_t first, uint32_t n, size_t size, void *data,
                                          VkDeviceSize stride, VkQueryResultFlags flags) {
    (void)d; (void)first; (void)size; (void)stride; (void)flags;
    query_reads++;
    if (!query_ok) return VK_NOT_READY;
    memcpy(data, results[(uintptr_t)pool - 1], n * 8);
    return VK_SUCCESS;
}
''' + state + r'''
static void vk_wait_seq(uint64_t seq) {
    assert(!locked); assert(wait_why && !strcmp(wait_why,"conditional samples"));
    waits++; waited_seq=seq;
    if (!timeout) mk8_oq_resolve_done(seq);
}
void renderer_flush(void) { assert(!locked); flushes++; vk_wait_seq(pending_seq++); }
typedef struct { uint64_t x[3]; } CmdRec;
enum { CMD_SET_RENDER_ENABLE };
static struct { bool render_off; } st;
static bool g_debug_frame;
static void condition(CmdRec *r) { switch (CMD_SET_RENDER_ENABLE) {
''' + condition + r'''
} }
static void reports(uint64_t seq,uint64_t *a,uint64_t *b,uint64_t first,uint64_t second) {
    Mk8Oq *o=&mk8_oq[0];
    memset(o,0,sizeof *o); o->pool=(VkQueryPool)(uintptr_t)1; o->seq=seq; o->n=2; o->nrep=2;
    o->rep_out[0]=a; o->rep_out[1]=b; o->rep_after[0]=1; o->rep_after[1]=2;
    results[0][0]=first; results[0][1]=second;
}
int main(void) {
    uint64_t a[3][2] = {{0}}, b[2][2] = {{0}};
    /* slot 5, submitted first: report a0 (no query before), query 0 = 100 samples, report a1, query 1 = 7, report a2,
       query 2 = 5 after the last report */
    Mk8Oq *s5 = &mk8_oq[5], *s2 = &mk8_oq[2];
    s5->pool = (VkQueryPool)(uintptr_t)6; s5->seq = 10; s5->n = 3; s5->nrep = 3;
    s5->dirty = 3;
    results[5][0] = 100; results[5][1] = 7; results[5][2] = 5;
    s5->rep_out[0] = a[0]; s5->rep_after[0] = 0;
    s5->rep_out[1] = a[1]; s5->rep_after[1] = 1;
    s5->rep_out[2] = a[2]; s5->rep_after[2] = 2;
    /* slot 2, submitted after it: query 0 = 3, report b0, query 1 = 0 (hidden), report b1 */
    s2->pool = (VkQueryPool)(uintptr_t)3; s2->seq = 11; s2->n = 2; s2->nrep = 2;
    results[2][0] = 3; results[2][1] = 0;
    s2->rep_out[0] = b[0]; s2->rep_after[0] = 1;
    s2->rep_out[1] = b[1]; s2->rep_after[1] = 2;
    mk8_oq_resolve_done(10);  /* only the first is done */
    if (a[0][0] != 0 || a[1][0] != 100 || a[2][0] != 107 || mk8_samples_total != 112 || b[0][0]) return 1;
    mk8_oq_reset_pool(NULL, s5);
    if (reset_calls != 1 || reset_count != 3 || s5->dirty) return 6;
    mk8_oq_reset_pool(NULL, s5);
    if (reset_calls != 1) return 7;
    mk8_oq_resolve_done(11);
    if (b[0][0] != 115 || b[1][0] != 115 || mk8_samples_total != 115) return 2;  /* b1 - b0 = 0: hidden */
    if (s5->n || s5->nrep || s2->n || s2->nrep) return 3;
    mk8_oq_resolve_done(99);  /* nothing left: no change */
    if (mk8_samples_total != 115) return 4;
    s2->seq = 100; s2->n = 2; s2->nrep = 3;
    results[2][0] = 12; results[2][1] = 6;
    s2->rep_out[0] = a[0]; s2->rep_after[0] = 1;
    s2->rep_out[1] = NULL; s2->rep_after[1] = 1;  /* ordered reset */
    s2->rep_out[2] = a[1]; s2->rep_after[2] = 2;
    mk8_oq_resolve_done(100);
    if (a[0][0] != 127 || a[1][0] != 6 || mk8_samples_total != 6 || query_reads!=3) return 5;
    query_ok=false; s2->seq=101; s2->n=2; s2->nrep=2;
    s2->rep_out[0]=a[2]; s2->rep_after[0]=1; s2->rep_out[1]=b[0]; s2->rep_after[1]=2;
    uint64_t old_a=a[2][0],old_b=b[0][0];
    if (!setjmp(failure)) { mk8_oq_resolve_done(101); return 9; }
    assert(a[2][0]==old_a && b[0][0]==old_b && s2->nrep==2);
    memset(mk8_oq,0,sizeof mk8_oq); query_ok=true; mk8_samples_total=0;
    uint64_t pair[4]={0},other[4]={0};
    CmdRec cmd={{1,(uintptr_t)pair,1}};
    /* Captured failure: initial 0/0 must become 153/0 before mode-1 comparison.
       Both operands are pending, in the current unsubmitted work. */
    reports(200,pair+2,pair,0,153); condition(&cmd);
    assert(!st.render_off && pair[0]==153 && pair[2]==0 && flushes==1 && waits==1 && waited_seq==200);
    condition(&cmd); assert(!st.render_off && waits==1);  // resolved: no repeated wait
    /* First operand alone pending, already submitted. The stale positive must become a real zero. */
    mk8_samples_total=0; pair[0]=99; pair[2]=0;
    reports(199,pair,other,0,0); condition(&cmd);
    assert(st.render_off && pair[0]==0 && flushes==1 && waits==2 && waited_seq==199);
    cmd.x[0]=0; condition(&cmd); assert(!st.render_off && waits==2); // equal mode
    pair[0]=12; condition(&cmd); assert(st.render_off && waits==2); // different in equal mode
    /* Both operands can have producers in different slots: wait once on the latest sequence.
       Resolving in submission order also applies an intervening reset. */
    mk8_samples_total=0; reports(199,pair,other,3,0);
    Mk8Oq *later=&mk8_oq[1]; later->pool=(VkQueryPool)(uintptr_t)2; later->seq=200;
    later->n=1; later->nrep=2; later->rep_out[0]=NULL; later->rep_after[0]=0;
    later->rep_out[1]=pair+2; later->rep_after[1]=1; results[1][0]=3;
    condition(&cmd); assert(!st.render_off && pair[0]==3 && pair[2]==3 && waits==3 && waited_seq==200 && flushes==1);
    /* No global flush for unrelated reports, plain enable/disable or null counters. */
    reports(pending_seq,other,other+2,10,0); condition(&cmd); assert(waits==3);
    cmd.x[2]=0; cmd.x[0]=1; condition(&cmd); assert(!st.render_off && waits==3);
    cmd.x[0]=0; condition(&cmd); assert(st.render_off && waits==3);
    cmd.x[2]=1; cmd.x[1]=0; condition(&cmd); assert(!st.render_off && waits==3);
    cmd.x[0]=1; condition(&cmd); assert(st.render_off && waits==3);
    /* Second operand alone pending: matching only the first address would miss this dependency. */
    mk8_samples_total=0; pair[0]=153; pair[2]=99; cmd.x[1]=(uintptr_t)pair;
    reports(200,other,pair+2,0,0); condition(&cmd);
    assert(!st.render_off && pair[2]==0 && waits==4 && flushes==1);
    /* Timeout and impossible future/zero sequence must never fall through to a stale decision. */
    cmd.x[0]=0; cmd.x[1]=(uintptr_t)pair; timeout=true;
    for (unsigned k=0;k<3;k++) {
        reports(k==0 ? pending_seq-1 : k==1 ? pending_seq+1 : 0,pair,other,0,0);
        st.render_off=true;
        if (!setjmp(failure)) { condition(&cmd); return 10; }
        assert(st.render_off && mk8_oq[0].nrep==2);
        locked=0; // mock lock is still held by the fatal zero-sequence path
    }
    return 0;
}
''', encoding='utf-8')
            exe = out/'occl.exe'
            env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(ROOT/'local/native-tools/cache'))
            build = subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu', '-O2', '-I'+str(DEPS/'include'),
                                    '-UNDEBUG', str(source), '-o', str(exe)], env=env, capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr[-3000:])
            subprocess.run([str(exe)], check=True, timeout=30)

    def test_boundaries_are_checked(self):
        with tempfile.TemporaryDirectory() as folder:
            runtime = Path(folder)
            (runtime/'nvn/vulkan').mkdir(parents=True)
            (runtime/'nvn/vulkan/exec.c').write_text('int x;\n')
            (runtime/'nvn/cmdbuf.c').write_text('NVN(nvnCommandBufferResetCounter) { (void)c; }\n')
            with self.assertRaises(ValueError):
                occlusion.prepare(runtime)


if __name__ == '__main__':
    unittest.main()
