"""Threads guest finalizadas liberam condições nativas sem perder o objeto sinalizado."""
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
import thread_resources


HARNESS = r'''
#include <windows.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#define FATAL(...) (fprintf(stderr,__VA_ARGS__),abort())
typedef struct KThread {
    struct { bool signaled; int refs; } obj;
    pthread_t host;
    pthread_cond_t cond;
    bool started, finished, joined;
    bool release, ready;
    struct KThread *anext;
} KThread;
static pthread_mutex_t g_klock=PTHREAD_MUTEX_INITIALIZER;
static KThread *g_threads,*g_waitlist,*g_syncwaiters;
static void *handles[16];
static int nregions,g_rt_unwind;
static uint64_t heap_size,tls_next,next_thread_id;
#define TLS_BASE 0x1000
static int destroys;
static int checked_destroy(pthread_cond_t *c) {
    int err=pthread_cond_destroy(c);
    assert(!err);
    destroys++;
    return err;
}
static int checked_signal(pthread_cond_t *c) {
    int err=pthread_cond_signal(c);
    assert(!err);
    return err;
}
#define pthread_cond_destroy checked_destroy
#define pthread_cond_signal checked_signal
'''

MAIN = r'''
static DWORD handle_count(void) {
    DWORD count;assert(GetProcessHandleCount(GetCurrentProcess(),&count));return count;
}
static void *worker(void *arg) {
    KThread *t=arg;
    pthread_mutex_lock(&g_klock);
    t->ready=true;
    while(!t->release) assert(!pthread_cond_wait(&t->cond,&g_klock));
    t->obj.signaled=true;t->finished=true;
    pthread_mutex_unlock(&g_klock);
    return NULL;
}
static KThread *spawn(bool release) {
    KThread *t=calloc(1,sizeof *t);assert(t);
    assert(!pthread_cond_init(&t->cond,NULL));
    t->started=true;t->release=release;t->obj.refs=1;
    pthread_mutex_lock(&g_klock);t->anext=g_threads;g_threads=t;pthread_mutex_unlock(&g_klock);
    assert(!pthread_create(&t->host,NULL,worker,t));
    for(;;) {
        pthread_mutex_lock(&g_klock);bool ready=t->ready && (!release || t->finished);pthread_mutex_unlock(&g_klock);
        if(ready)break;
        Sleep(1);
    }
    return t;
}
int main(void) {
    // Aquecer as estruturas internas da biblioteca antes de medir handles.
    spawn(true);threads_reap();kernel_reset();destroys=0;
    DWORD before=handle_count();
    for(int batch=0;batch<2;batch++) {
        for(int i=0;i<70;i++)spawn(true);
        threads_reap();threads_reap();threads_reap();
        for(KThread *t=g_threads;t;t=t->anext) {
            assert(t->joined && t->finished && t->obj.signaled && t->obj.refs==1);
            pthread_mutex_lock(&g_klock);wake(t);pthread_mutex_unlock(&g_klock);
        }
        DWORD after=handle_count();
        fprintf(stderr,"batch %d: handles %lu -> %lu, condicoes liberadas %d\n",batch,before,after,destroys);
        assert(after<=before+2);
    }
    assert(destroys==140);
    KThread *live=spawn(false);
    threads_reap();assert(!live->joined && !live->finished);
    pthread_mutex_lock(&g_klock);live->release=true;wake(live);pthread_mutex_unlock(&g_klock);
    assert(!pthread_join(live->host,NULL));
    // O encerramento normal une as threads restantes sem marcar joined; reset libera a condicao uma vez.
    assert(live->finished && !live->joined);
    kernel_reset();assert(destroys==141 && !g_threads);
    assert(handle_count()<=before+2);
    return 0;
}
'''


@unittest.skipUnless(os.name == 'nt' and DEFAULT_COMPILER.is_file(), 'Windows native compiler required')
class ThreadResourceTests(unittest.TestCase):
    def test_reap_releases_conditions_and_reset_preserves_single_destruction(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', out)
            text = (runtime/'kernel.c').read_text(encoding='utf-8')
            functions = []
            for boundary in ('static void wake(', 'static void threads_reap(', 'void kernel_reset('):
                start = text.index(boundary)
                end = text.index('\n', start) if boundary == 'static void wake(' else text.index('\n}\n', start)+2
                functions.append(text[start:end]+'\n')
            source = out/'thread_resources.c'
            source.write_text(HARNESS+'\n'.join(functions)+MAIN, encoding='utf-8')
            exe = out/'thread_resources.exe'
            result = subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu', '-O1', '-UNDEBUG',
                                     str(source), '-o', str(exe), '-lpthread'],
                                    capture_output=True, text=True, env=compiler_env())
            self.assertEqual(result.returncode, 0, result.stderr[-3000:])
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_unknown_or_duplicate_boundary_preserves_source(self):
        original = (ROOT/'upstream/smm2recomp/recomp/runtime/kernel.c').read_text(encoding='utf-8')
        with tempfile.TemporaryDirectory() as folder:
            runtime = Path(folder)
            kernel = runtime/'kernel.c'
            for old, _ in thread_resources.BOUNDARIES:
                for replacement in ('changed', old+'\n'+old):
                    text = original.replace(old, replacement)
                    kernel.write_text(text, encoding='utf-8')
                    with self.assertRaisesRegex(ValueError, 'boundary changed'):
                        thread_resources.prepare(runtime)
                    self.assertEqual(kernel.read_text(encoding='utf-8'), text)


if __name__ == '__main__':
    unittest.main()
