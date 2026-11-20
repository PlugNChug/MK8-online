// The guest worker waits while the SDL thread runs the same assignment editor used at character selection.
enum { MK8_APPLET_APPLIED = -1, MK8_APPLET_CANCELLED = -2, MK8_APPLET_ABANDONED = -3 };
static _Atomic int mk8_applet_request;
static _Atomic Uint64 mk8_applet_heartbeat;
static _Atomic Uint64 mk8_applet_suppressed;
static bool mk8_applet_editing;
static bool mk8_applet_resync;
static int mk8_applet_row;
static Uint64 mk8_applet_notice_at, mk8_applet_held;

void menu_notice(const char *, double);
void kernel_host_wait_begin(void);
void kernel_host_wait_end(void);

int mk8_controller_applet_wait(int min, int max, int live) {
    if (min != max || min < 2 || min > 4 || mk8_settings_shared_race() || input_user_blocked(NULL)) return live;
    unsigned seats = (1u << min) - 1;
    if ((mk8_live_seats() & seats) == seats) return min;
    int idle = 0;
    if (!atomic_compare_exchange_strong(&mk8_applet_request, &idle, min)) return 0;
    Uint64 start = SDL_GetTicks();
    kernel_host_wait_begin();
    int state;
    while ((state = atomic_load(&mk8_applet_request)) == min) {
        Uint64 heartbeat = atomic_load(&mk8_applet_heartbeat);
        if (g_rt_unwind || SDL_GetTicks() - SDL_max(start, heartbeat) > 5000) {
            // A stopped UI cannot show or dismiss the applet.
            if (atomic_compare_exchange_strong(&mk8_applet_request, &state, MK8_APPLET_ABANDONED)) break;
        }
        SDL_Delay(16);
    }
    kernel_host_wait_end();
    bool applied = state == MK8_APPLET_APPLIED;
    if (applied || state == MK8_APPLET_CANCELLED)
        atomic_compare_exchange_strong(&mk8_applet_request, &state, 0);
    return applied && (mk8_live_seats() & seats) == seats ? min : 0;
}

static bool mk8_applet_key(const SDL_Event *e) {
    if (atomic_load(&mk8_applet_request) < 2) return false;
    if (e->type == SDL_EVENT_WINDOW_FOCUS_GAINED || e->type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        mk8_applet_resync = true;
        return false;
    }
    if ((e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || e->type == SDL_EVENT_GAMEPAD_BUTTON_UP) &&
        (e->gbutton.button == SDL_GAMEPAD_BUTTON_BACK || e->gbutton.button == SDL_GAMEPAD_BUTTON_START ||
         e->gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE)) {
        mk8_edit_event(e);
        return true;
    }
    if (e->type != SDL_EVENT_KEY_DOWN && e->type != SDL_EVENT_KEY_UP) return false;
    SDL_Scancode key = e->key.scancode;
    if (key != SDL_SCANCODE_ESCAPE && key != SDL_SCANCODE_RETURN && key != SDL_SCANCODE_F1) return false;
    if (key == SDL_SCANCODE_RETURN && (!mk8_applet_editing || mk8_editor.keyboard >= 0 || mk8_editor.listening < 0)) return false;
    if (mk8_applet_editing && mk8_title_focused && mk8_editor.focused && !mk8_applet_resync &&
        e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat) {
        if (key == SDL_SCANCODE_ESCAPE) mk8_edit_command(MK8_EDIT_CANCEL, 0, 0);
        if (key == SDL_SCANCODE_RETURN) mk8_edit_command(MK8_EDIT_KEYBOARD, mk8_editor.listening, 0);
    }
    return true;
}

static Uint64 mk8_applet_p1_buttons(void) {
    Uint64 buttons = mk8_editor.keyboard == 0 ? key_buttons : 0;
    Mk8JoinDevice *left = mk8_edit_device(mk8_editor.slots[0][0]);
    if (!left) return buttons;
    Mk8JoinDevice *right = mk8_edit_device(mk8_editor.slots[0][1]);
    Uint32 host = right ? mk8_pair_buttons(left->pad,right->pad) : left->buttons;
    if (!right) {
        if (SDL_GetGamepadAxis(left->pad,SDL_GAMEPAD_AXIS_LEFT_TRIGGER)>16000) host |= 1u<<PADB_LTRIGGER;
        if (SDL_GetGamepadAxis(left->pad,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)>16000) host |= 1u<<PADB_RTRIGGER;
    }
    for (int i=BIND_A; i<=BIND_B; ++i)
        if (g_bind_pad[i]>=0 && g_bind_pad[i]<PADB_HOST_COUNT && (host & (1u<<g_bind_pad[i]))) buttons |= 1ull<<i;
    return buttons;
}

void mk8_edit_pump(void) {
    int players = atomic_load(&mk8_applet_request);
    if (players == MK8_APPLET_ABANDONED && mk8_applet_editing) mk8_edit_command(MK8_EDIT_CANCEL,0,0);
    mk8_edit_process();
    if (!players) {
        if (atomic_load(&mk8_applet_suppressed))
            atomic_fetch_and(&mk8_applet_suppressed,mk8_controller_p1_navigation());
        return;
    }
    if (players == MK8_APPLET_ABANDONED) {
        mk8_applet_editing = false;
        menu_notice("",0);
        atomic_store(&mk8_applet_request,0);
        return;
    }
    if (players < 0) return;
    atomic_store(&mk8_applet_heartbeat, SDL_GetTicks());
    if (!mk8_applet_editing) {
        if (mk8_edit_command(MK8_EDIT_BEGIN,players,0)) {
            mk8_applet_editing = true;
            mk8_applet_row = -2;
            mk8_applet_resync = true;
            return;
        }
        atomic_compare_exchange_strong(&mk8_applet_request,&players,MK8_APPLET_CANCELLED);
        return;
    }
    enum Mk8EditState state = mk8_edit_view(NULL,NULL);
    if (state == MK8_EDIT_APPLIED || state == MK8_EDIT_CLOSED || (!mk8_editor.rows && state == MK8_EDIT_FAILED)) {
        atomic_fetch_or(&mk8_applet_suppressed,mk8_controller_p1_navigation() | mk8_applet_held);
        mk8_controller_hold_extra_inputs();
        mk8_applet_editing = false;
        menu_notice("",0);
        atomic_compare_exchange_strong(&mk8_applet_request,&players,
            state == MK8_EDIT_APPLIED ? MK8_APPLET_APPLIED : MK8_APPLET_CANCELLED);
        return;
    }
    if (state == MK8_EDIT_PENDING) return;
    int row = 0;
    while (row < players && (mk8_editor.slots[row][0] || row == mk8_editor.keyboard)) ++row;
    Uint64 held = mk8_applet_p1_buttons();
    Uint64 edge = mk8_title_focused && mk8_editor.focused && !mk8_applet_resync ? held & ~mk8_applet_held : 0;
    mk8_applet_held = held;
    mk8_applet_resync = false;
    if (edge & (1ull << BIND_B)) { mk8_edit_command(MK8_EDIT_CANCEL,0,0); return; }
    if (row == players && (edge & (1ull << BIND_A))) { mk8_edit_command(MK8_EDIT_APPLY,0,0); return; }
    if (row < players && mk8_editor.listening != row) mk8_edit_command(MK8_EDIT_ASSIGN,row,0);
    Uint64 now = SDL_GetTicks();
    if (row != mk8_applet_row || now >= mk8_applet_notice_at) {
        char prompt[168];
        if (row < players) SDL_snprintf(prompt,sizeof prompt,"Player %d of %d: press L + R or SL + SR to join.\n%sEsc or P1 Back: cancel.",
            row+1, players, mk8_editor.keyboard < 0 ? "Enter: use keyboard.   " : "");
        else SDL_snprintf(prompt,sizeof prompt,"%d players ready. P1 Confirm: continue. Esc or P1 Back: cancel.",players);
        menu_notice(prompt,2);
        mk8_applet_row = row;
        mk8_applet_notice_at = now+1000;
    }
}
