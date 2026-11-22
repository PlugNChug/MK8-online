static void scan_gamepads(void);
static void mk8_sync_sensors(void);
bool input_user_blocked(const SDL_Event *event);
static void mk8_forget_seat(unsigned player);
static void mk8_forget_moved_grip(SDL_Gamepad *device);
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
    for (unsigned i = 0; i <= EXTRA_PADS; ++i)
        if (!slots || changed[i] || (keyboard == (int)i && mk8_assigned_keyboard() != keyboard))
            mk8_forget_seat(i);
    for (unsigned i=0; i<=EXTRA_PADS; ++i) if (changed[i])
        for (int side=0; side<2; ++side) if (staged[i][side]) mk8_forget_moved_grip(staged[i][side]);
    mk8_set_assigned_keyboard(keyboard);
    mk8_title_extra_seats = unchanged && mk8_title_extra_seats;
    mk8_explicit_slots = slots != NULL && (!unchanged || mk8_explicit_slots);
    scanned = mk8_explicit_slots;
    recompute_pad();
    for (unsigned i = 0; i < EXTRA_PADS; i++) recompute_extra(i);
    note_family();
    style_changed();
    SDL_UnlockJoysticks();
    if (!slots) scan_gamepads();
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

// Identity belongs to a player for this session; SDL attachment IDs alone do not survive reconnects.
static struct Mk8Grip {
    SDL_GUID guid;
    SDL_JoystickID attached;
    char *value;
    enum { MK8_SERIAL, MK8_PATH, MK8_NAME } kind;
    bool unique;
} mk8_seat_grips[1 + EXTRA_PADS][2];
static bool mk8_title_keyboard_fallback;
static struct Mk8PendingGrip {
    SDL_Gamepad *pad;
    struct Mk8PendingGrip *next;
} *mk8_pending_grips;

static void mk8_forget_seat(unsigned player) {
    struct Mk8Grip *grips = mk8_seat_grips[player];
    for (unsigned i=0; i<2; ++i) SDL_free(grips[i].value);
    memset(grips, 0, sizeof mk8_seat_grips[player]);
    mk8_reserved_seats &= ~(1u << player);
    if (!player) mk8_title_keyboard_fallback = false;
}

static const char *mk8_grip_value(SDL_Gamepad *g, int kind) {
    if (kind == MK8_SERIAL) return SDL_GetGamepadSerial(g);
    if (kind == MK8_PATH) return SDL_GetGamepadPath(g);
    return SDL_GetJoystickName(SDL_GetGamepadJoystick(g));
}

enum Mk8GripMatch { MK8_OTHER_GRIP, MK8_SAME_GRIP, MK8_ID_NOT_READY };

static enum Mk8GripMatch mk8_grip_match(const struct Mk8Grip *grip, SDL_Gamepad *g) {
    if (!grip->value || !g) return MK8_OTHER_GRIP;
    SDL_GUID guid = SDL_GetJoystickGUIDForID(SDL_GetGamepadID(g));
    if (memcmp(&guid, &grip->guid, sizeof guid)) return MK8_OTHER_GRIP;
    const char *value = mk8_grip_value(g, grip->kind);
    if (!value || !*value) return MK8_ID_NOT_READY;
    return !strcmp(value, grip->value) ? MK8_SAME_GRIP : MK8_OTHER_GRIP;
}

static SDL_JoystickID mk8_unique_grip(const struct Mk8Grip *grip) {
    SDL_JoystickID match = 0;
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    for (int i=0; i<count; ++i) {
        if (!real_gamepad(ids[i])) continue;
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

static void mk8_remember_connected(void) {
    for (unsigned player=0; player<=EXTRA_PADS; ++player) {
        for (int side=0; side<2; ++side) {
            SDL_Gamepad *g = side ? mk8_partner[player] : *mk8_primary_slot(player);
            struct Mk8Grip *grip = &mk8_seat_grips[player][side];
            if (!g || !SDL_GamepadConnected(g) || grip->attached == SDL_GetGamepadID(g)) continue;
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
}

static bool mk8_pending_identity(SDL_Gamepad *g) {
    for (unsigned p=0; p<=EXTRA_PADS; ++p) if (mk8_reserved_seats & (1u << p))
        for (int i=0; i<2; ++i) {
            struct Mk8Grip *grip = &mk8_seat_grips[p][i];
            if (grip->unique && mk8_grip_match(grip,g)==MK8_ID_NOT_READY) return true;
        }
    return false;
}

static void mk8_restore_seat(unsigned p) {
    SDL_LockJoysticks();
    SDL_Gamepad *handles[2] = {0};
    struct Mk8Grip *grips = mk8_seat_grips[p];
    for (int side=0; side<2 && grips[side].attached; ++side) {
        struct Mk8Grip *grip = &grips[side];
        SDL_Gamepad *g = real_gamepad(grip->attached) ? SDL_OpenGamepad(grip->attached) : NULL;
        if (!g || !SDL_GamepadConnected(g)) {
            if (g) SDL_CloseGamepad(g);
            SDL_JoystickID id = grip->unique ? mk8_unique_grip(grip) : 0;
            g = id ? SDL_OpenGamepad(id) : NULL;
        }
        handles[side] = g;
        if (!g || !SDL_GamepadConnected(g)) goto unavailable;
        for (unsigned j=0; j<=EXTRA_PADS; ++j)
            if (*mk8_primary_slot(j) == g || mk8_partner[j] == g) goto unavailable;
    }
    if (!handles[0]) goto unavailable;
    *mk8_primary_slot(p) = handles[0];
    mk8_partner[p] = handles[1];
    mk8_paired_slot[p] = handles[1] != NULL;
    for (int side=0; side<2 && handles[side]; ++side) {
        grips[side].attached = SDL_GetGamepadID(handles[side]);
        mk8_rumble_attach(p + (1+EXTRA_PADS)*side, handles[side]);
    }
    mk8_reserved_seats &= ~(1u << p);
    if (!p && mk8_title_keyboard_fallback) {
        mk8_set_assigned_keyboard(-1);
        mk8_title_keyboard_fallback = false;
    }
    if (p) recompute_extra(p-1); else recompute_pad();
    note_family();
    style_changed();
    SDL_UnlockJoysticks();
    return;

unavailable:
    for (int side=0; side<2; ++side) if (handles[side]) SDL_CloseGamepad(handles[side]);
    SDL_UnlockJoysticks();
}

static unsigned mk8_reconnect_candidates(SDL_Gamepad *g,int *strength_out) {
    SDL_JoystickID added=SDL_GetGamepadID(g);
    unsigned candidates = 0;
    int strongest = -1;
    for (unsigned p=0; p<=EXTRA_PADS; ++p) {
        for (int side=0; side<2; ++side) {
            struct Mk8Grip *grip = &mk8_seat_grips[p][side];
            enum Mk8GripMatch match = mk8_grip_match(grip,g);
            if (match!=MK8_SAME_GRIP && grip->attached!=added) continue;
            if (match==MK8_SAME_GRIP && grip->unique && grip->attached!=added && mk8_unique_grip(grip)!=added)
                grip->unique=false;
            if (!(mk8_reserved_seats & (1u << p))) continue;
            // An unchanged attachment or unique serial is stronger than a lookalike's model name.
            int strength = grip->attached==added ? 4 : grip->unique ? 3-(int)grip->kind : 0;
            if (strength>strongest) { strongest=strength; candidates=0; }
            if (strength==strongest) candidates |= 1u << p;
        }
    }
    *strength_out=strongest;
    return candidates;
}

static void mk8_forget_moved_grip(SDL_Gamepad *g) {
    int strength;
    unsigned candidates=mk8_reconnect_candidates(g,&strength);
    for (unsigned p=0; strength>0 && p<=EXTRA_PADS; ++p)
        if (candidates==(1u<<p)) mk8_forget_seat(p);
}

static void mk8_route_gamepad(SDL_JoystickID added) {
    if (!real_gamepad(added)) return;
    // SDL may deliver the replacement ADD before the old REMOVE.
    for (unsigned p=0; p<=EXTRA_PADS; ++p) {
        SDL_Gamepad *g = *mk8_primary_slot(p);
        if (g && !SDL_GamepadConnected(g)) mk8_remove_assigned(SDL_GetGamepadID(g));
        else if (mk8_partner[p] && !SDL_GamepadConnected(mk8_partner[p]))
            mk8_remove_assigned(SDL_GetGamepadID(mk8_partner[p]));
    }
    // Keep the driver open through identity checks and assignment, including their temporary SDL references.
    SDL_Gamepad *g = SDL_OpenGamepad(added);
    // Automatic discovery previously got a second attempt when the identity probe failed.
    if (!g && (mk8_title_extra_seats || !mk8_explicit_slots)) g = SDL_OpenGamepad(added);
    if (!g) return;
    int strongest;
    unsigned candidates=mk8_reconnect_candidates(g,&strongest);
    if (strongest<3 && mk8_pending_identity(g)) {
        // Retain the device while HID obtains the serial; a later input event retries that identity.
        struct Mk8PendingGrip *entry = mk8_pending_grips;
        while (entry && entry->pad != g) entry = entry->next;
        if (!entry) {
            entry = SDL_malloc(sizeof *entry);
            if (entry) {
                entry->pad = g;
                entry->next = mk8_pending_grips;
                mk8_pending_grips = entry;
                return;
            }
        }
    } else if (candidates) {
        for (unsigned p=0; p<=EXTRA_PADS; ++p)
            if (candidates == (1u << p)) mk8_restore_seat(p);
    } else if (mk8_title_extra_seats || (!mk8_explicit_slots && (mk8_reserved_seats & 1))) {
        open_extra(added);
    } else if (!mk8_explicit_slots) {
        mk8_open_gamepad(added);
    }
    SDL_CloseGamepad(g);
}

static void mk8_retry_pending_grips(SDL_JoystickID id, bool removed) {
    struct Mk8PendingGrip **link = &mk8_pending_grips;
    while (*link) {
        struct Mk8PendingGrip *p = *link;
        SDL_JoystickID candidate = SDL_GetGamepadID(p->pad);
        if (id && candidate != id) { link = &p->next; continue; }
        if (!removed && mk8_pending_identity(p->pad)) { link = &p->next; continue; }
        *link = p->next;
        if (!removed && SDL_GamepadConnected(p->pad)) mk8_route_gamepad(candidate);
        SDL_CloseGamepad(p->pad);
        SDL_free(p);
    }
}

static void mk8_remove_assigned(SDL_JoystickID id) {
    SDL_LockJoysticks();
    for (unsigned i = 0; i <= EXTRA_PADS; i++) {
        SDL_Gamepad *primary = *mk8_primary_slot(i), *right = mk8_partner[i];
        if ((primary && SDL_GetGamepadID(primary) == id) || (right && SDL_GetGamepadID(right) == id)) {
            if (mk8_seat_grips[i][0].attached) mk8_reserved_seats |= 1u << i;
            if (i == 0 && mk8_seat_grips[0][0].attached) {
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
