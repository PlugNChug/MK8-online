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

static SDL_JoystickID fake_motion_target;
SDL_JoystickID mk8_poll_steam_motion(void) { return fake_motion_target; }
void mk8_stop_steam_motion(void) {}

// Use the actual prepared player-slot discovery and backend handoff with SDL
// devices. Only game notifications and guest rumble bookkeeping are stubbed.
#ifndef MK8_TEST_TITLE_INPUT
#define EXTRA_PADS 3
#define LOG(...) ((void)0)
static SDL_Gamepad *pad, *extra_pad[EXTRA_PADS];
static bool real_gamepad(SDL_JoystickID id) { return SDL_IsGamepad(id) && !mk8_steam_pad_deferred(id); }
static bool pads_from_p2(void) { return false; }
static SDL_Gamepad *mk8_rumble_open(int player, SDL_JoystickID id) { return SDL_OpenGamepad(id); }
static void mk8_rumble_close(int player, SDL_Gamepad *p) { SDL_CloseGamepad(p); }
static void recompute_extra(int i) {}
static void recompute_pad(void) {}
static void style_changed(void) {}
static void note_family(void) {}
static void open_extra(SDL_JoystickID id) {
    for (int i=0; i<EXTRA_PADS; ++i)
        if (!extra_pad[i]) { extra_pad[i]=SDL_OpenGamepad(id); return; }
}
#include "slots.inc"
#endif

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
    SDL_VirtualJoystickDesc silent;
    SDL_INIT_INTERFACE(&silent);
    silent.type=SDL_JOYSTICK_TYPE_GAMEPAD; silent.naxes=6; silent.nbuttons=15;
    silent.vendor_id=0x045e; silent.product_id=0x028e;
    SDL_JoystickID ghost=SDL_AttachVirtualJoystick(&silent);
    assert(ghost);
    InterlockedExchange(&dirty,1);
    mk8_poll_steam_gamepads();
    assert(mk8_steam_pad_deferred(ghost));
    check_gameinput_title(pads[0].id, pads[1].id, &second.state.buttons);
    assert(SDL_DetachVirtualJoystick(ghost));
    InterlockedExchange(&dirty,1);
    device_changed(0,NULL,&first.device,0,0,connected);
    device_changed(0,NULL,&second.device,0,0,connected);
    awake=NULL;
    mk8_poll_steam_gamepads();
    assert(!first.refs && !second.refs);
#else
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
    // Steam can enumerate a silent Xbox endpoint while the physical receiver
    // still delivers L+R through Windows. Neither player's slot may be stolen.
    SDL_JoystickID fallback=pads[0].id, other=pads[1].id;
    SDL_Gamepad *receiver=SDL_OpenGamepad(fallback);
    desc.vendor_id=0x045e; desc.product_id=0x028e;
    SDL_JoystickID generic=SDL_AttachVirtualJoystick(&desc);
    assert(generic && receiver);
    SDL_Event added={.type=SDL_EVENT_GAMEPAD_ADDED}; added.gdevice.which=generic;
    InterlockedExchange(&dirty,1);
    mk8_steam_gamepad_event(&added);
    mk8_open_gamepad(generic); mk8_open_gamepad(fallback); mk8_open_gamepad(other);
    assert(SDL_GetGamepadID(pad)==fallback && SDL_GetGamepadID(extra_pad[0])==other && !extra_pad[1]);
    SDL_UpdateJoysticks();
    assert(mk8_steam_pad_deferred(generic) && pads[0].id==fallback && pads[1].id==other);
    assert(SDL_GetGamepadButton(receiver,SDL_GAMEPAD_BUTTON_LEFT_SHOULDER));
    assert(SDL_GetGamepadButton(receiver,SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER));
    assert(SDL_GetGamepadButton(p2,SDL_GAMEPAD_BUTTON_SOUTH));
    // Small idle drift must not take a player slot.
    SDL_Event drift={.type=SDL_EVENT_GAMEPAD_AXIS_MOTION};
    drift.gaxis.which=generic; drift.gaxis.value=500;
    assert(mk8_steam_gamepad_event(&drift));
    assert(mk8_steam_pad_deferred(generic));
    // Native input starts working. Use its mapping and hand off P1 in place.
    SDL_Joystick *emulated=SDL_GetJoystickFromID(generic);
    second.state.buttons |= 1u << 11;
    assert(SDL_SetJoystickVirtualButton(emulated,SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,true));
    SDL_UpdateJoysticks();
    SDL_Event e;
    while (SDL_PollEvent(&e))
        if (e.type==SDL_EVENT_GAMEPAD_BUTTON_DOWN && e.gbutton.which==generic) assert(mk8_steam_gamepad_event(&e));
    assert(mk8_steam_pad_deferred(generic) && SDL_GetGamepadID(pad)==fallback && SDL_GetGamepadID(extra_pad[0])==other);
    second.state.buttons &= ~(1u << 11);
    assert(SDL_SetJoystickVirtualButton(emulated,SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,false));
    assert(emulated && SDL_SetJoystickVirtualButton(emulated,SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,true));
    SDL_UpdateJoysticks();
    bool down=false;
    while (SDL_PollEvent(&e)) {
        if (e.type==SDL_EVENT_GAMEPAD_BUTTON_DOWN && e.gbutton.which==generic) {
            assert(!mk8_steam_gamepad_event(&e)); down=true;
        }
    }
    assert(down && !mk8_steam_pad_deferred(generic));
    assert(!pads[0].id && pads[1].id==other && SDL_GetGamepadID(pad)==generic);
    mk8_open_gamepad(generic);
    assert(SDL_GetGamepadID(extra_pad[0])==other && !extra_pad[1]);
    assert(SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER));
    assert(!SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER));
    assert(SDL_DetachVirtualJoystick(generic));
    InterlockedExchange(&dirty,1);
    mk8_poll_steam_gamepads();
    assert(pads[0].id && pads[1].id==other && SDL_GetGamepadID(pad)==pads[0].id);
    assert(SDL_GetGamepadID(extra_pad[0])==other && !extra_pad[1]);
    // A third SDL controller pressing a different button is independent.
    SDL_JoystickID independent=SDL_AttachVirtualJoystick(&desc);
    assert(independent);
    InterlockedExchange(&dirty,1);
    mk8_poll_steam_gamepads();
    assert(mk8_steam_pad_deferred(independent));
    SDL_Joystick *third=SDL_GetJoystickFromID(independent);
    assert(third && SDL_SetJoystickVirtualButton(third,SDL_GAMEPAD_BUTTON_EAST,true));
    SDL_UpdateJoysticks();
    while (SDL_PollEvent(&e)) {
        mk8_steam_gamepad_event(&e);
        if (e.type==SDL_EVENT_GAMEPAD_ADDED && e.gdevice.which==independent)
            mk8_open_gamepad(independent);
    }
    // SDL can postpone events pushed during polling until the next pump.
    while (SDL_PollEvent(&e))
        if (e.type==SDL_EVENT_GAMEPAD_ADDED && e.gdevice.which==independent)
            mk8_open_gamepad(independent);
    assert(SDL_GetGamepadID(pad)==pads[0].id && SDL_GetGamepadID(extra_pad[0])==other);
    assert(SDL_GetGamepadID(extra_pad[1])==independent && SDL_GetGamepadButton(extra_pad[1],SDL_GAMEPAD_BUTTON_EAST));
    SDL_CloseGamepad(extra_pad[1]); extra_pad[1]=NULL;
    assert(SDL_DetachVirtualJoystick(independent));
    SDL_CloseGamepad(receiver);
    SDL_CloseGamepad(pad); SDL_CloseGamepad(extra_pad[0]);
    pad=NULL; extra_pad[0]=NULL;
    device_changed(0,NULL,&first.device,0,0,connected);
    device_changed(0,NULL,&second.device,0,0,connected);
    awake=NULL;
    mk8_poll_steam_gamepads();
    assert(!pads[0].id && !pads[1].id && !first.refs && !second.refs);
    assert(second.rumble.low==0 && second.rumble.high==0);
    // Reverse startup order: even when the receiver and emulation expose the
    // same Xbox VID/PID, an idle endpoint must yield P1 to the new receiver.
    generic=SDL_AttachVirtualJoystick(&desc);
    assert(generic);
    InterlockedExchange(&dirty,1);
    mk8_poll_steam_gamepads();
    mk8_open_gamepad(generic);
    assert(SDL_GetGamepadID(pad)==generic);
    first.info.vendor=desc.vendor_id; first.info.product=desc.product_id;
    device_changed(0,NULL,&first.device,0,connected,0);
    mk8_poll_steam_gamepads();
    assert(!pad && mk8_steam_pad_deferred(generic));
    mk8_open_gamepad(pads[0].id);
    SDL_UpdateJoysticks();
    assert(SDL_GetGamepadButton(pad,SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) && !extra_pad[0]);
    device_changed(0,NULL,&first.device,0,0,connected);
    mk8_poll_steam_gamepads();
    assert(!first.refs && !mk8_steam_pad_deferred(generic));
    assert(SDL_DetachVirtualJoystick(generic));
    InterlockedExchange(&dirty,1); mk8_poll_steam_gamepads();
    SDL_CloseGamepad(pad);
    pad = NULL;

    // A GameInput record for Steam's own virtual endpoint must not make a second player.
    desc.vendor_id = 0x28de;
    desc.product_id = 0x11ff;
    first.info.vendor = desc.vendor_id;
    first.info.product = desc.product_id;
    second.info.vendor = 0x057e;
    second.info.product = 0x2009;
    Device duplicate = {.device={&methods},.info=first.info};
    steam_motion_enabled = true;
    device_changed(0,NULL,&first.device,0,connected,0);
    device_changed(0,NULL,&second.device,0,connected,0);
    device_changed(0,NULL,&duplicate.device,0,connected,0);
    mk8_poll_steam_gamepads();
    assert(pads[0].id && pads[1].id && pads[2].id);
    mk8_open_gamepad(pads[0].id);
    mk8_open_gamepad(pads[1].id);
    SDL_JoystickID steam = SDL_AttachVirtualJoystick(&desc);
    assert(steam);
    // The SDL mapping can be added after GameInput has occupied the earlier slots.
    mk8_open_gamepad(steam);
    assert(SDL_GetGamepadID(extra_pad[1]) == steam);
    fake_motion_target = steam;
    mk8_poll_steam_gamepads();
    assert(!pads[0].id && !pads[1].id && !pads[2].id);
    assert(SDL_GetGamepadID(pad) == steam && !extra_pad[0] && !extra_pad[1]);
    SDL_CloseGamepad(pad);
    pad = NULL;
    device_changed(0,NULL,&first.device,0,0,connected);
    device_changed(0,NULL,&second.device,0,0,connected);
    device_changed(0,NULL,&duplicate.device,0,0,connected);
    fake_motion_target = 0;
    assert(SDL_DetachVirtualJoystick(steam));
    mk8_poll_steam_gamepads();
    assert(!first.refs && !second.refs && !duplicate.refs);
    // A Valve mirror can return before Steam has a new SDL instance.
    device_changed(0,NULL,&duplicate.device,0,connected,0);
    mk8_poll_steam_gamepads();
    assert(!pads[0].id && !pads[1].id && !pads[2].id);
    steam = SDL_AttachVirtualJoystick(&desc);
    assert(steam);
    fake_motion_target = steam;
    device_changed(0,NULL,&second.device,0,connected,0);
    mk8_poll_steam_gamepads();
    mk8_open_gamepad(steam);
    assert(!pads[0].id && SDL_GetGamepadID(pad) == steam && !extra_pad[0]);
    SDL_CloseGamepad(pad);
    pad = NULL;
    device_changed(0,NULL,&second.device,0,0,connected);
    device_changed(0,NULL,&duplicate.device,0,0,connected);
    fake_motion_target = 0;
    assert(SDL_DetachVirtualJoystick(steam));
    mk8_poll_steam_gamepads();
    assert(!second.refs && !duplicate.refs);
#endif
    SDL_CloseGamepad(p1); SDL_CloseGamepad(p2);
    SDL_Quit();
    return 0;
}
