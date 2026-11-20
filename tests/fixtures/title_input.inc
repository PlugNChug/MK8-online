static void title_ready(bool ready) { mk8_title_input_active(ready); pump(); }

static void title_key(SDL_Scancode code, bool repeat) {
    SDL_Event key={.type=SDL_EVENT_KEY_DOWN};
    key.key.scancode=code; key.key.repeat=repeat;
    assert(SDL_PushEvent(&key)); pump();
    key.type=SDL_EVENT_KEY_UP; key.key.repeat=false;
    assert(SDL_PushEvent(&key)); pump();
}

static void title_accepted(void) {
    assert(guest_buttons(0)==(1ull<<BIND_A));
    title_ready(false);
    assert(!(guest_buttons(0)&(1ull<<BIND_A)));
}

static void detach_title_pad(enum Device device) {
    SDL_CloseJoystick(sticks[device]); assert(SDL_DetachVirtualJoystick(ids[device])); pump();
}

static int title_pad_seat(SDL_JoystickID id) {
    for(int i=0;i<4;i++) {
        SDL_Gamepad *primary=*mk8_primary_slot(i);
        if((primary && SDL_GetGamepadID(primary)==id) || (mk8_partner[i] && SDL_GetGamepadID(mk8_partner[i])==id))
            return i;
    }
    return -1;
}

static void check_title_identical(void) {
    attach(PRO); attach_as(SOLO_L,PRO); pump();
    title_ready(true); button(sticks[PRO],SDL_GAMEPAD_BUTTON_NORTH,true);
    button(sticks[PRO],SDL_GAMEPAD_BUTTON_NORTH,false); title_accepted();
    int companion=title_pad_seat(ids[SOLO_L]); assert(companion>0);
    detach_title_pad(PRO); attach(PRO); pump();
    assert(!pad && title_pad_seat(ids[SOLO_L])==companion);
    // A fresh press still distinguishes physically identical devices.
    title_ready(true); button(sticks[PRO],SDL_GAMEPAD_BUTTON_EAST,true);
    button(sticks[PRO],SDL_GAMEPAD_BUTTON_EAST,false); title_accepted();
    assert(SDL_GetGamepadID(pad)==ids[PRO] && title_pad_seat(ids[SOLO_L])==companion);
    detach_title_pad(PRO); detach_title_pad(SOLO_L);
}

static void check_title_manual_layout(void) {
    attach(PRO); attach(PAIR_L); pump();
    assert(edit(MK8_EDIT_BEGIN,2,0)==MK8_EDIT_READY);
    assert(edit(MK8_EDIT_SWAP,0,1)==MK8_EDIT_READY);
    assert(edit(MK8_EDIT_APPLY,0,0)==MK8_EDIT_APPLIED);
    assert(title_pad_seat(ids[PRO])==1);
    title_ready(true); button(sticks[PRO],SDL_GAMEPAD_BUTTON_EAST,true);
    button(sticks[PRO],SDL_GAMEPAD_BUTTON_EAST,false); title_accepted();
    assert(title_pad_seat(ids[PRO])==0 && title_pad_seat(ids[PAIR_L])==1);
    detach_title_pad(PRO); attach(SOLO_R); pump();
    assert(!pad && title_pad_seat(ids[SOLO_R])==-1);
    attach(PRO); pump();
    assert(title_pad_seat(ids[PRO])==0 && title_pad_seat(ids[PAIR_L])==1 && title_pad_seat(ids[SOLO_R])==-1);
    attach(SOLO_L); pump(); assert(title_pad_seat(ids[SOLO_L])==-1);
    assert(edit(MK8_EDIT_BEGIN,2,0)==MK8_EDIT_READY);
    detach_title_pad(PRO); attach(PRO); pump();
    assert(!pad && title_pad_seat(ids[PAIR_L])==1);
    assert(edit(MK8_EDIT_CANCEL,0,0)==MK8_EDIT_CLOSED);
    assert(title_pad_seat(ids[PRO])==0 && title_pad_seat(ids[SOLO_L])==-1 && title_pad_seat(ids[SOLO_R])==-1);
    detach_title_pad(PRO); detach_title_pad(PAIR_L); detach_title_pad(SOLO_L); detach_title_pad(SOLO_R);
}

static void check_title_reconnect(void) {
    attach(PRO); attach(PAIR_L); pump();
    title_ready(true); button(sticks[PRO],SDL_GAMEPAD_BUTTON_NORTH,true);
    button(sticks[PRO],SDL_GAMEPAD_BUTTON_NORTH,false); title_accepted();
    assert(SDL_GetGamepadID(extra_pad[0])==ids[PAIR_L]);
    SDL_JoystickID old=ids[PRO]; detach_title_pad(PRO);
    assert(!pad && SDL_GetGamepadID(extra_pad[0])==ids[PAIR_L]);
    attach(SOLO_R); pump();
    assert(!pad && mk8_assigned_keyboard()==0 && title_pad_seat(ids[SOLO_R])>0);
    attach(PRO); pump();
    assert(ids[PRO]!=old && SDL_GetGamepadID(pad)==ids[PRO]);
    assert(SDL_GetGamepadID(extra_pad[0])==ids[PAIR_L] && !mk8_partner[0]);
    button(sticks[PRO],SDL_GAMEPAD_BUTTON_EAST,true);
    assert((guest_buttons(0)&(1ull<<BIND_A)) && !guest_buttons(1));
    button(sticks[PRO],SDL_GAMEPAD_BUTTON_EAST,false);
    title_ready(true); title_key(SDL_SCANCODE_K,false); title_accepted();
    detach_title_pad(PRO); attach(PRO); pump();
    assert(!pad && mk8_assigned_keyboard()==0 && SDL_GetGamepadID(extra_pad[0])==ids[PAIR_L]);
    detach_title_pad(SOLO_R); attach(PAIR_R); pump();
    title_ready(true); button(sticks[PAIR_R],SDL_GAMEPAD_BUTTON_SOUTH,true);
    button(sticks[PAIR_R],SDL_GAMEPAD_BUTTON_SOUTH,false); title_accepted();
    assert(SDL_GetGamepadID(pad)==ids[PAIR_L] && SDL_GetGamepadID(mk8_partner[0])==ids[PAIR_R]);
    int pro_seat=title_pad_seat(ids[PRO]); assert(pro_seat>0);
    detach_title_pad(PAIR_L);
    assert(!pad && title_pad_seat(ids[PAIR_R])==-1 && !guest_buttons(0));
    attach(SOLO_R); pump(); assert(!pad && title_pad_seat(ids[PRO])==pro_seat);
    detach_title_pad(PAIR_R); attach(PAIR_L); pump();
    assert(!pad && title_pad_seat(ids[PAIR_L])==-1);
    attach(PAIR_R); pump();
    assert(SDL_GetGamepadID(pad)==ids[PAIR_L] && SDL_GetGamepadID(mk8_partner[0])==ids[PAIR_R]);
    assert(title_pad_seat(ids[PRO])==pro_seat);
    check_motion(0,PAIR_R,true); check_pulse(0,0,PAIR_L); check_pulse(0,1,PAIR_R);
    detach_title_pad(PAIR_L); detach_title_pad(PAIR_R); detach_title_pad(SOLO_R); detach_title_pad(PRO);
}

static void check_right_reconnect(bool early_add) {
    fixture_serials = returning_serial_ready = true;
    spare_serial_ready = early_add;
    attach(PRO); attach(PAIR_L); attach(PAIR_R); pump();
    title_ready(true);
    button(sticks[PAIR_R], SDL_GAMEPAD_BUTTON_SOUTH, true);
    button(sticks[PAIR_R], SDL_GAMEPAD_BUTTON_SOUTH, false);
    title_accepted();
    assert(SDL_GetGamepadID(pad) == ids[PAIR_L] && SDL_GetGamepadID(mk8_partner[0]) == ids[PAIR_R]);
    int pro_seat = title_pad_seat(ids[PRO]);
    assert(pro_seat > 0);

    SDL_JoystickID old_right = ids[PAIR_R];
    SDL_CloseJoystick(sticks[PAIR_R]);
    assert(SDL_DetachVirtualJoystick(old_right));
    if (!early_add) { pump(); returning_serial_ready = false; }
    attach(PAIR_R);
    assert(ids[PAIR_R] != old_right);
    if (early_add) {
        // SDL already disconnected the old device. Deliver the genuine new ADD before the queued REMOVE.
        SDL_Event added;
        SDL_PumpEvents();
        assert(SDL_PeepEvents(&added, 1, SDL_GETEVENT, SDL_EVENT_GAMEPAD_ADDED, SDL_EVENT_GAMEPAD_ADDED) == 1);
        assert(added.gdevice.which == ids[PAIR_R]);
        input_handle_event(&added);
    }
    pump();
    if (!early_add) assert(!pad && !mk8_partner[0] && title_pad_seat(ids[PAIR_R]) == -1);
    // The spare has the same model, name and GUID as the returning grip, but a different serial.
    attach_as(SOLO_R, PAIR_R); pump();
    if (!early_add) {
        assert(title_pad_seat(ids[SOLO_R]) == -1 && SDL_GetGamepadFromID(ids[SOLO_R]));
        SDL_JoystickID removed = ids[SOLO_R];
        detach_title_pad(SOLO_R);
        assert(!SDL_GetGamepadFromID(removed));
        attach_as(SOLO_R, PAIR_R); pump();
        spare_serial_ready = true;
        button(sticks[SOLO_R], SDL_GAMEPAD_BUTTON_SOUTH, true);
        button(sticks[SOLO_R], SDL_GAMEPAD_BUTTON_SOUTH, false);
    }
    int spare_seat = title_pad_seat(ids[SOLO_R]);
    assert(spare_seat > 0 && spare_seat != pro_seat);
    button(sticks[PRO], SDL_GAMEPAD_BUTTON_EAST, true);
    assert(guest_buttons(pro_seat) & (1ull << BIND_A));
    if (!early_add) {
        button(sticks[SOLO_R], SDL_GAMEPAD_BUTTON_SOUTH, true);
        assert(!guest_buttons(0) && guest_buttons(spare_seat));
        button(sticks[SOLO_R], SDL_GAMEPAD_BUTTON_SOUTH, false);
        returning_serial_ready = true;
        // The metadata becomes available without another ADD.
        button(sticks[PAIR_R], SDL_GAMEPAD_BUTTON_SOUTH, true);
        button(sticks[PAIR_R], SDL_GAMEPAD_BUTTON_SOUTH, false);
    }
    assert(SDL_GetGamepadID(pad) == ids[PAIR_L] && SDL_GetGamepadID(mk8_partner[0]) == ids[PAIR_R]);
    assert(title_pad_seat(ids[SOLO_R]) == spare_seat && title_pad_seat(ids[PRO]) == pro_seat);
    assert(guest_buttons(pro_seat) & (1ull << BIND_A));
    button(sticks[PRO], SDL_GAMEPAD_BUTTON_EAST, false);
    button(sticks[PAIR_L], SDL_GAMEPAD_BUTTON_SOUTH, true);
    button(sticks[PAIR_R], SDL_GAMEPAD_BUTTON_SOUTH, true);
    assert(guest_buttons(0) == ((1ull << BIND_LEFT) | (1ull << BIND_A)));
    assert(!guest_buttons(pro_seat) && !guest_buttons(spare_seat));
    button(sticks[PAIR_L], SDL_GAMEPAD_BUTTON_SOUTH, false);
    button(sticks[PAIR_R], SDL_GAMEPAD_BUTTON_SOUTH, false);
    check_motion(0, PAIR_R, true);
    check_pulse(0, 0, PAIR_L); check_pulse(0, 1, PAIR_R);
    detach_title_pad(PAIR_L); detach_title_pad(PAIR_R); detach_title_pad(SOLO_R); detach_title_pad(PRO);
}

static void check_title_hotplug(void) {
    title_ready(true); title_key(SDL_SCANCODE_K,false);
    assert(!pad && mk8_assigned_keyboard()==0); title_accepted();
    attach(PRO); pump();
    assert(!pad && mk8_assigned_keyboard()==0 && SDL_GetGamepadID(extra_pad[0])==ids[PRO]);
    button(sticks[PRO],SDL_GAMEPAD_BUTTON_EAST,true);
    assert(!guest_buttons(0) && (guest_buttons(1)&(1ull<<BIND_A)));
    button(sticks[PRO],SDL_GAMEPAD_BUTTON_EAST,false);
    title_ready(true);
    button(sticks[PRO],SDL_GAMEPAD_BUTTON_WEST,true); button(sticks[PRO],SDL_GAMEPAD_BUTTON_WEST,false);
    assert(SDL_GetGamepadID(pad)==ids[PRO] && mk8_assigned_keyboard()==1); title_accepted();
    assert(mk8_live_seats()==3);  // The keyboard and Pro occupy distinct seats.
    SDL_Event enter={.type=SDL_EVENT_KEY_DOWN}; enter.key.scancode=SDL_SCANCODE_RETURN;
    assert(SDL_PushEvent(&enter)); pump();
    assert(!(guest_buttons(0)&(1ull<<BIND_PLUS)) && (guest_buttons(1)&(1ull<<BIND_PLUS)));
    enter.type=SDL_EVENT_KEY_UP; assert(SDL_PushEvent(&enter)); pump();
    assert(!(guest_buttons(1)&(1ull<<BIND_PLUS)));
    assert(edit(MK8_EDIT_BEGIN,2,0)==MK8_EDIT_READY);
    assert(edit(MK8_EDIT_APPLY,0,0)==MK8_EDIT_APPLIED);
    attach(PAIR_L); pump();
    assert(SDL_GetGamepadID(pad)==ids[PRO]);
    bool found=false;
    for(unsigned i=0;i<EXTRA_PADS;i++) if(extra_pad[i] && SDL_GetGamepadID(extra_pad[i])==ids[PAIR_L]) found=true;
    assert(found);
    SDL_CloseJoystick(sticks[PAIR_L]); assert(SDL_DetachVirtualJoystick(ids[PAIR_L]));
    SDL_CloseJoystick(sticks[PRO]); assert(SDL_DetachVirtualJoystick(ids[PRO])); pump();
}

static void check_title_input(void) {
    attach(PRO); pump();
    SDL_VirtualJoystickDesc d; SDL_INIT_INTERFACE(&d);
    d.type=SDL_JOYSTICK_TYPE_GAMEPAD; d.name="Second USB gamepad";
    d.naxes=SDL_GAMEPAD_AXIS_COUNT; d.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
    SDL_JoystickID usb_id=SDL_AttachVirtualJoystick(&d);
    SDL_Joystick *usb=SDL_OpenJoystick(usb_id); assert(usb); pump();
    assert(SDL_GetGamepadID(pad)==ids[PRO] && SDL_GetGamepadID(extra_pad[0])==usb_id);

    // An already-held button, ordinary menu input, and modal input must not take ownership.
    button(usb,SDL_GAMEPAD_BUTTON_NORTH,true);
    title_ready(true);
    assert(SDL_GetGamepadID(pad)==ids[PRO]);
    button(usb,SDL_GAMEPAD_BUTTON_NORTH,false);
    title_key(SDL_SCANCODE_F1,false);
    button(usb,SDL_GAMEPAD_BUTTON_GUIDE,true); button(usb,SDL_GAMEPAD_BUTTON_GUIDE,false);
    button(usb,SDL_GAMEPAD_BUTTON_BACK,true); button(usb,SDL_GAMEPAD_BUTTON_START,true);
    button(usb,SDL_GAMEPAD_BUTTON_BACK,false); button(usb,SDL_GAMEPAD_BUTTON_START,false);
    assert(SDL_GetGamepadID(pad)==ids[PRO] && !guest_buttons(0));
    host_captures=true; button(usb,SDL_GAMEPAD_BUTTON_EAST,true);
    button(usb,SDL_GAMEPAD_BUTTON_EAST,false); host_captures=false;
    assert(SDL_GetGamepadID(pad)==ids[PRO]);
    button(usb,SDL_GAMEPAD_BUTTON_NORTH,true);
    assert(SDL_GetGamepadID(pad)==usb_id && SDL_GetGamepadID(extra_pad[0])==ids[PRO]);
    title_key(SDL_SCANCODE_K,false); // A second event in the same visit cannot steal P1.
    assert(SDL_GetGamepadID(pad)==usb_id);
    title_accepted();
    assert(!guest_buttons(0)); // The held claim button cannot activate the following menu.
    button(usb,SDL_GAMEPAD_BUTTON_NORTH,false); assert(!guest_buttons(0));
    button(usb,SDL_GAMEPAD_BUTTON_NORTH,true); assert(guest_buttons(0)&(1ull<<BIND_X));
    button(usb,SDL_GAMEPAD_BUTTON_NORTH,false);
    title_ready(true);
    button(sticks[PRO],SDL_GAMEPAD_BUTTON_START,true);
    assert(SDL_GetGamepadID(pad)==usb_id && !guest_buttons(0));
    button(sticks[PRO],SDL_GAMEPAD_BUTTON_START,false);
    assert(SDL_GetGamepadID(pad)==ids[PRO]); title_accepted();
    for(unsigned i=0;i<2;i++) {
        SDL_Joystick *next=i ? sticks[PRO] : usb;
        SDL_GamepadAxis trigger=i ? SDL_GAMEPAD_AXIS_RIGHT_TRIGGER : SDL_GAMEPAD_AXIS_LEFT_TRIGGER;
        assert(SDL_SetJoystickVirtualAxis(next,trigger,32767)); pump();
        title_ready(true);
        assert(SDL_SetJoystickVirtualAxis(next,SDL_GAMEPAD_AXIS_LEFTX,28000)); pump();
        assert(!guest_buttons(0)); // Already-held trigger and moving the stick cannot claim.
        assert(SDL_SetJoystickVirtualAxis(next,SDL_GAMEPAD_AXIS_LEFTX,0));
        assert(SDL_SetJoystickVirtualAxis(next,trigger,-32768)); pump();
        assert(SDL_SetJoystickVirtualAxis(next,trigger,32767)); pump();
        assert(SDL_GetGamepadID(pad)==SDL_GetJoystickID(next));
        assert(SDL_SetJoystickVirtualAxis(next,trigger,-32768)); pump(); title_accepted();
    }
    title_ready(true);
    button(usb,SDL_GAMEPAD_BUTTON_WEST,true); button(usb,SDL_GAMEPAD_BUTTON_WEST,false); title_accepted();
    button(sticks[PRO],SDL_GAMEPAD_BUTTON_EAST,true); button(sticks[PRO],SDL_GAMEPAD_BUTTON_EAST,false);
    assert(SDL_GetGamepadID(pad)==usb_id);

    for(int source=0;source<3;source++) {
        title_ready(true);
        title_key(SDL_SCANCODE_K,true);
        assert(pad);
        if(!source) title_key(SDL_SCANCODE_K,false);
        else {
            SDL_Event e={0};
            if(source==1) { e.type=SDL_EVENT_MOUSE_BUTTON_DOWN; e.button.button=SDL_BUTTON_LEFT; }
            else { e.type=SDL_EVENT_FINGER_DOWN; e.tfinger.fingerID=4; }
            assert(SDL_PushEvent(&e)); pump();
            e.type=source==1 ? SDL_EVENT_MOUSE_BUTTON_UP : SDL_EVENT_FINGER_UP;
            assert(SDL_PushEvent(&e)); pump();
        }
        assert(!pad && mk8_assigned_keyboard()==0); title_accepted();
        title_ready(true); button(usb,SDL_GAMEPAD_BUTTON_WEST,true); button(usb,SDL_GAMEPAD_BUTTON_WEST,false);
        assert(SDL_GetGamepadID(pad)==usb_id && mk8_assigned_keyboard()!=0); title_accepted();
    }

    attach(PAIR_L); attach(PAIR_R); pump();
    title_ready(true);
    button(sticks[PAIR_R],SDL_GAMEPAD_BUTTON_SOUTH,true); button(sticks[PAIR_R],SDL_GAMEPAD_BUTTON_SOUTH,false);
    assert(SDL_GetGamepadID(pad)==ids[PAIR_L] && SDL_GetGamepadID(mk8_partner[0])==ids[PAIR_R]);
    title_accepted();
    check_motion(0,PAIR_R,true); check_pulse(0,0,PAIR_L); check_pulse(0,1,PAIR_R);
    int usb_seat=title_pad_seat(usb_id); assert(usb_seat>0);
    SDL_CloseJoystick(usb); assert(SDL_DetachVirtualJoystick(usb_id)); pump();
    title_ready(true);
    button(sticks[PAIR_R],SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,true); button(sticks[PAIR_R],SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,false);
    assert(SDL_GetGamepadID(pad)==ids[PAIR_R] && !mk8_partner[0]); title_accepted();
    // The offline USB player keeps its seat; splitting P1's pair cannot evict it.
    assert(title_pad_seat(ids[PAIR_L])==-1 && !*mk8_primary_slot(usb_seat));
    title_ready(true);
    button(sticks[PAIR_L],SDL_GAMEPAD_BUTTON_WEST,true); button(sticks[PAIR_L],SDL_GAMEPAD_BUTTON_WEST,false);
    assert(SDL_GetGamepadID(pad)==ids[PAIR_L] && SDL_GetGamepadID(mk8_partner[0])==ids[PAIR_R]); title_accepted();
    attach(SOLO_L); attach(SOLO_R); pump();
    title_ready(true);
    button(sticks[SOLO_R],SDL_GAMEPAD_BUTTON_EAST,true); button(sticks[SOLO_R],SDL_GAMEPAD_BUTTON_EAST,false);
    assert(SDL_GetGamepadID(pad)==ids[PAIR_L] && !guest_buttons(0));
    button(sticks[SOLO_R],SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,true); button(sticks[SOLO_R],SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,false);
    assert(SDL_GetGamepadID(pad)==ids[SOLO_R] && !mk8_partner[0]); title_accepted();
    check_motion(0,SOLO_R,false); check_pulse(0,0,SOLO_R); check_pulse(0,1,SOLO_R);
    title_ready(true);
    button(sticks[SOLO_L],SDL_GAMEPAD_BUTTON_LEFT_PADDLE1,true);
    // The guest may leave and reopen the title between two SDL pumps.
    mk8_title_input_active(false); mk8_title_input_active(true); pump();
    button(sticks[PAIR_R],SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1,true);
    button(sticks[SOLO_L],SDL_GAMEPAD_BUTTON_LEFT_PADDLE1,false);
    button(sticks[PAIR_R],SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1,false);
    assert(SDL_GetGamepadID(pad)==ids[SOLO_R] && !guest_buttons(0));
    button(sticks[SOLO_L],SDL_GAMEPAD_BUTTON_LEFT_PADDLE1,true);
    button(sticks[PAIR_R],SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1,true);
    button(sticks[SOLO_L],SDL_GAMEPAD_BUTTON_LEFT_PADDLE1,false);
    assert(SDL_GetGamepadID(pad)==ids[SOLO_R]);
    button(sticks[PAIR_R],SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1,false);
    assert(SDL_GetGamepadID(pad)==ids[SOLO_L] && SDL_GetGamepadID(mk8_partner[0])==ids[PAIR_R]);
    title_accepted(); check_motion(0,PAIR_R,true); check_pulse(0,0,SOLO_L); check_pulse(0,1,PAIR_R);
    for(int i=0;i<DEVICE_COUNT;i++) { SDL_CloseJoystick(sticks[i]); assert(SDL_DetachVirtualJoystick(ids[i])); }
    pump();
}

static void check_extra_button(enum Device device,int player) {
    button(sticks[device],SDL_GAMEPAD_BUTTON_WEST,true);
    for(int i=0;i<4;i++) {
        uint64_t buttons=guest_buttons(i);
        if ((buttons!=0)!=(i==player))
            fprintf(stderr,"device %d (SDL %u) expected seat %d, seat %d received %llu\n",device,ids[device],player,i,(unsigned long long)buttons);
        assert((buttons!=0)==(i==player));
    }
    button(sticks[device],SDL_GAMEPAD_BUTTON_WEST,false);
}

static bool guest_connected(unsigned player) {
    Cpu cpu={0};
    uint64_t state[5];
    fill_state(&cpu,(uint8_t *)state,player,false);
    cpu.x[0]=(uintptr_t)&player;
    hid_style_set(&cpu);
    assert(cpu.x[0]==(uint32_t)state[4]);
    return cpu.x[0]==1;
}

static void check_race_disconnect(void) {
    title_ready(true); title_key(SDL_SCANCODE_K,false); title_accepted();
    attach(PRO); attach(SOLO_L); attach(SOLO_R); pump();
    assert(guest_connected(1) && guest_connected(2) && guest_connected(3));
    shared_race=true;
    mk8_input_race_players(4);
    assert(SDL_SetJoystickVirtualAxis(sticks[PRO], SDL_GAMEPAD_AXIS_LEFTX,23000));
    button(sticks[PRO], SDL_GAMEPAD_BUTTON_WEST, true);
    assert(guest_buttons(1));
    detach_title_pad(PRO);
    assert(guest_connected(1) && "disconnecting a racing player's pad removed the guest seat");
    Cpu cpu={0};
    uint64_t state[5];
    fill_state(&cpu,(uint8_t *)state,1,false);
    assert(!state[1] && !state[2] && !state[3] && input_players()==4);
    assert(!extra_pad[0]);
    check_extra_button(SOLO_L,2);
    attach(PRO); pump(); check_extra_button(PRO,1);
    detach_title_pad(PRO);
    shared_race=false;
    unsigned before_exit=style_notifications;
    mk8_input_race_players(0);
    assert(style_notifications==before_exit+1 && "menu missed the disconnected seat's style update");
    assert(!guest_connected(1) && guest_connected(2) && guest_connected(3) && input_players()==3);
    mk8_input_race_players(2);
    detach_title_pad(SOLO_R);
    assert(!guest_connected(1) && !guest_connected(3) && input_players()==2);
    mk8_input_race_players(0);
    detach_title_pad(SOLO_L);
}

static void pump_pro_hotplug(void) {
    // A physical hotplug has no fixture handle keeping the driver open between identity checks and assignment.
    SDL_CloseJoystick(sticks[PRO]);
    counted_id = ids[PRO];
    cold_opens = 0;
    pump();
    if (cold_opens != 1) fprintf(stderr, "Pro hotplug opened the joystick driver %u times\n", cold_opens);
    assert(cold_opens == 1);
    counted_id = 0;
    sticks[PRO] = SDL_OpenJoystick(ids[PRO]);
    assert(sticks[PRO]);
}

static void check_hotplug_open_failure(void) {
    title_ready(true); title_key(SDL_SCANCODE_K,false); title_accepted();
    for (unsigned failures = 1; failures <= 2; ++failures) {
        attach(PRO);
        counted_id = ids[PRO];
        failures_remaining = failures;
        pump();
        assert(failures_remaining == 0);
        assert(title_pad_seat(ids[PRO]) == (failures == 1 ? 1 : -1));
        detach_title_pad(PRO);
    }
}

static int SDLCALL request_players(void *arg) {
    int players=(int)(intptr_t)arg;
    return mk8_controller_applet_wait(players,players,input_players());
}

static SDL_Thread *open_prerace_applet(int players,int row) {
    SDL_Thread *worker=SDL_CreateThread(request_players,"Guest controller applet",(void *)(intptr_t)players);
    assert(worker);
    Uint64 until=SDL_GetTicks()+2000;
    while ((!SDL_GetAtomicInt(&core_released) || mk8_editor.listening!=row) && SDL_GetTicks()<until) {
        pump(); SDL_Delay(1);
    }
    assert(SDL_GetAtomicInt(&core_released) && mk8_editor.listening==row);
    assert(mk8_controller_join_players()==(unsigned)players);
    if (assignment_layout) assert(!applet_notice[0]);
    else {
        char label[24]; SDL_snprintf(label,sizeof label,"Player %d of %d",row+1,players);
        assert(strstr(applet_notice,label));
    }
    return worker;
}

static void check_prerace_applet(void) {
    attach(PRO); attach(SOLO_L); attach(SOLO_R); pump();
    title_ready(true);
    button(sticks[PRO],SDL_GAMEPAD_BUTTON_EAST,true);
    button(sticks[PRO],SDL_GAMEPAD_BUTTON_EAST,false); title_accepted();
    assert(title_pad_seat(ids[PRO])==0 && input_players()==3 && mk8_assigned_keyboard()<0);
    shared_race=true;
    assert(mk8_controller_applet_wait(4,4,3)==3 && !mk8_applet_request);
    shared_race=false;
    int left=title_pad_seat(ids[SOLO_L]), right=title_pad_seat(ids[SOLO_R]);
    int result=-1;
    SDL_Thread *worker=open_prerace_applet(4,3);
    title_key(SDL_SCANCODE_ESCAPE,false); pump();
    SDL_WaitThread(worker,&result);
    assert(!result && input_players()==3 && mk8_assigned_keyboard()<0);
    assert(!mk8_controller_join_back_pressed());
    worker=open_prerace_applet(4,3);
    button(sticks[PRO],SDL_GAMEPAD_BUTTON_SOUTH,true); pump();
    SDL_WaitThread(worker,&result);
    assert(!result && mk8_controller_join_back_pressed());
    button(sticks[PRO],SDL_GAMEPAD_BUTTON_SOUTH,false);
    worker=open_prerace_applet(4,3);
    title_key(SDL_SCANCODE_RETURN,false); pump();
    button(sticks[PRO],SDL_GAMEPAD_BUTTON_EAST,true);
    g_rt_unwind=1;
    SDL_WaitThread(worker,&result);
    assert(!result);
    pump(); g_rt_unwind=0;
    assert(!mk8_applet_request && !mk8_editor.rows && mk8_assigned_keyboard()<0);
    button(sticks[PRO],SDL_GAMEPAD_BUTTON_EAST,false);
    worker=open_prerace_applet(4,3);
    title_key(SDL_SCANCODE_RETURN,false); pump();
    assert(mk8_edit_view(NULL,NULL)==MK8_EDIT_READY && input_players()==3);
    title_key(SDL_SCANCODE_ESCAPE,false);
    button(sticks[PRO],SDL_GAMEPAD_BUTTON_SOUTH,true);
    button(sticks[PRO],SDL_GAMEPAD_BUTTON_SOUTH,false); pump();
    assert(mk8_applet_request==4 && !mk8_controller_join_back_pressed());
    button(sticks[PRO],SDL_GAMEPAD_BUTTON_EAST,true);
    button(sticks[PRO],SDL_GAMEPAD_BUTTON_EAST,false); pump();
    SDL_WaitThread(worker,&result);
    assert(result==4 && input_players()==4 && mk8_assigned_keyboard()==3 && !SDL_GetAtomicInt(&core_released));
    assert(title_pad_seat(ids[PRO])==0 && title_pad_seat(ids[SOLO_L])==left && title_pad_seat(ids[SOLO_R])==right);
    detach_title_pad(PRO); detach_title_pad(SOLO_L); detach_title_pad(SOLO_R);
}

static void check_prerace_sparse_seats(void) {
    title_ready(true); title_key(SDL_SCANCODE_K,false); title_accepted();
    attach(PRO); attach(SOLO_L); attach(SOLO_R); pump();
    SDL_JoystickID slots[4][2]={{ids[PRO]},{ids[SOLO_L]},{0},{ids[SOLO_R]}};
    assert(mk8_assign_layout(slots,2,false));
    detach_title_pad(PRO);
    assert(input_players()==4 && mk8_live_seats()==14);
    SDL_Thread *worker=open_prerace_applet(4,0);
    title_key(SDL_SCANCODE_ESCAPE,false); pump();
    int result; SDL_WaitThread(worker,&result); assert(!result);
    slots[0][0]=0; slots[1][0]=0; slots[2][0]=ids[SOLO_L]; slots[3][0]=0;
    assert(mk8_assign_layout(slots,0,false));
    assert(input_players()==2 && mk8_live_seats()==5);
    assignment_layout=false; // A previously prepared install can lack the roster layout.
    worker=open_prerace_applet(2,1);
    title_key(SDL_SCANCODE_ESCAPE,false); pump();
    SDL_WaitThread(worker,&result); assert(!result);
    assignment_layout=true;
    worker=open_prerace_applet(4,1);
    shoulders(sticks[SOLO_R]); pump();
    assert(mk8_editor.listening==3 && mk8_editor.slots[2][0]==ids[SOLO_L]);
    title_key(SDL_SCANCODE_ESCAPE,false); pump();
    SDL_WaitThread(worker,&result);
    assert(!result && mk8_live_seats()==5 && title_pad_seat(ids[SOLO_L])==2);
    detach_title_pad(SOLO_L); detach_title_pad(SOLO_R);
}

static void check_prerace_p1_only(void) {
    attach(PRO); pump();
    title_ready(true);
    button(sticks[PRO],SDL_GAMEPAD_BUTTON_EAST,true);
    button(sticks[PRO],SDL_GAMEPAD_BUTTON_EAST,false); title_accepted();
    SDL_Thread *worker=open_prerace_applet(4,1);
    assert(mk8_controller_join_keyboard());
    int row=-1;
    title_key(SDL_SCANCODE_RETURN,false); pump();
    assert(mk8_edit_view(NULL,&row)==MK8_EDIT_LISTEN && row==2 && !mk8_controller_join_keyboard());
    attach(SOLO_L); pump(); shoulders(sticks[SOLO_L]); pump();
    assert(mk8_edit_view(NULL,&row)==MK8_EDIT_LISTEN && row==3);
    attach(SOLO_R); pump(); shoulders(sticks[SOLO_R]); pump();
    assert(mk8_edit_view(NULL,NULL)==MK8_EDIT_READY);
    button(sticks[PRO],SDL_GAMEPAD_BUTTON_EAST,true); pump();
    int result; SDL_WaitThread(worker,&result);
    assert(result==4 && !mk8_controller_join_players() && title_pad_seat(ids[PRO])==0 && mk8_assigned_keyboard()==1);
    assert(title_pad_seat(ids[SOLO_L])==2 && title_pad_seat(ids[SOLO_R])==3);
    button(sticks[PRO],SDL_GAMEPAD_BUTTON_EAST,false);
    detach_title_pad(PRO); detach_title_pad(SOLO_L); detach_title_pad(SOLO_R);
}

static void check_prerace_p1_replacement(void) {
    attach(PRO); attach(SOLO_L); attach(SOLO_R); pump();
    title_ready(true); button(sticks[PRO],SDL_GAMEPAD_BUTTON_EAST,true);
    button(sticks[PRO],SDL_GAMEPAD_BUTTON_EAST,false); title_accepted();
    SDL_Thread *worker=open_prerace_applet(4,3);
    detach_title_pad(PRO); pump();
    assert(mk8_editor.listening==0);
    attach(PAIR_L); pump(); shoulders(sticks[PAIR_L]); pump();
    assert(mk8_editor.listening==3);
    title_key(SDL_SCANCODE_RETURN,false); pump();
    assert(mk8_edit_view(NULL,NULL)==MK8_EDIT_READY);
    SDL_Event focus={.type=SDL_EVENT_WINDOW_FOCUS_LOST};
    assert(SDL_PushEvent(&focus)); pump();
    button(sticks[PAIR_L],SDL_GAMEPAD_BUTTON_EAST,true);
    title_key(SDL_SCANCODE_ESCAPE,false);
    focus.type=SDL_EVENT_WINDOW_FOCUS_GAINED;
    assert(SDL_PushEvent(&focus)); pump();
    assert(mk8_applet_request==4);
    button(sticks[PAIR_L],SDL_GAMEPAD_BUTTON_EAST,false);
    button(sticks[SOLO_R],SDL_GAMEPAD_BUTTON_EAST,true);
    button(sticks[SOLO_R],SDL_GAMEPAD_BUTTON_EAST,false);
    assert(mk8_applet_request==4 && input_players()<4);
    button(sticks[PAIR_L],SDL_GAMEPAD_BUTTON_EAST,true); pump();
    int result; SDL_WaitThread(worker,&result);
    assert(result==4 && title_pad_seat(ids[PAIR_L])==0 && mk8_assigned_keyboard()==3);
    button(sticks[PAIR_L],SDL_GAMEPAD_BUTTON_EAST,false);
    detach_title_pad(PAIR_L); detach_title_pad(SOLO_L); detach_title_pad(SOLO_R);
}
static void applet_key_state(SDL_Scancode code,bool down) {
    SDL_Event e={.type=down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP};
    e.key.scancode=code;
    assert(SDL_PushEvent(&e)); pump();
}

static void check_keyboard_prerace(void) {
    title_ready(true); title_key(SDL_SCANCODE_K,false); title_accepted();
    attach(PRO); attach(SOLO_L); pump();
    assert(input_players()==3 && mk8_assigned_keyboard()==0);
    g_bind_key[BIND_A]=SDL_SCANCODE_RETURN;
    g_bind_key[BIND_B]=SDL_SCANCODE_J;
    int result;
    SDL_Thread *worker=open_prerace_applet(4,3);
    applet_key_state(SDL_SCANCODE_J,true); pump();
    SDL_WaitThread(worker,&result);
    assert(!result && !(guest_buttons(0)&(1ull<<BIND_B)));
    applet_key_state(SDL_SCANCODE_J,false);
    worker=open_prerace_applet(4,3);
    attach(SOLO_R); pump(); shoulders(sticks[SOLO_R]); pump();
    assert(mk8_edit_view(NULL,NULL)==MK8_EDIT_READY && input_players()==3);
    applet_key_state(SDL_SCANCODE_RETURN,true); pump();
    SDL_WaitThread(worker,&result);
    assert(result==4 && title_pad_seat(ids[SOLO_R])==3 && mk8_assigned_keyboard()==0);
    assert(!(guest_buttons(0)&(1ull<<BIND_A)));
    applet_key_state(SDL_SCANCODE_RETURN,false);
    applet_key_state(SDL_SCANCODE_RETURN,true);
    assert(guest_buttons(0)&(1ull<<BIND_A));
    applet_key_state(SDL_SCANCODE_RETURN,false);
    detach_title_pad(PRO); detach_title_pad(SOLO_L); detach_title_pad(SOLO_R);
}

static void check_all_reconnect(const char *scenario) {
    bool edited=!strcmp(scenario,"all-reconnect-edited");
    bool identical=!strcmp(scenario,"all-reconnect-identical");
    bool stock=!strcmp(scenario,"all-reconnect-stock");
    bool one_serial=!strcmp(scenario,"all-reconnect-one-serial");
    fixture_serials=!identical && !stock;
    returning_serial_ready=true; spare_serial_ready=!one_serial;
    enum Device p3=stock ? PAIR_L : PAIR_R;
    if (stock) g_settings.pads_from_p2=true;
    else { title_ready(true); title_key(SDL_SCANCODE_K,false); title_accepted(); }
    attach(PRO); attach(p3); attach_as(SOLO_R,PAIR_R); pump_pro_hotplug();
    int pro=1,right=2;
    if(edited) {
        assert(edit(MK8_EDIT_BEGIN,4,0)==MK8_EDIT_READY);
        assert(edit(MK8_EDIT_SWAP,1,2)==MK8_EDIT_READY);
        assert(edit(MK8_EDIT_APPLY,0,0)==MK8_EDIT_APPLIED);
        pro=2;right=1;
    }
    check_extra_button(PRO,pro); check_extra_button(p3,right); check_extra_button(SOLO_R,3);
    SDL_JoystickID old=ids[SOLO_R];
    detach_title_pad(p3); detach_title_pad(PRO); detach_title_pad(SOLO_R);
    // A different controller cannot fill someone else's reserved seat.
    attach(SOLO_L); pump(); check_extra_button(SOLO_L,-1); detach_title_pad(SOLO_L);
    spare_serial_ready=true;
    attach_as(SOLO_R,PAIR_R); pump();
    assert(ids[SOLO_R]!=old);
    check_extra_button(SOLO_R,identical || one_serial ? -1 : 3);
    attach(p3); attach(PRO); pump_pro_hotplug();
    check_extra_button(PRO,pro);
    check_extra_button(p3,identical ? -1 : right);
    check_extra_button(SOLO_R,identical || one_serial ? -1 : 3);
    if (edited) {
        // A device returns during a two-row edit. Its new explicit row supersedes its offline row.
        assert(edit(MK8_EDIT_BEGIN,2,0)==MK8_EDIT_READY);
        detach_title_pad(PRO); attach(PRO); pump();
        assert(edit(MK8_EDIT_ASSIGN,1,0)==MK8_EDIT_LISTEN); shoulders(sticks[PRO]);
        assert(edit(MK8_EDIT_APPLY,0,0)==MK8_EDIT_APPLIED);
        check_extra_button(PRO,1);
        detach_title_pad(PRO); attach(PRO); pump(); check_extra_button(PRO,1);
    }
    if (identical) {
        // Both ambiguous grips require an explicit registration; fresh gestures can swap their owners.
        assert(edit(MK8_EDIT_BEGIN,4,0)==MK8_EDIT_READY);
        assert(edit(MK8_EDIT_ASSIGN,right,0)==MK8_EDIT_LISTEN);
        shoulders(sticks[SOLO_R]);
        assert(edit(MK8_EDIT_ASSIGN,3,0)==MK8_EDIT_LISTEN);
        shoulders(sticks[p3]);
        assert(edit(MK8_EDIT_APPLY,0,0)==MK8_EDIT_APPLIED);
        check_extra_button(SOLO_R,right); check_extra_button(p3,3);
    }
    detach_title_pad(p3); detach_title_pad(PRO); detach_title_pad(SOLO_R);
}
