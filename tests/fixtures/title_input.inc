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
