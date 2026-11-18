"""Exercise the prepared cache writer against real files, including a locked destination."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'toolchain'))
from host import prepare_host_sources
from native import DEFAULT_COMPILER, compiler_env

HARNESS = r'''
#include <windows.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#define VK_SUCCESS 0
#define LOG(...) ((void)0)
static struct { int dev, pcache; } g_vk;
static int fault;
static int vkGetPipelineCacheData(int dev, int cache, size_t *n, void *p) {
    (void)dev; (void)cache;
    if (!p) { *n = 4; return 0; }
    memcpy(p, "new!", 4);
    return fault == 1 ? 5 : 0; // incomplete snapshot must not replace the old file
}
static size_t test_write(const void *p, size_t s, size_t n, FILE *f) {
    return fwrite(p, s, fault == 2 ? n - 1 : n, f);
}
static int test_close(FILE *f) {
    int result = fclose(f);
    return fault == 3 ? EOF : result;
}
#define fwrite test_write
#define fclose test_close
#include "writer.inc"
#undef fwrite
#undef fclose
static bool contents(const char *expected) {
    char buf[5] = {0}; FILE *f = fopen("cache/vk_pipelines.bin", "rb");
    if (!f) return false;
    size_t n = fread(buf, 1, sizeof buf, f); fclose(f);
    return n == 4 && !memcmp(buf, expected, 4);
}
int main(void) {
    mkdir("cache", 0755);
    FILE *f = fopen("cache/vk_pipelines.bin", "wb");
    if (!f) return 1;
    fwrite("old!", 1, 4, f); fclose(f);
    for (fault = 1; fault <= 3; fault++) {
        if (vk_pipeline_cache_save_locked() || !contents("old!")) return 10 + fault;
    }
    fault = 0;
    HANDLE locked = CreateFileA("cache/vk_pipelines.bin", GENERIC_READ, FILE_SHARE_READ,
                                NULL, OPEN_EXISTING, 0, NULL);
    if (locked == INVALID_HANDLE_VALUE) return 20;
    bool ok = vk_pipeline_cache_save_locked();
    CloseHandle(locked);
    if (ok || !contents("old!")) return 21;
    if (!vk_pipeline_cache_save_locked() || !contents("new!")) return 22;
    return 0;
}
'''


class PipelineCacheWriteTests(unittest.TestCase):
    @unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
    def test_failed_writes_preserve_previous_cache_and_retry_replaces_it(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            runtime = prepare_host_sources(ROOT / 'upstream/smm2recomp/recomp', out / 'host')
            text = (runtime / 'nvn/vulkan/device.c').read_text(encoding='utf-8')
            start = text.index('static bool vk_pipeline_cache_save_locked(void) {')
            end = text.index('\n}', start) + 2
            (out / 'writer.inc').write_text(text[start:end], encoding='utf-8')
            (out / 'test.c').write_text(HARNESS, encoding='utf-8')
            exe = out / 'test.exe'
            built = subprocess.run([DEFAULT_COMPILER, 'cc', '-target', 'x86_64-windows-gnu',
                                    '-O1', out / 'test.c', '-o', exe],
                                   capture_output=True, text=True, env=compiler_env())
            self.assertEqual(built.returncode, 0, built.stderr)
            run = subprocess.run([exe], cwd=out, capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stderr)


if __name__ == '__main__':
    unittest.main()
