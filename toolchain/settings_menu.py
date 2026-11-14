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
    text = replace(text, marker, '    if (mk8_settings_take_open()) set_open(true);\n' + marker + '\n    float mk8_phase = mk8_settings_phase(open);')
    marker = '!ov_img && !touchpad_visible() && !notice_active()) {'
    text = replace(text, marker, '!ov_img && !touchpad_visible() && !notice_active() && !mk8_settings_entry_visible()) {')
    marker = '    if (!open && !kbd) touchpad_draw((float)tw, (float)th);'
    text = replace(text, marker, '    if (!open && !kbd) mk8_settings_entry_draw(io.DisplaySize);\n' + marker)
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
    path = runtime / 'hle/input.c'
    text = path.read_text(encoding='utf-8')
    marker = '\n    *(uint64_t *)(s + 8) = b;'
    text = replace(text, marker, '''
    extern uint64_t mk8_settings_filter_buttons(uint64_t, bool);
    b = mk8_settings_filter_buttons(b, menu);
''' + marker)
    path.write_text(text, encoding='utf-8')
