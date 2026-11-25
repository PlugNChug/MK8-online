#include "ipc.h"
#include "ui/menu.h"
#include <pthread.h>
#include <time.h>

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

enum { MK8_PAIR_FREE, MK8_PAIR_ACTIVE, MK8_PAIR_RETIRING };
static SDL_AtomicInt mk8_pair_lease;
static SDL_AtomicInt mk8_pair_exiting;
static SDL_AtomicInt mk8_pair_basic_mask;
static SDL_AtomicInt mk8_pair_stop_done;
struct Mk8JoyConPair {
    SDL_hid_device *halves[2];
    SDL_Mutex *lock;
    SDL_Condition *wake;
    Mk8SwitchRumble wanted;
    Uint64 expires;
    uint64_t epoch;
    bool neutral_first, closing, broken, drained;
};

// SDL's joystick lock protects the open handles and applied actuator state.
static struct Mk8RumblePad {
    SDL_JoystickID id;
    Mk8Vibration side[2];
    Uint16 low, high;
    float rumble_gain;
    Uint64 sent_at;
    _Atomic Uint64 last_gyro_ns;
    Uint64 last_write_ns;
    bool hd, stop_first;
    Uint16 product;
    struct Mk8JoyConPair *pair;
    bool waiting_for_pair;
    Mk8SwitchRumble wanted, last;
    Uint64 expires_at;
    SDL_TimerID timer;
    uint64_t applied, epoch;
} mk8_rumble_pads[2 * (1 + EXTRA_PADS)];

static SDL_AtomicInt mk8_rumble_frame_pending;
static _Atomic uint64_t mk8_presented_at;

// The guest owns these requested values. The UI thread owns the SDL devices and consumes their snapshots.
enum { MK8_OUTPUT_PADS = SDL_arraysize(mk8_rumble_pads), MK8_OUTPUT_QUEUE = 64 };
static struct Mk8GuestPad {
    SDL_JoystickID id;
    uint64_t generation, issued;
    Mk8Vibration side[2];
} mk8_guest_pads[MK8_OUTPUT_PADS];
static bool mk8_guest_paired[1 + EXTRA_PADS];
static pthread_mutex_t mk8_guest_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t mk8_output_space = PTHREAD_COND_INITIALIZER;
static struct Mk8OutputBatch {
    unsigned mask;
    uint64_t at;
    struct Mk8GuestPad pad[MK8_OUTPUT_PADS];
} mk8_output_queue[MK8_OUTPUT_QUEUE], mk8_expired_stops;
static unsigned mk8_output_head, mk8_output_count;
static bool mk8_output_exiting;
static _Atomic uint64_t mk8_output_cancellation;

// An expired maneuver must never replay after discovery. Keep one ordered stop per affected binding.
static void mk8_retire_expired_output(Uint64 now) {
    while (mk8_output_count) {
        struct Mk8OutputBatch *batch = &mk8_output_queue[mk8_output_head];
        if (now < batch->at + 250) break;
        for (unsigned i = 0; i < MK8_OUTPUT_PADS; ++i) if (batch->mask & (1u << i)) {
            mk8_expired_stops.mask |= 1u << i;
            mk8_expired_stops.pad[i] = batch->pad[i];
        }
        mk8_output_head = (mk8_output_head + 1) % MK8_OUTPUT_QUEUE;
        --mk8_output_count;
    }
}

static void mk8_sync_guest_rumble(void) {
    pthread_mutex_lock(&mk8_guest_mutex);
    memcpy(mk8_guest_paired, mk8_paired_slot, sizeof mk8_guest_paired);
    for (unsigned i = 0; i < MK8_OUTPUT_PADS; ++i) {
        struct Mk8GuestPad *g = &mk8_guest_pads[i];
        struct Mk8RumblePad *p = &mk8_rumble_pads[i];
        if (p->id != g->id) {
            ++g->generation;
            g->id = p->id;
            p->applied = g->issued = 0;
        }
        if (p->applied == g->issued) memcpy(g->side, p->side, sizeof g->side);
    }
    pthread_mutex_unlock(&mk8_guest_mutex);
}

// Publish under the joystick lock so an idle present need not wait for device discovery.
static void mk8_publish_rumble_work(void) {
    mk8_sync_guest_rumble();
    for (unsigned i = 0; i < SDL_arraysize(mk8_rumble_pads); ++i) {
        struct Mk8RumblePad *p = &mk8_rumble_pads[i];
        if (p->waiting_for_pair || (p->hd && p->expires_at) || p->low || p->high) {
            SDL_SetAtomicInt(&mk8_rumble_frame_pending, 1);
            return;
        }
    }
    SDL_SetAtomicInt(&mk8_rumble_frame_pending, 0);
}

static bool mk8_joycon_endpoint(const SDL_hid_device_info *d, Uint16 product) {
    return d && d->vendor_id == 0x057e && d->product_id == product && d->path &&
           d->bus_type == SDL_HID_API_BUS_BLUETOOTH && d->usage_page == 1 && d->usage == 5 &&
           d->manufacturer_string && SDL_wcscmp(d->manufacturer_string, L"Nintendo") == 0;
}

static bool mk8_joycon_endpoints(SDL_Gamepad *device, SDL_hid_device *halves[2]) {
    const char *path = SDL_GetGamepadPath(device);
    if (!path || strcmp(path, "nintendo_joycons_combined") != 0 ||
        SDL_GetGamepadConnectionState(device) != SDL_JOYSTICK_CONNECTION_WIRELESS) return false;
    // SDL gives all combined pairs the same path and no child IDs. Only an unambiguous pair is safe.
    int count = 0, pairs = 0;
    bool separate = false;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    for (int i = 0; i < count; i++) {
        SDL_GamepadType type = SDL_GetRealGamepadTypeForID(ids[i]);
        if (type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR) pairs++;
        if (type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT ||
            type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT) separate = true;
    }
    SDL_free(ids);
    if (pairs != 1 || separate) return false;
    SDL_hid_device_info *devices = SDL_hid_enumerate(0x057e, 0);
    const char *paths[2] = {NULL, NULL};
    bool ok = true;
    for (SDL_hid_device_info *d = devices; d; d = d->next) {
        if (d->product_id == 0x200e) ok = false; // A charging grip can also supply children to SDL.
        if (d->product_id != 0x2006 && d->product_id != 0x2007) continue;
        int side = d->product_id - 0x2006;
        if (paths[side] || !mk8_joycon_endpoint(d, d->product_id)) ok = false;
        paths[side] = d->path;
    }
    ok = ok && paths[0] && paths[1];
    for (int i = 0; i < 2 && ok; i++) {
        halves[i] = SDL_hid_open_path(paths[i]);
        const SDL_hid_device_info *d = halves[i] ? SDL_hid_get_device_info(halves[i]) : NULL;
        ok = mk8_joycon_endpoint(d, (Uint16)(0x2006 + i)) && strcmp(d->path, paths[i]) == 0;
    }
    SDL_hid_free_enumeration(devices);
    if (!ok) {
        for (int i = 0; i < 2; i++) {
            if (halves[i]) SDL_hid_close(halves[i]);
            halves[i] = NULL;
        }
    }
    return ok;
}

static bool mk8_pair_needs_neutral(const struct Mk8JoyConPair *p, Uint64 now) {
    return p->closing || SDL_GetAtomicInt(&mk8_pair_exiting) || p->neutral_first || p->broken ||
           now >= p->expires || p->epoch != atomic_load(&mk8_output_cancellation);
}

static void mk8_pair_release_radios(struct Mk8JoyConPair *p) {
    for (int i = 0; i < 2; i++) {
        if (p->halves[i]) SDL_hid_close(p->halves[i]);
        p->halves[i] = NULL;
    }
}

static int SDLCALL mk8_pair_worker(void *userdata) {
    struct Mk8JoyConPair *p = userdata;
    Mk8SwitchRumble last = mk8_switch_neutral;
    Uint64 sent_at = 0;
    unsigned sequence = 0, neutral_failures = 0;
    bool active = false;
    SDL_LockMutex(p->lock);
    for (;;) {
        Uint64 now = SDL_GetTicks();
        if (!active) {
            p->neutral_first = false;
            if (p->closing) break;
            if (p->broken && !p->drained) {
                SDL_UnlockMutex(p->lock);
                mk8_pair_release_radios(p);
                SDL_LockMutex(p->lock);
                p->drained = true;
                continue;
            }
        }
        bool stop = mk8_pair_needs_neutral(p, now);
        if (stop && !active) {
            SDL_WaitCondition(p->wake, p->lock);
            continue;
        }
        Mk8SwitchRumble next = stop ? mk8_switch_neutral : p->wanted;
        Uint64 due = sent_at + (!stop && !memcmp(&last, &next, sizeof(next)) ? 45 : 30);
        if (!stop && p->expires < due) due = p->expires;
        if (sent_at && now < due) {
            SDL_WaitConditionTimeout(p->wake, p->lock, (Sint32)(due - now));
            continue;
        }
        // Windows HID writes can block for a second per half. Only this worker owns these handles.
        Uint8 raw[49] = {0};
        memcpy(raw, &next, sizeof(next));
        raw[1] = sequence++ & 15;
        bool ok = true;
        for (int side = 0; side < 2; side++) {
            // Close, expiry or a newer effect can arrive while the first physical write is in flight.
            if (!stop && (mk8_pair_needs_neutral(p, SDL_GetTicks()) || memcmp(&next, &p->wanted, sizeof(next)))) break;
            SDL_UnlockMutex(p->lock);
            int written = SDL_hid_write(p->halves[side], raw, sizeof(raw));
            SDL_LockMutex(p->lock);
            if (written != sizeof(raw)) {
                ok = false;
                p->broken = true;
            }
        }
        sent_at = SDL_GetTicks();
        last = next;
        // An unsuccessful active write may still have reached one device. Stop both before fallback.
        active = !stop || (!ok && ++neutral_failures < 2);
    }
    SDL_UnlockMutex(p->lock);
    mk8_pair_release_radios(p);
    SDL_DestroyCondition(p->wake);
    SDL_DestroyMutex(p->lock);
    SDL_free(p);
    SDL_hid_exit();
    SDL_SetAtomicInt(&mk8_pair_lease, MK8_PAIR_FREE);
    return 0;
}

static struct Mk8JoyConPair *mk8_pair_open(SDL_Gamepad *device) {
    if (SDL_GetAtomicInt(&mk8_pair_exiting)) return NULL;
    if (!SDL_CompareAndSwapAtomicInt(&mk8_pair_lease, MK8_PAIR_FREE, MK8_PAIR_ACTIVE)) return NULL;
    if (SDL_hid_init() == 0) {
        struct Mk8JoyConPair *p = SDL_calloc(1, sizeof(*p));
        if (p) {
            p->lock = SDL_CreateMutex();
            p->wake = SDL_CreateCondition();
            if (p->lock && p->wake && mk8_joycon_endpoints(device, p->halves)) {
                SDL_Thread *thread = SDL_CreateThread(mk8_pair_worker, "Joy-Con rumble", p);
                if (thread) {
                    SDL_DetachThread(thread);
                    return p;
                }
                mk8_pair_release_radios(p);
            }
            SDL_DestroyCondition(p->wake);
            SDL_DestroyMutex(p->lock);
            SDL_free(p);
        }
        SDL_hid_exit();
    }
    SDL_SetAtomicInt(&mk8_pair_lease, MK8_PAIR_FREE);
    return NULL;
}

static bool mk8_pair_request(struct Mk8JoyConPair *p, const Mk8SwitchRumble *next, Uint64 expires, uint64_t epoch) {
    SDL_LockMutex(p->lock);
    if (!expires) p->neutral_first = true;
    p->wanted = *next;
    p->expires = expires;
    p->epoch = epoch;
    bool available = !p->drained;
    SDL_SignalCondition(p->wake);
    SDL_UnlockMutex(p->lock);
    return available;
}

static void mk8_pair_close(struct Mk8JoyConPair *p) {
    SDL_LockMutex(p->lock);
    SDL_SetAtomicInt(&mk8_pair_lease, MK8_PAIR_RETIRING);
    p->closing = true;
    SDL_SignalCondition(p->wake);
    SDL_UnlockMutex(p->lock);
}

// Caller holds the joystick lock and owns the open handle.
static void mk8_rumble_attach(int player, SDL_Gamepad *device) {
    if (device && SDL_GetBooleanProperty(SDL_GetGamepadProperties(device), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false)) {
        SDL_JoystickID id = SDL_GetGamepadID(device);
        Uint16 product = SDL_GetGamepadProduct(device);
        bool nintendo = SDL_GetGamepadVendor(device) == 0x057e;
        bool joycon = nintendo && product >= 0x2006 && product <= 0x2008;
        struct Mk8RumblePad *p = &mk8_rumble_pads[player];
        // Calibrate Joy-Con motor output without changing the guest's requested values.
        *p = (struct Mk8RumblePad){.id = id, .product = product, .side = {mk8_silent, mk8_silent},
                                  .rumble_gain = joycon ? 0.5f : 1.0f,
                                  .wanted = mk8_switch_neutral, .epoch = atomic_load(&mk8_output_cancellation)};
        // SDL 3.4 marks HIDAPI GUIDs with 'h'. Its combined driver only forwards basic rumble.
        if (SDL_GetJoystickGUIDForID(id).data[14] == 'h' && nintendo) {
            p->waiting_for_pair = joycon &&
                                  SDL_GetAtomicInt(&mk8_pair_lease) == MK8_PAIR_RETIRING;
            if (product == 0x2008 && !p->waiting_for_pair) p->pair = mk8_pair_open(device);
            p->hd = product == 0x2009 || product == 0x2006 || product == 0x2007 || p->pair;
        }
        // A settings rescan can move a just-stopped device into another slot.
        Uint64 now = SDL_GetTicks();
        if (p->hd && now < mk8_switch_reopen_after) SDL_Delay((Uint32)(mk8_switch_reopen_after - now));
        mk8_publish_rumble_work();
    }
}

static SDL_Gamepad *mk8_rumble_open(int player, SDL_JoystickID id) {
    SDL_LockJoysticks();
    SDL_Gamepad *device = SDL_OpenGamepad(id);
    mk8_rumble_attach(player, device);
    SDL_UnlockJoysticks();
    return device;
}

static bool mk8_switch_playing(const struct Mk8RumblePad *p) {
    return p->last.report && memcmp(&p->last, &mk8_switch_neutral, sizeof(p->last));
}

static void mk8_pair_track_basic(struct Mk8RumblePad *p) {
    if (p->product != 0x2008) return;
    int bit = 1 << (p - mk8_rumble_pads);
    int mask = SDL_GetAtomicInt(&mk8_pair_basic_mask);
    SDL_SetAtomicInt(&mk8_pair_basic_mask, (p->low || p->high) ? mask | bit : mask & ~bit);
}

static void mk8_rumble_basic(struct Mk8RumblePad *p, Uint64 now) {
    Uint16 low = (Uint16)(65535.f * p->rumble_gain * fmaxf(p->side[0].low, p->side[1].low) + .5f);
    Uint16 high = (Uint16)(65535.f * p->rumble_gain * fmaxf(p->side[0].high, p->side[1].high) + .5f);
    if (low == p->low && high == p->high && ((!low && !high) || now - p->sent_at < 125)) return;
    SDL_Gamepad *device = SDL_GetGamepadFromID(p->id);
    if (!device || !SDL_RumbleGamepad(device, low, high, (low || high) ? 250 : 0)) {
        if (device && (p->low || p->high)) SDL_RumbleGamepad(device, 0, 0, 0);
        p->side[0] = p->side[1] = mk8_silent;
        low = high = 0;
    }
    p->low = low;
    p->high = high;
    mk8_pair_track_basic(p);
    p->sent_at = now;
}

static void mk8_rumble_sensor(const SDL_GamepadSensorEvent *e) {
    if (e->sensor != SDL_SENSOR_GYRO) return;
    for (unsigned i = 0; i < SDL_arraysize(mk8_rumble_pads); ++i) {
        struct Mk8RumblePad *p = &mk8_rumble_pads[i];
        if (p->id != e->which) continue;
        if (e->timestamp > atomic_load(&p->last_gyro_ns)) atomic_store(&p->last_gyro_ns, e->timestamp);
        return;
    }
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
    // A stalled Switch input stream cannot keep up with active HID writes. Coalesce until it recovers.
    // Use SDL's host timestamp so an older queued gyro event cannot release a newer write.
    if (active && device && p->last_write_ns && SDL_GamepadSensorEnabled(device, SDL_SENSOR_GYRO) &&
        atomic_load(&p->last_gyro_ns) <= p->last_write_ns) return;
    if (device && SDL_SendGamepadEffect(device, next, sizeof(*next))) {
        p->last_write_ns = SDL_GetTicksNS();
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
        if (p->epoch != atomic_load(&mk8_output_cancellation)) {
            p->side[0] = p->side[1] = mk8_silent;
            p->wanted = mk8_switch_neutral;
            p->expires_at = 0;
        }
        if (p->hd || p->stop_first) mk8_switch_write(p, now);
        if ((p->hd && p->expires_at) || p->stop_first || mk8_switch_playing(p)) {
            next = interval;
        } else {
            p->timer = 0;
            if (!p->hd) mk8_rumble_basic(p, now);
        }
        mk8_publish_rumble_work();
    }
    SDL_UnlockJoysticks();
    return next;
}

static void mk8_rumble_stop(struct Mk8RumblePad *p) {
    p->side[0] = p->side[1] = mk8_silent;
    p->wanted = mk8_switch_neutral;
    p->expires_at = 0;
    if (p->pair) mk8_pair_request(p->pair, &p->wanted, 0, p->epoch);
    p->stop_first = mk8_switch_playing(p);
    if (p->stop_first) mk8_switch_write(p, SDL_GetTicks());
    if (p->low || p->high) {
        SDL_Gamepad *device = SDL_GetGamepadFromID(p->id);
        if (device) SDL_RumbleGamepad(device, 0, 0, 0);
    }
    p->low = p->high = 0;
    mk8_pair_track_basic(p);
}

static void mk8_rumble_close(int player, SDL_Gamepad *device) {
    SDL_LockJoysticks();
    struct Mk8RumblePad *p = &mk8_rumble_pads[player];
    if (p->id && p->id == SDL_GetGamepadID(device)) {
        mk8_rumble_stop(p);
        // Closing can precede the first permitted neutral slot. Allow one retry if that write fails.
        for (int attempt = 0; p->stop_first && attempt < 2; attempt++) {
            Uint64 elapsed = SDL_GetTicks() - p->sent_at;
            if (elapsed < 30) SDL_Delay((Uint32)(30 - elapsed));
            mk8_switch_write(p, SDL_GetTicks());
        }
        if (p->timer) SDL_RemoveTimer(p->timer);
        if (p->pair) mk8_pair_close(p->pair);
        p->pair = NULL;
        p->waiting_for_pair = false;
        if (p->last.report && p->sent_at + 30 > mk8_switch_reopen_after) mk8_switch_reopen_after = p->sent_at + 30;
        p->timer = 0;
        p->id = 0;
        mk8_publish_rumble_work();
    }
    SDL_CloseGamepad(device);
    SDL_UnlockJoysticks();
}

static int SDLCALL mk8_pair_stop_output(void *unused) {
    (void)unused;
    SDL_LockJoysticks();
    for (unsigned i = 0; i < SDL_arraysize(mk8_rumble_pads); i++) {
        struct Mk8RumblePad *p = &mk8_rumble_pads[i];
        if (p->pair) {
            mk8_pair_close(p->pair);
            p->pair = NULL;
            p->hd = false;
            p->expires_at = 0;
        }
        if (p->product == 0x2008 && (p->low || p->high)) mk8_rumble_stop(p);
    }
    mk8_publish_rumble_work();
    SDL_UnlockJoysticks();
    SDL_SetAtomicInt(&mk8_pair_stop_done, 1);
    return 0;
}

void mk8_joycon_shutdown(void) {
    if (SDL_SetAtomicInt(&mk8_pair_exiting, 1)) return;
    pthread_mutex_lock(&mk8_guest_mutex);
    mk8_output_exiting = true;
    mk8_output_count = 0;
    mk8_expired_stops.mask = 0;
    atomic_fetch_add(&mk8_output_cancellation,1);
    pthread_cond_broadcast(&mk8_output_space);
    pthread_mutex_unlock(&mk8_guest_mutex);
    if (SDL_GetAtomicInt(&mk8_pair_lease) == MK8_PAIR_FREE && !SDL_GetAtomicInt(&mk8_pair_basic_mask)) return;
    // Keep even SDL's basic fallback and its joystick lock off the bounded window-exit path.
    Uint64 until = SDL_GetTicks() + 150;
    SDL_SetAtomicInt(&mk8_pair_stop_done, 0);
    SDL_Thread *thread = SDL_CreateThread(mk8_pair_stop_output, "Stop Joy-Con rumble", NULL);
    if (!thread) return;
    SDL_DetachThread(thread);
    while (SDL_GetTicks() < until && (!SDL_GetAtomicInt(&mk8_pair_stop_done) ||
           SDL_GetAtomicInt(&mk8_pair_lease) != MK8_PAIR_FREE)) SDL_Delay(1);
}

static struct Mk8GuestPad *mk8_guest_rumble_pad(uint32_t handle) {
    // Actuators are gone during the host's GPU/cache drain. The guest HID session is still open.
    if (mk8_output_exiting) return NULL;
    unsigned player = (handle >> 8) & 255;
    if ((handle & 0xfffe00ffu) != 3 || player >= 1 + EXTRA_PADS) return NULL;
    // In a software pair the right grip has its own SDL device and output timer.
    if ((handle & 0x10000u) && mk8_guest_paired[player]) player += 1 + EXTRA_PADS;
    struct Mk8GuestPad *p = &mk8_guest_pads[player];
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

static void mk8_switch_actuator(Uint8 out[4], Mk8Vibration value, float gain) {
    int low = isfinite(value.low_hz) && value.low_hz > 0 ? mk8_switch_level(value.low * gain) : 0;
    int high = isfinite(value.high_hz) && value.high_hz > 0 ? mk8_switch_level(value.high * gain) : 0;
    int lf = low ? mk8_switch_frequency(value.low_hz, 64) : 64;
    int hf = high ? 4 * mk8_switch_frequency(value.high_hz, 96) : 256;
    out[0] = (Uint8)hf;
    out[1] = (Uint8)((hf >> 8) | (high << 1));
    out[2] = (Uint8)(lf | ((low & 1) << 7));
    out[3] = (Uint8)(64 + (low >> 1));
}

static bool mk8_stronger_band(float amplitude, float hz, float other_amplitude, float other_hz) {
    return isfinite(hz) && hz > 0 && (!isfinite(other_hz) || other_hz <= 0 || amplitude > other_amplitude);
}

static void mk8_rumble_apply(struct Mk8RumblePad *p, Uint64 now) {
    if (p->product == 0x2008 && SDL_GetAtomicInt(&mk8_pair_exiting)) return;
    if (p->waiting_for_pair) {
        if (SDL_GetAtomicInt(&mk8_pair_lease) == MK8_PAIR_RETIRING) {
            p->expires_at = now + 250;
            return;
        }
        p->waiting_for_pair = false;
        if (p->product == 0x2008) {
            SDL_Gamepad *device = SDL_GetGamepadFromID(p->id);
            p->pair = device ? mk8_pair_open(device) : NULL;
            p->hd = p->pair != NULL;
        }
    }
    if (!p->hd) {
        if (!p->stop_first) mk8_rumble_basic(p, now);
        return;
    }
    if (p->product == 0x2006 || p->product == 0x2007) {
        // The game sees a FullKey with two grips even when one horizontal Joy-Con is the whole pad.
        Mk8Vibration single = p->side[0], right = p->side[1];
        if (mk8_stronger_band(right.low, right.low_hz, single.low, single.low_hz)) {
            single.low = right.low;
            single.low_hz = right.low_hz;
        }
        if (mk8_stronger_band(right.high, right.high_hz, single.high, single.high_hz)) {
            single.high = right.high;
            single.high_hz = right.high_hz;
        }
        mk8_switch_actuator(p->wanted.side[p->product - 0x2006], single, p->rumble_gain);
    } else {
        for (int side = 0; side < 2; side++) mk8_switch_actuator(p->wanted.side[side], p->side[side], p->rumble_gain);
    }
    bool active = memcmp(&p->wanted, &mk8_switch_neutral, sizeof(p->wanted)) != 0;
    if (!active && mk8_switch_playing(p)) p->stop_first = true;
    p->expires_at = active ? now + 250 : 0;
    if (p->pair) {
        if (!mk8_pair_request(p->pair, &p->wanted, p->expires_at, p->epoch)) {
            p->hd = false;
            p->expires_at = 0;
            mk8_rumble_basic(p, now);
        }
        return;
    }
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
    atomic_store(&mk8_presented_at, SDL_GetTicks());
}

void mk8_rumble_pump(void) {
    pthread_mutex_lock(&mk8_guest_mutex);
    mk8_retire_expired_output(SDL_GetTicks());
    unsigned queued = mk8_output_count + (mk8_expired_stops.mask != 0);
    pthread_mutex_unlock(&mk8_guest_mutex);
    if (!queued && !SDL_GetAtomicInt(&mk8_rumble_frame_pending)) return;
    SDL_LockJoysticks();
    Uint64 now = SDL_GetTicks();
    while (queued--) {
        pthread_mutex_lock(&mk8_guest_mutex);
        bool expired = mk8_expired_stops.mask != 0;
        if (!expired && !mk8_output_count) {
            pthread_mutex_unlock(&mk8_guest_mutex);
            break;
        }
        struct Mk8OutputBatch batch;
        if (expired) {
            batch = mk8_expired_stops;
            mk8_expired_stops.mask = 0;
        } else {
            batch = mk8_output_queue[mk8_output_head];
            mk8_output_head = (mk8_output_head + 1) % MK8_OUTPUT_QUEUE;
            --mk8_output_count;
        }
        uint64_t cancellation = atomic_load(&mk8_output_cancellation);
        pthread_cond_broadcast(&mk8_output_space);
        for (unsigned i = 0; i < MK8_OUTPUT_PADS; ++i)
            if (batch.pad[i].generation != mk8_guest_pads[i].generation) batch.mask &= ~(1u << i);
        pthread_mutex_unlock(&mk8_guest_mutex);
        for (unsigned i = 0; i < MK8_OUTPUT_PADS; ++i) if (batch.mask & (1u << i)) {
            if (atomic_load(&mk8_output_cancellation) != cancellation) break;
            struct Mk8RumblePad *p = &mk8_rumble_pads[i];
            p->epoch = cancellation;
            p->applied = batch.pad[i].issued;
            now = SDL_GetTicks();
            if (expired || now >= batch.at + 250 || mk8_rumble_menu_open()) {
                mk8_rumble_stop(p);
            } else {
                memcpy(p->side, batch.pad[i].side, sizeof p->side);
                mk8_rumble_apply(p, now);
            }
        }
    }
    now = SDL_GetTicks();
    Uint64 present = atomic_load(&mk8_presented_at);
    static Uint64 last_present;
    bool refresh = present != last_present && now < present + 250;
    last_present = present;
    for (unsigned i = 0; i < SDL_arraysize(mk8_rumble_pads); i++) {
        struct Mk8RumblePad *p = &mk8_rumble_pads[i];
        uint64_t epoch = atomic_load(&mk8_output_cancellation);
        if (p->epoch != epoch) {
            p->epoch = epoch;
            if (p->id) mk8_rumble_stop(p);
            continue;
        }
        if (p->waiting_for_pair) {
            if (now >= p->expires_at || mk8_rumble_menu_open()) mk8_rumble_stop(p);
            if (SDL_GetAtomicInt(&mk8_pair_lease) == MK8_PAIR_RETIRING) continue;
            mk8_rumble_apply(p, now);
        }
        if (p->hd && p->expires_at) {
            if (now >= p->expires_at || mk8_rumble_menu_open()) mk8_rumble_stop(p);
            else if (refresh && p->pair) mk8_rumble_apply(p, present);
            else if (refresh) p->expires_at = present + 250;
            continue;
        }
        if (!p->low && !p->high) continue;
        // Renew held effects while presenting. A presentation stall leaves a bounded pulse.
        if (now - p->sent_at >= 250 || mk8_rumble_menu_open()) mk8_rumble_stop(p);
        else if (refresh) mk8_rumble_apply(p, now);
    }
    mk8_publish_rumble_work();
    SDL_UnlockJoysticks();
}

static void mk8_rumble_clear(void) {
    pthread_mutex_lock(&mk8_guest_mutex);
    mk8_output_count = 0;
    mk8_expired_stops.mask = 0;
    atomic_fetch_add(&mk8_output_cancellation, 1);
    pthread_cond_broadcast(&mk8_output_space);
    for (unsigned i = 0; i < MK8_OUTPUT_PADS; ++i) {
        mk8_guest_pads[i].side[0] = mk8_guest_pads[i].side[1] = mk8_silent;
        mk8_rumble_pads[i].applied = ++mk8_guest_pads[i].issued;
    }
    pthread_mutex_unlock(&mk8_guest_mutex);
    for (unsigned i = 0; i < SDL_arraysize(mk8_rumble_pads); i++) mk8_rumble_stop(&mk8_rumble_pads[i]);
    mk8_publish_rumble_work();
}

static void mk8_rumble_focus(bool focused) {
    SDL_LockJoysticks();
    pthread_mutex_lock(&mk8_guest_mutex);
    mk8_rumble_focused = focused;
    pthread_mutex_unlock(&mk8_guest_mutex);
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

// Caller holds the guest mutex. A queued entry never retains pointers into guest memory.
static bool mk8_queue_output(unsigned mask, Uint64 requested_at) {
    if (!mask) return false;
    bool wake = !mk8_output_count;
    struct Mk8OutputBatch *batch = &mk8_output_queue[(mk8_output_head + mk8_output_count++) % MK8_OUTPUT_QUEUE];
    batch->mask = mask;
    batch->at = requested_at;
    for (unsigned i = 0; i < MK8_OUTPUT_PADS; ++i) if (mask & (1u << i)) {
        ++mk8_guest_pads[i].issued;
        batch->pad[i] = mk8_guest_pads[i];
    }
    return wake;
}

static void mk8_wake_output(void) {
    SDL_Event e = {.type = SDL_EVENT_USER};
    SDL_PushEvent(&e);  // The UI also checks the queue after its next 16 ms event wait.
}

// Caller holds the guest mutex. Device I/O stays on the existing output path.
static bool mk8_cancel_rumble_output(void) {
    unsigned stop = 0;
    mk8_output_count = 0;
    mk8_expired_stops.mask = 0;
    atomic_fetch_add(&mk8_output_cancellation, 1);
    for (unsigned i = 0; i < MK8_OUTPUT_PADS; ++i) if (mk8_guest_pads[i].id) {
        mk8_guest_pads[i].side[0] = mk8_guest_pads[i].side[1] = mk8_silent;
        stop |= 1u << i;
    }
    pthread_cond_broadcast(&mk8_output_space);
    return !mk8_output_exiting && mk8_queue_output(stop, SDL_GetTicks());
}

void mk8_rumble_set_enabled(bool enabled) {
    pthread_mutex_lock(&mk8_guest_mutex);
    bool previous = __atomic_exchange_n(&g_settings.controller_vibration, enabled, __ATOMIC_RELAXED);
    bool wake = previous && !enabled && mk8_cancel_rumble_output();
    pthread_mutex_unlock(&mk8_guest_mutex);
    if (wake) mk8_wake_output();
}

void mk8_hid_rumble(IpcReq *req, IpcResp *resp) {
    const uint8_t *handles = req->data, *values = NULL;
    size_t count = 1;
    bool wake = false;
    if (req->cmd == 204 || req->cmd == 205) {
        if (req->cmd == 204 && (!req->data || !req->data_size)) goto invalid;
        pthread_mutex_lock(&mk8_guest_mutex);
        if (req->cmd == 204) {
            mk8_rumble_permitted = (req->data[0] & 1) != 0;
            if (!mk8_rumble_permitted) wake = mk8_cancel_rumble_output();
        } else {
            resp->data[0] = mk8_rumble_permitted;
            resp_size(resp, 1);
        }
        pthread_mutex_unlock(&mk8_guest_mutex);
        if (wake) mk8_wake_output();
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
    Uint64 requested_at = SDL_GetTicks();
    pthread_mutex_lock(&mk8_guest_mutex);
    uint64_t cancellation = atomic_load(&mk8_output_cancellation);
    struct Mk8OutputBatch pending = {0};
    unsigned sides = 0;
    bool host_enabled = __atomic_load_n(&g_settings.controller_vibration, __ATOMIC_RELAXED);
    bool enabled = mk8_rumble_focused && mk8_rumble_permitted && !mk8_rumble_menu_open();
    for (size_t i = 0; i < count; i++) {
        uint32_t handle;
        memcpy(&handle, handles + i * sizeof(handle), sizeof(handle));
        unsigned side = (handle >> 16) & 1;
        struct Mk8GuestPad *p = mk8_guest_rumble_pad(handle);
        if (req->cmd == 202) {
            memcpy(resp->data, p ? &p->side[side] : &mk8_silent, sizeof(Mk8Vibration));
            resp_size(resp, sizeof(Mk8Vibration));
        } else if (p && host_enabled) {
            Mk8Vibration value;
            memcpy(&value, values + i * sizeof(value), sizeof(value));
            value.low = mk8_amplitude(value.low);
            value.high = mk8_amplitude(value.high);
            unsigned slot = (unsigned)(p - mk8_guest_pads);
            pending.pad[slot].generation = p->generation;
            pending.pad[slot].side[side] = enabled ? value : mk8_silent;
            pending.mask |= 1u << slot;
            sides |= 1u << (2 * slot + side);
        }
    }
    while (pending.mask && !mk8_output_exiting && cancellation == atomic_load(&mk8_output_cancellation)) {
        Uint64 now = SDL_GetTicks();
        mk8_retire_expired_output(now);
        if (mk8_output_count < MK8_OUTPUT_QUEUE) break;
        // Make room when the oldest entry expires even if device discovery still owns the UI.
        struct timespec until;
        timespec_get(&until,TIME_UTC);
        until.tv_nsec += (long)(mk8_output_queue[mk8_output_head].at + 250 - now) * 1000000L;
        until.tv_sec += until.tv_nsec / 1000000000L;
        until.tv_nsec %= 1000000000L;
        pthread_cond_timedwait(&mk8_output_space, &mk8_guest_mutex, &until);
    }
    if (cancellation != atomic_load(&mk8_output_cancellation)) pending.mask = 0;
    for (unsigned slot = 0; slot < MK8_OUTPUT_PADS; ++slot) if (pending.mask & (1u << slot)) {
        struct Mk8GuestPad *p = &mk8_guest_pads[slot];
        if (p->generation != pending.pad[slot].generation) {
            pending.mask &= ~(1u << slot);
            continue;
        }
        for (unsigned side = 0; side < 2; ++side)
            if (sides & (1u << (2 * slot + side))) p->side[side] = pending.pad[slot].side[side];
    }
    wake = mk8_queue_output(pending.mask, requested_at);
    pthread_mutex_unlock(&mk8_guest_mutex);
    if (wake) mk8_wake_output();
    return;
invalid:
    resp->result = (420u << 9) | 11u;  // HIPC InvalidCmifRequest
}
