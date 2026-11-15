"""O diagnóstico identifica esperas sem mudar resultados ou o caminho de execução."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from host import prepare_host_sources
from native import DEFAULT_COMPILER
import shader_wait_trace


class ShaderWaitTraceTests(unittest.TestCase):
    @unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
    def test_wait_results_and_identification(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', out/'prepared')
            text = (runtime/'nvn/vulkan/exec.c').read_text(encoding='utf-8')
            start = text.index('// Mede apenas o caminho original;')
            support = text[start:text.index('static pthread_mutex_t g_wait_lock', start)]
            source = out/'trace.c'
            source.write_text(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { int unused; } JobQueue;
typedef struct { int unused; } BuildJob;
typedef struct { uint64_t code[3]; } Prog;
static char message[1024];
#define LOG(...) snprintf(message, sizeof message, __VA_ARGS__)
static uint64_t ticks;
static unsigned bounded, full;
static bool result;
static uint64_t SDL_GetTicksNS(void) { return ticks; }
static void job_run_or_wait(JobQueue *q, BuildJob *j) { (void)q; (void)j; full++; ticks += 1000000000; }
''' + support + r'''
static bool mk8_job_ready(JobQueue *q, BuildJob *j) { (void)q; (void)j; bounded++; ticks += 25000000; return result; }
static bool mk8_targets_redrawn(void) { return true; }
uint64_t vk_frame_counter(void) { return 42; }
int main(int argc, char **argv) {
    (void)argv;
    Prog p = {{0x123, 0x456, 0}};
    result = false;
    assert(!mk8_traced_job(NULL, NULL, &p, "translate", 0, true));
    assert(bounded == 1 && full == 0);
    if (argc > 1) {
        assert(strstr(message, "phase=translate ms=25.000"));
        assert(strstr(message, "code=123/456/0"));
        assert(strstr(message, "ready=0"));
    } else assert(!message[0]);
    result = true;
    assert(mk8_traced_job(NULL, NULL, &p, "pipeline", 0xabc, true));
    assert(bounded == 2 && full == 0);
    if (argc > 1) assert(strstr(message, "pipeline=abc budgeted=1 redrawn=1 ready=1"));
    assert(mk8_traced_job(NULL, NULL, &p, "compute", 0, false));
    assert(bounded == 2 && full == 1);
    if (argc > 1) assert(strstr(message, "phase=compute ms=1000.000"));
    else assert(!message[0]);
    return 0;
}
''', encoding='utf-8')
            exe = out/'trace.exe'
            env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(ROOT/'local/native-tools/cache'))
            subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu', '-O2',
                            '-UNDEBUG', str(source), '-o', str(exe)], env=env, check=True)
            env.pop('MK8_TRACE_SHADER_WAITS', None)
            subprocess.run([str(exe)], env=env, check=True, timeout=10)
            for disabled in ('0', '-1', 'invalid'):
                subprocess.run([str(exe)], env=dict(env, MK8_TRACE_SHADER_WAITS=disabled), check=True, timeout=10)
            subprocess.run([str(exe), 'trace'], env=dict(env, MK8_TRACE_SHADER_WAITS='10'), check=True, timeout=10)

    def test_rejects_changed_source(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root/'nvn/vulkan').mkdir(parents=True)
            (root/'nvn/vulkan/exec.c').write_text('changed', encoding='utf-8')
            with self.assertRaises(ValueError):
                shader_wait_trace.prepare(root)
