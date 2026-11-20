#include "controller_edit.h"

// Only the SDL thread owns these device references and the draft. The guest sees copied labels and acknowledgments.
typedef struct Mk8JoinDevice {
    struct Mk8JoinDevice *next;
    SDL_Gamepad *pad;
    SDL_JoystickID id;
    Uint32 buttons, blocked;
    unsigned triggers;
    bool pause_chord;
    int half;  // 1 = left, 2 = right, 0 = full gamepad
} Mk8JoinDevice;

static struct {
    int rows, keyboard, listening;
    bool ascending, focused, ambiguous, retry, chose_keyboard;
    SDL_JoystickID slots[4][2], first, second;
    Mk8JoinDevice *devices;
} mk8_editor;

static bool mk8_title_observing;
static void mk8_title_choose(SDL_JoystickID left, SDL_JoystickID right);

static SDL_SpinLock mk8_edit_lock;
static struct {
    enum Mk8EditState state;
    int listening, row, other;
    enum Mk8EditCommand command;
    bool pending, committing, combined;
    uint16_t names[4][80];
} mk8_mail;

bool mk8_edit_command(enum Mk8EditCommand command, int row, int other) {
    SDL_LockSpinlock(&mk8_edit_lock);
    bool closed = mk8_mail.state == MK8_EDIT_CLOSED || mk8_mail.state == MK8_EDIT_APPLIED;
    bool ok = !mk8_mail.committing && (command == MK8_EDIT_CANCEL ||
              (!mk8_mail.pending && mk8_mail.state != MK8_EDIT_PENDING && (command == MK8_EDIT_BEGIN ? closed : !closed)));
    if (ok) {
        mk8_mail.command = command;
        mk8_mail.row = row;
        mk8_mail.other = other;
        mk8_mail.pending = true;
        mk8_mail.state = MK8_EDIT_PENDING;
    }
    SDL_UnlockSpinlock(&mk8_edit_lock);
    return ok;
}

enum Mk8EditState mk8_edit_view(uint16_t names[4][80], int *listening_row) {
    SDL_LockSpinlock(&mk8_edit_lock);
    enum Mk8EditState state = mk8_mail.state;
    if (names) memcpy(names, mk8_mail.names, sizeof mk8_mail.names);
    if (listening_row) *listening_row = mk8_mail.listening;
    SDL_UnlockSpinlock(&mk8_edit_lock);
    return state;
}

bool mk8_edit_has_combined(void) {
    SDL_LockSpinlock(&mk8_edit_lock);
    bool combined = mk8_mail.combined;
    SDL_UnlockSpinlock(&mk8_edit_lock);
    return combined;
}

static Mk8JoinDevice *mk8_edit_device(SDL_JoystickID id) {
    for (Mk8JoinDevice *d = mk8_editor.devices; d; d = d->next)
        if (d->id == id) return d;
    return NULL;
}

static void mk8_edit_publish(enum Mk8EditState state) {
    if (mk8_title_observing) return;
    uint16_t names[4][80] = {0};
    bool combined = false;
    for (Mk8JoinDevice *d = mk8_editor.devices; d; d = d->next)
        if (SDL_GetGamepadVendor(d->pad) == 0x057e && SDL_GetGamepadProduct(d->pad) == 0x2008) combined = true;
    for (int row = 0; row < mk8_editor.rows; row++) {
        Mk8JoinDevice *left = mk8_edit_device(mk8_editor.slots[row][0]);
        Mk8JoinDevice *right = mk8_edit_device(mk8_editor.slots[row][1]);
        if (row == mk8_editor.listening && mk8_editor.first) {
            left = mk8_edit_device(mk8_editor.first);
            right = mk8_edit_device(mk8_editor.second);
            if (right && left && left->half == 2) {
                Mk8JoinDevice *tmp = left; left = right; right = tmp;
            }
        }
        mk8_label_device(names[row], left ? left->pad : NULL, right ? right->pad : NULL, row == mk8_editor.keyboard);
    }
    SDL_LockSpinlock(&mk8_edit_lock);
    if (!mk8_mail.pending) {
        mk8_mail.state = state;
        mk8_mail.listening = mk8_editor.listening;
        mk8_mail.combined = combined;
        memcpy(mk8_mail.names, names, sizeof names);
    }
    mk8_mail.committing = false;
    SDL_UnlockSpinlock(&mk8_edit_lock);
}

static enum Mk8EditState mk8_listen_state(void) {
    if (mk8_editor.listening < 0) return MK8_EDIT_READY;
    if (mk8_editor.ambiguous) return MK8_EDIT_AMBIGUOUS;
    if (mk8_editor.second) return MK8_EDIT_PAIR_HELD;
    Mk8JoinDevice *first = mk8_edit_device(mk8_editor.first);
    if (!first) return MK8_EDIT_LISTEN;
    return first->half == 1 ? MK8_EDIT_NEED_RIGHT : MK8_EDIT_NEED_LEFT;
}

static Uint32 mk8_join_chord(void) {
    return (1u << SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) | (1u << SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
}

static Uint32 mk8_pair_button(int half) {
    if (!half) return 0;
    return 1u << (half == 1 ? SDL_GAMEPAD_BUTTON_LEFT_PADDLE1 : SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1);
}

static void mk8_edit_latch(Mk8JoinDevice *d) {
    d->buttons = 0;
    for (unsigned b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && b < 32; b++)
        if (SDL_GetGamepadButton(d->pad, (SDL_GamepadButton)b)) d->buttons |= 1u << b;
    d->blocked = (d->buttons & mk8_join_chord() ? mk8_join_chord() : 0) |
                 (d->buttons & mk8_pair_button(d->half));
    if (mk8_title_observing) {
        d->triggers = (SDL_GetGamepadAxis(d->pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) |
                     (SDL_GetGamepadAxis(d->pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000 ? 2u : 0u);
        d->pause_chord = (d->buttons & ((1u << SDL_GAMEPAD_BUTTON_BACK) | (1u << SDL_GAMEPAD_BUTTON_START))) != 0;
    }
}

static void mk8_edit_observe(SDL_JoystickID id) {
    if (!id || mk8_edit_device(id) || !real_gamepad(id) || !SDL_IsGamepad(id)) return;
    Mk8JoinDevice *d = SDL_calloc(1, sizeof(*d));
    if (!d) return;
    d->pad = SDL_OpenGamepad(id);
    if (!d->pad) { SDL_free(d); return; }
    d->id = id;
    if (SDL_GetGamepadVendor(d->pad) == 0x057e) {
        Uint16 pid = SDL_GetGamepadProduct(d->pad);
        if (pid == 0x2006 || pid == 0x2007) d->half = pid - 0x2005;
    }
    mk8_edit_latch(d);
    d->next = mk8_editor.devices;
    mk8_editor.devices = d;
}

static void mk8_edit_end(void) {
    Mk8JoinDevice *next;
    for (Mk8JoinDevice *d = mk8_editor.devices; d; d = next) {
        next = d->next;
        SDL_CloseGamepad(d->pad);
        SDL_free(d);
    }
    memset(&mk8_editor, 0, sizeof mk8_editor);
    mk8_editor.listening = -1;
}

static void mk8_edit_finish(void) {
    mk8_edit_end();
    // Replay hotplug events received while the draft owned device discovery.
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    for (int i=0; i<count; ++i) mk8_route_gamepad(ids[i]);
    SDL_free(ids);
}

static void mk8_edit_listen(int row, bool ascending) {
    mk8_editor.listening = row;
    mk8_editor.ascending = ascending;
    mk8_editor.first = mk8_editor.second = 0;
    mk8_editor.ambiguous = mk8_editor.retry = false;
    for (Mk8JoinDevice *d = mk8_editor.devices; d; d = d->next) mk8_edit_latch(d);
}

static void mk8_edit_next(void) {
    int next = -1;
    if (mk8_editor.ascending) {
        for (int i = 0; i < mk8_editor.rows; i++)
            if (!mk8_editor.slots[i][0] && i != mk8_editor.keyboard) { next = i; break; }
    }
    // Retain other devices' distinct edges in the same SDL event batch.
    mk8_editor.listening = next;
    mk8_editor.ascending = next >= 0;
    mk8_editor.first = mk8_editor.second = 0;
    mk8_editor.ambiguous = mk8_editor.retry = false;
}

static void mk8_edit_register(int row, SDL_JoystickID left, SDL_JoystickID right) {
    if (mk8_title_observing) { mk8_title_choose(left, right); return; }
    if (mk8_editor.keyboard == row) mk8_editor.keyboard = -1;
    mk8_editor.slots[row][0] = left;
    mk8_editor.slots[row][1] = right;
    mk8_edit_next();
}

// Non-owner devices can enroll only through a fresh chord, never by changing menu focus.
static void mk8_edit_button(Mk8JoinDevice *d, unsigned button, bool down) {
    if (button >= 32) return;
    Uint32 bit = 1u << button, chord = mk8_join_chord(), pair = mk8_pair_button(d->half);
    Uint32 before = d->buttons;
    if (down) d->buttons |= bit;
    else d->buttons &= ~bit;
    if (!(d->buttons & chord)) d->blocked &= ~chord;
    if (!(d->buttons & pair)) d->blocked &= ~pair;
    int row = mk8_editor.listening;
    if (row < 0 || before == d->buttons) return;
    if (mk8_title_observing && !d->half &&
        (button == SDL_GAMEPAD_BUTTON_BACK || button == SDL_GAMEPAD_BUTTON_START)) {
        // Back + Start belongs to Settings. A lone Start or Back claims only on its release.
        Uint32 shortcut = (1u << SDL_GAMEPAD_BUTTON_BACK) | (1u << SDL_GAMEPAD_BUTTON_START);
        if (!mk8_editor.focused || (d->buttons & shortcut) == shortcut) d->pause_chord = true;
        if (!down && !(d->buttons & shortcut)) {
            if (!d->pause_chord) mk8_title_choose(d->id, 0);
            d->pause_chord = false;
        }
        return;
    }
    if (!mk8_editor.focused) return;
    if (mk8_title_observing && down &&
        button != SDL_GAMEPAD_BUTTON_GUIDE && button != SDL_GAMEPAD_BUTTON_TOUCHPAD) {
        if (!d->half || (bit & chord)) { mk8_title_choose(d->id, 0); return; }
        if (button <= SDL_GAMEPAD_BUTTON_NORTH && !mk8_editor.first) {
            SDL_JoystickID halves[2] = {0};
            for (Mk8JoinDevice *p = mk8_editor.devices; p; p = p->next) {
                if (!p->half) continue;
                if (halves[p->half-1]) return;
                halves[p->half-1] = p->id;
            }
            if (halves[0] && halves[1]) mk8_title_choose(halves[0], halves[1]);
            return;
        }
    }
    if (mk8_editor.ambiguous) {
        if (!down) {
            mk8_editor.retry = true;
            for (Mk8JoinDevice *p = mk8_editor.devices; p; p = p->next)
                if (p->buttons & mk8_pair_button(p->half)) mk8_editor.retry = false;
            return;
        }
    }
    for (int i = 0; i < 4; i++)
        if (i != row && (mk8_editor.slots[i][0] == d->id || mk8_editor.slots[i][1] == d->id)) return;
    if (mk8_editor.ambiguous) {
        if (!mk8_editor.retry) return;
        mk8_editor.ambiguous = mk8_editor.retry = false;
    }
    if (down && (bit & chord) && !(d->blocked & chord) && (d->buttons & chord) == chord && !mk8_editor.first) {
        mk8_edit_register(row, d->id, 0);
    } else if (pair && bit == pair && !(d->blocked & pair)) {
        Mk8JoinDevice *first = mk8_edit_device(mk8_editor.first);
        Mk8JoinDevice *second = mk8_edit_device(mk8_editor.second);
        if (down && !first) {
            mk8_editor.first = d->id;
        } else if (down && first && first->id != d->id && (!second || second->id != d->id)) {
            if (first->half == d->half || second) {
                // Two equally plausible owners: require all grips to release before another attempt.
                mk8_editor.first = mk8_editor.second = 0;
                mk8_editor.ambiguous = true;
            } else mk8_editor.second = d->id;
        } else if (!down && first && !second && first == d) mk8_editor.first = 0;
        else if (!down && second && !(first->buttons & mk8_pair_button(first->half)) &&
                 !(second->buttons & mk8_pair_button(second->half))) {
            mk8_edit_register(row, first->half == 1 ? first->id : second->id,
                                   first->half == 2 ? first->id : second->id);
        }
    } else return;
    mk8_edit_publish(mk8_listen_state());
}

static void mk8_edit_detach(SDL_JoystickID id) {
    Mk8JoinDevice **link = &mk8_editor.devices;
    while (*link && (*link)->id != id) link = &(*link)->next;
    if (!*link) return;
    Mk8JoinDevice *gone = *link;
    *link = gone->next;
    SDL_CloseGamepad(gone->pad);
    SDL_free(gone);
    if (mk8_editor.first == id || mk8_editor.second == id || mk8_editor.ambiguous)
        mk8_edit_listen(mk8_editor.listening, mk8_editor.ascending);
    for (int i = 0; i < 4; i++)
        if (mk8_editor.slots[i][0] == id || mk8_editor.slots[i][1] == id)
            mk8_editor.slots[i][0] = mk8_editor.slots[i][1] = 0;
}

void mk8_edit_event(const SDL_Event *e) {
    if (mk8_pending_grips) {
        if (e->type == SDL_EVENT_GAMEPAD_REMOVED) mk8_retry_pending_grips(e->gdevice.which, true);
        else if (!mk8_editor.rows) {
            if (e->type == SDL_EVENT_GAMEPAD_REMAPPED) mk8_retry_pending_grips(e->gdevice.which, false);
            else if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) mk8_retry_pending_grips(e->gbutton.which, false);
            else if (e->type == SDL_EVENT_GAMEPAD_AXIS_MOTION && abs(e->gaxis.value) > 8000)
                mk8_retry_pending_grips(e->gaxis.which, false);
        }
    }
    if (!mk8_editor.rows) return;
    switch (e->type) {
    case SDL_EVENT_GAMEPAD_ADDED: mk8_edit_observe(e->gdevice.which); break;
    case SDL_EVENT_GAMEPAD_REMOVED: mk8_edit_detach(e->gdevice.which); break;
    case SDL_EVENT_GAMEPAD_REMAPPED: break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        Mk8JoinDevice *d = mk8_edit_device(e->gbutton.which);
        if (d) mk8_edit_button(d, e->gbutton.button, e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
        return;
    }
    case SDL_EVENT_WINDOW_FOCUS_LOST:
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        mk8_editor.focused = e->type == SDL_EVENT_WINDOW_FOCUS_GAINED;
        mk8_edit_listen(mk8_editor.listening, mk8_editor.ascending);
        break;
    default: return;
    }
    mk8_edit_publish(mk8_listen_state());
}

static bool mk8_edit_commit(void) {
    for (int i = 0; i < mk8_editor.rows; i++) {
        if (!mk8_editor.slots[i][0] && i != mk8_editor.keyboard) return false;
    }
    // Other rows may have lost a device during this dialog; they always keep their live ownership.
    for (int i = mk8_editor.rows; i < 4; i++) {
        SDL_Gamepad *primary = *mk8_primary_slot(i);
        mk8_editor.slots[i][0] = primary ? SDL_GetGamepadID(primary) : 0;
        mk8_editor.slots[i][1] = mk8_partner[i] ? SDL_GetGamepadID(mk8_partner[i]) : 0;
    }
    if (!mk8_assign_layout(mk8_editor.slots, mk8_editor.keyboard, true)) return false;
    if (mk8_editor.chose_keyboard && mk8_editor.keyboard >= 0)
        mk8_forget_seat((unsigned)mk8_editor.keyboard);
    return true;
}

void mk8_edit_pump(void) {
    if (mk8_pending_grips && !mk8_reserved_seats && !mk8_editor.rows) mk8_retry_pending_grips(0, false);
    SDL_LockSpinlock(&mk8_edit_lock);
    if (!mk8_mail.pending) { SDL_UnlockSpinlock(&mk8_edit_lock); return; }
    enum Mk8EditCommand action = mk8_mail.command;
    int row = mk8_mail.row, other = mk8_mail.other;
    mk8_mail.pending = false;
    mk8_mail.committing = action == MK8_EDIT_APPLY;
    SDL_UnlockSpinlock(&mk8_edit_lock);

    enum Mk8EditState state = MK8_EDIT_READY;
    if (action == MK8_EDIT_BEGIN) {
        mk8_edit_end();
        if (row < 2 || row > 4 || input_user_blocked(NULL)) {
            mk8_edit_publish(MK8_EDIT_FAILED);
            return;
        }
        mk8_editor.rows = row;
        mk8_editor.keyboard = mk8_assigned_keyboard();
        mk8_editor.focused = true;
        int count = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&count);
        for (int i = 0; i < count; i++) mk8_edit_observe(ids[i]);
        SDL_free(ids);
        for (int i = 0; i < 4; i++) {
            SDL_Gamepad *primary = *mk8_primary_slot(i);
            mk8_editor.slots[i][0] = primary ? SDL_GetGamepadID(primary) : 0;
            mk8_editor.slots[i][1] = mk8_partner[i] ? SDL_GetGamepadID(mk8_partner[i]) : 0;
        }
    } else if (action == MK8_EDIT_CANCEL) {
        mk8_edit_finish();
        state = MK8_EDIT_CLOSED;
    } else if (!mk8_editor.rows) state = MK8_EDIT_FAILED;
    else if (action == MK8_EDIT_APPLY) {
        if (mk8_editor.first || mk8_editor.ambiguous || !mk8_edit_commit()) state = MK8_EDIT_FAILED;
        else { mk8_edit_finish(); state = MK8_EDIT_APPLIED; }
    } else if (action == MK8_EDIT_CLEAR_ALL) {
        memset(mk8_editor.slots, 0, sizeof(mk8_editor.slots[0]) * mk8_editor.rows);
        if (mk8_editor.keyboard < mk8_editor.rows) mk8_editor.keyboard = -1;
        mk8_edit_listen(0, true);
        state = MK8_EDIT_LISTEN;
    } else if (action == MK8_EDIT_STOP_LISTEN) mk8_edit_listen(-1, false);
    else if (row < 0 || row >= mk8_editor.rows) state = MK8_EDIT_FAILED;
    else if (action == MK8_EDIT_ASSIGN) {
        mk8_edit_listen(row, false);
        state = MK8_EDIT_LISTEN;
    } else if (action == MK8_EDIT_KEYBOARD && mk8_editor.keyboard >= mk8_editor.rows) state = MK8_EDIT_FAILED;
    else if (action == MK8_EDIT_KEYBOARD || action == MK8_EDIT_CLEAR) {
        mk8_editor.slots[row][0] = mk8_editor.slots[row][1] = 0;
        if (action == MK8_EDIT_KEYBOARD) {
            mk8_editor.keyboard = row;
            mk8_editor.chose_keyboard = true;
            mk8_edit_next();
            state = mk8_listen_state();
        } else {
            if (mk8_editor.keyboard == row) mk8_editor.keyboard = -1;
            mk8_edit_listen(-1, false);
        }
    } else if (action == MK8_EDIT_SWAP && other >= 0 && other < mk8_editor.rows) {
        // The default P1 keyboard overlay is hidden behind its pad. Only swap a visible keyboard row.
        if (mk8_editor.keyboard >= 0 && !mk8_editor.slots[mk8_editor.keyboard][0]) {
            if (mk8_editor.keyboard == row) mk8_editor.keyboard = other;
            else if (mk8_editor.keyboard == other) mk8_editor.keyboard = row;
        }
        for (unsigned side = 0; side < 2; side++) {
            SDL_JoystickID id = mk8_editor.slots[row][side];
            mk8_editor.slots[row][side] = mk8_editor.slots[other][side];
            mk8_editor.slots[other][side] = id;
        }
        mk8_edit_listen(-1, false);
    } else state = MK8_EDIT_FAILED;
    mk8_edit_publish(state);
}
