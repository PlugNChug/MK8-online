"""Exercise the adapted split-view branch with synthetic race rules and no game data."""
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'toolchain'))
import split_race_cadence as cadence
import experimental
from host import DEPS, prepare_host_sources
from native import DEFAULT_COMPILER, compiler_env


KERNEL = r'''
#pragma once
#include <stdbool.h>
#include <stdint.h>
#define MAIN_BASE 0x7000000000ull
extern uint64_t arena[1024];
#define HEAP_BASE ((uintptr_t)arena)
#define LD32(p) (*(uint32_t *)(uintptr_t)(p))
#define LD8(p) (*(uint8_t *)(uintptr_t)(p))
uint64_t test_load64(uint64_t p);
#define LD64(p) test_load64(p)
uint64_t kernel_heap_size(void);
'''

# Return the interval selected by each branch. Only the adapter markers come
# from the recompiler; the objects and both branch bodies are test fixtures.
HEADER = '''#include "kernel.h"
typedef struct { uint64_t x[32]; bool other_layout, valid_view; } Cpu;
#define COND_NE(c) ((c)->other_layout)
#define COND_HI(c) ((c)->valid_view)
'''
SOURCE = (HEADER + cadence.ENTRY + cadence.BOUNDARY + '''    c->x[0] = 2; return;
L_70003cd688:;
    c->x[0] = 1;
}
''')

# Stock priority is half, full, then the original depth attachment. A fresh
# owner register is needed because the guest reuses x0 before the decision.
BLOOM_SOURCE = (cadence.BLOOM_ENTRY + '''    c->x[0] = 2;
''' + cadence.BLOOM_BOUNDARY + '''    return;
L_70006b5a70:;
    c->x[0] = 1; return;
L_70006b5a78:;
    c->x[0] = 0;
}
''')

HARNESS = r'''
#include <assert.h>
#include <stdlib.h>
#include "settings.h"
const char *const g_tunic_colours[N_TUNIC_COLOURS] = {0};
const char *const g_heart_colours[N_HEART_COLOURS] = {0};
void rt_log(const char *fmt, ...) { (void)fmt; }
int input_pad_family(void) { return 0; }
uint64_t arena[1024], root = MAIN_BASE + 0x133ebd0;
static uint64_t node, context;
uint64_t kernel_heap_size(void) { return sizeof arena; }
uint64_t test_load64(uint64_t p) {
    if (p == MAIN_BASE + 0x1358c70) return root;
    if (p == root + 0x20) return node;
    if (p == root + 0x28) return context;
    assert(p >= HEAP_BASE && p <= HEAP_BASE + sizeof arena - 8);
    return *(uint64_t *)(uintptr_t)p;
}
static unsigned interval(bool other_layout) {
    Cpu c = {.other_layout = other_layout};
    c.x[8] = HEAP_BASE - 0x4230;  // The unmodified branch reads one view pointer.
    f_70003cd5c8(&c);
    return (unsigned)c.x[0];
}
static unsigned bloom_depth(unsigned available, bool valid_view, bool authored, bool has_config) {
    uint64_t owner = HEAP_BASE + 0xa00;
    uint64_t config = HEAP_BASE + 0x800;
    *(uint64_t *)(uintptr_t)(owner + 8) = has_config ? config : 0;
    LD8(config + 0x16f0) = authored;
    Cpu c = {.valid_view = valid_view};
    c.x[0] = owner;
    c.x[10] = available;
    f_70006b5a20(&c);
    return (unsigned)c.x[0];
}
int main(int argc, char **argv) {
    assert(argc == 2);
    settings_load();
    bool enabled = atoi(argv[1]) == 1;
    uint64_t scene = HEAP_BASE + 0x100, config = HEAP_BASE + 0x700, rules = HEAP_BASE + 0x800;
    node = HEAP_BASE;
    context = HEAP_BASE + 0x400;
    arena[1] = scene;
    LD32(scene + 0x184) = 4;
    *(uint64_t *)(uintptr_t)(context + 0x240) = config;
    *(uint64_t *)(uintptr_t)(config + 0x10) = rules;
    for (int kind = 0; kind <= 4; kind += 4) {
        LD32(rules + 8) = kind;
        for (unsigned n = 0; n <= 8; ++n) {
            LD32(rules + 0x188) = kind == 4 ? 3 : n;
            LD32(rules + 0x19c) = kind == 4 ? n : 3;
            assert(interval(false) == (enabled && (n == 3 || n == 4) ? 1 : 2));
            assert(interval(true) == 1);
            assert(bloom_depth(0, true, false, true) == 2);
            assert(bloom_depth(1, true, false, true) == 0);
            assert(bloom_depth(0x800, true, false, true) == 1);
            assert(bloom_depth(0x801, true, false, true) == (enabled && (n == 3 || n == 4) ? 0 : 1));
            assert(bloom_depth(0x801, true, true, true) == 1);
            assert(bloom_depth(0x801, true, false, false) == 1);
            assert(bloom_depth(0x801, false, false, true) == 1);
        }
    }
    LD32(rules + 0x19c) = 4;
    __atomic_store_n(&g_settings.split_screen_60fps,!enabled,__ATOMIC_RELAXED);
    assert(interval(false) == (!enabled ? 1 : 2));
    LD32(scene + 0x184) = 0;
    assert(interval(false) == 2);
    assert(bloom_depth(0x801, true, false, true) == 1);
    LD32(scene + 0x184) = 4;
    *(uint64_t *)(uintptr_t)(config + 0x10) = HEAP_BASE + sizeof arena - 8;
    assert(interval(false) == 2);
    node++;
    assert(interval(false) == 2);
    root = 0;
    assert(interval(false) == 2);
    return 0;
}
'''


class SplitRaceCadenceTests(unittest.TestCase):
    @unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
    def test_compiler_keeps_mii_and_split_view_adaptations_together(self):
        from test_native_cache import compile_build, make_build
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            job = make_build(base, 'job')
            (job / 'runtime/runtime/kernel.h').write_text(KERNEL, encoding='utf-8')
            original = job / 'generated/main' / cadence.UNIT
            mii_entry = 'void f_70003e0ea0(Cpu *c) {'
            contents = SOURCE + mii_entry + '\n    c->x[0] = 7;\n}\n'
            original.write_bytes(contents.replace('\n', '\r\n').encode())
            fixture_hash = hashlib.sha256(original.read_bytes()).hexdigest()
            bloom = original.with_name(cadence.BLOOM_UNIT)
            bloom.write_text(HEADER + BLOOM_SOURCE, encoding='utf-8')
            bloom_hash = hashlib.sha256(bloom.read_bytes()).hexdigest()
            with patch.object(experimental, 'MII_HEAP_SHA256', fixture_hash), patch.object(cadence, 'SHA256', fixture_hash), \
                    patch.object(cadence, 'BLOOM_SHA256', bloom_hash):
                stamps, _ = compile_build(job, base / 'cache')
            adapted = job / 'native-optimized/split_race_cadence' / cadence.UNIT
            self.assertEqual(stamps[original.stem]['input']['source'], hashlib.sha256(adapted.read_bytes()).hexdigest())
            code = adapted.read_text(encoding='utf-8')
            self.assertIn(mii_entry + '\n    if (mk8_no_mii_renderer()) return;', code)
            self.assertIn(cadence.HOOK, code)
            bloom_adapted = adapted.with_name(cadence.BLOOM_UNIT)
            self.assertEqual(stamps[bloom.stem]['input']['source'], hashlib.sha256(bloom_adapted.read_bytes()).hexdigest())

    def test_rejects_wrong_revision_and_moved_branch(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'main' / cadence.UNIT
            path.parent.mkdir()
            path.write_text(SOURCE, encoding='utf-8')
            out = Path(folder) / 'out'
            with self.assertRaises(ValueError):
                cadence.patch_unit(path, out)
            self.assertFalse(out.exists())
            for source in (SOURCE.replace('L_70003cd688:;', ''), SOURCE + cadence.BOUNDARY,
                           SOURCE.replace(cadence.ENTRY, 'void unrelated(Cpu *c) {\n')):
                with self.assertRaises(ValueError):
                    cadence.patch(source)
            light = path.with_name(cadence.BLOOM_UNIT)
            light.write_text(BLOOM_SOURCE, encoding='utf-8')
            with self.assertRaises(ValueError):
                cadence.patch_unit(light, out)
            for source in (BLOOM_SOURCE.replace('L_70006b5a78:;', ''), BLOOM_SOURCE + cadence.BLOOM_BOUNDARY,
                           BLOOM_SOURCE.replace(cadence.BLOOM_ENTRY, 'void unrelated(Cpu *c) {\n')):
                with self.assertRaises(ValueError):
                    cadence.patch(source, cadence.BLOOM_UNIT)

    @unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
    def test_native_rate_selection(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            runtime = prepare_host_sources(ROOT / 'upstream/smm2recomp/recomp', out / 'host')
            (out / 'kernel.h').write_text(KERNEL, encoding='utf-8')
            source = out / 'test.c'
            source.write_text(cadence.patch(SOURCE) + cadence.patch(BLOOM_SOURCE, cadence.BLOOM_UNIT) + HARNESS, encoding='utf-8')
            exe = out / 'test.exe'
            subprocess.run([DEFAULT_COMPILER, 'cc', '-target', 'x86_64-windows-gnu', '-O1', '-UNDEBUG',
                            '-I' + str(out), '-I' + str(runtime), '-I' + str(DEPS / 'include'),
                            source, ROOT / 'runtime/mk8/graphics/split_screen.c', runtime / 'settings.c',
                            runtime / 'hle/bindings.c', DEPS / 'lib/libSDL3.dll.a',
                            '-o', exe], env=compiler_env(), check=True)
            ini = out / 'settings.ini'
            for value, setting, enabled in ((None, None, 0), ('1', None, 1), ('10', None, 0),
                                            ('0', '1', 1), ('1', '0', 0), ('1', 'invalid', 0)):
                with self.subTest(legacy=value, setting=setting):
                    if setting is not None:
                        ini.write_text(f'split_screen_60fps = {setting}\n')
                    env = dict(os.environ, LARECOMP_SETTINGS=str(ini),
                               PATH=str(DEPS / 'bin') + os.pathsep + os.environ['PATH'])
                    env.pop('MK8_SPLITSCREEN_60FPS', None)
                    if value is not None:
                        env['MK8_SPLITSCREEN_60FPS'] = value
                    subprocess.run([exe, str(enabled)], env=env, check=True, timeout=15)
