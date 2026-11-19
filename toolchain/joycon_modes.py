"""Prepare Windows gamepad initialization and the in-game Joy-Con settings."""


def insert(path, marker, new):
    text = path.read_text(encoding='utf-8')
    if text.count(marker) != 1:
        raise ValueError(f'{path.name}: controller initialization or settings boundary changed')
    path.write_text(text.replace(marker, new), encoding='utf-8')


def prepare(runtime):
    insert(runtime/'settings.h', '} Settings;', '''    int joycon_mode;
} Settings;
extern const char *const mk8_joycon_modes[2];
bool mk8_set_joycon_mode(int mode);''')
    insert(runtime/'settings.c', '        else if (!strcmp(key, "pads_from_p2"))',
           '        else if (!strcmp(key, "joycon_mode")) g_settings.joycon_mode = !strcmp(val, "horizontal");\n'
           '        else if (!strcmp(key, "pads_from_p2"))')
    insert(runtime/'settings.c', '    fprintf(f, "pads_from_p2',
           '    fprintf(f, "joycon_mode = %s\\n", g_settings.joycon_mode ? "horizontal" : "combined");\n'
           '    fprintf(f, "pads_from_p2')
    insert(runtime/'nvn/vulkan/device.c', '    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))',
           '    extern bool mk8_init_window_input(void);\n'
           '    mk8_set_joycon_mode(g_settings.joycon_mode);\n'
           '    if (!mk8_init_window_input())')
    insert(runtime/'nvn/vulkan/device.c', '        menu_apply_window_requests(window);',
           '        extern void mk8_poll_steam_gamepads(void);\n'
           '        mk8_poll_steam_gamepads();\n'
           '        menu_apply_window_requests(window);')
    insert(runtime/'ui/menu.mm', '    if (opt_toggle("Controllers start at player 2"', '''    int joycon_mode = g_settings.joycon_mode;
    if (opt_choice("Joy-Con mode", &joycon_mode, mk8_joycon_modes, 2, 0,
                   "Use both halves as one controller, or give each player a sideways Joy-Con.") &&
        mk8_set_joycon_mode(joycon_mode)) settings_save();
    if (opt_toggle("Controllers start at player 2"''')
