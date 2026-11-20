#include "gameinput_v0.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

enum { max_players = 4, gamepad_kind = 0x40000, connected = 1, enumerate_existing = 2 };
static HMODULE library;
static GI *input;
static uint64_t subscription;
static SRWLOCK device_lock = SRWLOCK_INIT;
static GIDevice *devices[max_players];
static LONG dirty;
static LONG device_count;

typedef struct { GIDevice *device; SDL_JoystickID id; } Pad;
static Pad pads[max_players];

// GameInput delivers device changes on its own thread. Only the SDL window
// thread attaches or detaches joysticks. Each side holds its own COM reference.
static void CALLBACK device_changed(uint64_t token, void *ctx, GIDevice *device, uint64_t time,
                                    unsigned status, unsigned previous) {
    AcquireSRWLockExclusive(&device_lock);
    int empty = -1;
    for (int i = 0; i < max_players; ++i) {
        if (devices[i] == device) {
            if (!(status & connected)) {
                devices[i] = NULL;
                InterlockedDecrement(&device_count);
                device->v->release(device);
            }
            goto done;
        }
        if (!devices[i] && empty < 0) empty = i;
    }
    if ((status & connected) && empty >= 0) {
        device->v->retain(device);
        devices[empty] = device;
        InterlockedIncrement(&device_count);
    }
done:
    ReleaseSRWLockExclusive(&device_lock);
    InterlockedExchange(&dirty, 1);
}

static bool SDLCALL watch_devices(void *ctx, SDL_Event *event) {
    if (event->type == SDL_EVENT_GAMEPAD_ADDED || event->type == SDL_EVENT_GAMEPAD_REMOVED)
        InterlockedExchange(&dirty, 1);
    return true;
}

static Sint16 stick(float value) {
    return (Sint16)(SDL_clamp(value, -1.0f, 1.0f) * (value < 0 ? 32768.0f : 32767.0f));
}

static void SDLCALL update_pad(void *ctx) {
    Pad *p = ctx;
    SDL_Joystick *joy = SDL_GetJoystickFromID(p->id);
    if (!joy) return;
    GIState state = {0};
    GIReading *r = NULL;
    if (SUCCEEDED(input->v->read(input, gamepad_kind, p->device, &r)) && r) {
        if (!r->v->gamepad(r, &state)) state = (GIState){0};
        r->v->release(r);
    }
    const int buttons[] = {
        SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_SOUTH,
        SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
        SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT,
        SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
        SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK
    };
    for (unsigned i = 0; i < SDL_arraysize(buttons); ++i)
        SDL_SetJoystickVirtualButton(joy, buttons[i], (state.buttons & (1u << i)) != 0);
    const float axes[] = {state.lx, -state.ly, state.rx, -state.ry, 2 * state.lt - 1, 2 * state.rt - 1};
    for (unsigned i = 0; i < SDL_arraysize(axes); ++i)
        SDL_SetJoystickVirtualAxis(joy, i, stick(axes[i]));
}

static bool SDLCALL rumble_pad(void *ctx, Uint16 low, Uint16 high) {
    Pad *p = ctx;
    GIRumble rumble = {low / 65535.0f, high / 65535.0f, 0, 0};
    p->device->v->rumble(p->device, &rumble);
    return true;
}

static void detach(Pad *p) {
    if (!p->device) return;
    if (p->id && SDL_WasInit(SDL_INIT_JOYSTICK)) {
        SDL_Joystick *joy = SDL_GetJoystickFromID(p->id);
        if (joy) SDL_RumbleJoystick(joy, 0, 0, 0);
        SDL_DetachVirtualJoystick(p->id);
    }
    p->device->v->release(p->device);
    *p = (Pad){0};
}

static bool ours(SDL_JoystickID id) {
    for (int i = 0; i < max_players; ++i)
        if (pads[i].id == id) return true;
    return false;
}

void mk8_poll_steam_gamepads(void) {
    if (!input) return;
    // A receiver can wake up after the blocking enumeration but before its
    // connection notification arrives. The current reading identifies its owner.
    static Uint64 retry_at;
    if (!InterlockedCompareExchange(&device_count, 0, 0) && SDL_GetTicks() >= retry_at) {
        retry_at = SDL_GetTicks() + 1000;
        GIReading *reading = NULL;
        if (SUCCEEDED(input->v->read(input, gamepad_kind, NULL, &reading)) && reading) {
            GIDevice *d = NULL;
            reading->v->device(reading, &d);
            if (d) {
                if (d->v->status(d) & connected) device_changed(0, NULL, d, 0, connected, 0);
                d->v->release(d);
            }
            reading->v->release(reading);
        }
    }
    if (!InterlockedExchange(&dirty, 0)) return;
    GIDevice *snapshot[max_players];
    AcquireSRWLockShared(&device_lock);
    for (int i = 0; i < max_players; ++i) {
        snapshot[i] = devices[i];
        if (snapshot[i]) snapshot[i]->v->retain(snapshot[i]);
    }
    ReleaseSRWLockShared(&device_lock);

    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    bool claimed[max_players] = {0};
    int generic = 0;
    for (int n = 0; ids && n < count; ++n) {
        if (ours(ids[n])) continue;
        const Uint16 vid = SDL_GetGamepadVendorForID(ids[n]), pid = SDL_GetGamepadProductForID(ids[n]);
        bool matched = false;
        for (int i = 0; i < max_players && !matched; ++i) {
            if (!snapshot[i] || claimed[i]) continue;
            const GIInfoPrefix *info = snapshot[i]->v->info(snapshot[i]);
            if (info && vid && vid == info->vendor && pid == info->product) matched = claimed[i] = true;
        }
        // Steam emulation or generic XInput may conceal the physical VID/PID.
        if (!matched && (!vid || vid == 0x28de || (vid == 0x045e && pid == 0x028e))) ++generic;
    }
    SDL_free(ids);
    for (int i = 0; i < max_players; ++i) {
        Pad *p = &pads[i];
        GIDevice *d = snapshot[i];
        if (d && !claimed[i] && generic > 0) { --generic; claimed[i] = true; }
        if (p->device != d || claimed[i]) detach(p);
        if (d && !p->device && !claimed[i]) {
            SDL_VirtualJoystickDesc desc;
            const GIInfoPrefix *info = d->v->info(d);
            SDL_INIT_INTERFACE(&desc);
            desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
            desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
            desc.nbuttons = SDL_GAMEPAD_BUTTON_DPAD_RIGHT + 1;
            desc.name = "Windows GameInput Controller";
            desc.userdata = p;
            desc.Update = update_pad;
            if (info && info->size >= sizeof(*info) && (info->rumble_motors & 3))
                desc.Rumble = rumble_pad;
            p->device = d;
            p->id = SDL_AttachVirtualJoystick(&desc);
            if (p->id) {
                fprintf(stderr, "[input] Windows GameInput connected (%04x:%04x)\n",
                        info ? info->vendor : 0, info ? info->product : 0);
                continue; // pad owns the snapshot reference
            }
            p->device = NULL;
        }
        if (d) d->v->release(d);
    }
}

static void stop_input(void) {
    if (subscription && !input->v->unsubscribe(input, subscription, UINT64_MAX)) return;
    subscription = 0;
    SDL_RemoveEventWatch(watch_devices, NULL);
    for (int i = 0; i < max_players; ++i) {
        detach(&pads[i]);
        if (devices[i]) {
            devices[i]->v->release(devices[i]);
            devices[i] = NULL;
        }
    }
    if (input) input->v->release(input);
    if (library) FreeLibrary(library);
    input = NULL;
    library = NULL;
}

bool mk8_init_window_input(void) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) return false;
    if (input || (!SDL_getenv("SteamAppId") && !SDL_getenv("SteamGameId"))) return true;
    library = LoadLibraryExW(L"GameInput.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HRESULT (WINAPI *create)(GI **) = library ? (void *)GetProcAddress(library, "GameInputCreate") : NULL;
    if (create && SUCCEEDED(create(&input)) && input &&
        SUCCEEDED(input->v->subscribe(input, NULL, gamepad_kind, connected, enumerate_existing,
                                      NULL, device_changed, &subscription))) {
        SDL_AddEventWatch(watch_devices, NULL);
        atexit(stop_input);
        fprintf(stderr, "[input] Windows GameInput monitoring Steam controller connections\n");
        mk8_poll_steam_gamepads();
    } else {
        fprintf(stderr, "[input] Windows GameInput unavailable; listening for SDL controllers\n");
        stop_input();
    }
    return true;
}
