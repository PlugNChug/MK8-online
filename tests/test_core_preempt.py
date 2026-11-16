"""A thread of better priority takes its emulated core at the running thread's next indirect call.

The prepared kernel's own core functions (core_release, core_acquire, core_check, the contention flags and
mk8_preempt_point) run in a native harness with real threads: one holds core 0 for a long job with no kernel call,
passing only the check rt_dispatch makes; another asks for the core meanwhile."""
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
import core_preempt

HARNESS = r'''
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define LOG(...) printf(__VA_ARGS__)
typedef struct KThread {
    uint64_t id;
    int prio, core;
    bool want_core;
    uint32_t last_svc;
    pthread_cond_t cond;
    struct KThread *anext;
} KThread;
pthread_mutex_t g_klock = PTHREAD_MUTEX_INITIALIZER;
static __thread KThread *t_cur;
static volatile int g_rt_unwind;
static __thread uint64_t g_blocked_ns;
void kernel_dump_threads(void) {}
static uint64_t plat_now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }
/*DEADLINE*/
/*CORES*/

static KThread low = {.id = 82, .prio = 45, .core = 0}, high = {.id = 80, .prio = 44, .core = 0};
static _Atomic uint64_t high_asked, high_got, high_left, low_done, overlap;
static _Atomic int high_holds;

static void *low_main(void *arg) {  // a long job holding core 0, with no kernel call; rt_dispatch's check only
    (void)arg;
    t_cur = &low;
    pthread_mutex_lock(&g_klock);
    core_acquire(&low);
    pthread_mutex_unlock(&g_klock);
    uint64_t end = plat_now_ns() + 300000000ull;
    while (plat_now_ns() < end) {
        if (atomic_load_explicit(&mk8_preempt_any, memory_order_relaxed)) mk8_preempt_point();
        if (atomic_load(&high_holds)) atomic_fetch_add(&overlap, 1);  // (never both on the core)
        for (volatile int i = 0; i < 2000; i++) {}
    }
    atomic_store(&low_done, plat_now_ns());
    pthread_mutex_lock(&g_klock);
    core_release(&low);
    pthread_mutex_unlock(&g_klock);
    return NULL;
}

static void *high_main(void *arg) {  // wakes 50 ms in and wants the same core for 20 ms
    (void)arg;
    t_cur = &high;
    struct timespec d = {0, 50000000};
    nanosleep(&d, NULL);
    atomic_store(&high_asked, plat_now_ns());
    pthread_mutex_lock(&g_klock);
    core_acquire(&high);
    pthread_mutex_unlock(&g_klock);
    atomic_store(&high_got, plat_now_ns());
    atomic_store(&high_holds, 1);
    d.tv_nsec = 20000000;
    nanosleep(&d, NULL);
    atomic_store(&high_holds, 0);
    atomic_store(&high_left, plat_now_ns());
    pthread_mutex_lock(&g_klock);
    core_release(&high);
    pthread_mutex_unlock(&g_klock);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc > 1) high.prio = atoi(argv[1]);
    pthread_cond_init(&low.cond, NULL);
    pthread_cond_init(&high.cond, NULL);
    low.anext = &high;
    g_threads = &low;
    pthread_t a, b;
    pthread_create(&a, NULL, low_main, NULL);
    struct timespec d = {0, 5000000};
    nanosleep(&d, NULL);  // (the job holds the core first)
    pthread_create(&b, NULL, high_main, NULL);
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    printf("waited %.1f ms; job ended %.1f ms after the other left; overlap %llu; flag %d\n",
           (high_got - high_asked) / 1e6, ((double)low_done - (double)high_left) / 1e6,
           (unsigned long long)overlap, atomic_load(&mk8_preempt_any));
    return 0;
}
'''


def run(exe, *args, env=None):
    result = subprocess.run([str(exe), *args], capture_output=True, text=True, timeout=60, env=env)
    assert result.returncode == 0, result.stdout + result.stderr
    words = result.stdout.split()
    return float(words[1]), float(words[5]), int(words[12].rstrip(';')), int(words[14])


class CorePreemptTests(unittest.TestCase):
    def test_boundaries_are_checked(self):
        with tempfile.TemporaryDirectory() as folder:
            runtime = Path(folder)
            (runtime/'kernel.c').write_text('int x;\n', encoding='utf-8')
            (runtime/'dispatch.c').write_text('int y;\n', encoding='utf-8')
            with self.assertRaises(ValueError):
                core_preempt.prepare(runtime)

    @unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
    def test_better_priority_takes_the_core_at_the_next_indirect_call(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', out/'host')
            kernel = (runtime/'kernel.c').read_text(encoding='utf-8')
            dispatch = (runtime/'dispatch.c').read_text(encoding='utf-8')
            self.assertIn('if (__builtin_expect(atomic_load_explicit(&mk8_preempt_any, memory_order_relaxed), 0)) '
                          'mk8_preempt_point();', dispatch)
            start = kernel.index('static void deadline(')
            deadline = kernel[start:kernel.index('\n}\n', start) + 3]
            cores = kernel[kernel.index('static KThread *g_threads;'):kernel.index('// A thread moved to another core')]
            harness = out/'harness.c'
            harness.write_text(HARNESS.replace('/*DEADLINE*/', deadline).replace('/*CORES*/', cores), encoding='utf-8')
            exe = out/'harness.exe'
            subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu', '-O1', str(harness), '-o', str(exe)],
                           env=compiler_env(), check=True, capture_output=True)
            env = {k: v for k, v in os.environ.items() if not k.startswith(('MK8_', 'RECOMP_'))}
            waited, after, overlap, flag = run(exe, env=env)
            self.assertLess(waited, 30, 'the waiter of better priority got the core at once')
            self.assertGreater(after, 100, 'the job resumed after it and went on')
            self.assertEqual(overlap, 0, 'never two threads of one core at once')
            self.assertEqual(flag, 0, 'no contention left')
            # equal priority: no preemption (as Horizon), it waits for the job
            waited, after, overlap, _ = run(exe, '45', env=env)
            self.assertGreater(waited, 200)
            self.assertEqual(overlap, 0)
            # off: as the pinned kernel, it waits for the job
            waited, after, overlap, _ = run(exe, env={**env, 'MK8_NO_PREEMPT': '1'})
            self.assertGreater(waited, 200)
            self.assertEqual(overlap, 0)


if __name__ == '__main__':
    unittest.main()
