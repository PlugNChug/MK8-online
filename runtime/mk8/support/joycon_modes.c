#include "settings.h"
#include <SDL3/SDL.h>
#include <stdlib.h>

void mk8_settings_pads_changed(void);

const char *const mk8_joycon_modes[] = {"Combined", "Individual (horizontal)"};

bool mk8_set_joycon_mode(int mode) {
    if (mode != 0 && mode != 1) return false;
    if (!SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_HIDAPI_VERTICAL_JOY_CONS, "0", SDL_HINT_OVERRIDE) ||
        !SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_HIDAPI_COMBINE_JOY_CONS, mode ? "0" : "1", SDL_HINT_OVERRIDE))
        return false;
    bool release_layout = mode != g_settings.joycon_mode;
    g_settings.joycon_mode = mode;
    if (release_layout) mk8_settings_pads_changed();
    return true;
}
