#include "controller_tips.h"

// Only the SDL thread touches device handles. The render thread publishes the selected icon family.
typedef struct { enum Mk8TipSource source; int half; uint32_t buttons; bool raw_pair; } Mk8TipDevice;
typedef struct {
    Mk8TipDevice p1, settings;
    int16_t keys[BIND_KEY_COUNT], buttons[BIND_PAD_COUNT];
} Mk8TipSnapshot;
static SDL_SpinLock mk8_tip_lock;
static Mk8TipSnapshot mk8_tip_snapshot;
static SDL_JoystickID mk8_tip_p1, mk8_tip_owner;
static _Atomic int mk8_tip_icons;
void mk8_tip_icon_choice(int icons) { mk8_tip_icons=icons; }
void mk8_tip_bindings(int16_t *keys, int16_t *buttons);

static Mk8TipDevice mk8_tip_device(SDL_Gamepad *device, bool keyboard, bool pair) {
    if (!device) return (Mk8TipDevice){ keyboard ? MK8_TIP_KEYBOARD : MK8_TIP_NONE };
    const Uint16 product = SDL_GetGamepadProduct(device);
    bool nintendo=SDL_GetGamepadVendor(device)==0x057e && product>=0x2006 && product<=0x2009;
    SDL_GamepadType type=SDL_GetGamepadType(device);
    if (!pair) {
        if ((nintendo && product==0x2006) || type==SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT)
            return (Mk8TipDevice){MK8_TIP_NINTENDO,1};
        if ((nintendo && product==0x2007) || type==SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT)
            return (Mk8TipDevice){MK8_TIP_NINTENDO,2};
    }
    switch (mk8_tip_icons) {
    case PROMPTS_NINTENDO: return (Mk8TipDevice){MK8_TIP_NINTENDO};
    case PROMPTS_XBOX: return (Mk8TipDevice){MK8_TIP_XBOX};
    case PROMPTS_PLAYSTATION: return (Mk8TipDevice){MK8_TIP_PLAYSTATION};
    }
    if(nintendo) return (Mk8TipDevice){MK8_TIP_NINTENDO};
    switch (type) {
    case SDL_GAMEPAD_TYPE_XBOX360: case SDL_GAMEPAD_TYPE_XBOXONE: return (Mk8TipDevice){MK8_TIP_XBOX};
    case SDL_GAMEPAD_TYPE_PS3: case SDL_GAMEPAD_TYPE_PS4: case SDL_GAMEPAD_TYPE_PS5: return (Mk8TipDevice){MK8_TIP_PLAYSTATION};
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
        return (Mk8TipDevice){MK8_TIP_NINTENDO};
    default: return (Mk8TipDevice){MK8_TIP_GENERIC};
    }
}

// Cache the SDL mapping until SDL_EVENT_GAMEPAD_REMAPPED (attachment IDs and their properties are transient).
static uint32_t mk8_tip_capabilities(SDL_Gamepad *pad) {
    if (!pad) return 0;
    SDL_PropertiesID props=SDL_GetGamepadProperties(pad);
    Sint64 cached=SDL_GetNumberProperty(props,"mk8.tip.buttons",-1);
    if (cached>=0) return (uint32_t)cached;
    uint32_t mask=0;
    for (int i=0;i<SDL_GAMEPAD_BUTTON_COUNT;i++)
        if(SDL_GamepadHasButton(pad,(SDL_GamepadButton)i)) mask|=1u<<i;
    if(SDL_GamepadHasAxis(pad,SDL_GAMEPAD_AXIS_LEFT_TRIGGER)) mask|=1u<<PADB_LTRIGGER;
    if(SDL_GamepadHasAxis(pad,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) mask|=1u<<PADB_RTRIGGER;
    if(SDL_GamepadHasAxis(pad,SDL_GAMEPAD_AXIS_LEFTX)) mask|=1u<<PADB_HOST_COUNT;
    if(SDL_GamepadHasAxis(pad,SDL_GAMEPAD_AXIS_LEFTY)) mask|=2u<<PADB_HOST_COUNT;
    SDL_SetNumberProperty(props,"mk8.tip.buttons",mask);
    return mask;
}

static Mk8TipDevice mk8_tip_player(SDL_Gamepad *primary, bool keyboard, bool pair) {
    Mk8TipDevice device=mk8_tip_device(primary,keyboard,pair);
    // The software pair supplies its upright d-pad, buttons and axes through mk8_pair_buttons.
    device.buttons=pair ? ((1u<<(SDL_GAMEPAD_BUTTON_DPAD_RIGHT+1))-1) | (1u<<PADB_LTRIGGER) |
                          (1u<<PADB_RTRIGGER) | (3u<<PADB_HOST_COUNT) : mk8_tip_capabilities(primary);
    return device;
}

void mk8_tip_sync(void) {
    SDL_JoystickID primary = pad ? SDL_GetGamepadID(pad) : 0;
    bool has_keyboard = mk8_assigned_keyboard()==0;
    if (primary!=mk8_tip_owner || (!mk8_tip_p1 && !has_keyboard) ||
        (mk8_tip_p1 && mk8_tip_p1!=primary && (!mk8_partner[0] || mk8_tip_p1!=SDL_GetGamepadID(mk8_partner[0]))))
        mk8_tip_p1=primary;
    mk8_tip_owner=primary;
    Mk8TipSnapshot snap = {0};
    snap.p1=mk8_tip_player(mk8_tip_p1 ? pad : NULL,has_keyboard,mk8_tip_p1 && mk8_partner[0]);
    snap.settings=snap.p1;
    if(snap.settings.source==MK8_TIP_NONE) snap.settings.source=MK8_TIP_KEYBOARD;
    // Settings polls the individual SDL handles directly, even for a software-paired P1.
    if (mk8_tip_p1 && mk8_partner[0]) {
        SDL_Gamepad *grip=mk8_tip_p1==primary ? pad : mk8_partner[0];
        snap.settings=mk8_tip_device(grip,false,false);
        snap.settings.buttons=mk8_tip_capabilities(grip);
        snap.settings.raw_pair=true;
    }
    uint64_t navigation=UINT64_MAX;
    static SDL_JoystickID draft_left,draft_right;
    static uint64_t held_at_handoff;
    if (mk8_editor.rows && !mk8_title_observing) {
        Mk8JoinDevice *left=mk8_edit_device(mk8_editor.slots[0][0]), *right=mk8_edit_device(mk8_editor.slots[0][1]);
        bool keyboard=mk8_editor.keyboard==0;
        SDL_Gamepad *first=left ? left->pad : NULL, *second=right ? right->pad : NULL;
        // Keep the current P1 available to cancel when its draft row is empty.
        if ((left || keyboard) && (first!=pad || second!=mk8_partner[0] || keyboard!=has_keyboard)) {
            snap.p1=mk8_tip_player(first,keyboard,right!=NULL);
            navigation=left ? mk8_edit_pad_navigation(left,right) : mk8_navigation_axes(key_buttons,key_lx,key_ly);
            if (atomic_load(&mk8_draft_navigation)==UINT64_MAX || draft_left!=mk8_editor.slots[0][0] || draft_right!=mk8_editor.slots[0][1])
                held_at_handoff=navigation;
            held_at_handoff &= navigation;
            navigation &= ~held_at_handoff;
            draft_left=mk8_editor.slots[0][0]; draft_right=mk8_editor.slots[0][1];
        }
    }
    atomic_store(&mk8_draft_navigation,navigation);
    mk8_tip_bindings(snap.keys,snap.buttons);
    SDL_LockSpinlock(&mk8_tip_lock);
    mk8_tip_snapshot=snap;
    SDL_UnlockSpinlock(&mk8_tip_lock);
}

void mk8_tip_event(const SDL_Event *e, bool settings) {
    if(e->type==SDL_EVENT_GAMEPAD_REMAPPED) {
        SDL_Gamepad *remapped=SDL_GetGamepadFromID(e->gdevice.which);
        if(remapped) SDL_ClearProperty(SDL_GetGamepadProperties(remapped),"mk8.tip.buttons");
        return;
    }
    SDL_JoystickID id=0;
    if (e->type==SDL_EVENT_KEY_DOWN && !e->key.repeat) {
        if (e->key.scancode==SDL_SCANCODE_F1) settings=true;
        if (!settings) {
            if (mk8_assigned_keyboard()!=0) return;
            int i=0;
            while(i<BIND_KEY_COUNT && mk8_tip_snapshot.keys[i]!=e->key.scancode) ++i;
            if(i==BIND_KEY_COUNT) return;
        }
    } else if (e->type==SDL_EVENT_GAMEPAD_BUTTON_DOWN) id=e->gbutton.which;
    else if (e->type==SDL_EVENT_GAMEPAD_AXIS_MOTION) id=e->gaxis.which;
    else return;
    SDL_Gamepad *device=id ? SDL_GetGamepadFromID(id) : NULL;
    if(e->type!=SDL_EVENT_KEY_DOWN && !device) return;
    if(e->type==SDL_EVENT_GAMEPAD_AXIS_MOTION) {
        // Stick noise, release and further motion within a held direction don't change the last active device.
        const char *axis;
        switch(e->gaxis.axis) {
        case SDL_GAMEPAD_AXIS_LEFTX: axis="mk8.tip.x"; break;
        case SDL_GAMEPAD_AXIS_LEFTY: axis="mk8.tip.y"; break;
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: {
            if(settings) return;
            int host=e->gaxis.axis==SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? PADB_LTRIGGER : PADB_RTRIGGER;
            int i=0;
            while(i<BIND_PAD_COUNT && mk8_tip_snapshot.buttons[i]!=host) ++i;
            if(i==BIND_PAD_COUNT) return;
            axis=host==PADB_LTRIGGER ? "mk8.tip.lt" : "mk8.tip.rt";
            break;
        }
        default: return;
        }
        SDL_PropertiesID props=SDL_GetGamepadProperties(device);
        int sign=(e->gaxis.value>16000)-(e->gaxis.value< -16000);
        if(SDL_GetNumberProperty(props,axis,0)==sign) return;
        SDL_SetNumberProperty(props,axis,sign);
        if(!sign) return;
    }
    bool owner=id ? device==pad || device==mk8_partner[0] : mk8_assigned_keyboard()==0;
    if (owner) {
        mk8_tip_owner=pad ? SDL_GetGamepadID(pad) : 0;
        mk8_tip_p1=id;
    }
}

static const char *mk8_tip_button(Mk8TipDevice d, int button) {
    if (button<0 || button>=PADB_HOST_COUNT || !(d.buttons & (1u<<button))) return "";
    if (d.half && button>=SDL_GAMEPAD_BUTTON_SOUTH && button<=SDL_GAMEPAD_BUTTON_NORTH) {
        static const char *const face[2][4]={{"Left","Down","Up","Right"},{"A","X","B","Y"}};
        return face[d.half-1][button-SDL_GAMEPAD_BUTTON_SOUTH];
    }
    if (d.half && button==SDL_GAMEPAD_BUTTON_START) return d.half==1 ? "Minus" : "Plus";
    if (d.half && button==SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) return "SL";
    if (d.half && button==SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) return "SR";
    if (d.half) {
        if (button==SDL_GAMEPAD_BUTTON_LEFT_STICK) return "Stick press";
        if (button==SDL_GAMEPAD_BUTTON_GUIDE) return d.half==1 ? "Capture" : "Home";
        if (d.half==1 && button==SDL_GAMEPAD_BUTTON_LEFT_PADDLE1) return "L";
        if (d.half==1 && button==SDL_GAMEPAD_BUTTON_LEFT_PADDLE2) return "ZL";
        if (d.half==2 && button==SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1) return "R";
        if (d.half==2 && button==SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2) return "ZR";
        if (d.half==2 && button==SDL_GAMEPAD_BUTTON_MISC1) return "Capture";
        // SDL's Switch mini mode exposes the physical +/- through START, and has no separate d-pad.
        return "";
    }
    if (d.source==MK8_TIP_GENERIC) {
        static const char *const face[]={"South","East","West","North"};
        if (button>=SDL_GAMEPAD_BUTTON_SOUTH && button<=SDL_GAMEPAD_BUTTON_NORTH) return face[button];
        if (button==PADB_LTRIGGER) return "Left trigger";
        if (button==PADB_RTRIGGER) return "Right trigger";
        const char *s=SDL_GetGamepadStringForButton((SDL_GamepadButton)button);
        return s ? s : "(none)";
    }
    return bind_pad_label(button,d.source==MK8_TIP_XBOX ? 1 : d.source==MK8_TIP_PLAYSTATION ? 2 : 0);
}

void mk8_control_tips(bool settings, struct Mk8ControlTips *out) {
    SDL_LockSpinlock(&mk8_tip_lock);
    Mk8TipSnapshot snap=mk8_tip_snapshot;
    SDL_UnlockSpinlock(&mk8_tip_lock);
    Mk8TipDevice d=settings ? snap.settings : snap.p1;
    memset(out,0,sizeof *out);
    out->source=d.source;
    if(d.source==MK8_TIP_NONE) return;
    static const int slots[MK8_TIP_COUNT]={BIND_A,BIND_B,BIND_X,BIND_Y,BIND_MINUS,BIND_UP,BIND_DOWN,BIND_LEFT,BIND_RIGHT,BIND_PLUS};
    static const char *const fixed_keys[MK8_TIP_COUNT]={"Enter","Esc",NULL,NULL,NULL,"Up","Down","Left","Right",NULL};
    bool swap=snap.buttons[BIND_A]==SDL_GAMEPAD_BUTTON_SOUTH && snap.buttons[BIND_B]==SDL_GAMEPAD_BUTTON_EAST;
    int fixed_pad[MK8_TIP_COUNT]={swap ? SDL_GAMEPAD_BUTTON_SOUTH : SDL_GAMEPAD_BUTTON_EAST,
        swap ? SDL_GAMEPAD_BUTTON_EAST : SDL_GAMEPAD_BUTTON_SOUTH,-1,-1,-1,SDL_GAMEPAD_BUTTON_DPAD_UP,
        SDL_GAMEPAD_BUTTON_DPAD_DOWN,SDL_GAMEPAD_BUTTON_DPAD_LEFT,SDL_GAMEPAD_BUTTON_DPAD_RIGHT,-1};
    for(int i=0;i<MK8_TIP_COUNT;i++) {
        if(settings && !fixed_keys[i]) continue;
        const char *label=d.source==MK8_TIP_KEYBOARD ?
            (settings ? fixed_keys[i] : bind_key_label(snap.keys[slots[i]])) :
            mk8_tip_button(d,settings ? fixed_pad[i] : snap.buttons[slots[i]]);
        if ((!*label || !strcmp(label,"(none)")) && i>=MK8_TIP_UP && i<=MK8_TIP_RIGHT) {
            int dir=i-MK8_TIP_UP; // Up, Down, Left, Right.
            if(d.source==MK8_TIP_KEYBOARD && !settings) label=bind_key_label(snap.keys[BIND_LS_UP+dir]);
            else if (d.buttons & ((dir<2 ? 2u : 1u)<<PADB_HOST_COUNT)) {
                static const char *const sticks[][4]={{"Stick up","Stick down","Stick left","Stick right"},
                    {"L stick right","L stick left","L stick up","L stick down"},
                    {"R stick left","R stick right","R stick down","R stick up"}};
                label=sticks[d.raw_pair ? d.half : 0][dir];
            }
        }
        SDL_strlcpy(out->label[i],label,sizeof out->label[i]);
    }
}
