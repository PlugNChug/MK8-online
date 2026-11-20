extern "C" void mk8_settings_values_sync(void);
extern "C" void mk8_settings_page_fixed(uint64_t);
extern "C" void mk8_settings_page_wheel(int);
extern "C" bool input_user_blocked(const SDL_Event *);
static bool menu_ab_swapped(void);

static uint64_t mk8_keys, mk8_test_pad, mk8_test_axes, mk8_pad, mk8_blocked;
static bool mk8_block_next, mk8_focused=true, mk8_last_swap;

// Sample open SDL handles after event processing/reassignment; a lost release must not latch a direction.
static void mk8_settings_poll(bool overlay) {
    uint64_t held=0;
    int count=0;
    SDL_JoystickID *ids=input_user_blocked(nullptr) ? nullptr : SDL_GetGamepads(&count);
    bool swap=menu_ab_swapped();
    for(int i=0;i<count;i++) {
        SDL_Gamepad *pad=SDL_GetGamepadFromID(ids[i]);
        if(!pad) continue;
        if(SDL_GetGamepadButton(pad,SDL_GAMEPAD_BUTTON_EAST)) held|=swap ? 2 : 1;
        if(SDL_GetGamepadButton(pad,SDL_GAMEPAD_BUTTON_SOUTH)) held|=swap ? 1 : 2;
        const SDL_GamepadButton directions[]={SDL_GAMEPAD_BUTTON_DPAD_LEFT,SDL_GAMEPAD_BUTTON_DPAD_UP,SDL_GAMEPAD_BUTTON_DPAD_RIGHT,SDL_GAMEPAD_BUTTON_DPAD_DOWN};
        for(unsigned d=0;d<4;d++) if(SDL_GetGamepadButton(pad,directions[d])) held|=1ull<<(12+d);
        int x=SDL_GetGamepadAxis(pad,SDL_GAMEPAD_AXIS_LEFTX), y=SDL_GetGamepadAxis(pad,SDL_GAMEPAD_AXIS_LEFTY);
        if(x<-16000) held|=1ull<<12;
        if(x>16000) held|=1ull<<14;
        if(y<-16000) held|=1ull<<13;
        if(y>16000) held|=1ull<<15;
    }
    SDL_free(ids);
    if(swap!=mk8_last_swap) { mk8_keys=mk8_test_pad=mk8_test_axes=0; mk8_block_next=true; mk8_last_swap=swap; }
    if(!mk8_settings_page_active() || overlay || !mk8_focused || bind_capture_active(nullptr,nullptr,nullptr)) {
        mk8_keys=mk8_test_pad=mk8_test_axes=mk8_pad=0; mk8_blocked=held;
    } else {
        if(mk8_block_next) mk8_blocked|=held;
        mk8_blocked&=held;
        mk8_pad=held&~mk8_blocked;
    }
    mk8_block_next=false;
    mk8_settings_page_fixed(mk8_keys|mk8_test_pad|mk8_test_axes|mk8_pad);
}

// Menu navigation stays usable after game remapping or player reassignment.
static bool mk8_settings_event(const SDL_Event *e, bool overlay) {
    if(e->type==SDL_EVENT_WINDOW_FOCUS_LOST || e->type==SDL_EVENT_WINDOW_FOCUS_GAINED) {
        mk8_focused=e->type==SDL_EVENT_WINDOW_FOCUS_GAINED;
        mk8_keys=mk8_test_pad=mk8_test_axes=mk8_pad=0; mk8_block_next=true;
        mk8_settings_page_fixed(0);
    }
    if (!mk8_settings_page_active() || overlay) {
        mk8_keys=mk8_test_pad=mk8_test_axes=mk8_pad=0; mk8_block_next=true;
        mk8_settings_page_fixed(0); return false;
    }
    bool swap=menu_ab_swapped();
    if(swap!=mk8_last_swap) {
        mk8_keys=mk8_test_pad=mk8_test_axes=mk8_pad=0; mk8_block_next=true; mk8_last_swap=swap;
    }
    if (bind_capture_event(e)) {
        mk8_keys=mk8_test_pad=mk8_test_axes=mk8_pad=0; mk8_block_next=true;
        mk8_settings_page_fixed(0);
        return e->type!=SDL_EVENT_KEY_UP && e->type!=SDL_EVENT_GAMEPAD_BUTTON_UP && e->type!=SDL_EVENT_GAMEPAD_AXIS_MOTION;
    }
    uint64_t bit=0; bool down=false;
    if (e->type==SDL_EVENT_KEY_DOWN || e->type==SDL_EVENT_KEY_UP) {
        down=e->type==SDL_EVENT_KEY_DOWN;
        int key=0;
        if (e->key.key>=SDLK_0 && e->key.key<=SDLK_9) key='0'+e->key.key-SDLK_0;
        else if (e->key.key>=SDLK_KP_1 && e->key.key<=SDLK_KP_9) key='1'+e->key.key-SDLK_KP_1;
        else if (e->key.key==SDLK_KP_0) key='0';
        else if (e->key.scancode==SDL_SCANCODE_BACKSPACE) key=8;
        else if (e->key.scancode==SDL_SCANCODE_RETURN || e->key.scancode==SDL_SCANCODE_KP_ENTER) key=13;
        else if (e->key.scancode==SDL_SCANCODE_ESCAPE) key=27;
        if (down && key && (!e->key.repeat || key==8 || (key>='0' && key<='9')) && mk8_settings_page_key(key)) return true;
        switch(e->key.scancode) {
        case SDL_SCANCODE_UP: bit=1ull<<13; break;
        case SDL_SCANCODE_DOWN: bit=1ull<<15; break;
        case SDL_SCANCODE_LEFT: bit=1ull<<12; break;
        case SDL_SCANCODE_RIGHT: bit=1ull<<14; break;
        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_SPACE: bit=1; break;
        case SDL_SCANCODE_ESCAPE: bit=2; break;
        default: return false;
        }
        if(down && e->key.repeat && (bit&3)) return true;
        mk8_keys=down ? mk8_keys|bit : mk8_keys&~bit;
    } else if(e->type==SDL_EVENT_GAMEPAD_BUTTON_DOWN || e->type==SDL_EVENT_GAMEPAD_BUTTON_UP) {
        down=e->type==SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        switch(e->gbutton.button) {
        case SDL_GAMEPAD_BUTTON_EAST: bit=menu_ab_swapped() ? 2 : 1; break;
        case SDL_GAMEPAD_BUTTON_SOUTH: bit=menu_ab_swapped() ? 1 : 2; break;
        case SDL_GAMEPAD_BUTTON_DPAD_UP: bit=1ull<<13; break;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: bit=1ull<<15; break;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: bit=1ull<<12; break;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: bit=1ull<<14; break;
        default: return false;
        }
        // Preserve the existing host-test event device; physical controllers are sampled above.
        if(e->gbutton.which==0x7E57u) mk8_test_pad=down ? mk8_test_pad|bit : mk8_test_pad&~bit;
    } else if(e->type==SDL_EVENT_GAMEPAD_AXIS_MOTION) {
        if(e->gaxis.which!=0x7E57u) return false;
        if(e->gaxis.axis==SDL_GAMEPAD_AXIS_LEFTX) {
            mk8_test_axes&=~((1ull<<12)|(1ull<<14));
            if(e->gaxis.value<-16000) mk8_test_axes|=1ull<<12;
            if(e->gaxis.value>16000) mk8_test_axes|=1ull<<14;
        } else if(e->gaxis.axis==SDL_GAMEPAD_AXIS_LEFTY) {
            mk8_test_axes&=~((1ull<<13)|(1ull<<15));
            if(e->gaxis.value<-16000) mk8_test_axes|=1ull<<13;
            if(e->gaxis.value>16000) mk8_test_axes|=1ull<<15;
        } else return false;
    } else if(e->type==SDL_EVENT_MOUSE_WHEEL) {
        mk8_settings_page_wheel(e->wheel.y>0 ? -1 : e->wheel.y<0 ? 1 : 0); return true;
    } else return false;
    mk8_settings_page_fixed(mk8_keys|mk8_test_pad|mk8_test_axes|mk8_pad);
    return e->type!=SDL_EVENT_KEY_UP && e->type!=SDL_EVENT_GAMEPAD_BUTTON_UP && e->type!=SDL_EVENT_GAMEPAD_AXIS_MOTION;
}
