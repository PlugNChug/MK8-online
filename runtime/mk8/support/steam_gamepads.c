#include "gameinput_v0.h"
#include "steam_motion_windows.h"
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
static bool steam_motion_enabled;
static SDL_JoystickID motion_target;
static bool motion_ambiguous, steam_pro_seen, steam_pro_mirrors;

// Unidentified Steam/XInput endpoints need to be opened to receive SDL events.
// Keep idle endpoints out of the player slots while Windows sees real pads.
typedef struct {
    SDL_Gamepad *pad;
    SDL_JoystickID id;
    bool active, deferred, check_owner;
    GIDevice *owner;
    SDL_Event activation;
} SteamEndpoint;
static SteamEndpoint endpoints[max_players];
static SDL_JoystickID player_source[max_players];
extern void mk8_move_gamepad(SDL_JoystickID old_id, SDL_JoystickID new_id, bool prefer_earlier_slot);

static SteamEndpoint *endpoint(SDL_JoystickID id) {
    for (int i = 0; i < max_players; ++i)
        if (endpoints[i].id == id) return &endpoints[i];
    return NULL;
}

static bool steam_virtual(SDL_JoystickID id) {
    if (!id) return false;
    Uint16 vendor = 0, product = 0;
    SDL_GetJoystickGUIDInfo(SDL_GetGamepadGUIDForID(id), &vendor, &product, NULL, NULL);
    return vendor == 0x28de && product == 0x11ff;
}

bool mk8_steam_pad_deferred(SDL_JoystickID id) {
    SteamEndpoint *p = endpoint(id);
    return (p && p->deferred) || (steam_pro_mirrors && id != motion_target && steam_virtual(id));
}

static void announce_gamepad(SDL_JoystickID id) {
    SDL_Event added = {.type = SDL_EVENT_GAMEPAD_ADDED};
    added.gdevice.which = id;
    SDL_PushEvent(&added);
}

static void clear_endpoint(SteamEndpoint *p) {
    if (p->owner) p->owner->v->release(p->owner);
    if (p->pad) SDL_CloseGamepad(p->pad);
    *p = (SteamEndpoint){0};
}

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
    if (event->type == SDL_EVENT_GAMEPAD_ADDED || event->type == SDL_EVENT_GAMEPAD_REMOVED ||
        event->type == SDL_EVENT_GAMEPAD_STEAM_HANDLE_UPDATED)
        InterlockedExchange(&dirty, 1);
    return true;
}

static Sint16 stick(float value) {
    return (Sint16)(SDL_clamp(value, -1.0f, 1.0f) * (value < 0 ? 32768.0f : 32767.0f));
}

static GIState read_pad_state(GIDevice *device) {
    GIState state = {0};
    GIReading *r = NULL;
    if (SUCCEEDED(input->v->read(input, gamepad_kind, device, &r)) && r) {
        if (!r->v->gamepad(r, &state)) state = (GIState){0};
        r->v->release(r);
    }
    return state;
}

static const SDL_GamepadButton buttons[] = {
    SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_SOUTH,
    SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
    SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT,
    SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
    SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK
};

static bool same_control(const SDL_Event *event, GIState s) {
    if (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        for (unsigned i = 0; i < SDL_arraysize(buttons); ++i)
            if (buttons[i] == event->gbutton.button) return (s.buttons & (1u << i)) != 0;
    }
    if (event->type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
        const float axes[] = {s.lx, -s.ly, s.rx, -s.ry, s.lt, s.rt};
        unsigned axis = event->gaxis.axis;
        return axis < SDL_arraysize(axes) &&
               (event->gaxis.value > 0 ? axes[axis] > 0.25f : axes[axis] < -0.25f);
    }
    return false;
}

static void SDLCALL update_pad(void *ctx) {
    Pad *p = ctx;
    SDL_Joystick *joy = SDL_GetJoystickFromID(p->id);
    if (!joy) return;
    GIState state = read_pad_state(p->device);
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
    bool motion_target_changed = false;
    if (steam_motion_enabled) {
        bool ambiguous = false;
        SDL_JoystickID found = mk8_poll_steam_motion(&ambiguous);
        motion_target_changed = found != motion_target;
        if (found != motion_target || ambiguous != motion_ambiguous) {
            motion_target = found;
            motion_ambiguous = ambiguous;
            InterlockedExchange(&dirty, 1);
        }
    }
    if (!input) return;
    Uint64 now = SDL_GetTicks();
    // A receiver can wake up after the blocking enumeration but before its
    // connection notification arrives. The current reading identifies its owner.
    static Uint64 retry_at;
    if (!InterlockedCompareExchange(&device_count, 0, 0) && now >= retry_at) {
        retry_at = now + 1000;
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
    SDL_JoystickID native[max_players] = {0};
    bool native_pro = false, other_steam = false;
    if (motion_target) steam_pro_seen = true;
    if (steam_pro_seen) {
        for (int n = 0; ids && n < count; ++n) {
            Uint16 vendor = 0, product = 0;
            SDL_GetJoystickGUIDInfo(SDL_GetGamepadGUIDForID(ids[n]), &vendor, &product, NULL, NULL);
            if (vendor == 0x057e && product == 0x2009) native_pro = true;
            if (ids[n] == motion_target || vendor != 0x28de || product != 0x11ff) continue;
            SDL_Gamepad *mapped = SDL_OpenGamepad(ids[n]);
            if (!mapped) continue;
            if (SDL_GetGamepadSteamHandle(mapped)) other_steam = true;
            SDL_CloseGamepad(mapped);
        }
    }
    // GameInput may retain mirrors after Steam drops the SDL attachment.
    bool one_pro = steam_pro_seen && !native_pro && !motion_ambiguous;
    bool had_mirrors = steam_pro_mirrors;
    steam_pro_mirrors = one_pro && !other_steam;
    bool pro_alias[max_players] = {0};
    if (one_pro) {
        for (int i = 0; i < max_players; ++i) {
            const GIInfoPrefix *info = snapshot[i] ? snapshot[i]->v->info(snapshot[i]) : NULL;
            if (!info) continue;
            bool physical = info->vendor == 0x057e && info->product == 0x2009;
            bool mirror = info->vendor == 0x28de && info->product == 0x11ff;
            if (physical || (mirror && steam_pro_mirrors)) {
                pro_alias[i] = true;
                native[i] = motion_target;
            }
        }
    }
    for (int i = 0; i < max_players; ++i) {
        SteamEndpoint *ep = &endpoints[i];
        if (!ep->pad) continue;
        if (one_pro && ep->id == motion_target) {
            bool announce = ep->deferred;
            clear_endpoint(ep);
            if (announce) announce_gamepad(motion_target);
        } else if (!SDL_GamepadConnected(ep->pad)) clear_endpoint(ep);
    }
    for (int n = 0; ids && n < count; ++n) {
        if (ours(ids[n])) continue;
        if (ids[n] != motion_target && steam_virtual(ids[n])) {
            if (steam_pro_mirrors) {
                mk8_move_gamepad(ids[n], 0, false);
                continue;
            }
            if (had_mirrors) announce_gamepad(ids[n]);
        }
        const Uint16 vid = SDL_GetGamepadVendorForID(ids[n]), pid = SDL_GetGamepadProductForID(ids[n]);
        Uint16 raw_vid = 0, raw_pid = 0;
        SDL_GetJoystickGUIDInfo(SDL_GetGamepadGUIDForID(ids[n]), &raw_vid, &raw_pid, NULL, NULL);
        if (raw_vid == 0x28de && raw_pid == 0x11ff) {
            // SDL presents the physical controller's metadata for Steam's emulated pad.
            // GameInput presents the same endpoint as Valve's virtual device.
            bool matched = false;
            for (int i = 0; i < max_players; ++i) {
                if (!snapshot[i] || native[i] || pro_alias[i]) continue;
                const GIInfoPrefix *info = snapshot[i]->v->info(snapshot[i]);
                if (!info) continue;
                if (!matched && info->vendor == raw_vid && info->product == raw_pid) {
                    native[i] = ids[n];
                    matched = true;
                }
            }
            if (matched) continue;
        }
        // Emulated Xbox 360 has the same VID/PID as a physical Xbox 360.
        bool anonymous = !vid || vid == 0x28de || (vid == 0x045e && pid == 0x028e);
        if (anonymous) {
            if (!endpoint(ids[n])) {
                SteamEndpoint *ep = endpoint(0);
                if (ep && (ep->pad = SDL_OpenGamepad(ids[n]))) ep->id = ids[n];
            }
            continue;
        }
        for (int i = 0; i < max_players; ++i) {
            if (!snapshot[i] || native[i] || pro_alias[i]) continue;
            const GIInfoPrefix *info = snapshot[i]->v->info(snapshot[i]);
            if (info && vid == info->vendor && pid == info->product) {
                native[i] = ids[n];
                break;
            }
        }
    }
    SDL_free(ids);
    // Preserve established routes across unrelated hotplug. An anonymous SDL ID
    // alone cannot identify which of several Windows controllers it represents.
    for (int n = 0; n < max_players; ++n) {
        SteamEndpoint *ep = &endpoints[n];
        if (!ep->owner) continue;
        int i = 0;
        while (i < max_players && (snapshot[i] != ep->owner || native[i] || pro_alias[i])) ++i;
        if (i < max_players) native[i] = ep->id;
        else { ep->owner->v->release(ep->owner); ep->owner = NULL; }
    }
    for (int n = 0; n < max_players; ++n) {
        SteamEndpoint *ep = &endpoints[n];
        if (!ep->pad || ep->owner) continue;
        int candidates = 0, matches = 0, selected = -1, only_candidate = -1;
        for (int i = 0; i < max_players; ++i) {
            if (!snapshot[i] || native[i] || pro_alias[i]) continue;
            ++candidates;
            only_candidate = i;
            if (pads[i].device != snapshot[i]) ep->check_owner = true;
            if (ep->activation.type && same_control(&ep->activation, read_pad_state(snapshot[i]))) {
                ++matches;
                selected = i;
            }
        }
        bool was_deferred = ep->deferred;
        ep->deferred = candidates > 0 && (!ep->active || (was_deferred && ep->check_owner));
        // Steam can remap every button. A Steam-assigned handle and a single
        // remaining Windows pad allow that mapping to win even when A becomes B.
        if (ep->activation.type && candidates == 1 && SDL_GetGamepadSteamHandle(ep->pad)) {
            matches = 1;
            selected = only_candidate;
        }
        if (matches == 1) {
            ep->owner = snapshot[selected];
            ep->owner->v->retain(ep->owner);
            native[selected] = ep->id;
        }
        if (ep->activation.type) {
            // If both players press the same button, keep their existing routes
            // and wait for a distinguishable input before adopting the SDL ID.
            ep->check_owner = matches > 1;
            ep->deferred = ep->check_owner && was_deferred;
        }
        ep->activation.type = 0;
        if (!was_deferred && ep->deferred) mk8_move_gamepad(ep->id, 0, false);
        if (was_deferred && !ep->deferred) announce_gamepad(ep->id);
    }
    for (int i = 0; i < max_players; ++i) {
        Pad *p = &pads[i];
        GIDevice *d = snapshot[i];
        if (p->device != d || native[i] || pro_alias[i]) detach(p);
        if (d && !p->device && !native[i] && !pro_alias[i]) {
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
                d = NULL; // pad owns the snapshot reference
            } else p->device = NULL;
        }
        SDL_JoystickID next = native[i] ? native[i] : p->id;
        if (player_source[i] && next && player_source[i] != next)
            mk8_move_gamepad(player_source[i], next, pro_alias[i]);
        player_source[i] = next;
        if (d) d->v->release(d);
    }
    if (motion_target_changed && motion_target) announce_gamepad(motion_target);
}

bool mk8_steam_gamepad_event(const SDL_Event *e) {
    if (!input) return false;
    if (e->type == SDL_EVENT_GAMEPAD_ADDED || e->type == SDL_EVENT_GAMEPAD_REMOVED ||
        e->type == SDL_EVENT_GAMEPAD_STEAM_HANDLE_UPDATED) {
        mk8_poll_steam_gamepads();
        return false;
    }
    SDL_JoystickID id;
    bool activity = e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    if (activity || e->type == SDL_EVENT_GAMEPAD_BUTTON_UP) id = e->gbutton.which;
    else if (e->type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
        id = e->gaxis.which;
        activity = SDL_abs((int)e->gaxis.value) > 8192;
    } else return false;
    SteamEndpoint *ep = endpoint(id);
    if (ep && activity && (!ep->active || ep->check_owner)) {
        ep->active = true;
        ep->activation = *e;
        InterlockedExchange(&dirty, 1);
        mk8_poll_steam_gamepads();
    }
    return mk8_steam_pad_deferred(id);
}

static void stop_input(void) {
    if (subscription && !input->v->unsubscribe(input, subscription, UINT64_MAX)) return;
    subscription = 0;
    SDL_RemoveEventWatch(watch_devices, NULL);
    for (int i = 0; i < max_players; ++i) {
        clear_endpoint(&endpoints[i]);
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
    if (!steam_motion_enabled) {
        steam_motion_enabled = true;
        atexit(mk8_stop_steam_motion);
    }
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
