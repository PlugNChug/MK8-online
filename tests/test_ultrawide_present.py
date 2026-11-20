"""Each queued image retains its camera decision across producer waits and repeated presentation."""
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


HARNESS = r'''
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "nvn/nvn.h"
#include "nvn/output_scale.h"
#include "settings.h"
#ifdef NDEBUG
#error Presentation checks require assertions
#endif
// Failed regressions must exit, rather than wait for Windows crash reporting.
#undef assert
#define assert(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "Assertion failed: %s, line %d\n", #condition, __LINE__); \
        exit(1); \
    } \
} while (0)
#undef NVN
#define NVN(name) void name(Cpu *c)
Settings g_settings;
static _Atomic double pending;
static _Atomic bool gate, entered;
static _Atomic int drawable_width=1680;
static bool resize_before_consume, resize_before_repeat;
static double after_wait, latched;
static int after_wait_aspect, takes, latches, shown;
static uint64_t now=1000000000;
static unsigned references;
static NvnTexture *last_image;
void rt_log(const char *fmt, ...) { (void)fmt; }
void rt_fatal_trace(void) { abort(); }
static NvnSync *sync_of(uint64_t guest) { (void)guest; abort(); }
void backend_prefetch(const uint64_t *handles, int n) { (void)handles; (void)n; }
void backend_execute(uint64_t handle) { (void)handle; abort(); }
void renderer_prefetch_verify(void) {}
uint64_t renderer_fence_seq(void) { return 0; }
void renderer_submit(void) {}
void mk8_texture_translator_thread(void) {}
void mk8_textures_drain(void) {}
void mk8_descriptors_drain(void) {}
NvnTexture *mk8_texture_retain(NvnTexture *t) { references++; return t; }
void mk8_texture_release(NvnTexture *t) { (void)t; assert(references); references--; }
int mk8_textures_release_range(uint64_t lo, uint64_t hi, uint64_t *bytes) {
    (void)lo; (void)hi; (void)bytes; abort();
}
void mk8_texture_readback(NvnTexture *t, const NvnTextureView *view, const int32_t *region,
                          uint64_t destination, int64_t row, int64_t image) {
    (void)t; (void)view; (void)region; (void)destination; (void)row; (void)image; abort();
}
uint64_t plat_now_ns(void) { return now; }
void kernel_host_wait_begin(void) {
    atomic_store(&pending, after_wait); g_settings.aspect=after_wait_aspect;
    if (resize_before_consume) atomic_store(&drawable_width,2560);
}
void kernel_host_wait_end(void) {}
static void display_pace(int interval) { (void)interval; }
const uint8_t *game_overlay_image(int *w, int *h, int *full, uint64_t *seq) {
    (void)w; (void)h; (void)full; (void)seq; abort();
}
double mk8_ultrawide_take_present(void) { takes++; return atomic_exchange(&pending, 16.0/9.0); }
double mk8_ultrawide_present_mode(void) { return latched; }
@@QUEUE@@
@@PRESENT@@
void mk8_ultrawide_present(double decision) {
    assert(pthread_mutex_trylock(&g_present_mx)==EBUSY);
    latched=decision; latches++;
}
void backend_present(NvnTexture *t) {
    assert(pthread_mutex_trylock(&g_present_mx)==EBUSY);
    if (!t->debug_id) {
        atomic_store(&entered, true);
        while (!atomic_load(&gate)) Sleep(1);
    }
    const double decisions[]={21.0/9.0,16.0/9.0,0,16.0/9.0,32.0/9.0};
    assert(t->debug_id<5 && latched==decisions[t->debug_id]);
    OutScale scale;
    int dw=atomic_load(&drawable_width);
    output_scale(dw,720,false,1,-1,&scale);
    // Expected pixel rectangles for these authored images, independent of the placement implementation.
    double image_width=dw, image_height=720;
    if (latched==16.0/9.0 || (latched==0 && g_settings.aspect==ASPECT_ORIGINAL)) image_width=1280;
    else if (latched==21.0/9.0) image_width=1680;
    else if (latched==32.0/9.0 && dw==1680) image_height=472.5;
    assert(fabs(scale.vw-image_width)<1e-9 && fabs(scale.vh-image_height)<1e-9);
    assert(fabs(scale.vx-(dw-image_width)/2)<1e-9 && fabs(scale.vy-(720-image_height)/2)<1e-9);
    assert(scale.shape==1 && scale.sx==scale.sy);
    last_image=t; shown++;
}
void backend_represent(void) { assert(last_image); backend_present(last_image); }
static void test_present(NvnWindow *window, int index, double decision, double next) {
    uint64_t guest=(uintptr_t)window;
    Cpu c={0}; c.x[1]=(uintptr_t)&guest; c.x[2]=(uint64_t)index;
    atomic_store(&pending, decision); after_wait=next;
    int before=takes;
    nvnQueuePresentTexture(&c);
    assert(takes==before+(index>=0 && index<window->num));
}
static void repeat(double expected) {
    int old_takes=takes, old_latches=latches, old_shown=shown;
    atomic_store(&pending, expected==16.0/9.0 ? 32.0/9.0 : 16.0/9.0);
    double next=atomic_load(&pending);
    if (resize_before_repeat && expected==21.0/9.0) atomic_store(&drawable_width,2560);
    now+=200000000;
    nvn_represent();
    assert(takes==old_takes && latches==old_latches && shown==old_shown+1);
    assert(latched==expected && atomic_load(&pending)==next);
}
int main(int argc, char **argv) {
    assert(argc==2);
    resize_before_consume=!strcmp(argv[1],"resize-before-consume");
    resize_before_repeat=!strcmp(argv[1],"resize-before-repeat");
    NvnTexture textures[5]={0};
    uint64_t guests[5];
    NvnWindow window={.num=5,.interval=1};
    for (unsigned i=0;i<5;i++) {
        textures[i].debug_id=i; guests[i]=(uintptr_t)&textures[i];
        window.textures[i]=(uintptr_t)&guests[i];
    }
    g_settings.aspect=ASPECT_UNLOCKED; g_settings.res_mode=RES_MATCH;
    after_wait_aspect=ASPECT_ORIGINAL;
    atomic_store(&gate,!mk8_async_on());
    test_present(&window,0,21.0/9.0,32.0/9.0);  // pacing changes the next producer decision before presentation
    while (!atomic_load(&entered)) Sleep(1);
    test_present(&window,1,16.0/9.0,32.0/9.0);
    test_present(&window,2,0,16.0/9.0);
    atomic_store(&pending,32.0/9.0); atomic_store(&gate,true);
    mk8_q_drain();
    assert(shown==3 && !references && takes==3 && latches==3);
    g_settings.aspect=ASPECT_UNLOCKED;
    repeat(0);  // disabled experiment keeps ordinary Stretch/Original behavior for the old image
    g_settings.aspect=ASPECT_ORIGINAL; after_wait_aspect=ASPECT_UNLOCKED;
    test_present(&window,3,16.0/9.0,32.0/9.0); mk8_q_drain(); repeat(16.0/9.0);
    test_present(&window,0,21.0/9.0,16.0/9.0); mk8_q_drain(); repeat(21.0/9.0);
    atomic_store(&drawable_width,2560);
    test_present(&window,4,32.0/9.0,21.0/9.0); mk8_q_drain(); repeat(32.0/9.0); // new image fills 32:9
    resize_before_consume=false; resize_before_repeat=false;
    atomic_store(&drawable_width,1680); repeat(32.0/9.0); // old 32:9 image fits the smaller drawable
    test_present(&window,4,32.0/9.0,21.0/9.0); mk8_q_drain(); // same fit for a queued image
    test_present(&window,0,21.0/9.0,32.0/9.0); mk8_q_drain(); repeat(21.0/9.0); // next 21:9 image fills
    test_present(&window,-1,16.0/9.0,32.0/9.0); test_present(&window,5,16.0/9.0,32.0/9.0); mk8_q_drain();
    assert(takes==8 && latches==8 && shown==14 && !references);
    for (int ordinary=ASPECT_UNLOCKED; ordinary<=ASPECT_ORIGINAL; ordinary++) {
        g_settings.aspect=ASPECT_ULTRAWIDE; after_wait_aspect=ordinary;
        test_present(&window,0,21.0/9.0,16.0/9.0); mk8_q_drain();
        assert(g_settings.aspect==ordinary); repeat(21.0/9.0);
        after_wait_aspect=ASPECT_ULTRAWIDE;
        test_present(&window,1,16.0/9.0,21.0/9.0); mk8_q_drain();
        assert(g_settings.aspect==ASPECT_ULTRAWIDE); repeat(16.0/9.0);
    }
    assert(takes==12 && latches==12 && shown==22 && !references);
    return 0;
}
'''


@unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
class UltrawidePresentTests(unittest.TestCase):
    def test_image_decision_survives_queue_waits_and_represent(self):
        with tempfile.TemporaryDirectory(prefix='mk8-present-') as folder:
            out = Path(folder)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', out/'host')
            objects = (runtime/'nvn/objects.c').read_text(encoding='utf-8')
            start = objects.index('// MK8 (toolchain/async_submit.py): submissions, presents')
            queue = objects[start:objects.index('void renderer_wait_seq(uint64_t seq);', start)]
            start = objects.index('_Atomic int g_present_hidden;')
            present = objects[start:objects.index('// ================================================================== small state objects', start)]
            source = out/'present.c'
            source.write_text(HARNESS.replace('@@QUEUE@@', queue).replace('@@PRESENT@@', present), encoding='utf-8')
            exe = out/'present.exe'
            build = subprocess.run([DEFAULT_COMPILER, 'cc', '-target', 'x86_64-windows-gnu', '-O1', '-UNDEBUG',
                                    '-I'+str(runtime), '-I'+str(runtime/'nvn'), '-I'+str(DEPS/'include'), source,
                                    runtime/'nvn/output_scale.c', '-o', exe], env=compiler_env(), capture_output=True,
                                   text=True, timeout=120)
            self.assertEqual(build.returncode, 0, build.stderr)
            env = dict(os.environ, MK8_ASYNC_AHEAD='8')
            for key in ('LARECOMP_SCALE', 'LARECOMP_WIDE_TEST'):
                env.pop(key, None)
            for mode in ('1', '0'):
                for case in ('unchanged', 'resize-before-consume', 'resize-before-repeat'):
                    with self.subTest(async_submit=mode, case=case):
                        result = subprocess.run([exe, case], env=dict(env, MK8_ASYNC_SUBMIT=mode),
                                                capture_output=True, text=True, timeout=30)
                        self.assertEqual(result.returncode, 0, (mode, case, result.stdout, result.stderr))


if __name__ == '__main__':
    unittest.main()
