"""A raised thread-name exception (0x406D1388) is continued by the runtime's vectored handler; real faults are not."""
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
import thread_name_exception


@unittest.skipUnless(os.name == 'nt' and DEFAULT_COMPILER.is_file(), 'Windows and the native compiler required')
class ThreadNameExceptionTests(unittest.TestCase):
    def test_thread_name_exception_continues(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', out)
            text = (runtime/'platform/windows.c').read_text(encoding='utf-8')
            start = text.index('static uint64_t g_lo, g_hi;')
            end = text.index('\n}\n', text.index('static LONG CALLBACK vectored_handler')) + 3
            source = out/'handler.c'
            source.write_text(r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <windows.h>
static bool rt_fault_watch(uint64_t a, uint64_t pc) { (void)a; (void)pc; return false; }
''' + text[start:end] + r'''
static LONG WINAPI last(EXCEPTION_POINTERS *ep) { ExitProcess(ep->ExceptionRecord->ExceptionCode == 0xC0000005 ? 7 : 9); }
int main(int argc, char **argv) {
    (void)argv;
    AddVectoredExceptionHandler(1, vectored_handler);
    SetUnhandledExceptionFilter(last);
    ULONG_PTR info[4] = {0x1000, (ULONG_PTR)"worker", (ULONG_PTR)-1, 0};
    RaiseException(0x406D1388, 0, 4, info);  /* a thread name: continued */
    if (argc > 1) *(volatile int *)16 = 1;  /* a real fault still reaches the crash filter */
    return 0;
}
''', encoding='utf-8')
            exe = out/'handler.exe'
            env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(ROOT/'local/native-tools/cache'))
            build = subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu', '-O1', str(source), '-o', str(exe)],
                                   env=env, capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr[-3000:])
            self.assertEqual(subprocess.run([str(exe)], timeout=30).returncode, 0)
            self.assertEqual(subprocess.run([str(exe), 'fault'], timeout=30).returncode, 7)

    def test_boundary_is_checked(self):
        with tempfile.TemporaryDirectory() as folder:
            runtime = Path(folder)
            (runtime/'platform').mkdir()
            (runtime/'platform/windows.c').write_text('int x;\n')
            with self.assertRaises(ValueError):
                thread_name_exception.prepare(runtime)


if __name__ == '__main__':
    unittest.main()
