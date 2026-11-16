"""Read-only entry tracing (MK8_TRACE_ENTRY): list parsing, the logged line with a memory dump, the per-function limit
and that registers are left alone; and the prepared fnprof.c calls it first."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from host import prepare_host_sources
from native import DEFAULT_COMPILER, compiler_env
import entry_trace

RT_H = r'''
#include <stdint.h>
#include <stdio.h>
#define GUEST_LO 0x1000ull
#define GUEST_HI 0xffffffffffffull
typedef struct { uint64_t x[31]; } Cpu;
Cpu *kernel_current_cpu(void);
extern char logged[128][1024];
extern int nlogged;
#define LOG(...) (nlogged < 128 ? snprintf(logged[nlogged++], 1024, __VA_ARGS__) : 0)
'''
HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"
volatile int g_fnprof;
char logged[128][1024];
int nlogged;
static Cpu cpu;
Cpu *kernel_current_cpu(void) { return &cpu; }
int mk8_trace_entry_parse(const char *spec);
int mk8_trace_entry(uint64_t addr, uint64_t lr);
int main(void) {
    static uint8_t record[8] = {0x10, 0, 0, 0, 0x79, 0x05, 0, 0};  // (a course record: id 16, name label 1401)
    if (mk8_trace_entry_parse("700043c2e8:x1:8,7000418ed0") != 2) return 1;
    if (mk8_trace_entry_parse("zz") || mk8_trace_entry_parse("12") || mk8_trace_entry_parse("700043c2e8:y1:8")
        || mk8_trace_entry_parse("700043c2e8:x1:0") || mk8_trace_entry_parse("700043c2e8:x40:8")) return 2;
    if (mk8_trace_entry_parse("700043c2e8:x1:8,7000418ed0") != 2) return 3;
    cpu.x[0] = 0x2000; cpu.x[1] = (uint64_t)(uintptr_t)record; cpu.x[2] = 7; cpu.x[3] = 9;
    Cpu before = cpu;
    if (mk8_trace_entry(0x700043c2e8ull, 0x700043beefull) != 1) return 4;  // (profiler off: nothing to count)
    if (nlogged != 1 || !strstr(logged[0], "f_700043c2e8 from 700043beef x0=2000") || !strstr(logged[0], "x2=7 x3=9")
        || !strstr(logged[0], "[x1] 10000000 79050000")) { puts(logged[0]); return 5; }
    if (memcmp(&before, &cpu, sizeof cpu)) return 6;  // (read only)
    mk8_trace_entry(0x7000418ed0ull, 0);
    if (nlogged != 2 || strstr(logged[1], "[x")) return 7;  // (no dump asked)
    mk8_trace_entry(0x7000999999ull, 0);
    if (nlogged != 2) return 8;  // (not listed)
    for (int i = 0; i < 100; i++) mk8_trace_entry(0x7000418ed0ull, 0);
    if (nlogged != 2 + 63) return 9;  // (64 entries per function by default)
    cpu.x[1] = 0x10;  // (outside guest memory: says so instead of reading it)
    mk8_trace_entry_parse("700043c2e8:x1:8");
    nlogged = 0;
    mk8_trace_entry(0x700043c2e8ull, 0);
    if (!strstr(logged[0], "fora da memória guest")) return 10;
    cpu.x[1] = (uint64_t)(uintptr_t)record;  // (an offset from the register: x1+4, the name label)
    if (mk8_trace_entry_parse("700043c2e8:x1+4:4") != 1 || mk8_trace_entry_parse("700043c2e8:x1+zz:4")) return 11;
    mk8_trace_entry_parse("700043c2e8:x1+4:4");
    nlogged = 0;
    mk8_trace_entry(0x700043c2e8ull, 0);
    if (nlogged != 1 || !strstr(logged[0], "[x1] 79050000") || strstr(logged[0], "10000000")) { puts(logged[0]); return 12; }
    return 0;
}
'''


class EntryTraceTests(unittest.TestCase):
    def test_boundary_is_checked(self):
        with tempfile.TemporaryDirectory() as folder:
            (Path(folder)/'fnprof.c').write_text('int x;\n', encoding='utf-8')
            with self.assertRaises(ValueError):
                entry_trace.prepare(Path(folder))

    @unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
    def test_entries_are_logged_read_only(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', out/'host')
            fnprof = (runtime/'fnprof.c').read_text(encoding='utf-8')
            start = fnprof.index('void fnprof_hit(uint64_t a, uint64_t lr, uint64_t x0) {')
            self.assertLess(fnprof.index('if (mk8_trace_entry(a, lr)) return;', start), fnprof.index('if (ring_n)', start))
            (out/'inc').mkdir()
            (out/'inc/rt.h').write_text(RT_H, encoding='utf-8')
            (out/'harness.c').write_text(HARNESS, encoding='utf-8')
            exe = out/'harness.exe'
            subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu', '-O1', '-I', str(out/'inc'),
                            str(out/'harness.c'), str(ROOT/'runtime/mk8/support/entry_trace.c'), '-o', str(exe)],
                           env=compiler_env(), check=True, capture_output=True)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout)


if __name__ == '__main__':
    unittest.main()
