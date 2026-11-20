"""Exercise the native runtime's Windows encoding with the same compiler and manifest."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'toolchain'))
from native import DEFAULT_COMPILER, compiler_env


@unittest.skipUnless(os.name == 'nt' and DEFAULT_COMPILER.is_file(), 'Windows runtime compiler required')
class RuntimeWindowsPathsTest(unittest.TestCase):
    def test_unicode_data_directory_and_game_argument(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            game = root / 'Corrida João 東京'
            game.mkdir()
            source = root / 'paths.c'
            source.write_text(r'''
#include <direct.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
int main(int argc, char **argv) {
    const char *root = getenv("LARECOMP_DATA_ROOT");
    if (argc != 2 || !root || GetACP() != CP_UTF8 || strcmp(root, argv[1])) return 1;
    if (_chdir(root)) return 2;
    FILE *f = fopen("save-test", "wb");
    if (!f) return 3;
    return fputs("ok", f) < 0 || fclose(f) != 0;
}
''', encoding='ascii')
            exe = root / 'paths.exe'
            subprocess.run([DEFAULT_COMPILER, 'cc', '-target', 'x86_64-windows-gnu', source,
                            ROOT / 'runtime/mk8/windows.rc', '-o', exe],
                           env=compiler_env(), capture_output=True, check=True, timeout=150)
            subprocess.run([exe, game], env=dict(os.environ, LARECOMP_DATA_ROOT=str(game)),
                           cwd=root, check=True, timeout=20)
            self.assertEqual((game / 'save-test').read_bytes(), b'ok')
