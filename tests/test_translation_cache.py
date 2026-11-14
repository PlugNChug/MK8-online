"""Maxwell -> GLSL translations stored on disk come back identical, only for the same inputs, and never damaged.

Runs the real translator (prepared translate.c) and runtime/mk8/graphics/translation_cache.c in a native harness,
in several processes over the same data folder (cache/xlate.pack)."""
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
import translation_cache

HARNESS = r'''
#include "rt.h"
#include "maxwell/shader.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

ShaderInfo *mk8_translate_cached(const uint8_t *blob, size_t max_size, int stage, const ShaderKey *key);
size_t mk8_translate_extent(const uint8_t *blob, size_t max_size, int stage);
size_t mk8_translate_cache_entries(void);
long mk8_translate_cache_record(const uint8_t *blob, size_t max_size, int stage, const ShaderKey *key);
void mk8_translate_cache_counts(uint64_t *read, uint64_t *made);
void rt_log(const char *fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); }
bool plat_exe_path(char *buf, size_t size) { snprintf(buf, size, "stamp.bin"); return true; }
uint64_t plat_now_ns(void) { struct timespec t; timespec_get(&t, TIME_UTC); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }

static int same(const ShaderInfo *a, const ShaderInfo *b) {  // (every field; strings by content)
    ShaderInfo x = *a, y = *b;
    if (!a->msl || !b->msl || strcmp(a->msl, b->msl)) return 0;
    x.msl = y.msl = x.gs_params = y.gs_params = x.vs_entry_params = y.vs_entry_params = x.vs_call_args = y.vs_call_args = NULL;
    return !memcmp(&x, &y, sizeof x);
}
static void flip(long offset) {  // (one byte of the pack, from outside)
    FILE *f = fopen("cache/xlate.pack", "r+b");
    fseek(f, offset, SEEK_SET);
    int c = fgetc(f);
    fseek(f, offset, SEEK_SET);
    fputc(c ^ 0x20, f);
    fclose(f);
}
#define CHECK(c, code) do { if (!(c)) { printf("check failed: %s\n", #c); return code; } } while (0)

static uint64_t words[32];
static uint8_t *blob = (uint8_t *)words;
static ShaderKey key, flipped;
static ShaderInfo *xlate(const ShaderKey *k) { return mk8_translate_cached(blob, sizeof words, STAGE_FRAGMENT, k); }
static ShaderInfo *direct(const ShaderKey *k) { return maxwell_translate(blob, sizeof words, STAGE_FRAGMENT, k); }

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "first";
    *(uint32_t *)(blob + 0x78) = 15;     // (fragment SPH: colour target 0 written)
    words[17] = 0x50b0000000070f00ull;   // NOP
    words[18] = 0xe30000000007000full;   // EXIT
    words[19] = 0xe2400fffff87000full;   // BRA to itself: the end of the code (word 16 schedules the group)
    key.glsl = 1;
    flipped = key;
    flipped.flip_y = 1;
    maxwell_gmem_base = 0x7200000000ull, maxwell_gmem_size = 0xD0000000ull;
    uint64_t read, made;
    if (!strcmp(mode, "count")) return 100 + (int)mk8_translate_cache_entries();
    if (!strcmp(mode, "plain")) {  // (off or bypassed: translated, nothing kept)
        CHECK(same(xlate(&key), direct(&key)), 40);
        CHECK(same(xlate(&key), direct(&key)), 41);
        mk8_translate_cache_counts(&read, &made);
        CHECK(read == 0 && made == 0 && mk8_translate_cache_entries() == 0, 42);
        return 0;
    }
    if (!strcmp(mode, "again")) {  // (a later session of the same runtime: everything stored comes back)
        CHECK(mk8_translate_cache_entries() == 4, 50);
        CHECK(same(xlate(&key), direct(&key)) && same(xlate(&flipped), direct(&flipped)), 51);
        mk8_translate_cache_counts(&read, &made);
        CHECK(read == 2 && made == 0, 52);
        ShaderKey other = key;
        other.depth_minus_one = 1;
        CHECK(same(xlate(&other), direct(&other)) && mk8_translate_cache_entries() == 5, 53);
        return 100 + (int)mk8_translate_cache_entries();
    }
    // the extent: header + code up to the end scan's stop, the self-branch (code word 3) included
    CHECK(mk8_translate_extent(blob, sizeof words, STAGE_FRAGMENT) == 0x80 + 4 * 8, 1);
    ShaderInfo *want = direct(&key);
    CHECK(want && want->msl, 2);
    CHECK(mk8_translate_cache_entries() == 0, 3);
    ShaderInfo *made1 = xlate(&key);
    CHECK(same(made1, want) && mk8_translate_cache_entries() == 1, 4);
    ShaderInfo *again = xlate(&key);
    mk8_translate_cache_counts(&read, &made);
    CHECK(again != made1 && same(again, want) && read == 1 && made == 1, 5);
    // past the extent: the translator never reads it, the same record serves
    words[28] = 0x1234567812345678ull;
    CHECK(same(direct(&key), want) && same(xlate(&key), want) && mk8_translate_cache_entries() == 1, 6);
    // another key, other code or a translator global: a record each, each the same as translating
    CHECK(same(xlate(&flipped), direct(&flipped)) && mk8_translate_cache_entries() == 2, 7);
    *(uint32_t *)(blob + 0x78) = 3;
    CHECK(same(xlate(&key), direct(&key)) && mk8_translate_cache_entries() == 3, 8);
    *(uint32_t *)(blob + 0x78) = 15;
    maxwell_glsl_bda = !maxwell_glsl_bda;
    CHECK(same(xlate(&key), direct(&key)) && mk8_translate_cache_entries() == 4, 9);
    maxwell_glsl_bda = !maxwell_glsl_bda;
    // a damaged record is never served: the key's second hash, the GLSL text, a field of the stored ShaderInfo
    // (each time the record in use: a damaged one is replaced by a new record)
    for (int i = 0; i < 3; i++) {
        long at = mk8_translate_cache_record(blob, sizeof words, STAGE_FRAGMENT, &key);
        CHECK(at > 0, 10);
        flip(i == 0 ? at + 16 : i == 1 ? at + 48 + (long)sizeof(ShaderInfo) + 10 : at + 48 + 4);
        uint64_t r0, m0;
        mk8_translate_cache_counts(&r0, &m0);
        CHECK(same(xlate(&key), want), 11);
        mk8_translate_cache_counts(&read, &made);
        CHECK(read == r0 && made == m0 + 1, 12);  // (translated again, stored as a new record)
        CHECK(same(xlate(&key), want), 13);       // (and that one serves)
        mk8_translate_cache_counts(&read, &made);
        CHECK(read == r0 + 1, 14);
    }
    CHECK(mk8_translate_cache_entries() == 4, 15);
    return 100 + (int)mk8_translate_cache_entries();
}
'''


class TranslationCacheTests(unittest.TestCase):
    def test_boundaries_are_checked(self):
        with tempfile.TemporaryDirectory() as folder:
            execute = Path(folder)/'nvn/vulkan/exec.c'
            execute.parent.mkdir(parents=True)
            execute.write_text('static pthread_mutex_t translate_lock = PTHREAD_MUTEX_INITIALIZER;\n'
                               'x = maxwell_translate(a);\n', encoding='utf-8')
            with self.assertRaises(ValueError):
                translation_cache.prepare(Path(folder))

    @unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
    def test_translations_come_back_identical_and_only_for_the_same_inputs(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', out/'host')
            execute = (runtime/'nvn/vulkan/exec.c').read_text(encoding='utf-8')
            self.assertEqual(execute.count('= mk8_translate_cached('), 4)
            self.assertNotIn('= maxwell_translate(', execute)
            harness = out/'harness.c'
            harness.write_text(HARNESS, encoding='utf-8')
            exe = out/'harness.exe'
            subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu', '-O1', '-UNDEBUG',
                            '-I'+str(runtime), '-I'+str(runtime/'nvn'), str(harness),
                            str(runtime/'nvn/maxwell/translate.c'), str(ROOT/'runtime/mk8/graphics/translation_cache.c'),
                            '-o', str(exe)], env=compiler_env(), check=True, capture_output=True)
            env = {k: v for k, v in os.environ.items() if not k.startswith(('MK8_', 'LARECOMP_'))}

            def run(folder, *args, extra=None):
                return subprocess.run([str(exe), *args], cwd=folder, env={**env, **(extra or {})},
                                      capture_output=True, text=True, timeout=120)

            work = out/'data'
            work.mkdir()
            (work/'stamp.bin').write_bytes(b'runtime')
            first = run(work)
            self.assertEqual(first.returncode, 104, first.stdout)
            self.assertIn('[mk8:xlate] translation cache on: 0 translations', first.stdout)
            pack = work/'cache/xlate.pack'
            # a session cut short while writing: garbage after the last record is ignored and written over
            with pack.open('ab') as f:
                f.write(b'XREC' + bytes(60))
            again = run(work, 'again')
            self.assertEqual(again.returncode, 105, again.stdout)
            self.assertIn('bytes after the last whole record are written over', again.stdout)
            self.assertEqual(run(work, 'count').returncode, 105)
            # another runtime build: a new pack, nothing of the old one served
            (work/'stamp.bin').write_bytes(b'runtime, rebuilt')
            self.assertEqual(run(work, 'count').returncode, 100)
            # off, and a debug variable that changes the output: translated, nothing stored
            for name, extra, text in (('off', {'MK8_TRANSLATION_CACHE': '0'}, 'translation cache off'),
                                      ('debug', {'LARECOMP_NO_BRA_IF': '1'}, 'translation cache on')):
                folder = out/name
                folder.mkdir()
                (folder/'stamp.bin').write_bytes(b'runtime')
                result = run(folder, 'plain', extra=extra)
                self.assertEqual(result.returncode, 0, result.stdout)
                self.assertIn(text, result.stdout)


if __name__ == '__main__':
    unittest.main()
