"""Place MK8's presented image without changing its render targets."""

PLACEMENT = '''    // where the image goes in the window: fills it when reshaped, else a fitted 16:9 box
    if (reshape) { o->vw = dw; o->vh = dh; }
'''
STRETCH = '''    // MK8: use this image's camera decision, even if settings changed while it was queued.
    extern double mk8_ultrawide_present_mode(void);
    double present_mode = mk8_ultrawide_present_mode();
    if (present_mode > 16.0/9.0) {
        double height = fmin(dw / present_mode, dh);
        o->vw = present_mode * height; o->vh = height;
    } else if (present_mode == 0 && (reshape || g_settings.aspect == ASPECT_UNLOCKED)) {
        o->vw = dw; o->vh = dh;
    }
'''

HISTORY = '        static FgState fg_state;\n'
RESET_HISTORY = HISTORY + '''        // The image can change framing without changing the drawable size or UV crop.
        static double fg_previous_rect[5];
        extern double mk8_ultrawide_present_mode(void);
        const double fg_rect[5] = {vx, vy, vw, vh, mk8_ultrawide_present_mode()};
        if (memcmp(fg_previous_rect, fg_rect, sizeof fg_rect)) fg_state.valid = false;
        memcpy(fg_previous_rect, fg_rect, sizeof fg_rect);
'''

PUBLISH_DRAWABLE = '''static void mk8_ultrawide_update_drawable(void) {
    extern void mk8_ultrawide_drawable(int width, int height);
    int width = 0, height = 0;
    if ((SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) ||
        !SDL_GetWindowSizeInPixels(window, &width, &height)) width = height = 0;
    mk8_ultrawide_drawable(width, height);
}

'''
UI_START = '''int ui_run(void) {
    ui_window_init();
'''
UI_EVENTS = '        while (SDL_WaitEventTimeout(&e, 16)) {\n'
UI_WINDOW_REQUESTS = '        menu_apply_window_requests(window);\n'

MENU = '''    const char *aspects[] = {"Unlocked", "Original 16:9"};
    opt_choice("Aspect ratio", &g_settings.aspect, aspects, 2, ASPECT_UNLOCKED,
               "Unlocked: the view fills the window, so wider (or taller) windows show more of the island - up to "
               "ultrawide; houses and dungeons keep their framing. Original 16:9: the game's own framing, with bars.");
'''
STRETCH_MENU = '''    const char *aspects[] = {"Stretch", "Original 16:9", "Ultrawide (experimental)"};
    opt_choice("Aspect ratio", &g_settings.aspect, aspects, 3, ASPECT_UNLOCKED,
               "Stretch: fills the window by stretching the image. "
               "Original 16:9: preserves the game's proportions with black bars. "
               "Ultrawide: wider solo Time Trial, Grand Prix and no-team VS views from 16:9 to 32:9. "
               "Frame generation or unsupported views use 16:9.");
'''

EDITS = (
    ('settings.h', 'enum { ASPECT_UNLOCKED = 0, ASPECT_ORIGINAL = 1 };',
     'enum { ASPECT_UNLOCKED = 0, ASPECT_ORIGINAL = 1, ASPECT_ULTRAWIDE = 2 };'),
    ('settings.h', '// ASPECT_*: unlocked = fill the window (wider/taller views), original = 16:9 with bars',
     '// ASPECT_*: unlocked = stretch, original = 16:9, ultrawide = experimental solo camera'),
    ('settings.c', 'g_settings.aspect = !strcmp(val, "original") ? ASPECT_ORIGINAL : ASPECT_UNLOCKED;',
     'g_settings.aspect = !strcmp(val, "ultrawide") ? ASPECT_ULTRAWIDE : !strcmp(val, "original") ? ASPECT_ORIGINAL : ASPECT_UNLOCKED;'),
    ('settings.c', 'g_settings.aspect == ASPECT_ORIGINAL ? "original" : "unlocked"',
     'g_settings.aspect == ASPECT_ULTRAWIDE ? "ultrawide" : g_settings.aspect == ASPECT_ORIGINAL ? "original" : "unlocked"'),
    ('nvn/output_scale.c', PLACEMENT, STRETCH),
    ('nvn/output_scale.c', '    if (wide_test > 0) {', '    if (wide_test > 0 && present_mode == 0) {'),
    ('nvn/vulkan/device.c', HISTORY, RESET_HISTORY),
    ('nvn/vulkan/device.c', UI_START, PUBLISH_DRAWABLE + UI_START + '    mk8_ultrawide_update_drawable();\n'),
    ('nvn/vulkan/device.c', UI_EVENTS, UI_EVENTS + '''            if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
                e.type == SDL_EVENT_WINDOW_MINIMIZED || e.type == SDL_EVENT_WINDOW_RESTORED)
                mk8_ultrawide_update_drawable();
'''),
    ('nvn/vulkan/device.c', UI_WINDOW_REQUESTS, UI_WINDOW_REQUESTS + '        mk8_ultrawide_update_drawable();\n'),
    ('nvn/metal.m', HISTORY, RESET_HISTORY),
    ('ui/menu.mm', MENU, STRETCH_MENU),
)


def prepare(runtime):
    prepared = {}
    for name, old, new in EDITS:
        path = runtime/name
        text = prepared[path] if path in prepared else path.read_text(encoding='utf-8')
        if text.count(old) != 1:
            raise ValueError(f'Aspect ratio boundary changed: {name}')
        prepared[path] = text.replace(old, new)
    for path, text in prepared.items():
        path.write_text(text, encoding='utf-8')
