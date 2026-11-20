// SDL enumerates each horizontal half separately. Read a pair in its upright orientation.
static SDL_Gamepad *mk8_partner[1 + EXTRA_PADS];
static struct { SDL_JoystickID device; uint32_t buttons; } mk8_shortcut[1 + EXTRA_PADS];
extern bool mk8_settings_shared_race(void);

// The guest samples each pad several times per presentation. Start timing a released
// standalone pause button only once its player actually reads it.
static SDL_SpinLock mk8_pause_lock;
static struct Mk8PausePulse {
    unsigned pending, visible;
    uint64_t first_frame;
} mk8_pause[1 + EXTRA_PADS];

static void mk8_reset_pause(unsigned player) {
    SDL_LockSpinlock(&mk8_pause_lock);
    mk8_pause[player] = (struct Mk8PausePulse){0};
    SDL_UnlockSpinlock(&mk8_pause_lock);
}

static unsigned mk8_read_pause(unsigned player) {
    extern uint64_t renderer_present_count(void);
    SDL_LockSpinlock(&mk8_pause_lock);
    struct Mk8PausePulse *pulse = &mk8_pause[player];
    if (!mk8_settings_shared_race()) {
        *pulse = (struct Mk8PausePulse){0};
    } else if (pulse->pending || pulse->visible) {
        uint64_t frame = renderer_present_count();
        if (frame - pulse->first_frame > 1) pulse->visible = 0;
        if (pulse->pending) {
            pulse->visible |= pulse->pending;
            pulse->pending = 0;
            pulse->first_frame = frame;
        }
    }
    unsigned visible = pulse->visible;
    SDL_UnlockSpinlock(&mk8_pause_lock);
    return visible;
}

// Filter the two guest pause actions only. A paddle remapped to Plus, or Back
// remapped to a held driving action, retains its ordinary binding behavior.
static unsigned mk8_pause_bindings(uint32_t host) {
    unsigned buttons = 0;
    for (int i = BIND_PLUS; i <= BIND_MINUS; ++i) {
        int source = g_bind_pad[i];
        if ((source == SDL_GAMEPAD_BUTTON_BACK || source == SDL_GAMEPAD_BUTTON_START) &&
            (host & (1u << source))) buttons |= 1u << i;
    }
    return buttons;
}

// SDL still reports held shortcut buttons on every axis event after the menu consumes their press.
static uint32_t mk8_filter_shortcut(unsigned player, SDL_Gamepad *primary, uint32_t host) {
    uint32_t held = mk8_shortcut[player].buttons & host;
    if (held) {
        SDL_JoystickID id = mk8_shortcut[player].device;
        SDL_Gamepad *partner = mk8_partner[player];
        if ((!primary || SDL_GetGamepadID(primary) != id) && (!partner || SDL_GetGamepadID(partner) != id))
            held = 0;
    }
    mk8_shortcut[player].buttons = held;
    return host & ~held;
}
static bool mk8_paired_slot[1 + EXTRA_PADS];
static bool mk8_explicit_slots;
static bool mk8_title_extra_seats;
static unsigned mk8_reserved_seats;  // Disconnected owners retain their seat until they return or it is reassigned.
static _Atomic int mk8_startup_preset;  // 0 pending, 1 applied, -1 invalid
static void mk8_rumble_attach(int player, SDL_Gamepad *device);

// The SDL thread owns the gamepad handles. Publish only copied UTF-16 text to the guest UI.
static SDL_SpinLock mk8_name_lock;
static uint16_t mk8_slot_names[1 + EXTRA_PADS][80];

static const char *mk8_attached_name(SDL_Gamepad *gamepad) {
    // SDL can reuse a mapping name across different devices of the same model.
    const char *name = SDL_GetJoystickName(SDL_GetGamepadJoystick(gamepad));
    return name && *name ? name : SDL_GetGamepadName(gamepad);
}

static size_t mk8_device_name(const char *name, uint16_t out[81]) {
    size_t used = 0;
    if (!name || !*name) name = "Unnamed controller";
    while (*name && used < 80) {
        Uint32 cp = SDL_StepUTF8(&name, NULL);
        if (cp < 0x10000) out[used++] = (uint16_t)cp;
        else {
            cp -= 0x10000;
            out[used++] = (uint16_t)(0xd800 + (cp >> 10));
            out[used++] = (uint16_t)(0xdc00 + (cp & 1023));
        }
    }
    return used;
}

static size_t mk8_copy_name(uint16_t *out, const uint16_t *name, size_t length, size_t capacity) {
    if (length <= capacity) {
        memcpy(out, name, length * sizeof(*out));
        return length;
    }
    size_t keep = capacity - 1;
    if (keep && name[keep - 1] >= 0xd800 && name[keep - 1] <= 0xdbff) --keep;
    memcpy(out, name, keep * sizeof(*out));
    out[keep++] = 0x2026;
    return keep;
}

static void mk8_label_device(uint16_t row[80], SDL_Gamepad *primary, SDL_Gamepad *partner, bool keyboard) {
    if (!primary) {
        if (keyboard) memcpy(row, u"Keyboard", sizeof(u"Keyboard"));
        else memcpy(row, u"Unassigned", sizeof(u"Unassigned"));
        return;
    }
    uint16_t left[81], right[81];
    size_t n = mk8_device_name(mk8_attached_name(primary), left);
    if (!partner) {
        mk8_copy_name(row, left, n, 79);
        return;
    }
    size_t m = mk8_device_name(mk8_attached_name(partner), right);
    if (n + 3 + m > 24) {
        memcpy(row, u"Paired Joy-Cons", sizeof(u"Paired Joy-Cons"));
        return;
    }
    memcpy(row, left, n * sizeof(*row));
    memcpy(row + n, u" + ", 3 * sizeof(*row));
    memcpy(row + n + 3, right, m * sizeof(*row));
    row[n + 3 + m] = 0;
}

static void mk8_publish_names(void) {
    uint16_t rows[1 + EXTRA_PADS][80] = {0};
    for (unsigned i = 0; i <= EXTRA_PADS; i++)
        mk8_label_device(rows[i], i ? extra_pad[i - 1] : pad, mk8_partner[i], mk8_assigned_keyboard() == (int)i);
    SDL_LockSpinlock(&mk8_name_lock);
    memcpy(mk8_slot_names, rows, sizeof rows);
    SDL_UnlockSpinlock(&mk8_name_lock);
}

void mk8_controller_names(uint16_t out[4][80]) {
    SDL_LockSpinlock(&mk8_name_lock);
    memcpy(out, mk8_slot_names, sizeof mk8_slot_names);
    SDL_UnlockSpinlock(&mk8_name_lock);
    if (!out[0][0]) memcpy(out[0], u"Keyboard", sizeof(u"Keyboard"));
    for (unsigned i = 1; i <= EXTRA_PADS; i++)
        if (!out[i][0]) memcpy(out[i], u"Unassigned", sizeof(u"Unassigned"));
}

static uint64_t mk8_navigation_axes(uint64_t b, int x, int y) {
    if (x < -16000) b |= 1ull << BTN_LEFT;
    if (x > 16000) b |= 1ull << BTN_RIGHT;
    if (y > 16000) b |= 1ull << BTN_UP;
    if (y < -16000) b |= 1ull << BTN_DOWN;
    return b;
}

// SDL publishes the draft owner's buttons only while the assignment popup owns navigation.
static _Atomic uint64_t mk8_draft_navigation = UINT64_MAX;
uint64_t mk8_controller_p1_navigation(void) {
    uint64_t draft = atomic_load(&mk8_draft_navigation);
    if (draft != UINT64_MAX) return draft;
    Mk8KeyState keys = mk8_player_keys(0);
    return mk8_navigation_axes(keys.buttons | pad_buttons,keys.lx + pad_lx,keys.ly + pad_ly);
}

// A held control must be released before a newly uncovered character-select screen sees it.
static _Atomic uint64_t mk8_extra_held[EXTRA_PADS];
static _Atomic unsigned mk8_extra_axes_held[EXTRA_PADS];
void mk8_controller_hold_extra_inputs(void) {
    for (int i=0;i<EXTRA_PADS;i++) {
        Mk8KeyState keys = mk8_player_keys(i+1);
        mk8_extra_held[i]=keys.buttons | extra_buttons[i] | (i==0 ? script2_buttons : 0);
        int32_t axes[]={keys.lx+extra_lx[i]+(i==0 ? script2_lx : 0),keys.ly+extra_ly[i]+(i==0 ? script2_ly : 0),keys.rx+extra_rx[i],keys.ry+extra_ry[i]};
        unsigned mask=0;
        for (unsigned j=0;j<4;j++) if (abs(axes[j])>8000) mask|=1u<<j;
        mk8_extra_axes_held[i]=mask;
    }
}
static void mk8_controller_filter_extra(int i,uint64_t *b,int32_t *lx,int32_t *ly,int32_t *rx,int32_t *ry) {
    if (mk8_extra_held[i]) *b &= ~atomic_fetch_and(&mk8_extra_held[i],*b);
    unsigned mask=mk8_extra_axes_held[i];
    if (!mask) return;
    int32_t *axes[]={lx,ly,rx,ry};
    for (unsigned j=0;j<4;j++) if (mask & (1u<<j)) {
        if (abs(*axes[j])<=8000) atomic_fetch_and(&mk8_extra_axes_held[i],~(1u<<j));
        else *axes[j]=0;
    }
}

static int mk8_gamepad_axis(SDL_Gamepad *left, SDL_Gamepad *right, SDL_GamepadAxis axis) {
    if (!right) return SDL_GetGamepadAxis(left, axis);
    switch (axis) {
    case SDL_GAMEPAD_AXIS_LEFTX: return -(int)SDL_GetGamepadAxis(left, SDL_GAMEPAD_AXIS_LEFTY);
    case SDL_GAMEPAD_AXIS_LEFTY: return SDL_GetGamepadAxis(left, SDL_GAMEPAD_AXIS_LEFTX);
    case SDL_GAMEPAD_AXIS_RIGHTX: return SDL_GetGamepadAxis(right, SDL_GAMEPAD_AXIS_LEFTY);
    case SDL_GAMEPAD_AXIS_RIGHTY: return -(int)SDL_GetGamepadAxis(right, SDL_GAMEPAD_AXIS_LEFTX);
    default: return 0;
    }
}

static uint32_t mk8_pair_buttons(SDL_Gamepad *left, SDL_Gamepad *right) {
    // SDL HIDAPI Switch mini-mode maps the upright shoulders/triggers onto the paddles.
    static const struct { SDL_GamepadButton input; unsigned output; } left_map[] = {
        {SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_DPAD_UP},
        {SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_DPAD_DOWN},
        {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_DPAD_LEFT},
        {SDL_GAMEPAD_BUTTON_NORTH, SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
        {SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_BACK},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_LEFT_STICK},
        {SDL_GAMEPAD_BUTTON_LEFT_PADDLE1, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
        {SDL_GAMEPAD_BUTTON_LEFT_PADDLE2, PADB_LTRIGGER},
    }, right_map[] = {
        {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST},
        {SDL_GAMEPAD_BUTTON_NORTH, SDL_GAMEPAD_BUTTON_WEST},
        {SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_NORTH},
        {SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_SOUTH},
        {SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_START},
        {SDL_GAMEPAD_BUTTON_GUIDE, SDL_GAMEPAD_BUTTON_GUIDE},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK},
        {SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
        {SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2, PADB_RTRIGGER},
    };
    uint32_t buttons = 0;
    for (size_t i = 0; i < SDL_arraysize(left_map); i++)
        if (SDL_GetGamepadButton(left, left_map[i].input)) buttons |= 1u << left_map[i].output;
    for (size_t i = 0; i < SDL_arraysize(right_map); i++)
        if (SDL_GetGamepadButton(right, right_map[i].input)) buttons |= 1u << right_map[i].output;
    return buttons;
}
