"""Native top-menu event routing, opt-in bounds, and verified guest adaptation."""
import hashlib
import os
import re
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
    @unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
    def test_light_palette_has_readable_text_and_selected_rows(self):
        menu = (ROOT / 'upstream/smm2recomp/recomp/runtime/ui/menu.mm').read_text(encoding='utf-8')
        palette = re.search(r'typedef struct \{\n    ImVec4 window, child, popup;.*?\} ThemePalette;', menu, re.S)[0]
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / 'palette.cpp'
            source.write_text('#include "imgui.h"\n#include <math.h>\n#include <assert.h>\n'
                              'static bool hub, native_settings;\n'
                              'static bool mk8_hub_enabled() { return hub; }\n'
                              'extern "C" bool mk8_settings_style_active() { return native_settings; }\n' + palette + '\n' +
                              (ROOT / 'runtime/mk8/ui/settings_theme.inc').read_text(encoding='utf-8') + r'''
static float channel(float v) { return v <= .04045f ? v/12.92f : powf((v+.055f)/1.055f,2.4f); }
static float light(ImVec4 v) { return .2126f*channel(v.x)+.7152f*channel(v.y)+.0722f*channel(v.z); }
static float contrast(ImVec4 a, ImVec4 b) { float x=light(a),y=light(b); return (fmaxf(x,y)+.05f)/(fminf(x,y)+.05f); }
int main() {
    for (int flags = 0; flags < 4; flags++) {
        hub = flags & 1; native_settings = flags & 2;
        assert(mk8_settings_overlay_style_active() == (native_settings && !hub));
    }
    ThemePalette base={}; const ThemePalette &p=mk8_settings_palette(base);
    assert(contrast(p.text,p.window)>4.5f && contrast(p.text,p.hover)>4.5f);
    assert(contrast(p.text_dim,p.child)>4.5f && contrast(p.accent,p.window)>4.5f);
    assert(p.hover.x>.9f && p.hover.y>.7f && p.hover.z<.2f);
    assert(!p.row_notch && p.window_round>0);
}
''', encoding='utf-8')
            exe = root / 'palette.exe'
            build = subprocess.run([str(DEFAULT_COMPILER), 'c++', '-target', 'x86_64-windows-gnu', '-O2', '-UNDEBUG',
                                    '-I'+str(ROOT / 'upstream/smm2recomp/recomp/third_party/imgui'),
                                    str(source), '-o', str(exe)], env=compiler_env(), capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr[-3000:])
            subprocess.run([str(exe)], check=True, timeout=10)

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
    def test_native_button_routes_only_its_own_events_and_preserves_original_actions(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'rt.h').write_text(r"""
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
#define LD8(a) (*(uint8_t *)(uintptr_t)(a))
#define LOG(...) ((void)0)
#define FATAL(...) abort()
#define RT_ON_RESET(name) void name(void)
bool plat_exe_path(char *, size_t);
""", encoding='utf-8')
            (root / 'hle.h').write_text('', encoding='utf-8')
            source = root / 'test.c'
            source.write_text(r"""
#include "rt.h"
#include <assert.h>
#include <string.h>
uint8_t memory[4096];
static int constructions, openings, hovers;
bool mk8_settings_top_event(uint64_t, uint64_t);
uint64_t mk8_settings_navigation(uint64_t, uint64_t, uint64_t);
void settings_menu_reset(void);
void mk8_settings_button_build(uint64_t page) { assert(page==HEAP_BASE); constructions++; }
void mk8_settings_button_hover(uint64_t page, unsigned instant) { assert(page==HEAP_BASE && !instant); hovers++; }
void mk8_settings_page_build(uint64_t page) { assert(page==HEAP_BASE); }
void mk8_settings_page_tick(uint64_t page, bool open) { assert(page==HEAP_BASE); if(open) openings++; }
bool plat_exe_path(char *p, size_t n) { return false; }
int main(int argc, char **argv) {
    uint64_t page=HEAP_BASE, event=HEAP_BASE+2048;
    if (argc>1 && !strcmp(argv[1],"bad-page")) page=GUEST_HI-8;
    if (argc>1 && !strcmp(argv[1],"bad-event")) event=GUEST_HI-8;
    if (argc>1) { mk8_settings_top_event(page,event); return 99; }
    const char *flag=getenv("MK8_SETTINGS_EXPERIMENT");
    bool on=flag && !strcmp(flag,"1");
    assert(!mk8_settings_top_event(page,0));
    assert(constructions==(int)on);
    const uint64_t handled=1ull<<32, owner=page+0x420;
    assert(mk8_settings_navigation(owner,9,0x100)==(on ? handled|11 : 0));
    assert(mk8_settings_navigation(owner,10,0x80)==(on ? handled|11 : 0));
    assert(mk8_settings_navigation(owner,11,0x80)==(on ? handled|9 : 0));
    assert(mk8_settings_navigation(owner,11,0x100)==(on ? handled|10 : 0));
    assert(mk8_settings_navigation(owner,11,0x20)==(on ? handled|3 : 0));
    assert(mk8_settings_navigation(owner,11,0x40)==(on ? handled : 0));
    assert(!mk8_settings_navigation(owner,4,0x100));
    assert(!mk8_settings_navigation(owner,9,0x120));
    assert(!mk8_settings_navigation(owner+8,9,0x100));
    assert(!mk8_settings_top_event(page,1));
    assert(!openings);
    for (unsigned id=0; id<=107; id++) for(unsigned kind=0;kind<=2;kind++) {
        *(uint32_t *)(uintptr_t)(event+8)=id;
        *(uint32_t *)(uintptr_t)(event+0x28)=kind;
        uint8_t before[4096]; memcpy(before,memory,sizeof memory);
        bool ours=on && id==107;
        int before_hover=hovers;
        assert(mk8_settings_top_event(page,event)==ours);
        assert(hovers==before_hover+(ours && kind==0));
        assert(!memcmp(before,memory,sizeof memory));
        int before_open=openings;
        assert(!mk8_settings_top_event(page,1));
        assert(openings==before_open+(ours && kind==2));
        assert(!mk8_settings_top_event(page,1));
        assert(openings==before_open+(ours && kind==2));
    }
    settings_menu_reset();
    assert(!mk8_settings_navigation(owner,9,0x100));
    return 0;
}
""", encoding='utf-8')
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
