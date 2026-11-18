"""Keep trying startup devices for player one after a failed SDL open."""

HELPER = '''// MK8: startup enumeration and hotplug share the same slot assignment.
static void mk8_open_gamepad(SDL_JoystickID id) {
    if (!real_gamepad(id) || (pad && SDL_GetGamepadID(pad) == id)) return;
    for (int i = 0; i < EXTRA_PADS; i++)
        if (extra_pad[i] && SDL_GetGamepadID(extra_pad[i]) == id) return;
    if (pad || pads_from_p2()) { open_extra(id); return; }
    SDL_Gamepad *opened = SDL_OpenGamepad(id);
    if (!opened) {
        LOG("[input] cannot open gamepad %u: %s\\n", (unsigned)id, SDL_GetError());
        return;
    }
    pad = opened;
    LOG("[input] gamepad: %s\\n", SDL_GetGamepadName(pad));
    note_family();
    recompute_pad();
    style_changed();
}

'''

SCAN = '''        if (n > 0 && !pad && !pads_from_p2() && real_gamepad(ids[0])) { pad = SDL_OpenGamepad(ids[0]); LOG("[input] gamepad: %s\\n", SDL_GetGamepadName(pad)); note_family(); }
        for (int i = pad ? 1 : 0; i < n; i++) open_extra(ids[i]);'''
ADDED = '''        if (!pad && !pads_from_p2() && real_gamepad(e->gdevice.which)) { pad = SDL_OpenGamepad(e->gdevice.which); LOG("[input] gamepad: %s\\n", SDL_GetGamepadName(pad)); note_family(); }
        else open_extra(e->gdevice.which);'''


def prepare(runtime):
    path = runtime / 'hle/input.c'
    text = path.read_text(encoding='utf-8')
    anchor = 'void input_handle_event(const SDL_Event *e) {'
    for old, new in [(anchor, HELPER + anchor),
                     (SCAN, '        for (int i = 0; i < n; i++) mk8_open_gamepad(ids[i]);'),
                     (ADDED, '        mk8_open_gamepad(e->gdevice.which);')]:
        if text.count(old) != 1:
            raise ValueError('Controller discovery boundary changed')
        text = text.replace(old, new)
    path.write_text(text, encoding='utf-8')
