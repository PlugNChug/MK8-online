"""Opt-in Settings entry experiment for the verified MK8 4.0.1 top menu."""
import hashlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
UNIT = 'main_114.c'
SOURCE_SHA256 = 'ace7dcd1961f46f6213dfe20d2dc534e1854f48401040aa2ae6d6be0f7d960cf'
ENTRY = 'void f_70004d92e8(Cpu *c) {'
BUILD_SHA256 = '4b4cf4a1aec1b9d1d0479a920a3fbf46fe518d23d7624a19caea00808f7d88f7'
NAV_SHA256 = 'c6d46af3e71b558fa38894d3b26de9569671c93a026f7a0cce99eb2e16707082'
TICK_SHA256 = 'e7f979c9bae346fca7da9f76349d27eeec5fcc4b23c87b43d396c39b605ef048'
SHARED_HASHES = {'main_100.c': '187122fd53c501e91020efb776088154977181fd647ba02cf084d1e9d4c518cc',
                 'main_191.c': 'f61eefe25d08a209796ba4519ea8aa491121f2f85089715fe63fbb7fa029d133'}


def patch_guest(source, original, output):
    if original.parent.name != 'main' or original.name not in (UNIT, 'main_113.c', 'main_082.c', 'main_115.c', *SHARED_HASHES):
        return source
    expected = {UNIT: SOURCE_SHA256, 'main_113.c': BUILD_SHA256, 'main_082.c': NAV_SHA256, 'main_115.c': TICK_SHA256, **SHARED_HASHES}[original.name]
    if hashlib.sha256(original.read_bytes()).hexdigest() != expected:
        raise ValueError('Settings experiment requires verified MK8 4.0.1 top-menu code')
    text = source.read_text(encoding='utf-8').replace('\r\n', '\n')
    if original.name in SHARED_HASHES:
        function, marker, hook = ('f_700049a8fc', '    { uint64_t ea = c->x[19] + 0x1f0ull; ST32(ea, 0u); }', 'mk8_settings_shared_build') if original.name == 'main_100.c' else (
            'f_70008bd6d8', '    { uint64_t ea = c->sp + 352; uint64_t a = LD64(ea)', 'mk8_settings_page_frame')
        start = text.index('void '+function+'(Cpu *c) {')
        end = text.index('\n}', start)
        body = text[start:end]
        if body.count(marker) != 1:
            raise ValueError('Settings shared UI boundary changed')
        at = body.index(marker)
        if original.name == 'main_100.c':
            at += len(marker)
        body = body[:at]+'\n    '+hook+'(c->x[19]);\n'+body[at:]
        text = text[:start]+body+text[end:]
    elif original.name == UNIT:
        text = replace(text, ENTRY, ENTRY + '\n    if (mk8_settings_top_event(c->x[0], c->x[1])) return;')
    elif original.name == 'main_115.c':
        marker = 'void f_70004da71c(Cpu *c) {'
        text = replace(text, marker, marker + '\n    mk8_settings_top_event(c->x[0], 1);')
    elif original.name == 'main_113.c':
        marker = '    c->x[30] = 0x70004d8334ull; f_70008af1c4(c);'
        text = replace(text, marker, '    mk8_settings_top_event(c->x[19], 0);\n' + marker)
    else:
        marker = 'void f_70003fc4a8(Cpu *c) {'
        text = replace(text, marker, marker + '''
    uint64_t mk8_move = mk8_settings_navigation(c->x[0], c->x[1], c->x[2]);
    if (mk8_move >> 32) { c->x[0] = (uint32_t)mk8_move; return; }
''')
    text = ('#include <stdint.h>\n#include <stdbool.h>\nextern bool mk8_settings_top_event(uint64_t, uint64_t);\n'
            'extern void mk8_settings_shared_build(uint64_t);\nextern void mk8_settings_page_frame(uint64_t);\n'
            'extern uint64_t mk8_settings_navigation(uint64_t, uint64_t, uint64_t);\n') + text
    target = output / 'experimental' / original.name
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(text, encoding='utf-8')
    return target


def replace(text, old, new):
    if text.count(old) != 1:
        raise ValueError('Settings experiment UI boundary changed: ' + old[:80])
    return text.replace(old, new)


def prepare(runtime):
    path = runtime / 'nvn/vulkan/device.c'
    if path.is_file():
        text = path.read_text(encoding='utf-8')
        text = replace(text, '        while (SDL_WaitEventTimeout(&e, 16)) {',
                       '        // Continuous controller events must not starve window/input requests.\n'
                       '        unsigned events = 0;\n'
                       '        while (events++ < 64 && SDL_WaitEventTimeout(&e, events == 1 ? 16 : 0)) {')
        path.write_text(text, encoding='utf-8')
    path = runtime / 'hle/input.c'
    if path.is_file():
        text = path.read_text(encoding='utf-8')
        text = '#include <stdbool.h>\nextern bool mk8_settings_page_active(void);\n' + text
        scan = '''    if (!scanned) {
        scanned = true;
        int n = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&n);
        if (n > 0 && !pad && !pads_from_p2() && real_gamepad(ids[0])) { pad = SDL_OpenGamepad(ids[0]); LOG("[input] gamepad: %s\\n", SDL_GetGamepadName(pad)); note_family(); }
        for (int i = pad ? 1 : 0; i < n; i++) open_extra(ids[i]);
        SDL_free(ids);
    }'''
        text = replace(text, scan, '    scan_gamepads();')
        text = replace(text, '// The setting changed: every controller let go',
                       'static void scan_gamepads(void) {\n' + scan + '\n}\n\n// The setting changed: every controller let go')
        text = replace(text, '    scanned = false;  // (taken again on the next event)',
                       '    scanned = false;\n    scan_gamepads();  // Reopen now so SDL keeps delivering button releases.')
        text = replace(text, '    if (test_event(e)) return false;',
                       '    if (!e) return true;  // Query the physical-input policy without an event.\n    if (test_event(e)) return false;')
        text = replace(text, '        bool menu = menu_input_captured();',
                       '        bool menu = menu_input_captured() || mk8_settings_page_active();')
        text = replace(text, '*(uint64_t *)(s + 8) = padscript_buttons();',
                       '*(uint64_t *)(s + 8) = mk8_settings_page_active() ? 0 : padscript_buttons();')
        marker = '\n    *(uint64_t *)(s + 8) = b;'
        text = replace(text, marker, '''
    { extern uint64_t mk8_settings_page_input(uint64_t, int32_t[4], uint64_t);
      int32_t st[4] = {lx, ly, rx, ry};
      b = mk8_settings_page_input(b, st, script_buttons | touch_b);
      lx=st[0]; ly=st[1]; rx=st[2]; ry=st[3]; }
''' + marker)
        path.write_text(text, encoding='utf-8')
    path = runtime / 'hle/touch.c'
    if path.is_file():
        text = path.read_text(encoding='utf-8')
        text = '#include <stdbool.h>\nextern bool mk8_settings_page_active(void);\n' + text
        text = replace(text, 'bool hidden = game_overlay_captured();',
                       'bool hidden = game_overlay_captured() || mk8_settings_page_active();')
        path.write_text(text,encoding='utf-8')
    # Windows runtime defaults to binary I/O; explicitly translate CRLF in user INIs.
    settings = runtime / 'settings.c'
    if settings.is_file():
        original = settings.read_text(encoding='utf-8')
        original = replace(original, 'fopen(SETTINGS_FILE, "r")', 'fopen(SETTINGS_FILE, "rt")')
        original = '#include <SDL3/SDL_filesystem.h>\n#include <pthread.h>\n' + original
        original = replace(original, 'void settings_save(void) {', '''static pthread_mutex_t settings_write_lock = PTHREAD_MUTEX_INITIALIZER;
static bool settings_write(void) {
    pthread_mutex_lock(&settings_write_lock);
    char temporary[4096];
    int length = snprintf(temporary, sizeof temporary, "%s.tmp", SETTINGS_FILE);
    if (length < 0 || (size_t)length >= sizeof temporary) { pthread_mutex_unlock(&settings_write_lock); return false; }''')
        original = replace(original, 'FILE *f = fopen(SETTINGS_FILE, "w");', 'FILE *f = fopen(temporary, "w");')
        original = replace(original, 'if (!f) { LOG("[settings] cannot write %s\\n", SETTINGS_FILE); return; }',
                           'if (!f) { LOG("[settings] cannot write %s\\n", SETTINGS_FILE); pthread_mutex_unlock(&settings_write_lock); return false; }')
        original = replace(original, '    fclose(f);\n}\n\nbool settings_ahead_fast', '''    bool ok = !ferror(f);
    if (fclose(f)) ok = false;
    if (ok) ok = SDL_RenamePath(temporary, SETTINGS_FILE);
    if (!ok) SDL_RemovePath(temporary);
    if (!ok) LOG("[settings] write failed for %s\\n", SETTINGS_FILE);
    pthread_mutex_unlock(&settings_write_lock);
    return ok;
}
void settings_save(void) { (void)settings_write(); }
bool mk8_settings_save_checked(void) { return settings_write(); }

bool settings_ahead_fast''')
        settings.write_text(original, encoding='utf-8')
    path = runtime / 'ui/menu.mm'
    text = path.read_text(encoding='utf-8')
    marker = '// ------------------------------------------------------------------ state shared with the main thread'
    own = (ROOT / 'runtime/mk8/ui/settings_entry.inc').read_text(encoding='utf-8')
    own += '\n' + (ROOT / 'runtime/mk8/ui/settings_controls.inc').read_text(encoding='utf-8')
    text = replace(text, marker, own + '\n' + marker)
    marker = 'bool menu_handle_event(const SDL_Event *e) {'
    text = replace(text, marker, marker + '\n    if (mk8_settings_event(e, g_open.load())) return true;')
    text = replace(text, '            if (!typing) set_open(!g_open.load());',
                   '            if (!typing && !g_open.load() && !(e->key.mod & SDL_KMOD_SHIFT) && mk8_settings_page_toggle()) return true;\n'
                   '            if (!typing) set_open(!g_open.load());')
    text = replace(text, '            set_open(!g_open.load());\n            return true;',
                   '            if (!g_open.load() && mk8_settings_page_toggle()) return true;\n'
                   '            set_open(!g_open.load());\n            return true;')
    marker = 'void menu_apply_window_requests(void *win) {'
    text = replace(text, marker, '''static std::atomic<bool> mk8_pads_apply{false};
extern "C" void mk8_settings_window_changed(void) { g_want_fullscreen_apply.store(true); }
extern "C" void mk8_settings_pads_changed(void) { mk8_pads_apply.store(true); }
''' + marker + '\n    if (mk8_pads_apply.exchange(false)) input_pads_reassign();\n    mk8_settings_poll(g_open.load());')
    marker = '    bool open = g_open.load();\n    bool kbd = g_kbd.load() == 1;'
    text = replace(text, marker, '    mk8_settings_values_sync();\n' + marker + '\n    float mk8_phase = mk8_settings_phase(open);')
    marker = 'static const ThemePalette &theme_palette(void) {'
    theme = (ROOT / 'runtime/mk8/ui/settings_theme.inc').read_text(encoding='utf-8')
    text = replace(text, marker, theme + '\n' + marker + '\n    if (mk8_settings_style_active()) return mk8_settings_palette(kThemes[0]);')
    marker = '    if (ov_img) game_overlay_draw(ov_img, ov_w, ov_h, ov_full, ov_seq, io.DisplaySize);'
    text = replace(text, marker, marker + '\n    if (open) mk8_settings_backdrop(io.DisplaySize);')
    marker = '        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));'
    position = marker.replace('io.DisplaySize.y * 0.5f)', 'io.DisplaySize.y * 0.5f + (mk8_settings_style_active() ? 28 * scale * (1-mk8_phase) : 0))')
    text = replace(text, marker, '        if (mk8_settings_style_active()) { w = io.DisplaySize.x * 0.94f; h = io.DisplaySize.y * 0.82f; }\n' + position)
    marker = '    opt_choice("Theme", &g_settings.theme, g_theme_names, N_THEMES, THEME_DEFAULT,'
    text = replace(text, marker, '    if (!mk8_settings_style_active()) ' + marker.strip())
    old = ('               "Unlocked: the view fills the window, so wider (or taller) windows show more of the island - up to "\n'
           '               "ultrawide; houses and dungeons keep their framing. Original 16:9: the game\'s own framing, with bars.");')
    text = replace(text, old, '               "Choose between the window aspect ratio and original 16:9 framing.");')
    marker = '        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 110));'
    text = replace(text, marker, '        if (!mk8_settings_style_active()) ' + marker.strip())
    marker = '    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_WindowBg];'
    text = replace(text, marker, marker + '\n    mk8_settings_style_extras(s);')
    text = text.replace('IM_COL32(220, 216, 205, 255)', '(mk8_settings_style_active() ? IM_COL32(40,65,85,255) : IM_COL32(220, 216, 205, 255))')
    path.write_text(text, encoding='utf-8')
    path = runtime / 'ui/option_rows.mm'
    text = path.read_text(encoding='utf-8')
    text = 'extern "C" bool mk8_settings_style_active(void);\n' + text
    marker = '    ImU32 fill = hov || foc ? IM_COL32(255, 255, 255, 22) : IM_COL32(0, 0, 0, 40);'
    text = replace(text, marker, marker + '\n    if (mk8_settings_style_active()) fill = hov || foc ? IM_COL32(255,215,42,255) : IM_COL32(255,255,255,230);')
    marker = '    float lead = nes ? 8 * ap : 0;'
    text = replace(text, marker, '    if (mk8_settings_style_active()) label_col = ImGui::GetColorU32(off ? ImGuiCol_TextDisabled : ImGuiCol_Text);\n' + marker)
    # Hand-drawn row values must follow the light palette as well.
    text = text.replace('IM_COL32(240, 236, 224, 255)', '(mk8_settings_style_active() ? ImGui::GetColorU32(ImGuiCol_Text) : IM_COL32(240, 236, 224, 255))')
    for colour in ('IM_COL32(255, 255, 255, 255)', 'IM_COL32(205, 205, 195, 255)'):
        text = text.replace(colour, '(mk8_settings_style_active() ? ImGui::GetColorU32(ImGuiCol_Text) : ' + colour + ')')
    path.write_text(text, encoding='utf-8')
