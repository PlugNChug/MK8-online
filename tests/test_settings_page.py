"""Execute the native selector controller with a synthetic guest renderer/input."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from native import DEFAULT_COMPILER, compiler_env


class SettingsPageTests(unittest.TestCase):
    @unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
    def test_settings_scrolling_editing_and_animated_safe_return(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root/'hle.h').write_text('')
            (root/'rt.h').write_text(r'''
#pragma once
#include "cpu.h"
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
extern unsigned char memory[32768];
#define HEAP_BASE ((uint64_t)(uintptr_t)memory)
#define HEAP_MAX 32768ull
#define MAIN_BASE 0x7000000000ull
#define GUEST_LO HEAP_BASE
#define GUEST_HI (HEAP_BASE+HEAP_MAX)
#define LOG(...) ((void)0)
#define FATAL(...) do { fprintf(stderr,__VA_ARGS__); abort(); } while(0)
#define RT_ON_RESET(name) void name(void)
uint64_t test_read(uint64_t);
#undef LD64
#define LD64(p) test_read(p)
Cpu *kernel_current_cpu(void);
void guest_call(Cpu *,uint64_t);
uint64_t plat_now_ns(void);
''')
            (root/'test.c').write_text((ROOT/'tests/native_settings_page.c').read_text(encoding='utf-8'),encoding='utf-8')
            exe = root/'test.exe'
            result = subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu', '-O2', '-UNDEBUG',
                                     '-I'+str(root), '-I'+str(ROOT/'upstream/smm2recomp/recomp/runtime'),
                                     '-I'+str(ROOT/'runtime/mk8/support'), str(root/'test.c'),
                                     str(ROOT/'runtime/mk8/support/settings_button.c'), '-o', str(exe)],
                                    env=compiler_env(), capture_output=True, text=True)
            self.assertEqual(result.returncode,0,result.stderr[-3000:])
            subprocess.run([str(exe)],check=True,timeout=10)


if __name__ == '__main__':
    unittest.main()
