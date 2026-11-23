import os
from pathlib import Path
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


class EnvCacheTests(unittest.TestCase):
    def test_literal_options_are_cached_and_runtime_names_are_not(self):
        text, count = env_cache.rewrite(SOURCE)
        self.assertEqual(count, 4)
        self.assertIn('MK8_ENV("MK8_FSR4") || MK8_ENV("MK8_DLSS")', text)
        self.assertIn('strcmp(MK8_ENV("MK8_DLSS_FG"), "0")', text)
        self.assertIn('getenv("LARECOMP_GAME_LIB")', text)
        self.assertIn('getenv(name)', text)  # (not a literal)
        self.assertIn('mygetenv("A")', text)
        self.assertEqual(text.count(env_cache.MARKER), 1)
        # After the first #include: code can sit between later ones.
        self.assertLess(text.index(env_cache.MARKER), text.index('#include <string.h>'))
        self.assertEqual(env_cache.rewrite(text), (text, 0))  # (prepare runs on an already prepared tree too)
        old = SOURCE.replace('#include <string.h>\n', '#include <string.h>\n' + env_cache.MACRO + '\n')
        moved, _ = env_cache.rewrite(old.replace('getenv("MK8_FSR4")', 'MK8_ENV("MK8_FSR4")'))
        self.assertEqual(moved, text)  # (a tree prepared with the earlier placement)
        self.assertEqual(env_cache.rewrite('int f(void) { return 0; }\n'), ('int f(void) { return 0; }\n', 0))

    def test_each_call_site_reads_its_variable_once(self):
        compiler = Path(os.environ.get('MK8_NATIVE_TOOLS', ''))/'zig-x86_64-windows-0.17.0/zig.exe'
        if os.name != 'nt' or not compiler.is_file():
            import shutil
            found = shutil.which('cc') or shutil.which('clang')
            if not found:
                self.skipTest('Requires a C compiler')
            command = [found]
        else:
            command = [str(compiler), 'cc']
        program = '''#include <stdio.h>
#include <stdlib.h>
static int calls;
static const char *counted(const char *name) { calls++; return name[0] == 'O' ? "1" : NULL; }
#define getenv counted
''' + env_cache.MACRO + '''
int main(void) {
    int on = 0, off = 0;
    for (int i = 0; i < 1000; i++) { on += MK8_ENV("ON") != NULL; off += MK8_ENV("NO") != NULL; }
    printf("%d %d %d\\n", calls, on, off);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as folder:
            source, exe = Path(folder)/'env.c', Path(folder)/('env.exe' if os.name == 'nt' else 'env')
            source.write_text(program, encoding='utf-8')
            subprocess.run([*command, '-O2', str(source), '-o', str(exe)], check=True, capture_output=True)
            out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout.split()
        self.assertEqual(out, ['2', '1000', '0'])  # (one read per call site, the cached NULL included)


if __name__ == '__main__':
    unittest.main()
