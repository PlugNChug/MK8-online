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
    def test_invalid_destination_is_reported_before_creating_the_build(self):
        from build import validate_romfs_paths
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            src = root/'input'
            (src/'Course').mkdir(parents=True)
            (src/'Course/example').write_bytes(b'track')
            dest = root/'build'
            for _ in range(136):
                dest /= 'p' * 250
            with self.assertRaisesRegex(ValueError, 'caminho estendido'):
                validate_romfs_paths(src, dest)
            self.assertFalse((root/'build').exists())

    def test_index_and_guest_storage_read_long_romfs_and_overlay(self):
        from build import filesystem_path, mirror, validate_romfs_paths
        from host import prepare_host_sources
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', root/'host')
            fs = (runtime/'services/fs.c').read_text(encoding='utf-8')
            reader = fs[fs.index('static ssize_t rix_read('):fs.index('RT_ON_RESET(rix_reset)')]
            windows = (runtime/'platform/windows.c').read_text(encoding='utf-8')
            pread = windows[windows.index('static ssize_t prw('):windows.index('ssize_t pwrite(')]
            c = root/'read.c'
            c.write_text('''#include "mk8_romfs_paths.h"
#include "platform.h"
#include "romfs_build.h"
#include <assert.h>
#define LOG(...) ((void)0)
''' + pread + reader + r'''
int main(int argc, char **argv) {
    RomfsIndex ix;
    char why[512] = "";
    assert(argc == 4 && romfs_index(argv[1], argv[2], &ix, why, sizeof why));
    assert(ix.nfiles == 6);
    int seen = 0;
    int empty = 0;
    for (int i = 0; i < ix.nfiles; i++) {
        RomfsIndexFile *f = &ix.files[i];
        if (!strcmp(f->path, "empty")) {
            assert(f->size == 0);
            empty++;
            continue;
        }
        assert(f->size == 2);
        uint8_t data[2];
        assert(rix_read(&ix, data, 2, f->off) == 2 && data[0] == 0xff);
        assert(data[1] < 5 && !(seen & (1 << data[1])));
        seen |= 1 << data[1];
        close(f->fd);
        f->fd = -1;
        if (!strcmp(f->path, "short")) {
            assert(data[1] == 4);  // The overlay replaced the short base file.
            wchar_t *wide = mk8_romfs_path(f->host);
            assert(wide);
            FILE *truncated = _wfopen(wide, L"wb");
            assert(truncated && fputc(0xff, truncated) == 0xff && fclose(truncated) == 0);
            assert(rix_read(&ix, data, 2, f->off) < 0);
            close(f->fd);
            f->fd = -1;
            assert(DeleteFileW(wide));
            free(wide);
            assert(rix_read(&ix, data, 2, f->off) < 0);
        }
    }
    assert(seen == 31 && empty == 1);
    // Check UNC normalization without depending on a network share.
    wchar_t *unc = mk8_romfs_path("//example/share/dir/../asset");
    assert(unc && !wcscmp(unc, L"\\\\?\\UNC\\example\\share\\asset"));
    free(unc);
    RomfsIndex missing;
    assert(!romfs_index(argv[3], NULL, &missing, why, sizeof why));
}
''', encoding='utf-8')
            exe = root/'read.exe'
            subprocess.run([DEFAULT_COMPILER, 'cc', '-target', 'x86_64-windows-gnu',
                            '-D_FILE_OFFSET_BITS=64', '-UNDEBUG', '-I'+str(runtime), c,
                            runtime/'romfs_build.c', ROOT/'runtime/mk8/windows.rc', '-o', exe],
                           check=True, env=compiler_env(), timeout=120)
            src = root/'source'
            dest = root/'game-data/0123456789abcdef/extracted/romfs'
            sizes = [259, 260]
            names = ['Course/'+'a'*(n-len(str(dest/'Course'))-1) for n in sizes]
            names += ['Course/' + 'é' * 50 + '/' + '東京🏁' * 45 + '/asset',
                      'Course/' + 'LongFolder/' * 20 + 'asset', 'short']
            for i, name in enumerate(names):
                path = filesystem_path(src/name)
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(bytes((255, i if i != 4 else 0)))
            (src/'empty').touch()
            overlay = root/('Overlay-'+'z'*90)/('Mod-'+'x'*90)/'romfs'
            target = filesystem_path(overlay/'short')
            target.parent.mkdir(parents=True)
            target.write_bytes(bytes((255, 4)))
            validate_romfs_paths(src, dest)
            mirror(dest, src)
            self.assertTrue(os.path.samefile(filesystem_path(src/names[2]), filesystem_path(dest/names[2])))
            for romfs in (str(dest), str(filesystem_path(dest)), os.path.relpath(dest, root)):
                target.write_bytes(bytes((255, 4)))
                subprocess.run([exe, romfs, str(overlay), str(root/'missing')], cwd=root, check=True, timeout=20)

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
