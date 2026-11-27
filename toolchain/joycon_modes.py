"""Prepare Windows gamepad initialization and remove superseded global assignment options."""


def insert(path, marker, new):
    text = path.read_text(encoding='utf-8')
    if text.count(marker) != 1:
        raise ValueError(f'{path.name}: controller initialization or settings boundary changed')
    path.write_text(text.replace(marker, new), encoding='utf-8')


def prepare(runtime):
    insert(runtime/'main.c', 'int main(int argc, char **argv) {',
           'int main(int argc, char **argv) {\n'
           '#ifdef _WIN32\n'
           '    if (argc > 1 && !strcmp(argv[1], "--steam-motion-reader")) {\n'
           '        extern int mk8_run_steam_motion_reader(const char *, const char *);\n'
           '        return argc == 4 ? mk8_run_steam_motion_reader(argv[2], argv[3]) : 1;\n'
           '    }\n'
           '#endif')
    insert(runtime/'settings.h', '} Settings;', '} Settings;\nbool mk8_init_joycon_input(void);')
    insert(runtime/'settings.c',
           '        else if (!strcmp(key, "pads_from_p2")) g_settings.pads_from_p2 = atoi(val) != 0;\n', '')
    insert(runtime/'settings.c', '    fprintf(f, "pads_from_p2 = %d\\n", g_settings.pads_from_p2);\n', '')
    insert(runtime/'nvn/vulkan/device.c', '    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))',
           '#ifdef _WIN32\n'
           '    extern bool mk8_init_window_input(void);\n'
           '    bool input_ready = mk8_init_joycon_input() && mk8_init_window_input();\n'
           '#else\n'
           '    bool input_ready = mk8_init_joycon_input() && SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);\n'
           '#endif\n'
           '    if (!input_ready)')
    insert(runtime/'nvn/vulkan/device.c', '        menu_apply_window_requests(window);',
           '#ifdef _WIN32\n'
           '        extern void mk8_poll_steam_gamepads(void);\n'
           '        mk8_poll_steam_gamepads();\n'
           '#endif\n'
           '        menu_apply_window_requests(window);')
    insert(runtime/'nvn/vulkan/device.c', '            extern bool input_user_blocked(const SDL_Event *e);',
           '#ifdef _WIN32\n'
           '            extern bool mk8_steam_gamepad_event(const SDL_Event *e);\n'
           '            if (mk8_steam_gamepad_event(&e)) continue;\n'
           '#endif\n'
           '            extern bool input_user_blocked(const SDL_Event *e);')
    insert(runtime/'ui/menu.mm', '''    if (opt_toggle("Controllers start at player 2", &g_settings.pads_from_p2, false,
                   "The keyboard and the on-screen controls are player 1, and every controller is the next player "
                   "(player 2, 3, 4), for local multiplayer with a keyboard and one controller. Off: the first "
                   "controller is player 1's too.")) {
        settings_save();
        input_pads_reassign();
    }
''', '')
