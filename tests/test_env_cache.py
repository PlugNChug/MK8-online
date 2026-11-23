import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
import env_cache  # noqa: E402

SOURCE = '''#include <stdlib.h>
#include <string.h>
static int draw(void) {
    if (getenv("MK8_FSR4") || getenv("MK8_DLSS") || (getenv("MK8_DLSS_FG") && strcmp(getenv("MK8_DLSS_FG"), "0"))) return 1;
    const char *lib = getenv("LARECOMP_GAME_LIB");  // (set by the process while it runs)
    const char *name = "MK8_X"; return getenv(name) != NULL || mygetenv("A") || lib != NULL;
}
'''

# The first form of the patch, as an already prepared tree may still have it.
OLD_FORM = '''#include <stdlib.h>
#include <stdlib.h>
// MK8 (toolchain/env_cache.py): read an option once per call site; the launcher fixes them before the game starts.
#ifndef MK8_ENV
#define MK8_ENV(name) (__extension__({ \\
    static const char *mk8_env_value_; static int mk8_env_read_; \\
    if (!__atomic_load_n(&mk8_env_read_, __ATOMIC_ACQUIRE)) { \\
        mk8_env_value_ = getenv(name); __atomic_store_n(&mk8_env_read_, 1, __ATOMIC_RELEASE); } \\
    mk8_env_value_; }))
#endif

#include <string.h>
static int draw(void) {
    if (MK8_ENV("MK8_FSR4") || MK8_ENV("MK8_DLSS") || (MK8_ENV("MK8_DLSS_FG") && strcmp(MK8_ENV("MK8_DLSS_FG"), "0"))) return 1;
    const char *lib = getenv("LARECOMP_GAME_LIB");  // (set by the process while it runs)
    const char *name = "MK8_X"; return getenv(name) != NULL || mygetenv("A") || lib != NULL;
}
'''


def compiler():
    tools = Path(os.environ.get('MK8_NATIVE_TOOLS', ''))/'zig-x86_64-windows-0.17.0/zig.exe'
    if os.name == 'nt' and tools.is_file():
        return [str(tools), 'cc']
    found = shutil.which('cc') or shutil.which('clang')
    return [found] if found else None


class EnvCacheTests(unittest.TestCase):
    def test_literal_options_are_cached_and_runtime_names_are_not(self):
        text, count = env_cache.rewrite(SOURCE)
        self.assertEqual(count, 4)
        self.assertEqual(text.count(env_cache.READ + 'getenv("MK8_DLSS_FG")'), 2)
        self.assertIn('getenv("MK8_FSR4")', text)  # (still found by searches for the call)
        self.assertNotIn(env_cache.READ + 'getenv("LARECOMP_GAME_LIB")', text)
        self.assertIn('getenv(name)', text)  # (not a literal)
        self.assertIn('mygetenv("A")', text)
        self.assertNotIn('MK8_ENV', text)  # (no macro: a function copied out of the file compiles on its own)
        self.assertEqual(env_cache.rewrite(text), (text, 0))  # (prepare runs on an already prepared tree too)
        self.assertEqual(env_cache.rewrite(OLD_FORM)[0], text)
        self.assertEqual(env_cache.plain(text), SOURCE)
        self.assertEqual(env_cache.rewrite('int f(void) { return 0; }\n'), ('int f(void) { return 0; }\n', 0))

    def test_each_call_site_reads_its_variable_once(self):
        command = compiler()
        if not command:
            self.skipTest('Requires a C compiler')
        program = env_cache.rewrite('''#include <stdio.h>
#include <stdlib.h>
static int calls;
static const char *counted(const char *name) { calls++; return name[0] == 'O' ? "1" : NULL; }
#define getenv counted
int main(void) {
    int on = 0, off = 0;
    for (int i = 0; i < 1000; i++) { on += getenv("ON") != NULL; off += getenv("NO") != NULL; }
    printf("%d %d %d\\n", calls, on, off);
    return 0;
}
''')[0]
        with tempfile.TemporaryDirectory() as folder:
            source, exe = Path(folder)/'env.c', Path(folder)/('env.exe' if os.name == 'nt' else 'env')
            source.write_text(program, encoding='utf-8')
            subprocess.run([*command, '-O2', str(source), '-o', str(exe)], check=True, capture_output=True)
            out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout.split()
        self.assertEqual(out, ['2', '1000', '0'])  # (one read per call site, the cached NULL included)


if __name__ == '__main__':
    unittest.main()
