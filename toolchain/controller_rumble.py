"""Route MK8's final HID vibration values through the existing SDL player assignments."""
from pathlib import Path


def replace(text, old, new, count=1):
    if text.count(old) != count:
        raise ValueError(f'Controller vibration boundary changed: {old.strip()[:70]}')
    return text.replace(old, new)


def prepare(runtime):
    input_path = runtime/'hle/input.c'
    service_path = runtime/'services/hid_set.c'
    text = input_path.read_text(encoding='utf-8')
    service = service_path.read_text(encoding='utf-8')
    implementation = (Path(__file__).resolve().parents[1]/'runtime/mk8/services/rumble.inc').read_text(encoding='utf-8')
    marker = 'static SDL_Gamepad *pad;'
    declarations = ('static SDL_Gamepad *mk8_rumble_open(int player, SDL_JoystickID id);\n'
                    'static void mk8_rumble_close(int player, SDL_Gamepad *device);\n'
                    'static void mk8_rumble_frame(void);\n'
                    'static void mk8_rumble_focus(bool focused);\n')
    text = replace(text, marker, declarations + marker)
    text = replace(text, 'SDL_Gamepad *opened = SDL_OpenGamepad(id);', 'SDL_Gamepad *opened = mk8_rumble_open(0, id);')
    text = replace(text, 'extra_pad[i] = SDL_OpenGamepad(id);', 'extra_pad[i] = mk8_rumble_open(i+1, id);')
    text = replace(text, 'SDL_CloseGamepad(pad)', 'mk8_rumble_close(0, pad)', 2)
    text = replace(text, 'SDL_CloseGamepad(extra_pad[i])', 'mk8_rumble_close(i+1, extra_pad[i])', 2)
    text = replace(text, 'void input_frame(uint64_t frame) {', 'void input_frame(uint64_t frame) {\n    mk8_rumble_frame();')
    marker = 'void input_handle_event(const SDL_Event *e) {'
    text = replace(text, marker, marker + '''
    if (e->type == SDL_EVENT_WINDOW_FOCUS_LOST) mk8_rumble_focus(false);
    if (e->type == SDL_EVENT_WINDOW_FOCUS_GAINED) mk8_rumble_focus(true);''')
    text = replace(text, '// Vibration devices: accepted but inert (the SDK\'s vibration node graph stays recompiled).',
                   '// Vibration devices: the SDK mixes the effects before sending them to HID.\n' + implementation)
    text = replace(text, 'int n = (int)c->x[1] < 2 ? (int)c->x[1] : 2;',
                   '''int capacity = (int)c->x[1];
    if (!out || !c->x[2] || capacity <= 0) { c->x[0] = 0; return; }
    int n = capacity < 2 ? capacity : 2;''')
    text = replace(text, 'for (int i = 0; i < n; i++) out[i] = (id & 0xFF)',
                   'if (id > EXTRA_PADS) n = 0;\n    for (int i = 0; i < n; i++) out[i] = (id & 0xFF)')
    text = replace(text, '(id & 0xFF) << 8 | (uint32_t)i | 0x03000000u;  // type, player, position',
                   '3u | (id & 0xff) << 8 | (uint32_t)i << 16;  // FullKey, player, left/right')
    text = replace(text, 'info[1] = h & 1;    // position: left / right',
                   'info[1] = 1 + ((h >> 16) & 1);  // position: left = 1, right = 2')
    marker = '    case 201: case 202: case 203: case 206: case 207: case 208: case 209: break;  // vibration'
    service = replace(service, marker, '''    case 201: case 202: case 204: case 205: case 206:
        mk8_hid_rumble(req, resp);
        break;
    case 203: case 207: case 208: case 209: break;  // other vibration commands''')
    marker = 'SERVICE("hid", hid) {'
    service = replace(service, marker, 'extern void mk8_hid_rumble(IpcReq *req, IpcResp *resp);\n\n' + marker)
    input_path.write_text(text, encoding='utf-8')
    service_path.write_text(service, encoding='utf-8')
