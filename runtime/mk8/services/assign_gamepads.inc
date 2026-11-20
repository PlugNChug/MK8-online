static void scan_gamepads(void);
static void mk8_sync_sensors(void);
bool input_user_blocked(const SDL_Event *event);
static void mk8_forget_title_pad(void);
static void mk8_remove_assigned(SDL_JoystickID id);

static SDL_Gamepad **mk8_primary_slot(unsigned player) {
    return player ? &extra_pad[player - 1] : &pad;
}

int mk8_pause_owner(SDL_JoystickID id) {
    SDL_Gamepad *device = SDL_GetGamepadFromID(id);
    if (device) for (int i = 0; i <= EXTRA_PADS; ++i)
        if (device == *mk8_primary_slot(i) && !mk8_partner[i]) return i;
    return -1;
}

void mk8_release_pause_button(SDL_JoystickID id, unsigned button) {
    int player = mk8_pause_owner(id);
    if (player < 0 || !mk8_settings_shared_race()) return;
    unsigned buttons = mk8_pause_bindings(1u << button);
    SDL_LockSpinlock(&mk8_pause_lock);
    mk8_pause[player].pending |= buttons;
    SDL_UnlockSpinlock(&mk8_pause_lock);
}

void mk8_suppress_settings_buttons(SDL_JoystickID id, uint32_t buttons) {
    SDL_Gamepad *device = SDL_GetGamepadFromID(id);
    if (!device) return;
    for (unsigned i = 0; i < SDL_arraysize(mk8_shortcut); ++i) {
        if (device != *mk8_primary_slot(i) && device != mk8_partner[i]) continue;
        mk8_shortcut[i].device = id;
        mk8_shortcut[i].buttons |= buttons;
        if (i) recompute_extra(i - 1);
        else recompute_pad();
        break;
    }
}

static void mk8_release_slot(unsigned player) {
    SDL_Gamepad **primary = mk8_primary_slot(player);
    mk8_reset_pause(player);
    if (*primary) mk8_rumble_close(player, *primary);
    if (mk8_partner[player]) mk8_rumble_close(player + 1 + EXTRA_PADS, mk8_partner[player]);
    *primary = mk8_partner[player] = NULL;
    mk8_paired_slot[player] = false;
}

// SDL event thread only. SDL instance IDs last for one attachment; NULL restores normal discovery.
static bool mk8_assign_layout(const SDL_JoystickID slots[1 + EXTRA_PADS][2], int keyboard, bool keep_unchanged_discovery) {
    SDL_Gamepad *staged[1 + EXTRA_PADS][2] = {0};
    if (slots && input_user_blocked(NULL)) return false;
    SDL_LockJoysticks();
    for (unsigned i = 0; slots && i <= EXTRA_PADS; i++) {
        for (unsigned side = 0; side < 2; side++) {
            SDL_JoystickID id = slots[i][side];
            if (!id) continue;
            for (unsigned j = 0; j < i; j++)
                if (slots[j][0] == id || slots[j][1] == id) goto invalid;
            if (side && (!slots[i][0] || slots[i][0] == id)) goto invalid;
            if (!real_gamepad(id) || !SDL_IsGamepad(id)) goto invalid;
            staged[i][side] = SDL_OpenGamepad(id);
            if (!staged[i][side] || !SDL_GamepadConnected(staged[i][side])) goto invalid;
            if (slots[i][1] &&
                (SDL_GetGamepadVendor(staged[i][side]) != 0x057e ||
                 SDL_GetGamepadProduct(staged[i][side]) != 0x2006 + side ||
                 SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_VERTICAL_JOY_CONS, false))) goto invalid;
        }
    }
    // References are secured before releasing the old slots, including devices moving between players.
    bool changed[1 + EXTRA_PADS];
    bool unchanged = keep_unchanged_discovery && keyboard == mk8_assigned_keyboard();
    for (unsigned i = 0; i <= EXTRA_PADS; i++) {
        changed[i] = staged[i][0] != *mk8_primary_slot(i) || staged[i][1] != mk8_partner[i];
        if (changed[i]) {
            unchanged = false;
            mk8_release_slot(i);
        }
    }
    mk8_sync_sensors();  // Discard old sensor history before publishing the new assignments.
    for (unsigned i = 0; i <= EXTRA_PADS; i++) {
        if (!changed[i]) {
            for (unsigned side = 0; side < 2; side++)
                if (staged[i][side]) SDL_CloseGamepad(staged[i][side]);
            continue;
        }
        *mk8_primary_slot(i) = staged[i][0];
        mk8_partner[i] = staged[i][1];
        mk8_paired_slot[i] = staged[i][1] != NULL;
        if (staged[i][0]) mk8_rumble_attach(i, staged[i][0]);
        if (staged[i][1]) mk8_rumble_attach(i + 1 + EXTRA_PADS, staged[i][1]);
    }
    bool same_owner = keep_unchanged_discovery && !changed[0] && (keyboard == 0) == (mk8_assigned_keyboard() == 0);
    mk8_set_assigned_keyboard(keyboard);
    if (!same_owner) mk8_forget_title_pad();
    mk8_title_extra_seats = unchanged && mk8_title_extra_seats;
    mk8_explicit_slots = slots != NULL && (!unchanged || mk8_explicit_slots);
    scanned = mk8_explicit_slots;
    recompute_pad();
    for (unsigned i = 0; i < EXTRA_PADS; i++) recompute_extra(i);
    note_family();
    style_changed();
    SDL_UnlockJoysticks();
    if (!slots) {
        mk8_set_joycon_mode(g_settings.joycon_mode);
        scan_gamepads();
    }
    return true;

invalid:
    for (unsigned i = 0; i <= EXTRA_PADS; i++)
        for (unsigned side = 0; side < 2; side++)
            if (staged[i][side]) SDL_CloseGamepad(staged[i][side]);
    SDL_UnlockJoysticks();
    return false;
}

bool mk8_assign_gamepads(const SDL_JoystickID slots[1 + EXTRA_PADS][2]) {
    return mk8_assign_layout(slots, slots ? mk8_assigned_keyboard() : 0, false);
}

// SDL attachment IDs change on reconnect. Keep only the current title owner's identity in memory.
static struct Mk8TitleGrip {
    SDL_GUID guid;
    SDL_JoystickID attached;
    char *value;
    enum { MK8_SERIAL, MK8_PATH, MK8_NAME } kind;
    bool unique;
} mk8_title_grips[2];
static bool mk8_title_lost, mk8_title_had_pair, mk8_title_keyboard_fallback;
static struct Mk8PendingGrip {
    SDL_Gamepad *pad;
    struct Mk8PendingGrip *next;
} *mk8_pending_grips;

static void mk8_forget_title_pad(void) {
    for (unsigned i=0; i<2; ++i) SDL_free(mk8_title_grips[i].value);
    memset(mk8_title_grips, 0, sizeof mk8_title_grips);
    mk8_title_lost = mk8_title_had_pair = mk8_title_keyboard_fallback = false;
}

static const char *mk8_grip_value(SDL_Gamepad *g, int kind) {
    if (kind == MK8_SERIAL) return SDL_GetGamepadSerial(g);
    if (kind == MK8_PATH) return SDL_GetGamepadPath(g);
    return SDL_GetJoystickName(SDL_GetGamepadJoystick(g));
}

enum Mk8GripMatch { MK8_OTHER_GRIP, MK8_SAME_GRIP, MK8_ID_NOT_READY };

static enum Mk8GripMatch mk8_grip_match(const struct Mk8TitleGrip *grip, SDL_Gamepad *g) {
    if (!grip->value || !g) return MK8_OTHER_GRIP;
    SDL_GUID guid = SDL_GetJoystickGUIDForID(SDL_GetGamepadID(g));
    if (memcmp(&guid, &grip->guid, sizeof guid)) return MK8_OTHER_GRIP;
    const char *value = mk8_grip_value(g, grip->kind);
    if (!value || !*value) return MK8_ID_NOT_READY;
    return !strcmp(value, grip->value) ? MK8_SAME_GRIP : MK8_OTHER_GRIP;
}

static SDL_JoystickID mk8_unique_grip(const struct Mk8TitleGrip *grip) {
    SDL_JoystickID match = 0;
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    for (int i=0; i<count; ++i) {
        SDL_GUID guid = SDL_GetJoystickGUIDForID(ids[i]);
        if (memcmp(&guid, &grip->guid, sizeof guid)) continue;
        SDL_Gamepad *g = SDL_OpenGamepad(ids[i]);
        bool same = g && SDL_GamepadConnected(g) && mk8_grip_match(grip, g) == MK8_SAME_GRIP;
        if (g) SDL_CloseGamepad(g);
        if (!same) continue;
        if (match) { match = 0; break; }
        match = ids[i];
    }
    SDL_free(ids);
    return match;
}

static void mk8_remember_title_pad(void) {
    if (!pad) return;
    mk8_title_had_pair = mk8_partner[0] != NULL;
    for (int i=0; i<=mk8_title_had_pair; ++i) {
        SDL_Gamepad *g = i ? mk8_partner[0] : pad;
        struct Mk8TitleGrip *grip = &mk8_title_grips[i];
        grip->attached = SDL_GetGamepadID(g);
        grip->guid = SDL_GetJoystickGUIDForID(grip->attached);
        for (int kind=MK8_SERIAL; kind<=MK8_NAME; ++kind) {
            const char *value = mk8_grip_value(g, kind);
            if (!value || !*value) continue;
            SDL_free(grip->value);
            grip->kind = kind;
            grip->value = SDL_strdup(value);
            grip->unique = mk8_unique_grip(grip) == grip->attached;
            if (grip->unique) break;
        }
    }
}

static bool mk8_recover_title_pad(SDL_JoystickID added) {
    if (!mk8_title_grips[0].attached) return false;
    // Physical detach and replacement can both precede delivery of the old REMOVED event.
    if (pad && !SDL_GamepadConnected(pad)) mk8_remove_assigned(mk8_title_grips[0].attached);
    else if (mk8_partner[0] && !SDL_GamepadConnected(mk8_partner[0]))
        mk8_remove_assigned(mk8_title_grips[1].attached);
    SDL_Gamepad *g = SDL_OpenGamepad(added);
    if (!g) return false;
    bool relevant = false, unknown = false;
    for (int i=0; i<=mk8_title_had_pair; ++i) {
        struct Mk8TitleGrip *grip = &mk8_title_grips[i];
        enum Mk8GripMatch match = mk8_grip_match(grip, g);
        if (match == MK8_ID_NOT_READY && grip->unique) unknown = true;
        if (match != MK8_SAME_GRIP && grip->attached != added) continue;
        relevant = true;
        if (match == MK8_SAME_GRIP && grip->unique && mk8_unique_grip(grip) != added) grip->unique = false;
    }
    if (mk8_title_lost && !relevant && unknown) {
        // Hold one open reference while the HID driver obtains its serial. Never guess by model.
        struct Mk8PendingGrip *p = mk8_pending_grips;
        while (p && p->pad != g) p = p->next;
        if (!p) {
            p = SDL_malloc(sizeof *p);
            if (!p) {
                LOG("[input] unable to track an unidentified reconnecting gamepad\n");
                SDL_CloseGamepad(g);
                return true;
            }
            p->pad = g;
            p->next = mk8_pending_grips;
            mk8_pending_grips = p;
        } else SDL_CloseGamepad(g);
        return true;
    }
    SDL_CloseGamepad(g);
    if (!relevant || !mk8_title_lost) return false;
    SDL_JoystickID slots[4][2] = {0};
    for (int i=1; i<4; ++i) {
        SDL_Gamepad *owner = *mk8_primary_slot(i);
        slots[i][0] = owner ? SDL_GetGamepadID(owner) : 0;
        slots[i][1] = mk8_partner[i] ? SDL_GetGamepadID(mk8_partner[i]) : 0;
    }
    for (int i=0; i<=mk8_title_had_pair; ++i) {
        struct Mk8TitleGrip *grip = &mk8_title_grips[i];
        SDL_JoystickID id = grip->attached;
        // An unchanged SDL attachment uniquely identifies the surviving half, even with lookalikes.
        SDL_Gamepad *original = SDL_OpenGamepad(id);
        bool present = original && SDL_GamepadConnected(original);
        if (original) SDL_CloseGamepad(original);
        if (!present) id = grip->unique ? mk8_unique_grip(grip) : 0;
        if (!id) return true;
        for (int p=1; p<4; ++p) if (slots[p][0] == id || slots[p][1] == id) return true;
        slots[0][i] = id;
    }
    int keyboard = mk8_title_keyboard_fallback ? -1 : mk8_assigned_keyboard();
    bool automatic_extras = mk8_title_extra_seats;
    if (mk8_assign_layout(slots, keyboard, false)) {
        mk8_title_extra_seats = automatic_extras;
        mk8_remember_title_pad();
    }
    return true;
}

static void mk8_retry_pending_grips(SDL_JoystickID id, bool removed) {
    struct Mk8PendingGrip **link = &mk8_pending_grips;
    while (*link) {
        struct Mk8PendingGrip *p = *link;
        SDL_JoystickID candidate = SDL_GetGamepadID(p->pad);
        if (id && candidate != id) { link = &p->next; continue; }
        if (!removed && mk8_title_lost) {
            bool unknown = false;
            for (int i=0; i<=mk8_title_had_pair; ++i)
                if (mk8_grip_match(&mk8_title_grips[i], p->pad) == MK8_ID_NOT_READY) unknown = true;
            if (unknown) { link = &p->next; continue; }
        }
        *link = p->next;
        if (!removed && SDL_GamepadConnected(p->pad) &&
            !(mk8_title_lost && mk8_recover_title_pad(candidate)) && mk8_title_extra_seats)
            open_extra(candidate);
        SDL_CloseGamepad(p->pad);
        SDL_free(p);
    }
}

static void mk8_remove_assigned(SDL_JoystickID id) {
    SDL_LockJoysticks();
    for (unsigned i = 0; i <= EXTRA_PADS; i++) {
        SDL_Gamepad *primary = *mk8_primary_slot(i), *right = mk8_partner[i];
        if ((primary && SDL_GetGamepadID(primary) == id) || (right && SDL_GetGamepadID(right) == id)) {
            if (i == 0 && mk8_title_grips[0].attached) {
                mk8_title_lost = true;
                if (mk8_assigned_keyboard() < 0) {
                    mk8_set_assigned_keyboard(0);
                    mk8_title_keyboard_fallback = true;
                }
            }
            mk8_release_slot(i);
            if (i) recompute_extra(i - 1);
            else recompute_pad();
            note_family();
            style_changed();
            break;
        }
    }
    SDL_UnlockJoysticks();
}

// Resolve one-based enumeration positions once. Removed devices never give their slot to a stranger.
static bool mk8_startup_layout(const char *layout, const SDL_JoystickID *ids, int count) {
    SDL_JoystickID slots[1 + EXTRA_PADS][2] = {0};
    for (unsigned i = 0; i <= EXTRA_PADS; i++) {
        for (unsigned side = 0; side < 2; side++) {
            if (*layout < '0' || *layout > '9') return false;
            char *end;
            unsigned long index = strtoul(layout, &end, 10);
            if (count < 0 || index > (unsigned)count || (!index && side)) return false;
            slots[i][side] = index ? ids[index - 1] : 0;
            layout = end;
            if (side || *layout != '+') break;
            layout++;
        }
        if (i < EXTRA_PADS && *layout++ != ',') return false;
    }
    return !*layout && mk8_assign_gamepads(slots);
}
