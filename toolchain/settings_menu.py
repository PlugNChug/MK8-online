"""Opt-in Settings entry experiment for the verified MK8 4.0.1 top menu."""
import hashlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
UNIT = 'main_114.c'
SOURCE_SHA256 = 'ace7dcd1961f46f6213dfe20d2dc534e1854f48401040aa2ae6d6be0f7d960cf'
ENTRY = 'void f_70004d92e8(Cpu *c) {'


def patch_guest(source, original, output):
    if original.parent.name != 'main' or original.name != UNIT:
        return source
    if hashlib.sha256(original.read_bytes()).hexdigest() != SOURCE_SHA256:
        raise ValueError('Settings experiment requires verified MK8 4.0.1 top-menu code')
    text = source.read_text(encoding='utf-8').replace('\r\n', '\n')
    if text.count(ENTRY) != 1:
        raise ValueError('Settings experiment top-menu boundary changed')
    text = text.replace(ENTRY, ENTRY + '\n    mk8_settings_top_event(c->x[0], c->x[1]);')
    text = '#include <stdint.h>\nextern void mk8_settings_top_event(uint64_t, uint64_t);\n' + text
    target = output / 'experimental' / original.name
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(text, encoding='utf-8')
    return target


def replace(text, old, new):
    if text.count(old) != 1:
        raise ValueError('Settings experiment UI boundary changed: ' + old[:80])
    return text.replace(old, new)


def prepare(runtime):
    path = runtime / 'ui/menu.mm'
    text = path.read_text(encoding='utf-8')
    marker = '// ------------------------------------------------------------------ state shared with the main thread'
    own = (ROOT / 'runtime/mk8/ui/settings_entry.inc').read_text(encoding='utf-8')
    text = replace(text, marker, own + '\n' + marker)
    marker = '    bool open = g_open.load();\n    bool kbd = g_kbd.load() == 1;'
    text = replace(text, marker, '    if (mk8_settings_take_open()) set_open(true);\n' + marker)
    marker = '!ov_img && !touchpad_visible() && !notice_active()) {'
    text = replace(text, marker, '!ov_img && !touchpad_visible() && !notice_active() && !mk8_settings_entry_visible()) {')
    marker = '    if (!open && !kbd) touchpad_draw((float)tw, (float)th);'
    text = replace(text, marker, '    if (!open && !kbd) mk8_settings_entry_draw(io.DisplaySize);\n' + marker)
    path.write_text(text, encoding='utf-8')
    path = runtime / 'hle/input.c'
    text = path.read_text(encoding='utf-8')
    marker = '\n    *(uint64_t *)(s + 8) = b;'
    text = replace(text, marker, '''
    extern uint64_t mk8_settings_filter_buttons(uint64_t, bool);
    b = mk8_settings_filter_buttons(b, menu);
''' + marker)
    path.write_text(text, encoding='utf-8')
