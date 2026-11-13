"""Adversarial native tests of the actual shader tables and job queue, without a GPU."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from host import prepare_host_sources
from native import DEFAULT_COMPILER, compiler_env


@unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
class ShaderStorageTests(unittest.TestCase):
    def compile_run(self, body):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', out/'host')
            source = out/'test.c'
            if callable(body):
                body = body((runtime/'nvn/vulkan/exec.c').read_text(encoding='utf-8'))
            source.write_text('#ifdef NDEBUG\n#error Assertions must be enabled\n#endif\n' + body, encoding='utf-8')
            exe = out/'test.exe'
            result = subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu', '-O1', '-UNDEBUG',
                                     '-I'+str(runtime/'nvn'), '-I'+str(ROOT/'runtime/mk8/graphics'),
                                     str(source), '-o', str(exe)], env=compiler_env(), capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_tables_grow_preserving_collisions_and_payloads(self):
        def fixture(text):
            tables = text[text.index('// MK8 shader storage: prog_tab'):text.index('static Prog *get_program')]
            tables += text[text.index('// MK8 shader storage: pipe_tab'):text.index('// 8 bytes a step')]
            return r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#define FATAL(...) exit(77)
typedef struct { int id; } Prog;
typedef struct { int id; } Pipe;
''' + tables + r'''
int main(void) {
    enum { N = 20000 };
    Prog *progs = calloc(N, sizeof *progs);
    Pipe *pipes = calloc(N, sizeof *pipes);
    assert(progs && pipes);
    for (int i = 0; i < N; i++) {
        uint64_t key = (uint64_t)(i + 1) * 8192; // adversarial collisions
        uint32_t s = mk8_prog_tab_slot(key), p = mk8_pipe_tab_slot(key);
        assert(!prog_tab[s].key && !pipe_tab[p].key);
        prog_tab[s] = (Mk8_prog_tab_entry){key, &progs[i]}; prog_tab_count++;
        pipe_tab[p] = (Mk8_pipe_tab_entry){key, &pipes[i]}; pipe_tab_count++;
    }
    assert(prog_tab_capacity > N && pipe_tab_capacity > N);
    for (int i = 0; i < N; i++) {
        uint64_t key = (uint64_t)(i + 1) * 8192;
        assert(prog_tab[mk8_prog_tab_slot(key)].prog == &progs[i]);
        assert(pipe_tab[mk8_pipe_tab_slot(key)].pipe == &pipes[i]);
    }
    assert(prog_tab_count == N && pipe_tab_count == N);
    free(prog_tab); free(pipe_tab); free(progs); free(pipes);
    return 0;
}
'''
        self.compile_run(fixture)

    def test_repeated_prefetch_keeps_one_request_per_hash(self):
        def fixture(text):
            start = text.index('    if (atomic_load(&pr->state) == BUILD_PENDING) {  // compiling:')
            end = text.index('    if (atomic_load(&pr->state) != BUILD_READY) return;', start)
            return r'''
#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#define FATAL(...) exit(77)
#define BUILD_PENDING 0
typedef struct PipeReq { struct PipeReq *next; uint64_t hash; int k; } PipeReq;
typedef struct { _Atomic int state; PipeReq *waiting; } Prog;
static pthread_mutex_t g_wait_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { int pipes; } g_prefetch;
static void request(Prog *pr, uint64_t ph) {
    int k = 5;
    pthread_mutex_lock(&g_wait_lock);
''' + text[start:end] + r'''
}
int main(void) {
    Prog pr = {0};
    for (int i = 0; i < 20000; i++) request(&pr, 123);
    request(&pr, 456);
    assert(g_prefetch.pipes == 2);
    assert(pr.waiting->hash == 456 && pr.waiting->next->hash == 123);
    assert(!pr.waiting->next->next);
    free(pr.waiting->next); free(pr.waiting);
    return 0;
}
'''
        self.compile_run(fixture)

    def test_full_pending_table_rejects_unowned_pipeline(self):
        self.compile_run(r'''
#include <assert.h>
#define LOG(...) ((void)0)
#include "buildsync.c"
int main(void) {
    int values[PEND_TAB + 1];
    for (int i = 1; i < PEND_TAB; i++) assert(pend_get_or_put(i, &values[i]) == &values[i]);
    assert(pend_get_or_put(PEND_TAB, &values[PEND_TAB]) == NULL);
    assert(pend_take(PEND_TAB) == NULL);
    assert(pend_get_or_put(5, &values[0]) == &values[5]); // duplicates still resolve when full
    for (int i = 1; i < PEND_TAB; i++) assert(pend_take(i) == &values[i]);
    assert(npend == 0);
    return 0;
}
''')

    def test_worker_claims_job_before_publishing_dequeue(self):
        self.compile_run(r'''
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <windows.h>
static pthread_mutex_t *observed;
static _Atomic int arm, paused, resume_worker, calls;
static void publish_unlock(pthread_mutex_t *m) { pthread_mutex_unlock(m); }
static int instrumented_unlock(pthread_mutex_t *m) {
    int r = pthread_mutex_unlock(m);
    if (m == observed && atomic_exchange(&arm, 0)) {
        atomic_store(&paused, 1);
        while (!atomic_load(&resume_worker)) Sleep(1);
    }
    return r;
}
#define pthread_mutex_unlock instrumented_unlock
#define LOG(...) ((void)0)
#include "buildsync.c"
static void job(void *p) { (void)p; atomic_fetch_add(&calls, 1); }
static void *waiter(void *p) { BuildJob *j = p; job_run_or_wait((JobQueue *)j->arg, j); return NULL; }
int main(void) {
    JobQueue *q = jobq_create(1);
    observed = &q->lock;
    BuildJob j = {0};
    // Arm only the worker's unlock, after submit has published the job while holding q->lock.
    j.fn = job; j.arg = q; atomic_store(&j.state, JOB_QUEUED);
    pthread_mutex_lock(&q->lock); q->head = q->tail = &j;
    pthread_cond_signal(&q->cond);
    atomic_store(&arm, 1);
    publish_unlock(&q->lock); // bypass wrapper on the publishing thread
    while (!atomic_load(&paused)) Sleep(1);
    assert(atomic_load(&j.state) == JOB_RUNNING); // old code left a stealable pointer outside q->lock
    pthread_t thread; assert(!pthread_create(&thread, NULL, waiter, &j));
    atomic_store(&resume_worker, 1); pthread_join(thread, NULL);
    assert(atomic_load(&j.state) == JOB_DONE && atomic_load(&calls) == 1);
    return 0;
}
''')


if __name__ == '__main__':
    unittest.main()
