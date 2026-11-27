#pragma once

#include <SDL3/SDL.h>

SDL_JoystickID mk8_poll_steam_motion(bool *ambiguous);
void mk8_stop_steam_motion(void);
int mk8_run_steam_motion_reader(const char *owner, const char *session);

// Windows calls these on the SDL event thread through the assigned gamepad ID.
void mk8_motion_steam_sample(SDL_JoystickID id, Uint64 source, const float accel[3], const float gyro[3]);
void mk8_motion_steam_lost(SDL_JoystickID id);
