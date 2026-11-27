#include <assert.h>
#include <SDL3/SDL.h>

static SDL_JoystickID *virtual_pads(int *count) {
    SDL_JoystickID *ids = SDL_GetGamepads(count);
    int n = 0;
    for (int i = 0; ids && i < *count; ++i)
        if (SDL_IsJoystickVirtual(ids[i])) ids[n++] = ids[i];
    *count = n;
    return ids;
}

#define SDL_GetGamepads virtual_pads
#include "steam_motion_windows.c"
#undef SDL_GetGamepads

void mk8_motion_steam_sample(SDL_JoystickID id, Uint64 source, const float a[3], const float g[3]) {}
void mk8_motion_steam_lost(SDL_JoystickID id) {}

static SDL_AtomicInt sensor_enable_count;

static bool SDLCALL sensors_enabled(void *unused, bool enabled) {
    if (enabled) SDL_AddAtomicInt(&sensor_enable_count, 1);
    return true;
}

static SDL_Joystick *attach_pro(void) {
    SDL_VirtualJoystickSensorDesc sensors[] = {{SDL_SENSOR_ACCEL, 200}, {SDL_SENSOR_GYRO, 200}};
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.vendor_id = 0x057e;
    desc.product_id = 0x2009;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.nsensors = SDL_arraysize(sensors);
    desc.sensors = sensors;
    desc.SetSensorsEnabled = sensors_enabled;
    SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    assert(id);
    SDL_Joystick *joy = SDL_OpenJoystick(id);
    assert(joy);
    return joy;
}

static MotionMailbox snapshot(void) {
    assert(lock_channel(&channel, 200));
    MotionMailbox sample = *channel.data;
    ReleaseMutex(channel.lock);
    return sample;
}

static MotionMailbox wait_pair(SDL_Joystick *joy, float value) {
    Uint64 until = SDL_GetTicks() + 3000;
    do {
        float accel[] = {value, -value, 0};
        float gyro[] = {0, value, -value};
        Uint64 stamp = SDL_GetTicksNS();
        assert(SDL_SendJoystickVirtualSensorData(joy, SDL_SENSOR_ACCEL, stamp, accel, 3));
        assert(SDL_SendJoystickVirtualSensorData(joy, SDL_SENSOR_GYRO, stamp, gyro, 3));
        SDL_Delay(10);
        MotionMailbox sample = snapshot();
        if (sample.received && sample.accel[0] == value && sample.gyro[2] == -value) return sample;
    } while (SDL_GetTicks() < until);
    abort();
}

static void detach_pro(SDL_Joystick *joy) {
    SDL_JoystickID id = SDL_GetJoystickID(joy);
    SDL_CloseJoystick(joy);
    assert(SDL_DetachVirtualJoystick(id));
}

static int SDLCALL exercise(void *owner) {
    SDL_Joystick *joy = attach_pro();
    MotionMailbox before = wait_pair(joy, 2);
    int enabled = SDL_GetAtomicInt(&sensor_enable_count);
    Uint64 until = SDL_GetTicks() + 3500;
    MotionMailbox stalled;
    do {
        SDL_Delay(10);
        stalled = snapshot();
    } while (stalled.generation == before.generation && SDL_GetTicks() < until);
    // The controller stays attached but produces no reports until it is re-enabled.
    assert(SDL_JoystickConnected(joy) && SDL_GetAtomicInt(&sensor_enable_count) > enabled);
    assert(!stalled.received && stalled.generation != before.generation);
    MotionMailbox recovered = wait_pair(joy, 5);
    assert(recovered.reader == before.reader && recovered.sequence > before.sequence);

    detach_pro(joy);
    until = SDL_GetTicks() + 2000;
    MotionMailbox disconnected;
    do {
        SDL_Delay(10);
        disconnected = snapshot();
    } while (disconnected.received && SDL_GetTicks() < until);
    assert(!disconnected.received && disconnected.generation != recovered.generation);
    joy = attach_pro();
    MotionMailbox reconnected = wait_pair(joy, 7);
    assert(reconnected.generation != disconnected.generation);
    detach_pro(joy);
    assert(SetEvent(owner));
    return 0;
}

int main(void) {
    assert(SDL_Init(SDL_INIT_GAMEPAD));
    GUID id;
    assert(SUCCEEDED(CoCreateGuid(&id)));
    assert(StringFromGUID2(&id, channel_id, SDL_arraysize(channel_id)));
    assert(open_channel(&channel, channel_id, true));
    HANDLE owner = CreateEventW(NULL, TRUE, FALSE, NULL);
    assert(owner);
    SDL_Thread *thread = SDL_CreateThread(exercise, "Virtual Pro sensor test", owner);
    assert(thread);
    read_controller(&channel, owner);
    SDL_WaitThread(thread, NULL);
    close_channel(&channel);
    CloseHandle(owner);
    SDL_Quit();
    return 0;
}
