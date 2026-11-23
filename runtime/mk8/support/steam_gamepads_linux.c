// Portable fallback for the Windows GameInput/Steam gamepad path.
//
// Upstream's support/steam_gamepads.c is Windows-only (GameInput + XInput). The generated device code calls the same
// two entry points on every platform, so Linux provides no-op definitions here and host.py excludes the Windows file
// from the Linux source list. SDL handles Linux gamepads; nothing is polled through GameInput.
#if !defined(_WIN32)

#include <stdbool.h>
#include <SDL3/SDL.h>

// The 0.5.0 Linux build returned true here without SDL_Init: SDL_WasInit reported only video and events,
// SDL_GetGamepads() found nothing and no controller worked (#120). Whoever calls this gets the same subsystems
// as Windows.
bool mk8_init_window_input(void) {
    return SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
}

void mk8_poll_steam_gamepads(void) {
}

#endif
