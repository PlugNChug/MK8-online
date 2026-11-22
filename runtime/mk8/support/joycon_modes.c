#include "settings.h"
#include <SDL3/SDL.h>

bool mk8_init_joycon_input(void) {
    // The assignment popup pairs physical grips per player.
    return SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_HIDAPI_VERTICAL_JOY_CONS, "0", SDL_HINT_OVERRIDE) &&
           SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_HIDAPI_COMBINE_JOY_CONS, "0", SDL_HINT_OVERRIDE);
}
