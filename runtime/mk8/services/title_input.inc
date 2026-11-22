// The guest publishes the native title's interactive phase. SDL owns device handles and assignments.
enum { MK8_TITLE_OFF, MK8_TITLE_WAIT, MK8_TITLE_ACCEPTED, MK8_TITLE_ARMED, MK8_LAN_WAIT, MK8_LAN_ACCEPTED };
static _Atomic int mk8_title_state;
static _Atomic bool mk8_lan_chosen;
static SDL_Scancode mk8_lan_claim_key;
static SDL_Event mk8_lan_pointer;
static _Atomic unsigned mk8_title_visit;
static _Atomic unsigned mk8_title_neutral_visit;
static _Atomic bool mk8_title_confirm_ready;
static _Atomic unsigned mk8_title_prelude_frame;
static struct {
    unsigned visit, frame;
    SDL_JoystickID left, right;
} mk8_title_pending;
static unsigned mk8_observed_visit;
static pthread_mutex_t mk8_title_lock = PTHREAD_MUTEX_INITIALIZER;

bool mk8_title_manual_input(void) {
    const char *layout = getenv("MK8_GAMEPAD_LAYOUT");
    const char *script = getenv("RECOMP_PAD_SCRIPT");
    return !(layout && *layout && mk8_startup_preset >= 0) && !(script && *script) && !input_user_blocked(NULL);
}

void mk8_title_input_phase(enum Mk8TitlePhase phase) {
    int state = mk8_title_state;
    // The direct LAN room browser owns the claim until the guest leaves it.
    if (state >= MK8_LAN_WAIT) return;
    int target = phase == MK8_TITLE_INACTIVE ? MK8_TITLE_OFF :
                 phase == MK8_TITLE_PRELUDE ? MK8_TITLE_ARMED : MK8_TITLE_WAIT;
    if (target == MK8_TITLE_ARMED) ++mk8_title_prelude_frame;
    // Claim during the entrance, then deliver a fresh edge when the stock prompt can act.
    // Multiple Npad reads in one guest frame must see the same neutral state.
    mk8_title_confirm_ready = phase == MK8_TITLE_PROMPT && state != MK8_TITLE_OFF &&
                              mk8_title_neutral_visit == mk8_title_visit;
    if (state == target || (target == MK8_TITLE_WAIT && state == MK8_TITLE_ACCEPTED)) return;
    if (state == MK8_TITLE_OFF && target != MK8_TITLE_OFF && !mk8_title_manual_input()) return;
    pthread_mutex_lock(&mk8_title_lock);
    if (mk8_title_state < MK8_LAN_WAIT && (target != MK8_TITLE_WAIT || mk8_title_state != MK8_TITLE_ACCEPTED)) {
        if (target != MK8_TITLE_OFF && (state == MK8_TITLE_OFF || target == MK8_TITLE_ARMED))
            ++mk8_title_visit;
        mk8_title_state = target;
    }
    pthread_mutex_unlock(&mk8_title_lock);
}

// Called by the guest's page observer. Discover rooms while SDL waits for the first fresh input.
int mk8_lan_input_active(bool room_list) {
    int state = mk8_title_state;
    if (state == MK8_LAN_WAIT && room_list) return 1;
    if (state < MK8_LAN_WAIT && (!room_list || mk8_lan_chosen || !mk8_title_manual_input())) return 0;
    pthread_mutex_lock(&mk8_title_lock);
    int result = 0;
    if (mk8_title_state == MK8_LAN_ACCEPTED) {
        mk8_title_state = MK8_TITLE_OFF;
        result = room_list ? 2 : 0;
    } else if (room_list && !mk8_lan_chosen && mk8_title_state == MK8_TITLE_OFF) {
        ++mk8_title_visit;
        mk8_title_state = MK8_LAN_WAIT;
        result = 1;
    } else if (!room_list && mk8_title_state == MK8_LAN_WAIT) {
        mk8_title_state = MK8_TITLE_OFF;
    }
    pthread_mutex_unlock(&mk8_title_lock);
    return result;
}

static bool mk8_claim_waiting(int state) { return state == MK8_TITLE_WAIT || state == MK8_LAN_WAIT; }

static bool mk8_title_pending_current(void) {
    return mk8_title_pending.visit && mk8_title_pending.visit == mk8_title_visit &&
           mk8_title_pending.frame == mk8_title_prelude_frame;
}

static void mk8_title_choose(SDL_JoystickID left, SDL_JoystickID right) {
    if (mk8_title_pending_current()) return;
    if (mk8_title_state == MK8_TITLE_ARMED) {
        mk8_title_pending.visit = mk8_observed_visit;
        mk8_title_pending.frame = mk8_title_prelude_frame;
        mk8_title_pending.left = left;
        mk8_title_pending.right = right;
        return;
    }
    SDL_JoystickID slots[4][2];
    int keyboard = mk8_assigned_keyboard(), vacancy = -1;
    for (unsigned i = 0; i < 4; ++i) {
        SDL_Gamepad *primary = *mk8_primary_slot(i);
        slots[i][0] = primary ? SDL_GetGamepadID(primary) : 0;
        slots[i][1] = mk8_partner[i] ? SDL_GetGamepadID(mk8_partner[i]) : 0;
    }
    bool move_keyboard = left && keyboard == 0 && !slots[0][0];
    for (int i = 0; i < 4 && left; ++i) {
        bool primary = slots[i][0] == left || (right && slots[i][0] == right);
        bool partner = slots[i][1] == left || (right && slots[i][1] == right);
        if (partner) slots[i][1] = 0;
        if (primary) {
            slots[i][0] = slots[i][1];  // A partner whose grip wasn't selected stays seated sideways.
            slots[i][1] = 0;
            if (i && !slots[i][0] && vacancy < 0) vacancy = i;
        }
    }
    if (!left) {
        if (keyboard > 0 && !slots[keyboard][0]) vacancy = keyboard;
        keyboard = 0;
    } else if (keyboard == 0) keyboard = -1;
    if (vacancy < 0) for (int i = 1; i < 4; ++i)
        if (!slots[i][0] && i != keyboard && !(mk8_reserved_seats & (1u << i))) { vacancy = i; break; }
    if (move_keyboard) keyboard = vacancy;
    if (vacancy >= 0 && slots[0][0]) memcpy(slots[vacancy], slots[0], sizeof slots[0]);
    slots[0][0] = left;
    slots[0][1] = right;
    pthread_mutex_lock(&mk8_title_lock);
    bool automatic_extras = !mk8_explicit_slots || mk8_title_extra_seats;
    int state = mk8_title_state;
    if (mk8_claim_waiting(state) && mk8_title_visit == mk8_observed_visit &&
        mk8_assign_layout(slots, keyboard, false)) {
        mk8_title_extra_seats = automatic_extras;
        if (!left) mk8_forget_seat(0);
        if (state == MK8_LAN_WAIT) {
            // The next HID poll can follow immediately, before the page observer sees the claim.
            atomic_fetch_or(&mk8_applet_suppressed, mk8_controller_p1_navigation());
            mk8_controller_hold_extra_inputs();
            mk8_lan_chosen = true;
            mk8_title_state = MK8_LAN_ACCEPTED;
        } else mk8_title_state = MK8_TITLE_ACCEPTED;
    }
    pthread_mutex_unlock(&mk8_title_lock);
}

// Only the last prelude frame can straddle the guest update that enables input.
void mk8_title_after_events(void) {
    extern bool menu_input_captured(void);
    if (!mk8_title_pending.visit) return;
    int state = mk8_title_state;
    if (mk8_title_pending_current() && mk8_title_focused && !mk8_editor.rows &&
        !menu_input_captured() && !mk8_settings_page_active()) {
        if (state == MK8_TITLE_ARMED) return;
        // A full SDL batch can leave a focus change or menu shortcut queued for the next batch.
        if (state == MK8_TITLE_WAIT && SDL_HasEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST)) return;
        mk8_title_pending.visit = 0;
        if (state == MK8_TITLE_WAIT) mk8_title_choose(mk8_title_pending.left, mk8_title_pending.right);
        return;
    }
    mk8_title_pending.visit = 0;
}

void mk8_title_pump(void) {
    int title_state = mk8_title_state;
    bool waiting = mk8_claim_waiting(title_state) || title_state == MK8_TITLE_ARMED;
    unsigned visit = mk8_title_visit;
    if (mk8_editor.rows || (waiting == mk8_title_observing && (!waiting || visit == mk8_observed_visit))) return;
    if (mk8_title_observing) {
        mk8_edit_end();
        mk8_title_observing = false;
    }
    if (!waiting) return;
    mk8_title_observing = true;
    mk8_observed_visit = visit;
    mk8_editor.focused = mk8_title_focused;
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    for (int i = 0; i < count; ++i) mk8_edit_observe(ids[i]);
    SDL_free(ids);
    mk8_edit_listen(0, false);
}

bool mk8_title_event(const SDL_Event *e, bool captured) {
    if (e->type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        mk8_lan_claim_key = SDL_SCANCODE_UNKNOWN;
        mk8_lan_pointer.type = 0;
    }
    // A release without its matching down can still affect the touch gesture counter.
    if (mk8_lan_pointer.type == SDL_EVENT_FINGER_DOWN &&
        (e->type == SDL_EVENT_FINGER_MOTION || e->type == SDL_EVENT_FINGER_UP || e->type == SDL_EVENT_FINGER_CANCELED) &&
        e->tfinger.touchID == mk8_lan_pointer.tfinger.touchID && e->tfinger.fingerID == mk8_lan_pointer.tfinger.fingerID) {
        if (e->type != SDL_EVENT_FINGER_MOTION) mk8_lan_pointer.type = 0;
        return true;
    }
    if (mk8_lan_pointer.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        if (e->type == SDL_EVENT_MOUSE_MOTION && e->motion.which == mk8_lan_pointer.button.which) return true;
        if (e->type == SDL_EVENT_MOUSE_BUTTON_UP && e->button.which == mk8_lan_pointer.button.which &&
            e->button.button == mk8_lan_pointer.button.button) {
            mk8_lan_pointer.type = 0;
            return true;
        }
    }
    if (mk8_lan_claim_key && (e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_KEY_UP) &&
        e->key.scancode == mk8_lan_claim_key) {
        if (e->type == SDL_EVENT_KEY_DOWN && e->key.repeat) return true;
        mk8_lan_claim_key = SDL_SCANCODE_UNKNOWN;
        if (e->type == SDL_EVENT_KEY_UP) return true;
    }
    if (mk8_applet_key(e)) return true;
    if (e->type == SDL_EVENT_WINDOW_FOCUS_LOST || e->type == SDL_EVENT_WINDOW_FOCUS_GAINED)
        mk8_title_focused = e->type == SDL_EVENT_WINDOW_FOCUS_GAINED;
    if (captured || !mk8_title_focused) mk8_title_pending.visit = 0;
    int state = mk8_title_state;
    if (!mk8_title_observing || (!mk8_claim_waiting(state) && state != MK8_TITLE_ARMED) ||
        mk8_observed_visit != mk8_title_visit) return false;
    bool focused = mk8_title_focused && !captured;
    if (focused != mk8_editor.focused) {
        mk8_editor.focused = focused;
        // While a menu owns input, events keep these physical states current. Polling on its
        // first uncaptured down would mistake the newly arrived button for a pre-held one.
        if (!focused || e->type == SDL_EVENT_WINDOW_FOCUS_GAINED) mk8_edit_listen(0, false);
    }
    switch (e->type) {
    case SDL_EVENT_GAMEPAD_ADDED: mk8_edit_observe(e->gdevice.which); break;
    case SDL_EVENT_GAMEPAD_REMOVED: mk8_edit_detach(e->gdevice.which); break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        Mk8JoinDevice *d = mk8_edit_device(e->gbutton.which);
        if (d) mk8_edit_button(d, e->gbutton.button, e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
        break;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        if (e->gaxis.axis != SDL_GAMEPAD_AXIS_LEFT_TRIGGER && e->gaxis.axis != SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) break;
        Mk8JoinDevice *d = mk8_edit_device(e->gaxis.which);
        if (!d || d->half) break;
        unsigned bit = e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? 1u : 2u;
        bool was_down = (d->triggers & bit) != 0;
        if (e->gaxis.value > 16000) d->triggers |= bit;
        else d->triggers &= ~bit;
        if (focused && !was_down && (d->triggers & bit)) mk8_title_choose(d->id, 0);
        break;
    }
    case SDL_EVENT_KEY_DOWN: {
        SDL_Scancode key = e->key.scancode;
        if (!focused || e->key.repeat || key <= SDL_SCANCODE_UNKNOWN || key >= SDL_SCANCODE_COUNT ||
            keys_down[key] || key == SDL_SCANCODE_ESCAPE || key == SDL_SCANCODE_AC_BACK ||
            (key >= SDL_SCANCODE_F1 && key <= SDL_SCANCODE_F12) ||
            (key >= SDL_SCANCODE_F13 && key <= SDL_SCANCODE_F24) ||
            (key >= SDL_SCANCODE_LCTRL && key <= SDL_SCANCODE_RGUI) ||
            (e->key.mod & (SDL_KMOD_CTRL|SDL_KMOD_ALT|SDL_KMOD_GUI))) break;
        mk8_title_choose(0, 0);
        if (state == MK8_LAN_WAIT && mk8_lan_chosen) {
            mk8_lan_claim_key = key;
            return true;
        }
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (e->button.which == SDL_TOUCH_MOUSEID) break;
        // fall through
    case SDL_EVENT_FINGER_DOWN:
        if (!focused) break;
        mk8_title_choose(0, 0);
        if (state == MK8_LAN_WAIT && mk8_lan_chosen) { mk8_lan_pointer = *e; return true; }
        return mk8_title_state == MK8_TITLE_ACCEPTED;
    }
    return false;
}

// Supply the stock confirm only at the title. Keep held claim inputs out of the following menu.
static void mk8_title_hid(unsigned player, uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    int request = atomic_load(&mk8_applet_request);
    if (request > 0 || request == MK8_APPLET_ABANDONED) {
        *buttons = 0;
        *lx = *ly = *rx = *ry = 0;
        return;
    }
    if (!player && atomic_load(&mk8_applet_suppressed))
        *buttons &= ~atomic_fetch_and(&mk8_applet_suppressed,*buttons);
    static _Atomic uint64_t held[4];
    int state = mk8_title_state;
    if (state == MK8_LAN_ACCEPTED || state == MK8_TITLE_ARMED) state = MK8_TITLE_OFF;
    int32_t *axes[4] = {lx, ly, rx, ry};
    if (state == MK8_TITLE_OFF && !held[player] && !mk8_title_held_axes[player]) return;
    unsigned mask = 0;
    for (int i = 0; i < 4; ++i) if (abs(*axes[i]) > 8000) mask |= 1u << i;
    if (state != MK8_TITLE_OFF) {
        held[player] = *buttons;
        mk8_title_held_axes[player] = mask;
        *buttons = player == 0 && state == MK8_TITLE_ACCEPTED && mk8_title_confirm_ready ? 1ull << BTN_A : 0;
        if (player == 0 && !*buttons)
            mk8_title_neutral_visit = mk8_title_visit;
        *lx = *ly = *rx = *ry = 0;
    } else {
        *buttons &= ~atomic_fetch_and(&held[player], *buttons);
        mask &= atomic_fetch_and(&mk8_title_held_axes[player], mask);
        // HID publishes each stick direction as both an axis and a button (bits 16–23).
        static const uint64_t axis_buttons[] = {5ull<<16, 5ull<<17, 5ull<<20, 5ull<<21};
        for (int i = 0; i < 4; ++i) if (mask & (1u << i)) {
            *axes[i] = 0;
            *buttons &= ~axis_buttons[i];
        }
    }
}
