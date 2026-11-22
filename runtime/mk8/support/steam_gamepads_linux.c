// Portable fallback for the Windows GameInput/Steam gamepad path.
//
// Upstream's support/steam_gamepads.c is Windows-only (GameInput + XInput). The generated device code calls the same
// two entry points on every platform, so Linux provides no-op definitions here and host.py excludes the Windows file
// from the Linux source list. SDL handles Linux gamepads; nothing is polled through GameInput.
#if !defined(_WIN32)

#include <stdbool.h>

bool mk8_init_window_input(void) {
    return true;
}

void mk8_poll_steam_gamepads(void) {
}

#endif
