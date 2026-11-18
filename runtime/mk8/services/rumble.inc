#include "ipc.h"
#include "ui/menu.h"

typedef struct {
    float low, low_hz, high, high_hz;
} Mk8Vibration;

static const Mk8Vibration mk8_silent = {0, 160, 0, 320};
static bool mk8_rumble_permitted = true;
static bool mk8_rumble_focused = true;

// SDL's joystick lock also protects the player assignments and guest readback.
static struct Mk8RumblePad {
    SDL_JoystickID id;
    Mk8Vibration side[2];
    Uint16 low, high;
    Uint64 sent_at;
} mk8_rumble_pads[1 + EXTRA_PADS];

static SDL_Gamepad *mk8_rumble_open(int player, SDL_JoystickID id) {
    SDL_LockJoysticks();
    SDL_Gamepad *device = SDL_OpenGamepad(id);
    if (device && SDL_GetBooleanProperty(SDL_GetGamepadProperties(device), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false)) {
        mk8_rumble_pads[player] = (struct Mk8RumblePad){.id = id, .side = {mk8_silent, mk8_silent}};
    }
    SDL_UnlockJoysticks();
    return device;
}

static void mk8_rumble_stop(struct Mk8RumblePad *p) {
    if (p->low || p->high) {
        SDL_Gamepad *device = SDL_GetGamepadFromID(p->id);
        if (device) SDL_RumbleGamepad(device, 0, 0, 0);
    }
    p->low = p->high = 0;
    p->side[0] = p->side[1] = mk8_silent;
}

static void mk8_rumble_close(int player, SDL_Gamepad *device) {
    SDL_LockJoysticks();
    struct Mk8RumblePad *p = &mk8_rumble_pads[player];
    if (p->id && p->id == SDL_GetGamepadID(device)) {
        mk8_rumble_stop(p);
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

static void mk8_rumble_apply(struct Mk8RumblePad *p, Uint64 now) {
    Uint16 low = (Uint16)(65535.f * fmaxf(p->side[0].low, p->side[1].low) + .5f);
    Uint16 high = (Uint16)(65535.f * fmaxf(p->side[0].high, p->side[1].high) + .5f);
    if (low == p->low && high == p->high && ((!low && !high) || now - p->sent_at < 125)) return;
    SDL_Gamepad *device = SDL_GetGamepadFromID(p->id);
    if (!device || !SDL_RumbleGamepad(device, low, high, (low || high) ? 250 : 0)) {
        mk8_rumble_stop(p);
        return;
    }
    p->low = low;
    p->high = high;
    p->sent_at = now;
}

static bool mk8_rumble_menu_open(void) {
    return menu_is_open() || mk8_settings_page_active();
}

static void mk8_rumble_frame(void) {
    SDL_LockJoysticks();
    Uint64 now = SDL_GetTicks();
    for (unsigned i = 0; i < 1 + EXTRA_PADS; i++) {
        struct Mk8RumblePad *p = &mk8_rumble_pads[i];
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
