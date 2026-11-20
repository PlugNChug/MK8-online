#include <SDL3/SDL.h>
#include <assert.h>

// Isolate the test from controllers currently connected to the test PC.
static SDL_JoystickID *virtual_ids(int *count) {
    SDL_JoystickID *ids = SDL_GetGamepads(count);
    int n = 0;
    for (int i = 0; i < *count; ++i)
        if (SDL_IsJoystickVirtual(ids[i])) ids[n++] = ids[i];
    *count = n;
    return ids;
}
#define SDL_GetGamepads virtual_ids
#include "steam_gamepads.c"
#undef SDL_GetGamepads

// On Windows SDK builds also check our x64 ABI declarations against the OS header.
#if __has_include(<GameInput.h>)
#include <GameInput.h>
#include <stddef.h>
#define CHECK_SLOT(a, x, b, y) _Static_assert(offsetof(a, x) == offsetof(b, y), #y)
CHECK_SLOT(GIMethods, read, IGameInputVtbl, GetCurrentReading);
CHECK_SLOT(GIMethods, subscribe, IGameInputVtbl, RegisterDeviceCallback);
CHECK_SLOT(GIMethods, unsubscribe, IGameInputVtbl, UnregisterCallback);
CHECK_SLOT(GIDeviceMethods, info, IGameInputDeviceVtbl, GetDeviceInfo);
CHECK_SLOT(GIDeviceMethods, status, IGameInputDeviceVtbl, GetDeviceStatus);
CHECK_SLOT(GIDeviceMethods, rumble, IGameInputDeviceVtbl, SetRumbleState);
CHECK_SLOT(GIReadingMethods, gamepad, IGameInputReadingVtbl, GetGamepadState);
CHECK_SLOT(GIReadingMethods, device, IGameInputReadingVtbl, GetDevice);
CHECK_SLOT(GIInfoPrefix, product, GameInputDeviceInfo, productId);
CHECK_SLOT(GIInfoPrefix, rumble_motors, GameInputDeviceInfo, supportedRumbleMotors);
CHECK_SLOT(GIState, ry, GameInputGamepadState, rightThumbstickY);
_Static_assert(sizeof(GIState) == sizeof(GameInputGamepadState), "GameInputGamepadState");
_Static_assert(sizeof(GIRumble) == sizeof(GameInputRumbleParams), "GameInputRumbleParams");
#endif

typedef struct {
    GIDevice device;
    unsigned refs;
    GIInfoPrefix info;
    GIRumble rumble;
    GIState state;
} Device;
static bool readable = true;
static Device *reading_device;
static Device *awake;
static ULONG WINAPI retain(GIDevice *d) { return ++((Device *)d)->refs; }
static ULONG WINAPI release(GIDevice *d) { assert(((Device *)d)->refs > 0); return --((Device *)d)->refs; }
static const GIInfoPrefix *WINAPI info(GIDevice *d) { return &((Device *)d)->info; }
static unsigned WINAPI status(GIDevice *d) { return connected; }
static void WINAPI rumble(GIDevice *d, const GIRumble *r) { ((Device *)d)->rumble = *r; }
static ULONG WINAPI release_reading(GIReading *r) { return 0; }
static bool WINAPI read_pad(GIReading *r, GIState *s) { *s = reading_device->state; return readable; }
static void WINAPI owner(GIReading *r, GIDevice **out) { *out=&reading_device->device; retain(*out); }
static HRESULT WINAPI read_state(GI *api, unsigned kind, GIDevice *d, GIReading **out) {
    static const GIReadingMethods methods = {.release=release_reading,.device=owner,.gamepad=read_pad};
    static GIReading reading = {&methods};
    assert(kind == gamepad_kind);
    reading_device = d ? (Device *)d : awake;
    if (!reading_device) return E_FAIL;
    *out = &reading;
    return S_OK;
}

#ifdef MK8_TEST_TITLE_INPUT
extern void check_gameinput_title(SDL_JoystickID, SDL_JoystickID, uint32_t *);
int steam_gamepads_title_test(void) {
#else
int main(void) {
#endif
    assert(mk8_init_window_input() && !input);
    static const GIMethods api_methods = {.read=read_state};
    GI api = {&api_methods};
    input = &api;
    const GIDeviceMethods methods = {.retain=retain,.release=release,.info=info,.status=status,.rumble=rumble};
    Device first = {.device={&methods},.info={.size=sizeof(GIInfoPrefix),.vendor=0x3537,.product=0x1040}};
    Device second = {.device={&methods},.info={.size=sizeof(GIInfoPrefix),.vendor=0x4321,.product=0x1234,.rumble_motors=3}};
    awake=&first; // Connected before its notification: recover from a real reading.
    mk8_poll_steam_gamepads();
    assert(pads[0].id && first.refs==2);
    device_changed(0,NULL,&first.device,0,connected,0);
    device_changed(0,NULL,&second.device,0,connected,0);
    mk8_poll_steam_gamepads();
    assert(pads[0].id && pads[1].id && first.refs==2 && second.refs==2);
    SDL_Gamepad *p1=SDL_OpenGamepad(pads[0].id), *p2=SDL_OpenGamepad(pads[1].id);
    assert(p1 && p2);
#ifdef MK8_TEST_TITLE_INPUT
    check_gameinput_title(pads[0].id, pads[1].id, &second.state.buttons);
#endif
    first.state=(GIState){.buttons=0xc00,.lx=-1,.ly=1,.rt=1};
    second.state=(GIState){.buttons=4,.lx=1};
    SDL_UpdateJoysticks();
    assert(SDL_GetGamepadButton(p1,SDL_GAMEPAD_BUTTON_LEFT_SHOULDER));
    assert(SDL_GetGamepadButton(p1,SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER));
    assert(SDL_GetGamepadAxis(p1,SDL_GAMEPAD_AXIS_LEFTX)==-32768);
    assert(SDL_GetGamepadAxis(p1,SDL_GAMEPAD_AXIS_LEFTY)==-32768);
    assert(SDL_GetGamepadAxis(p1,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)==32767);
    assert(SDL_GetGamepadAxis(p1,SDL_GAMEPAD_AXIS_LEFT_TRIGGER)==0);
    assert(SDL_GetGamepadButton(p2,SDL_GAMEPAD_BUTTON_SOUTH));
    assert(SDL_GetGamepadAxis(p2,SDL_GAMEPAD_AXIS_LEFTX)==32767);
    assert(SDL_RumbleGamepad(p2,65535,0,200));
    assert(!SDL_RumbleGamepad(p1,65535,0,200));
    assert(second.rumble.low==1 && second.rumble.high==0 && first.rumble.low==0);
    readable=false;
    SDL_UpdateJoysticks();
    assert(!SDL_GetGamepadButton(p1,SDL_GAMEPAD_BUTTON_LEFT_SHOULDER));
    assert(SDL_GetGamepadAxis(p1,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)==0);
    readable=true;
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type=SDL_JOYSTICK_TYPE_GAMEPAD; desc.naxes=6; desc.nbuttons=15;
    desc.vendor_id=first.info.vendor; desc.product_id=first.info.product;
    SDL_JoystickID native=SDL_AttachVirtualJoystick(&desc);
    assert(native);
    InterlockedExchange(&dirty,1);
    mk8_poll_steam_gamepads();
    assert(!pads[0].id && pads[1].id==SDL_GetGamepadID(p2));
    assert(SDL_DetachVirtualJoystick(native));
    InterlockedExchange(&dirty,1);
    mk8_poll_steam_gamepads();
    assert(pads[0].id && first.refs==2);
    device_changed(0,NULL,&first.device,0,0,connected);
    device_changed(0,NULL,&second.device,0,0,connected);
    awake=NULL;
    mk8_poll_steam_gamepads();
    assert(!pads[0].id && !pads[1].id && !first.refs && !second.refs);
    assert(second.rumble.low==0 && second.rumble.high==0);
    SDL_CloseGamepad(p1); SDL_CloseGamepad(p2);
    SDL_Quit();
    return 0;
}
