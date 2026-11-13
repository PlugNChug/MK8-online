"""Native top-menu event routing, opt-in bounds, and verified guest adaptation."""
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
from native import DEFAULT_COMPILER, compiler_env
import settings_menu


class SettingsMenuTests(unittest.TestCase):
    def test_guest_hash_and_boundaries_fail_closed(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            original = root / 'main' / settings_menu.UNIT
            original.parent.mkdir()
            original.write_text('changed', encoding='utf-8')
            with self.assertRaises(ValueError):
                settings_menu.patch_guest(original, original, root / 'out')
            with patch.object(settings_menu, 'SOURCE_SHA256', hashlib.sha256(original.read_bytes()).hexdigest()):
                with self.assertRaises(ValueError):
                    settings_menu.patch_guest(original, original, root / 'out')
            self.assertEqual(original.read_text(), 'changed')
            (root / 'ui').mkdir()
            (root / 'ui/menu.mm').write_text('changed', encoding='utf-8')
            with self.assertRaises(ValueError):
                settings_menu.prepare(root)

    @unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
    def test_native_focus_filters_confirm_before_guest_activation_and_queues_once(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            original = root / 'main' / settings_menu.UNIT
            original.parent.mkdir()
            # Synthetic callback body: no commercial guest source in the fixture.
            original.write_text(settings_menu.ENTRY + '\n    body_calls++;\n}\n', encoding='utf-8')
            with patch.object(settings_menu, 'SOURCE_SHA256', hashlib.sha256(original.read_bytes()).hexdigest()):
                adapted = settings_menu.patch_guest(original, original, root / 'out')
            (root / 'rt.h').write_text(r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
extern uint8_t memory[4096];
#define HEAP_BASE ((uint64_t)(uintptr_t)memory)
#define HEAP_MAX 4096ull
#define GUEST_LO HEAP_BASE
#define GUEST_HI (HEAP_BASE + HEAP_MAX)
#define LD32(a) (*(uint32_t *)(uintptr_t)(a))
#define LOG(...) ((void)0)
#define FATAL(...) abort()
#define RT_ON_RESET(name) void name(void)
''', encoding='utf-8')
            source = root / 'test.c'
            (root / 'hle.h').write_text('', encoding='utf-8')
            source.write_text(r'''
#include "rt.h"
#include <assert.h>
#include <string.h>
uint8_t memory[4096];
static unsigned body_calls;
typedef struct { uint64_t x[32]; } Cpu;
bool mk8_settings_take_open(void);
bool mk8_settings_entry_visible(void);
bool mk8_settings_entry_focused(void);
uint64_t mk8_settings_filter_buttons(uint64_t, bool);
void settings_menu_reset(void);
''' + adapted.read_text(encoding='utf-8') + r'''
static void event(Cpu *c, uint32_t id, uint32_t kind) {
    uint32_t *data = (uint32_t *)(uintptr_t)c->x[1];
    data[2] = id; data[10] = kind;
    uint8_t before[4096]; memcpy(before, memory, sizeof memory);
    Cpu saved = *c;
    f_70004d92e8(c);
    assert(!memcmp(&saved, c, sizeof saved) && !memcmp(before, memory, sizeof memory));
}
int main(int argc, char **argv) {
    Cpu c = {{HEAP_BASE, HEAP_BASE + 2048}};
    if (argc > 1 && !strcmp(argv[1], "bad-event")) c.x[1] = GUEST_HI - 8;
    if (argc > 1 && !strcmp(argv[1], "bad-page")) c.x[0] = GUEST_HI - 8;
    if (argc > 1 && !strncmp(argv[1], "bad-", 4)) { f_70004d92e8(&c); return 99; }
    const char *flag = getenv("MK8_SETTINGS_EXPERIMENT");
    if (!flag || strcmp(flag, "1")) {
        event(&c, 105, 2);
        assert(body_calls == 1 && !mk8_settings_take_open() && !mk8_settings_entry_visible());
        assert(mk8_settings_filter_buttons(0x8001, false) == 0x8001);
        return 0;
    }
    assert(!mk8_settings_entry_visible() && !mk8_settings_entry_focused());
    event(&c, 0, 0); assert(body_calls == 1 && mk8_settings_entry_visible());
    event(&c, 10, 2); assert(body_calls == 2 && !mk8_settings_entry_visible());
    event(&c, 105, 0); assert(body_calls == 3 && mk8_settings_entry_visible() && mk8_settings_entry_focused());
    assert(mk8_settings_filter_buttons(0x8001, false) == 0x8000);
    assert(body_calls == 3 && mk8_settings_take_open());
    assert(!mk8_settings_take_open());
    assert(mk8_settings_filter_buttons(1, true) == 0 && !mk8_settings_take_open());
    assert(mk8_settings_filter_buttons(1, false) == 0 && mk8_settings_take_open());
    event(&c, 105, 1); assert(mk8_settings_filter_buttons(1, false) == 1);
    assert(!mk8_settings_entry_focused());
    event(&c, 103, 2); assert(body_calls == 5 && !mk8_settings_entry_visible());
    c.x[1] = GUEST_HI - 0x38; event(&c, 105, 0);
    assert(mk8_settings_filter_buttons(1, false) == 0 && mk8_settings_take_open());
    mk8_settings_filter_buttons(1, false); settings_menu_reset();
    assert(!mk8_settings_take_open() && !mk8_settings_entry_visible());
    return 0;
}
''', encoding='utf-8')
            exe = root / 'test.exe'
            build = subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu',
                                    '-O2', '-UNDEBUG', '-I' + str(root), str(source),
                                    str(ROOT / 'runtime/mk8/support/settings_menu.c'), '-o', str(exe)],
                                   env=compiler_env(), capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr[-3000:])
            env = dict(os.environ)
            env.pop('MK8_SETTINGS_EXPERIMENT', None)
            for value in (None, '0', '1x', '1'):
                current = env if value is None else dict(env, MK8_SETTINGS_EXPERIMENT=value)
                subprocess.run([str(exe)], env=current, check=True, timeout=10)
            for invalid in ('bad-event', 'bad-page'):
                result = subprocess.run([str(exe), invalid], env=dict(env, MK8_SETTINGS_EXPERIMENT='1'),
                                        capture_output=True, timeout=10)
                self.assertNotIn(result.returncode, (0, 99))


if __name__ == '__main__':
    unittest.main()
