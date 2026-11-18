#include "ipc.h"
#include "ui/menu.h"

typedef struct {
    float low, low_hz, high, high_hz;
} Mk8Vibration;

static const Mk8Vibration mk8_silent = {0, 160, 0, 320};
static bool mk8_rumble_permitted = true;
static bool mk8_rumble_focused = true;

typedef struct {
    Uint8 report, sequence, side[2][4];
} Mk8SwitchRumble;

static const Mk8SwitchRumble mk8_switch_neutral = {0x10, 0, {{0, 1, 0x40, 0x40}, {0, 1, 0x40, 0x40}}};
static Uint64 mk8_switch_reopen_after;

// SDL's joystick lock also protects the player assignments and guest readback.
static struct Mk8RumblePad {
    SDL_JoystickID id;
    Mk8Vibration side[2];
    Uint16 low, high;
    Uint64 sent_at;
    bool hd, stop_first;
    Mk8SwitchRumble wanted, last;
    Uint64 expires_at;
    SDL_TimerID timer;
} mk8_rumble_pads[1 + EXTRA_PADS];

static SDL_Gamepad *mk8_rumble_open(int player, SDL_JoystickID id) {
    SDL_LockJoysticks();
    SDL_Gamepad *device = SDL_OpenGamepad(id);
    if (device && SDL_GetBooleanProperty(SDL_GetGamepadProperties(device), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false)) {
        Uint16 product = SDL_GetGamepadProduct(device);
        // SDL 3.4 marks HIDAPI GUIDs with 'h'. Its combined Joy-Con driver rejects raw effects.
        bool native = SDL_GetJoystickGUIDForID(id).data[14] == 'h' && SDL_GetGamepadVendor(device) == 0x057e &&
                      (product == 0x2009 || product == 0x2006 || product == 0x2007);
        // A settings rescan can move a just-stopped device into another slot.
        Uint64 now = SDL_GetTicks();
        if (native && now < mk8_switch_reopen_after) SDL_Delay((Uint32)(mk8_switch_reopen_after - now));
        mk8_rumble_pads[player] = (struct Mk8RumblePad){.id = id, .side = {mk8_silent, mk8_silent},
                                                       .hd = native, .wanted = mk8_switch_neutral};
    }
    SDL_UnlockJoysticks();
    return device;
}

static bool mk8_switch_playing(const struct Mk8RumblePad *p) {
    return p->last.report && memcmp(&p->last, &mk8_switch_neutral, sizeof(p->last));
}

static void mk8_rumble_basic(struct Mk8RumblePad *p, Uint64 now) {
    Uint16 low = (Uint16)(65535.f * fmaxf(p->side[0].low, p->side[1].low) + .5f);
    Uint16 high = (Uint16)(65535.f * fmaxf(p->side[0].high, p->side[1].high) + .5f);
    if (low == p->low && high == p->high && ((!low && !high) || now - p->sent_at < 125)) return;
    SDL_Gamepad *device = SDL_GetGamepadFromID(p->id);
    if (!device || !SDL_RumbleGamepad(device, low, high, (low || high) ? 250 : 0)) {
        if (device && (p->low || p->high)) SDL_RumbleGamepad(device, 0, 0, 0);
        p->side[0] = p->side[1] = mk8_silent;
        low = high = 0;
    }
    p->low = low;
    p->high = high;
    p->sent_at = now;
}

static void mk8_switch_write(struct Mk8RumblePad *p, Uint64 now) {
    if (p->expires_at && now >= p->expires_at) {
        p->side[0] = p->side[1] = mk8_silent;
        p->wanted = mk8_switch_neutral;
        p->expires_at = 0;
    }
    const Mk8SwitchRumble *next = p->stop_first ? &mk8_switch_neutral : &p->wanted;
    bool active = memcmp(next, &mk8_switch_neutral, sizeof(*next)) != 0;
    Uint64 elapsed = now - p->sent_at;
    // The Switch driver allows one write per 30 ms and refreshes held effects every 50 ms.
    if (!active && !mk8_switch_playing(p)) return;
    if (p->last.report && elapsed < 30) return;
    if (active && !memcmp(next, &p->last, sizeof(*next)) && elapsed < 45) return;
    SDL_Gamepad *device = SDL_GetGamepadFromID(p->id);
    if (device && SDL_SendGamepadEffect(device, next, sizeof(*next))) {
        p->last = *next;
        p->stop_first = false;
    } else {
        // Give the previous HD pulse one neutral retry, then stop refreshing it and use basic output.
        p->stop_first = p->hd && device && mk8_switch_playing(p);
        p->hd = false;
        if (!p->stop_first) {
            if (device && mk8_switch_playing(p)) SDL_RumbleGamepad(device, 0, 0, 0);
            p->last = mk8_switch_neutral;
            mk8_rumble_basic(p, now);
        }
    }
    p->sent_at = SDL_GetTicks();
}

static Uint32 SDLCALL mk8_switch_timer(void *userdata, SDL_TimerID timer, Uint32 interval) {
    struct Mk8RumblePad *p = userdata;
    SDL_LockJoysticks();
    Uint32 next = 0;
    if (p->timer == timer) {
        Uint64 now = SDL_GetTicks();
        if (p->hd || p->stop_first) mk8_switch_write(p, now);
        if ((p->hd && p->expires_at) || p->stop_first || mk8_switch_playing(p)) {
            next = interval;
        } else {
            p->timer = 0;
            if (!p->hd) mk8_rumble_basic(p, now);
        }
    }
    SDL_UnlockJoysticks();
    return next;
}

static void mk8_rumble_stop(struct Mk8RumblePad *p) {
    p->side[0] = p->side[1] = mk8_silent;
    p->wanted = mk8_switch_neutral;
    p->expires_at = 0;
    p->stop_first = mk8_switch_playing(p);
    if (p->stop_first) mk8_switch_write(p, SDL_GetTicks());
    if (p->low || p->high) {
        SDL_Gamepad *device = SDL_GetGamepadFromID(p->id);
        if (device) SDL_RumbleGamepad(device, 0, 0, 0);
    }
    p->low = p->high = 0;
}

static void mk8_rumble_close(int player, SDL_Gamepad *device) {
    SDL_LockJoysticks();
    struct Mk8RumblePad *p = &mk8_rumble_pads[player];
    if (p->id && p->id == SDL_GetGamepadID(device)) {
        mk8_rumble_stop(p);
        if (p->stop_first) {
            // Reassignment closes the handle. Give its final neutral a valid write slot first.
            Uint64 elapsed = SDL_GetTicks() - p->sent_at;
            if (elapsed < 30) SDL_Delay((Uint32)(30 - elapsed));
            mk8_switch_write(p, SDL_GetTicks());
        }
        if (p->timer) SDL_RemoveTimer(p->timer);
        if (p->last.report && p->sent_at + 30 > mk8_switch_reopen_after) mk8_switch_reopen_after = p->sent_at + 30;
        p->timer = 0;
        p->id = 0;
    }
    SDL_CloseGamepad(device);
    SDL_UnlockJoysticks();
}

static struct Mk8RumblePad *mk8_rumble_pad(uint32_t handle) {
    unsigned player = (handle >> 8) & 255;
    if ((handle & 0xfffe00ffu) != 3 || player >= 1 + EXTRA_PADS) return NULL;
    struct Mk8RumblePad *p = &mk8_rumble_pads[player];
    return p->id ? p : NULL;
}

static float mk8_amplitude(float value) {
    return isfinite(value) ? fminf(1.0f, fmaxf(0.0f, value)) : 0.0f;
}

static int mk8_switch_level(float amplitude) {
    if (amplitude < .003922f) return 0;
    if (amplitude < .0097f) return 1;
    // Invert the three slopes of the measured actuator table. Index 100 is the last safe entry.
    float code;
    if (amplitude < .112491f) code = 15 + 4 * log2f(amplitude / .112491f);
    else if (amplitude < .224982f) code = 15 + 16 * log2f(amplitude / .112491f);
    else code = 31 + 32 * log2f(amplitude / .224982f);
    return (int)fminf(100, fmaxf(1, roundf(code)));
}

static int mk8_switch_frequency(float hz, int base) {
    float code = roundf(32 * log2f(fmaxf(hz, 40.f) / 10.f)) - base;
    return (int)fminf(127, fmaxf(1, code));
}

static void mk8_switch_actuator(Uint8 out[4], Mk8Vibration value) {
    int low = isfinite(value.low_hz) && value.low_hz > 0 ? mk8_switch_level(value.low) : 0;
    int high = isfinite(value.high_hz) && value.high_hz > 0 ? mk8_switch_level(value.high) : 0;
    int lf = low ? mk8_switch_frequency(value.low_hz, 64) : 64;
    int hf = high ? 4 * mk8_switch_frequency(value.high_hz, 96) : 256;
    out[0] = (Uint8)hf;
    out[1] = (Uint8)((hf >> 8) | (high << 1));
    out[2] = (Uint8)(lf | ((low & 1) << 7));
    out[3] = (Uint8)(64 + (low >> 1));
}

static void mk8_rumble_apply(struct Mk8RumblePad *p, Uint64 now) {
    if (!p->hd) {
        if (!p->stop_first) mk8_rumble_basic(p, now);
        return;
    }
    for (int side = 0; side < 2; side++) mk8_switch_actuator(p->wanted.side[side], p->side[side]);
    bool active = memcmp(&p->wanted, &mk8_switch_neutral, sizeof(p->wanted)) != 0;
    if (!active && mk8_switch_playing(p)) p->stop_first = true;
    p->expires_at = active ? now + 250 : 0;
    if (active && !p->timer) p->timer = SDL_AddTimer(10, mk8_switch_timer, p);
    if (active && !p->timer) {
        p->hd = false;
        mk8_rumble_basic(p, now);
        return;
    }
    mk8_switch_write(p, now);
}

static bool mk8_rumble_menu_open(void) {
    return menu_is_open() || mk8_settings_page_active();
}

static void mk8_rumble_frame(void) {
    SDL_LockJoysticks();
    Uint64 now = SDL_GetTicks();
    for (unsigned i = 0; i < 1 + EXTRA_PADS; i++) {
        struct Mk8RumblePad *p = &mk8_rumble_pads[i];
        if (p->hd && p->expires_at) {
            if (now >= p->expires_at || mk8_rumble_menu_open()) mk8_rumble_stop(p);
            else p->expires_at = now + 250;
            continue;
        }
        if (!p->low && !p->high) continue;
        // Renew held effects while presenting. A presentation stall leaves a bounded pulse.
        if (now - p->sent_at >= 250 || mk8_rumble_menu_open()) mk8_rumble_stop(p);
        else mk8_rumble_apply(p, now);
    }
    SDL_UnlockJoysticks();
}

static void mk8_rumble_clear(void) {
    for (unsigned i = 0; i < 1 + EXTRA_PADS; i++) mk8_rumble_stop(&mk8_rumble_pads[i]);
}

static void mk8_rumble_focus(bool focused) {
    SDL_LockJoysticks();
    mk8_rumble_focused = focused;
    if (!focused) mk8_rumble_clear();
    SDL_UnlockJoysticks();
}

RT_ON_RESET(mk8_rumble_reset) {
    SDL_LockJoysticks();
    mk8_rumble_clear();
    SDL_UnlockJoysticks();
}

static bool mk8_valid_rumble_buffer(const IpcBuf *b) {
    return b->size <= UINT16_MAX && (!b->size ||
           (b->addr >= GUEST_LO && b->addr < GUEST_HI && b->size <= GUEST_HI - b->addr));
}

void mk8_hid_rumble(IpcReq *req, IpcResp *resp) {
    const uint8_t *handles = req->data, *values = NULL;
    size_t count = 1;
    if (req->cmd == 204 || req->cmd == 205) {
        if (req->cmd == 204 && (!req->data || !req->data_size)) goto invalid;
        SDL_LockJoysticks();
        if (req->cmd == 204) {
            mk8_rumble_permitted = (req->data[0] & 1) != 0;
            if (!mk8_rumble_permitted) mk8_rumble_clear();
        } else {
            resp->data[0] = mk8_rumble_permitted;
            resp_size(resp, 1);
        }
        SDL_UnlockJoysticks();
        return;
    }
    if (req->cmd == 206) {
        if (req->nx != 2 || req->na) goto invalid;
        const IpcBuf *h = &req->x[0], *v = &req->x[1];
        if (!mk8_valid_rumble_buffer(h) || !mk8_valid_rumble_buffer(v) ||
            h->size % sizeof(uint32_t) || v->size % sizeof(Mk8Vibration) ||
            h->size / sizeof(uint32_t) != v->size / sizeof(Mk8Vibration)) goto invalid;
        handles = (const uint8_t *)(uintptr_t)h->addr;
        values = (const uint8_t *)(uintptr_t)v->addr;
        count = h->size / sizeof(uint32_t);
    } else {
        if (!handles || req->data_size < (req->cmd == 201 ? 4 + sizeof(Mk8Vibration) : 4)) goto invalid;
        if (req->cmd == 201) values = handles + 4;
    }
    SDL_LockJoysticks();
    unsigned changed = 0;
    bool enabled = mk8_rumble_focused && mk8_rumble_permitted && !mk8_rumble_menu_open();
    for (size_t i = 0; i < count; i++) {
        uint32_t handle;
        memcpy(&handle, handles + i * sizeof(handle), sizeof(handle));
        unsigned side = (handle >> 16) & 1;
        struct Mk8RumblePad *p = mk8_rumble_pad(handle);
        if (req->cmd == 202) {
            memcpy(resp->data, p ? &p->side[side] : &mk8_silent, sizeof(Mk8Vibration));
            resp_size(resp, sizeof(Mk8Vibration));
        } else if (p) {
            Mk8Vibration value;
            memcpy(&value, values + i * sizeof(value), sizeof(value));
            value.low = mk8_amplitude(value.low);
            value.high = mk8_amplitude(value.high);
            p->side[side] = enabled ? value : mk8_silent;
            changed |= 1u << (p - mk8_rumble_pads);
        }
    }
    // A batch can contain both sides of several controllers. Publish once per player.
    for (unsigned i = 0; i < 1 + EXTRA_PADS; i++) {
        if (changed & (1u << i)) mk8_rumble_apply(&mk8_rumble_pads[i], SDL_GetTicks());
    }
    SDL_UnlockJoysticks();
    return;
invalid:
    resp->result = (420u << 9) | 11u;  // HIPC InvalidCmifRequest
}
