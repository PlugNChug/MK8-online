"""Opt-in Settings entry experiment for the verified MK8 4.0.1 top menu."""
import hashlib
from pathlib import Path

from guest_source import verified_bytes

ROOT = Path(__file__).resolve().parents[1]
UNIT = 'main_114.c'
SOURCE_SHA256 = 'ace7dcd1961f46f6213dfe20d2dc534e1854f48401040aa2ae6d6be0f7d960cf'
ENTRY = 'void f_70004d92e8(Cpu *c) {'
BUILD_SHA256 = '4b4cf4a1aec1b9d1d0479a920a3fbf46fe518d23d7624a19caea00808f7d88f7'
NAV_SHA256 = 'c6d46af3e71b558fa38894d3b26de9569671c93a026f7a0cce99eb2e16707082'
TICK_SHA256 = 'e7f979c9bae346fca7da9f76349d27eeec5fcc4b23c87b43d396c39b605ef048'
FADER_BUILD_SHA256 = '62e1f3c522ad9e61462a0fe27cd842afd0598d4c570872321caeeef4b6ebe362'
SHARED_HASHES = {'main_100.c': '187122fd53c501e91020efb776088154977181fd647ba02cf084d1e9d4c518cc',
                 'main_191.c': 'f61eefe25d08a209796ba4519ea8aa491121f2f85089715fe63fbb7fa029d133'}


def patch_guest(source, original, output):
    if original.parent.name != 'main' or original.name not in (UNIT, 'main_113.c', 'main_082.c', 'main_115.c', *SHARED_HASHES):
        return source
    expected = {UNIT: SOURCE_SHA256, 'main_113.c': BUILD_SHA256, 'main_082.c': NAV_SHA256, 'main_115.c': TICK_SHA256, **SHARED_HASHES}[original.name]
    verified_bytes(original, expected, 'Settings experiment requires verified MK8 4.0.1 top-menu code')
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
        if original.name == 'main_100.c':
            signature = 'void f_70004a01c0(Cpu *c) {'
            if text.count(signature) != 1:
                raise ValueError('Settings fader constructor changed')
            start = text.index(signature)
            end = text.index('\n}', start)
            body = text[start:end]
            if hashlib.sha256(body.encode('utf-8')).hexdigest() != FADER_BUILD_SHA256:
                raise ValueError('Settings fader constructor changed')
            # All stock parts exist here, and guest layout allocation is still active.
            marker = '    { uint64_t ea = c->x[19] + 0x1e8ull; ST32(ea, 0u); }'
            body = replace(body, marker, marker + '\n    mk8_settings_fader_build(c->x[19]);')
            text = 'extern void mk8_settings_fader_build(uint64_t);\n' + text[:start] + body + text[end:]
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
    target.write_text(text, encoding='utf-8', newline='\r\n')
    return target


def replace(text, old, new):
    if text.count(old) != 1:
        raise ValueError('Settings experiment UI boundary changed: ' + old[:80])
    return text.replace(old, new)


def shared_video_controls(text, runtime):
    marker = 'static void tab_display(void) {'
    helpers = (ROOT / 'runtime/mk8/ui/menu_video.inc').read_text(encoding='utf-8')
    text = replace(text, marker, helpers + '\n' + marker)
    def omit(start, end):
        nonlocal text
        a = text.index(start)
        b = text.index(end, a)
        text = replace(text, text[a:b], '')
    omit('    const char *aspects[] =', '    opt_choice("Aspect ratio"')
    text = replace(text, 'opt_choice("Aspect ratio", &g_settings.aspect, aspects, 3, ASPECT_UNLOCKED,',
                   'mk8_video_choice(MK8_MENU_ASPECT,')
    text = replace(text, 'opt_toggle("Fullscreen", &fs, false,',
                   'opt_toggle(mk8_menu_option(MK8_MENU_FULLSCREEN)->label, &fs, mk8_menu_option(MK8_MENU_FULLSCREEN)->default_index != 0,')
    for label, field, option in (('Show FPS','show_fps','SHOW_FPS'), ('Show memory use','show_memory','SHOW_MEMORY'),
                                 ('Skip intro','skip_intro','SKIP_INTRO'), ('Prepare at full speed','ahead_fast','AHEAD_FAST')):
        text = replace(text, f'opt_toggle("{label}", &g_settings.{field}, false,', f'mk8_video_toggle(MK8_MENU_{option},')
    omit('    // display order -> RES_', '        float sx, sy;')
    text = replace(text, '        float sx, sy;', '    {\n        float sx, sy;')
    text = replace(text, 'if (opt_choice("Internal resolution", &ri, res, NRES, RES_MATCH_AT, d, false)) g_settings.res_mode = res_mode[ri];',
                   'mk8_video_choice(MK8_MENU_RESOLUTION, d, false);')
    text = replace(text, 'opt_choice("Upscaler", &selected, choices.names, choices.count, 1, help)',
                   'opt_choice(mk8_menu_option(MK8_MENU_UPSCALER)->label, &selected, choices.names, choices.count, 1, help)')
    omit('    static const char *aa[] =', '    opt_choice("Anti-aliasing"')
    text = replace(text, 'opt_choice("Anti-aliasing", &g_settings.antialias, aa, 2, AA_OFF,', 'mk8_video_choice(MK8_MENU_ANTIALIAS,')
    omit('#if defined(__APPLE__)\n    static const char *fg[]', '    opt_choice("Frame generation"')
    text = replace(text, 'opt_choice("Frame generation", &g_settings.frame_gen, fg, nfg, FRAMEGEN_OFF,', 'mk8_video_choice(MK8_MENU_FRAME_GEN,')
    text = replace(text, 'opt_action("Shader preparation", v,', 'opt_action(mk8_menu_option(MK8_MENU_SHADERS)->label, v,')
    begin = text.index('    header("Button prompts");')
    end = text.index('    hint("The game\'s text language', begin)
    text = replace(text, text[begin:end], (ROOT/'runtime/mk8/ui/menu_controls.inc').read_text(encoding='utf-8'))
    controls = runtime/'ui/controls_ui.mm'
    source = controls.read_text(encoding='utf-8')
    at = source.index('        static const int order[] =')
    end = source.index('for (int slot : order)', at) + len('for (int slot : order)')
    source = replace(source, source[at:end], '        for (int slot : mk8_menu_bind_order)')
    for old, light in (
            ('0.55f, 0.42f, 0.12f', '.98f, .85f, .56f'), ('0.55f, 0.16f, 0.12f', '.99f, .76f, .73f'),
            ('0.98f, 0.82f, 0.38f', '.43f, .18f, .04f'), ('0.93f, 0.76f, 0.33f', '.43f, .18f, .04f')):
        color = f'ImVec4({old}, 1)'
        source = replace(source, color, f'mk8_menu_light() ? ImVec4({light}, 1) : {color}')
    source = replace(source, '    ImGui::TextUnformatted(line);', '    ImGui::TextWrapped("%s",line);')
    marker = '    ImGui::SameLine();\n    ImGui::BeginDisabled(bind_is_default(BIND_DEV_KEY));'
    source = replace(source, marker, '''    const float reset_width=ImGui::CalcTextSize("Reset keyboard to defaults").x+2*ImGui::GetStyle().FramePadding.x;
    if (ImGui::GetItemRectMax().x+ImGui::GetStyle().ItemSpacing.x+reset_width <=
        ImGui::GetWindowPos().x+ImGui::GetWindowContentRegionMax().x) ImGui::SameLine();
    ImGui::BeginDisabled(bind_is_default(BIND_DEV_KEY));''')
    controls.write_text('#include "settings_catalog.h"\n' + source, encoding='utf-8')
    for title in ("Display", "Rendering", "Game", "Shaders", "Sound"):
        text = replace(text, f'opt_section("{title}");', f'opt_heading("{title}");')
    return text.replace('    header("', '    opt_heading("')


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
        for (int i = 0; i < n; i++) mk8_open_gamepad(ids[i]);
        SDL_free(ids);
    }'''
        text = replace(text, scan, '    scan_gamepads();')
        text = replace(text, '// The setting changed: every controller let go',
                       'static void mk8_open_gamepad(SDL_JoystickID id);\n'
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
        text += '\n' + (ROOT / 'runtime/mk8/ui/controller_pointer.inc').read_text(encoding='utf-8')
        path.write_text(text,encoding='utf-8')
    # Windows runtime defaults to binary I/O; explicitly translate CRLF in user INIs.
    settings = runtime / 'settings.c'
    if settings.is_file():
        header = runtime / 'settings.h'
        fields = header.read_text(encoding='utf-8')
        fields = replace(fields, 'extern Settings g_settings;', '''extern Settings g_settings;

enum { MK8_VOLUME_MIN = 0, MK8_VOLUME_MAX = 200, MK8_VOLUME_STEP = 5, MK8_VOLUME_DEFAULT = 100 };
static inline bool mk8_menu_light(void) {
    return g_settings.theme==THEME_SIMPLE_LIGHT || g_settings.theme==THEME_NES_LIGHT;
}
static inline bool mk8_settings_volume_valid(int percent) {
    return percent >= MK8_VOLUME_MIN && percent <= MK8_VOLUME_MAX;
}
static inline int mk8_settings_volume_percent(void) {
    return (int)(g_settings.volume * 100.0f + 0.5f);
}
static inline void mk8_settings_set_volume_percent(int percent) {
    if (mk8_settings_volume_valid(percent)) g_settings.volume = percent / 100.0f;
}''')
        fields = replace(fields, '} Settings;', '''    int camera_distance;
} Settings;
enum { MK8_CAMERA_DISTANCE_MIN = 75, MK8_CAMERA_DISTANCE_MAX = 150, MK8_CAMERA_DISTANCE_STOCK = 100 };
int mk8_settings_camera_distance(void);
void mk8_settings_set_camera_distance(int percent);
bool mk8_settings_set_camera_distance_text(const char *text);''')
        fields = replace(fields, 'THEME_NES = 1, N_THEMES = 2', 'THEME_NES = 1, THEME_NATIVE = 2, THEME_SIMPLE_LIGHT = 3, THEME_NES_LIGHT = 4, N_THEMES = 5')
        header.write_text(fields, encoding='utf-8')
        original = settings.read_text(encoding='utf-8')
        original = replace(original, 'g_theme_names[N_THEMES] = {"Default", "NES"}', 'g_theme_names[N_THEMES] = {"Default", "NES", "Native", "SimpleLight", "NESLight"}')
        original = replace(original, '            g_settings.theme = THEME_DEFAULT;',
                           """            size_t length = strlen(val);
            while (length && (val[length-1]==' ' || val[length-1]=='\\t')) val[--length]=0;
            g_settings.theme = *val ? THEME_DEFAULT : THEME_NATIVE;""")
        original = replace(original, '    .volume = 1.0f,', '    .theme = THEME_NATIVE,\n    .camera_distance = MK8_CAMERA_DISTANCE_STOCK,\n    .volume = MK8_VOLUME_DEFAULT / 100.0f,')
        original = replace(original, '''    if (g_settings.volume < 0) g_settings.volume = 0;
    if (g_settings.volume > 2) g_settings.volume = 2;''', '''    if (!(g_settings.volume >= MK8_VOLUME_MIN / 100.0f)) g_settings.volume = MK8_VOLUME_MIN / 100.0f;
    if (g_settings.volume > MK8_VOLUME_MAX / 100.0f) g_settings.volume = MK8_VOLUME_MAX / 100.0f;''')
        original = replace(original, 'void settings_load(void) {', '''int mk8_settings_camera_distance(void) {
    return __atomic_load_n(&g_settings.camera_distance, __ATOMIC_RELAXED);
}
void mk8_settings_set_camera_distance(int percent) {
    if (percent >= MK8_CAMERA_DISTANCE_MIN && percent <= MK8_CAMERA_DISTANCE_MAX)
        __atomic_store_n(&g_settings.camera_distance, percent, __ATOMIC_RELAXED);
}
bool mk8_settings_set_camera_distance_text(const char *text) {
    if (*text < '0' || *text > '9') return false;
    char *end;
    long n = strtol(text, &end, 10);
    if (end == text || end[strspn(end, " \\t\\r")] || n < MK8_CAMERA_DISTANCE_MIN || n > MK8_CAMERA_DISTANCE_MAX) return false;
    mk8_settings_set_camera_distance((int)n);
    return true;
}

void settings_load(void) {''')
        original = replace(original, '        else if (!strcmp(key, "volume"))', '''        else if (!strcmp(key, "camera_distance")) {
            if (!mk8_settings_set_camera_distance_text(val)) mk8_settings_set_camera_distance(MK8_CAMERA_DISTANCE_STOCK);
        }
        else if (!strcmp(key, "volume"))''')
        original = replace(original, '    fprintf(f, "volume =', '    fprintf(f, "camera_distance = %d\\n", mk8_settings_camera_distance());\n    fprintf(f, "volume =')
        fields = replace(header.read_text(encoding='utf-8'), '} Settings;', '''    bool split_screen_60fps;
} Settings;
bool mk8_split_screen_60fps_enabled(void);
void mk8_set_split_screen_60fps(bool enabled);''')
        header.write_text(fields, encoding='utf-8')
        original = replace(original, 'void settings_load(void) {', '''bool mk8_split_screen_60fps_enabled(void) {
    return __atomic_load_n(&g_settings.split_screen_60fps, __ATOMIC_RELAXED);
}
void mk8_set_split_screen_60fps(bool enabled) {
    __atomic_store_n(&g_settings.split_screen_60fps, enabled, __ATOMIC_RELAXED);
}
void settings_load(void) {''')
        original = replace(original, '    loaded = true;', '''    loaded = true;
    const char *legacy_split = getenv("MK8_SPLITSCREEN_60FPS");
    mk8_set_split_screen_60fps(legacy_split && !strcmp(legacy_split, "1"));''')
        original = replace(original, '        else if (!strcmp(key, "volume"))', '''        else if (!strcmp(key, "split_screen_60fps"))
            mk8_set_split_screen_60fps(*val == '1' && !val[1 + strspn(val + 1, " \t")]);
        else if (!strcmp(key, "volume"))''')
        original = replace(original, '    fprintf(f, "volume =', '    fprintf(f, "split_screen_60fps = %d\\n", mk8_split_screen_60fps_enabled());\n    fprintf(f, "volume =')
        fields = replace(header.read_text(encoding='utf-8'), '} Settings;', '''    bool bloom;
} Settings;
bool mk8_bloom_enabled(void);
void mk8_set_bloom(bool enabled);''')
        header.write_text(fields, encoding='utf-8')
        original = replace(original, 'void settings_load(void) {', '''bool mk8_bloom_enabled(void) {
    return __atomic_load_n(&g_settings.bloom, __ATOMIC_RELAXED);
}
void mk8_set_bloom(bool enabled) {
    __atomic_store_n(&g_settings.bloom, enabled, __ATOMIC_RELAXED);
}
void settings_load(void) {''')
        original = replace(original, '    .volume = MK8_VOLUME_DEFAULT / 100.0f,', '    .bloom = true,\n    .volume = MK8_VOLUME_DEFAULT / 100.0f,')
        original = replace(original, '        else if (!strcmp(key, "volume"))', '''        else if (!strcmp(key, "bloom"))
            mk8_set_bloom(*val != '0' || val[1 + strspn(val + 1, " \t")]);
        else if (!strcmp(key, "volume"))''')
        original = replace(original, '    fprintf(f, "volume =', '    fprintf(f, "bloom = %d\\n", mk8_bloom_enabled());\n    fprintf(f, "volume =')
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
    # The standard F1 overlay shares its keyboard and paste handling with the other editable rows.
    signature = 'bool opt_text(const char *label, char *buf, size_t cap, const char *hint, const char *desc)'
    rows = runtime / 'ui/option_rows.mm'
    if rows.is_file():
        header = rows.with_suffix('.h')
        header.write_text(replace(header.read_text(encoding='utf-8'), signature, signature[:-1] + ', int *step = NULL)'), encoding='utf-8')
        source = replace(rows.read_text(encoding='utf-8'), signature, signature[:-1] + ', int *step)')
        marker = 'if (row(label, buf[0] ? buf : (hint ? hint : ""), false, desc) == 2) editing = id, focus = 2;'
        source = replace(source, marker, '''int action = row(label, buf[0] ? buf : (hint ? hint : ""), false, desc, true, step != NULL);
        if (action == 2) editing = id, focus = 2;
        else if (step) *step = action;''')
        rows.write_text(source, encoding='utf-8')
    path = runtime / 'ui/menu.mm'
    text = path.read_text(encoding='utf-8')
    catalog = ROOT / 'runtime/mk8/support/settings_catalog.h'
    (runtime / 'settings_catalog.h').write_bytes(catalog.read_bytes())
    text = replace(text, '#include "option_rows.h"', '#include "option_rows.h"\n#include "settings_catalog.h"')
    text = replace(text, '''    float pct = roundf(g_settings.volume * 100);
    if (opt_slider("Volume", &pct, 0, 200, 5, 100, "%g%%",
                   "The game's volume. 100% is the game's own level; above it, louder."))
        g_settings.volume = pct / 100.0f;''', '''    int pct = mk8_settings_volume_percent();
    if (opt_int(mk8_menu_option(MK8_MENU_VOLUME)->label, &pct, MK8_VOLUME_MIN, MK8_VOLUME_MAX, MK8_VOLUME_STEP, MK8_VOLUME_DEFAULT, "%d%%",
                "The game's volume. 100% is the game's own level; above it, louder.", NULL))
        mk8_settings_set_volume_percent(pct);''')
    text = replace(text, '''    opt_toggle("Reverb", &g_settings.reverb, true, "The game's reverb effect.");
    opt_toggle("Movie audio", &g_settings.movie_audio, true,
               "Sound in the game's movies. Applies the next time a movie starts.");
    opt_toggle("Mute in background", &g_settings.mute_in_background, false,
               "Silences the game while its window is not in front (another app has focus, or it is minimized) or "
               "the app is in the background. The game keeps running; the sound fades back when you return.");''', '''    for (int id=0; id<MK8_MENU_FIELD_COUNT; ++id) {
        const Mk8MenuOption *o=mk8_menu_option(id);
        if (o->group==MK8_MENU_AUDIO && o->toggle) opt_toggle(o->label,o->toggle,o->default_index,o->help);
    }''')
    marker = '    bool fs = g_settings.fullscreen;'
    text = replace(text, marker, '''    bool split_60 = mk8_split_screen_60fps_enabled();
    if (opt_toggle("Split-screen 60 FPS", &split_60, false, "Target 60 FPS per view with 3-4 players."))
        mk8_set_split_screen_60fps(split_60);
''' + marker)
    text = replace(text, marker, '''    bool bloom = mk8_bloom_enabled();
    if (opt_toggle("Bloom", &bloom, true, "Add a glow around bright objects."))
        mk8_set_bloom(bloom);
''' + marker)
    text = replace(text, marker, '''    int distance = mk8_settings_camera_distance();
    const char *distance_label = "Driving camera distance (%)";
    static char distance_text[128];
    if (!opt_text_editing(distance_label)) snprintf(distance_text, sizeof distance_text, "%d", distance);
    int step = 0;
    if (opt_text(distance_label, distance_text, sizeof distance_text, "75-150",
                 "Solo forward and rear views. Use the arrows or enter 75-150. 100% is stock.", &step))
        mk8_settings_set_camera_distance_text(distance_text);
    if (step) {
        int next = distance + 10 * step;
        mk8_settings_set_camera_distance(next < MK8_CAMERA_DISTANCE_MIN ? MK8_CAMERA_DISTANCE_MIN :
                                         next > MK8_CAMERA_DISTANCE_MAX ? MK8_CAMERA_DISTANCE_MAX : next);
    }
''' + marker)
    marker = '// ------------------------------------------------------------------ state shared with the main thread'
    own = (ROOT / 'runtime/mk8/ui/settings_entry.inc').read_text(encoding='utf-8')
    own += '\n' + (ROOT / 'runtime/mk8/ui/settings_controls.inc').read_text(encoding='utf-8')
    text = replace(text, marker, own + '\n' + marker)
    text = replace(text, 'static void set_open(bool open);',
                   'extern "C" bool mk8_settings_shared_race(void);\nstatic bool mk8_block_shared_settings(void);\n'
                   'static void set_open(bool open);')
    text = replace(text, '    if (open == g_open.load()) return;',
                   '    if (open == g_open.load() || (open && mk8_block_shared_settings())) return;')
    marker = '    else { settings_save(), osk_cancel(), game_mod_rows_hide(); g_text_busy = false; }'
    text = replace(text, marker, """    else {
        if (!mk8_settings_save_checked()) menu_notice(mk8_menu_save_error(), 8.0);
        osk_cancel(); game_mod_rows_hide(); g_text_busy = false;
    }""")
    text = '#include "menu_language.h"\nextern "C" bool mk8_settings_save_checked(void);\nextern "C" void menu_notice(const char *, double);\n' + text
    marker = 'void menu_tabs_focus(bool first_tab);'
    child = (ROOT/'runtime/mk8/ui/menu_child.inc').read_text(encoding='utf-8')
    text = replace(text, marker, child + '\n' + marker)
    text = replace(text, 'void menu_toggle(void) { set_open(!g_open.load()); }',
                   'void menu_toggle(void) { if (!g_open.load() && (mk8_block_shared_settings() || mk8_settings_page_toggle())) return; set_open(!g_open.load()); }')
    marker = 'static double notice_until;'
    text = replace(text, marker, 'static Uint64 notice_started;\nstatic bool notice_fades;\n' + marker)
    marker = '    notice_until = SDL_GetTicks() / 1000.0 + seconds;'
    text = replace(text, marker, '    notice_fades = false;\n' + marker)
    marker = 'static bool notice_active(void) { return SDL_GetTicks() / 1000.0 < notice_until; }'
    text = replace(text, marker, (ROOT / 'runtime/mk8/ui/race_settings_notice.inc').read_text(encoding='utf-8') +
                   '\nstatic bool notice_active(void) {\n'
                   '    pthread_mutex_lock(&notice_lock);\n    bool active=SDL_GetTicks()/1000.0 < notice_until;\n'
                   '    pthread_mutex_unlock(&notice_lock);\n    return active || mk8_lan_claim_pending.load();\n}')
    marker = '        snprintf(t, sizeof t, "%s", notice_text);'
    text = replace(text, marker, '''        bool claim=mk8_lan_claim_pending.load() && SDL_GetTicks()/1000.0 >= notice_until;
        snprintf(t, sizeof t, "%s", claim ? mk8_lan_claim_hint : notice_text);
        bool fading=claim || notice_fades;
        float alpha=!claim && fading ? mk8_race_notice_alpha(SDL_GetTicks()-notice_started) : 1.f;''')
    marker = '#ifdef __APPLE__\nvoid menu_render('
    text = replace(text, marker, (ROOT / 'runtime/mk8/ui/race_settings_tip.inc').read_text(encoding='utf-8') + '\n' + marker)
    start = text.index('        float w = fminf(io.DisplaySize.x * 0.8f, 1100 * scale);', text.index('    if (!open && notice_active()) {'))
    end = text.index('        ImGui::End();', start) + len('        ImGui::End();')
    ordinary = text[start:end]
    text = replace(text, ordinary, '        if (fading) {\n            mk8_draw_race_settings_tip(io.DisplaySize, scale, t, alpha);\n'
                   '        } else {\n' + '\n'.join('    '+line for line in ordinary.splitlines()) + '\n        }')
    marker = 'bool menu_handle_event(const SDL_Event *e) {'
    text = replace(text, marker, 'extern "C" bool mk8_controller_pointer_event(const SDL_Event *);\n'
                   'extern "C" bool mk8_controller_layout_active(void);\n'
                   'static bool mk8_hub_enabled();\n' + marker + '\n    bool settings_chord = mk8_settings_chord(e, capturing() || mk8_settings_page_active() || mk8_controller_layout_active());\n    if (g_kbd.load()!=1 && mk8_controller_pointer_event(e)) return true;'
                   '\n    if (g_kbd.load()!=1 && mk8_controller_layout_active()) return false;'
                   '\n    if (mk8_settings_event(e, capturing())) return true;')
    text = replace(text, '            if (!typing) set_open(!g_open.load());',
                   '            if (!typing && !g_open.load() && mk8_block_shared_settings()) return true;\n'
                   '            if (!typing && !g_open.load() && !(e->key.mod & SDL_KMOD_SHIFT) && mk8_settings_page_toggle()) return true;\n'
                   '            if (!typing) set_open(!g_open.load());')
    text = replace(text, '    static bool back_down, start_down;\n', '')
    text = replace(text, '        if (e->gbutton.button == SDL_GAMEPAD_BUTTON_BACK) back_down = down;\n'
                         '        if (e->gbutton.button == SDL_GAMEPAD_BUTTON_START) start_down = down;\n', '')
    text = replace(text, '(back_down && start_down)', 'settings_chord')
    text = replace(text, '            set_open(!g_open.load());\n            return true;',
                   '            if (!g_open.load() && mk8_block_shared_settings()) {\n'
                   '                uint32_t mask = settings_chord ? (1u << SDL_GAMEPAD_BUTTON_BACK) | (1u << SDL_GAMEPAD_BUTTON_START) : 1u << SDL_GAMEPAD_BUTTON_GUIDE;\n'
                   '                mk8_suppress_settings_buttons(e->gbutton.which, mask);\n                return true;\n            }\n'
                   '            if (!g_open.load() && mk8_settings_page_toggle()) return true;\n'
                   '            set_open(!g_open.load());\n            return true;')
    marker = 'void menu_apply_window_requests(void *win) {'
    text = replace(text, marker, '''static std::atomic<bool> mk8_pads_apply{false};
extern "C" void mk8_settings_window_changed(void) { g_want_fullscreen_apply.store(true); }
extern "C" void mk8_settings_pads_changed(void) { mk8_pads_apply.store(true); }
''' + marker + '\n    if (g_open.load() && mk8_settings_shared_race()) set_open(false);'
                   '\n    if (mk8_pads_apply.exchange(false)) input_pads_reassign();\n    mk8_settings_poll(capturing());')
    marker = '    mk8_settings_poll(capturing());'
    text = replace(text, marker, "    if (mk8_open_overlay.exchange(false)) set_open(true);\n" + marker)
    marker = '    bool open = g_open.load();\n    bool kbd = g_kbd.load() == 1;'
    text = replace(text, marker, '    mk8_publish_settings(g_open.load() || mk8_settings_page_active(), SDL_GetTicks());\n    mk8_settings_values_sync();\n' + marker + '\n    float mk8_phase = mk8_settings_phase(open);')
    marker = 'static const ThemePalette &theme_palette(void) {'
    theme = (ROOT / 'runtime/mk8/ui/settings_theme.inc').read_text(encoding='utf-8')
    text = replace(text, marker, theme + '\n' + marker + '\n    if (mk8_settings_overlay_style_active()) return mk8_settings_palette(kThemes[0]);')
    marker = '    if (ov_img) game_overlay_draw(ov_img, ov_w, ov_h, ov_full, ov_seq, io.DisplaySize);'
    text = replace(text, marker, marker + '\n    if (open) mk8_settings_backdrop(io.DisplaySize);')
    marker = '        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));'
    position = marker.replace('io.DisplaySize.y * 0.5f)', 'io.DisplaySize.y * 0.5f + (mk8_settings_overlay_style_active() ? 28 * scale * (1-mk8_phase) : 0))')
    text = replace(text, marker, '        if (mk8_settings_overlay_style_active()) { w = io.DisplaySize.x * 0.94f; h = io.DisplaySize.y * 0.82f; }\n' + position)
    begin = text.index('    opt_choice("Theme", &g_settings.theme,')
    end = text.index('    const char *aspects[]', begin)
    text = replace(text,text[begin:end], '    mk8_video_choice(MK8_MENU_THEME, mk8_menu_option(MK8_MENU_THEME)->help);\n')
    text = replace(text, 'return kThemes[t >= 0 && t < N_THEMES ? t : THEME_DEFAULT];',
                   """static const ThemePalette simple_light = mk8_light_palette(kThemes[THEME_DEFAULT],false);
    static const ThemePalette nes_light = mk8_light_palette(kThemes[THEME_NES],true);
    if (t == THEME_SIMPLE_LIGHT) return simple_light;
    if (t == THEME_NES_LIGHT) return nes_light;
    return kThemes[t == THEME_NES ? THEME_NES : THEME_DEFAULT];""")
    marker = 'static void theme_apply(ImGuiStyle &s) {'
    text = replace(text, marker, marker + '\n    if (mk8_menu_light()) ImGui::StyleColorsLight(&s); else ImGui::StyleColorsDark(&s);')
    text = replace(text, 'kThemes[N_THEMES] = {', 'kThemes[] = {')
    marker = '        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 110));'
    text = replace(text, marker, '        if (!mk8_settings_overlay_style_active()) ' + marker.strip())
    marker = '    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_WindowBg];'
    text = replace(text, marker, marker + '\n    mk8_settings_style_extras(s);')
    text = text.replace('IM_COL32(220, 216, 205, 255)', '(mk8_menu_light() ? ImGui::GetColorU32(ImGuiCol_Text) : mk8_settings_overlay_style_active() ? IM_COL32(40,65,85,255) : IM_COL32(220, 216, 205, 255))')
    text = replace(text, 'theme_palette().window), NES_WHITE,', 'theme_palette().window), ImGui::GetColorU32(theme_palette().text),')
    text = replace(text, 'static void header(const char *t) {\n    ImGui::PushFont(g_font_big);',
                   'void opt_heading(const char *t) {\n    if (mk8_collecting_rows()) { mk8_collect_section(t); return; }\n    ImGui::PushFont(nes_theme() ? menu_font_big() : g_font_big);')
    marker = '    ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.45f);'
    text = replace(text, marker, '''    if (nes_theme()) {
        const float available=ImGui::GetContentRegionAvail().x;
        const float label_end=ImGui::CalcTextSize(label).x+ImGui::GetStyle().ItemSpacing.x;
        const float combo=ImGui::CalcTextSize(names[*value]).x+ImGui::GetFrameHeight()+2*ImGui::GetStyle().FramePadding.x+reserve;
        if (label_end+combo<=available)
            ImGui::SameLine(fmaxf(label_end,fminf(.45f*available,available-combo)));
    } else ''' + marker.strip())
    path.write_text(shared_video_controls(text, runtime), encoding='utf-8')
    from menu_content import prepare as prepare_menu_content
    prepare_menu_content(runtime)
    path = runtime / 'ui/option_rows.mm'
    text = path.read_text(encoding='utf-8')
    text = 'extern "C" {\n#include "../settings.h"\n}\nextern "C" bool mk8_settings_overlay_style_active(void);\n' + text
    marker = '    ImU32 fill = hov || foc ? IM_COL32(255, 255, 255, 22) : IM_COL32(0, 0, 0, 40);'
    text = replace(text, marker, marker + '\n    if (mk8_settings_overlay_style_active()) fill = hov || foc ? IM_COL32(255,215,42,255) : IM_COL32(255,255,255,230);')
    marker = '    float lead = nes ? 8 * ap : 0;'
    text = replace(text, marker, '    if (mk8_settings_overlay_style_active()) label_col = ImGui::GetColorU32(off ? ImGuiCol_TextDisabled : ImGuiCol_Text);\n' + marker)
    marker = '    const bool lit = (hov || foc) && !off;'
    text = replace(text, marker, marker + """
    const bool light = mk8_menu_light();
    if (light) {
        fill = ImGui::GetColorU32(lit ? ImGuiCol_Header : ImGuiCol_FrameBg);
        edge = lit ? kGold : ImGui::GetColorU32(ImGuiCol_Separator);
    }""")
    marker = '        nes_box(dl, p, ImVec2(p.x + w, p.y + h),'
    text = replace(text, marker, '        if (light) { nfill=fill; nedge=edge; nshadow=ImGui::GetColorU32(ImGuiCol_Separator); }\n' + marker)
    text = replace(text, 'if (mk8_settings_overlay_style_active()) label_col', 'if (light || mk8_settings_overlay_style_active()) label_col')
    text = replace(text, 'const ImU32 pc[] = {NES_WHITE}', 'const ImU32 pc[] = {label_col}')
    text = replace(text, '        if (nes) {  // stepped pixel arrows', '        if (light) ac = lit ? kGold : label_col;\n        if (nes) {  // stepped pixel arrows')
    text = replace(text, 'dl->AddText(ImVec2(vx, ty), off ?', 'dl->AddText(ImVec2(vx, ty), light ? label_col : off ?')
    text = replace(text, '    if (menu_row_notch() > 0)', '    if (mk8_menu_light()) col = selected ? kGold : menu_muted();\n    if (menu_row_notch() > 0)')
    text = replace(text, 'ImGuiCol_ChildBg), NES_GREY,', 'ImGuiCol_ChildBg), mk8_menu_light() ? menu_muted() : NES_GREY,')
    # Hand-drawn row values must follow the light palette as well.
    text = text.replace('IM_COL32(240, 236, 224, 255)', '(mk8_settings_overlay_style_active() ? ImGui::GetColorU32(ImGuiCol_Text) : IM_COL32(240, 236, 224, 255))')
    for colour in ('IM_COL32(255, 255, 255, 255)', 'IM_COL32(205, 205, 195, 255)'):
        text = text.replace(colour, '(mk8_settings_overlay_style_active() ? ImGui::GetColorU32(ImGuiCol_Text) : ' + colour + ')')
    header = 'menu_nes_row.h'
    (path.parent/header).write_bytes((ROOT/'runtime/mk8/ui'/header).read_bytes())
    text = replace(text, '#include "theme_draw.h"', '#include "theme_draw.h"\n#include "menu_nes_row.h"')
    text = replace(text, 'static float g_scale = 1;', 'static float g_scale = 1;\nstatic bool desc_below;')
    marker = '    ImGui::BeginChild("##rows", ImVec2(avail.x * 0.48f, avail.y),'
    text = replace(text, marker, '''    bool pixel=menu_row_notch()>0;
    desc_below=pixel && avail.x<60*ImGui::GetFontSize();
    ImVec2 rows=desc_below ? ImVec2(avail.x,avail.y*.66f) : ImVec2(avail.x*(pixel ? .66f : .48f),avail.y);
    ImGui::BeginChild("##rows", rows,''')
    text = replace(text, '    ImGui::SameLine(0, 0);\n    ImVec2 p', '    if (!desc_below) ImGui::SameLine(0, 0);\n    ImVec2 p')
    text = replace(text, 'float gap = 24 * g_scale;', 'float gap = desc_below ? 0 : 24 * g_scale;')
    marker = '    ImGui::PushStyleColor(ImGuiCol_NavCursor,'
    text = replace(text, marker, '''    Mk8NesRow layout{};
    if (nes) { layout=mk8_nes_row(label,value.c_str(),arrows,w,g_scale,h); h=layout.height; }
''' + marker)
    text = replace(text, '    {  // a label that would run into the value', '    if (!nes) {  // a label that would run into the value')
    marker = '    dl->AddText(ImVec2(p.x + pad + lead, ty), label_col, t.c_str());'
    text = replace(text, marker, '''    if (nes) dl->AddText(ImGui::GetFont(), fs, ImVec2(p.x+pad+lead,p.y+layout.label_top), label_col,
                         t.c_str(), nullptr, layout.label_wrap);
    else ''' + marker.strip() + '\n    if (nes) ty=p.y+layout.value_top;')
    text = replace(text, 'lx = rx - gap - ImGui::CalcTextSize(value.c_str()).x - gap - aw;',
                   'lx = rx - gap - (nes ? layout.value_width : ImGui::CalcTextSize(value.c_str()).x) - gap - aw;')
    for x in ('lx','rx'):
        text = replace(text, f'ImVec2({x}, p.y + (h - 9 * ap) * 0.5f)', f'ImVec2({x}, cy - 4.5f*ap)')
    text = replace(text, '    if (!shown.empty() && ImGui::CalcTextSize', '    if (!nes && !shown.empty() && ImGui::CalcTextSize')
    text = replace(text, 'float vw = ImGui::CalcTextSize(value.c_str()).x, vx = right - vw;',
                   'float vw = nes ? layout.value_width : ImGui::CalcTextSize(value.c_str()).x, vx = right - vw;')
    text = replace(text, 'dl->AddText(ImVec2(vx, ty), light ? label_col', 'dl->AddText(ImGui::GetFont(), fs, ImVec2(vx, ty), light ? label_col')
    text = replace(text, 'value.c_str());\n    }\n    ImGui::PopID();', 'value.c_str(), nullptr, nes ? layout.value_wrap : 0);\n    }\n    ImGui::PopID();')
    path.write_text(text, encoding='utf-8')
