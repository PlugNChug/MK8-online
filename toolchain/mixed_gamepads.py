"""Apply explicit per-player assignments after the normal SDL discovery and settings adaptations."""
from pathlib import Path
import shutil

from controller_rumble import replace

SOURCE = Path(__file__).resolve().parents[1] / 'runtime/mk8/services'


def prepare(runtime):
    # Capture (SDL), Reset (render thread) and reload share these arrays with the tip snapshot.
    bindings = runtime / 'hle/bindings.c'
    text = bindings.read_text(encoding='utf-8')
    marker = 'static const char *const k_ini[BIND_KEY_COUNT]'
    text = replace(text, marker, '''static SDL_SpinLock mk8_binding_lock;
void mk8_tip_bindings(int16_t *keys, int16_t *buttons) {
    SDL_LockSpinlock(&mk8_binding_lock);
    memcpy(keys, g_bind_key, sizeof g_bind_key);
    memcpy(buttons, g_bind_pad, sizeof g_bind_pad);
    SDL_UnlockSpinlock(&mk8_binding_lock);
}
''' + marker)
    for start, end in [
        ('void bind_reset(int dev) {', '    LOG("[input] %s bindings reset'),
        ('static void assign(int dev, int slot, int host) {', '    LOG("[input] %s %s ->'),
        ('    if (!pad && !kb) return false;', '    return true;\n}\n\nvoid bind_settings_save'),
    ]:
        text = replace(text, start, start + '\n    SDL_LockSpinlock(&mk8_binding_lock);')
        text = replace(text, end, '    SDL_UnlockSpinlock(&mk8_binding_lock);\n' + end)
    bindings.write_text(text, encoding='utf-8')
    menu = runtime / 'ui/menu.mm'
    text = menu.read_text(encoding='utf-8')
    text = replace(text, 'extern "C" void mk8_settings_values_sync(void);',
                   'extern "C" void mk8_tip_icon_choice(int);\nextern "C" void mk8_settings_values_sync(void);')
    text = replace(text, '    mk8_settings_values_sync();',
                   '    mk8_settings_values_sync();\n    mk8_tip_icon_choice(g_settings.prompts);')
    menu.write_text(text, encoding='utf-8')
    path = runtime / 'hle/input.c'
    text = path.read_text(encoding='utf-8')
    marker = '        if (lx < -16000) b |= 1ull << 16;'
    text = replace(text, marker, '        if (!menu) mk8_controller_filter_extra(i,&b,&lx,&ly,&rx,&ry);\n' + marker)
    text = replace(text, 'static bool extra_connected(int i) { return extra_pad[i] || (i == 0 && script2_on()); }',
                   '''// Guest threads can query all four slots while the SDL thread stages a swap.
static _Atomic int mk8_keyboard_player;  // Default shares the keyboard with P1.
static int mk8_assigned_keyboard(void) { return mk8_keyboard_player; }
static void mk8_set_assigned_keyboard(int slot) { mk8_keyboard_player = slot; }

typedef struct { uint64_t buttons; int32_t lx, ly, rx, ry; } Mk8KeyState;
static Mk8KeyState mk8_player_keys(int player) {
    if (player != mk8_assigned_keyboard()) return (Mk8KeyState){0};
    return (Mk8KeyState){key_buttons, key_lx, key_ly, key_rx, key_ry};
}
static _Atomic unsigned mk8_connected_mask;
static void mk8_publish_connections(void) {
    unsigned mask = 0;
    for (int i = 0; i < EXTRA_PADS; i++)
        if (extra_pad[i] || mk8_assigned_keyboard() == i+1) mask |= 1u << i;
    atomic_store(&mk8_connected_mask, mask);
}
static bool extra_connected(int i) {
    return (atomic_load(&mk8_connected_mask) & (1u << i)) || (i == 0 && script2_on());
}''')
    text = replace(text, 'static void style_changed(void) {\n    mk8_sync_sensors();',
                   'static void style_changed(void) {\n    mk8_remember_connected();\n    mk8_publish_connections();\n    mk8_publish_names();\n    mk8_sync_sensors();')
    text = replace(text, 'pad = NULL; note_family(); }', 'pad = NULL; note_family(); mk8_publish_names(); }')
    text = replace(text, '    case SDL_EVENT_GAMEPAD_REMOVED:\n',
                   '    case SDL_EVENT_GAMEPAD_REMAPPED: mk8_publish_names(); break;\n    case SDL_EVENT_GAMEPAD_REMOVED:\n')
    start = 'static void recompute_pad(void) {'
    text = replace(text, start, (SOURCE / 'mixed_gamepads.inc').read_text(encoding='utf-8') + '\n' + start)
    for index, slot, device in [('i', '0', 'pad'), ('k', 'i+1', 'g')]:
        marker = f'    uint64_t b = 0;\n    for (int {index} = 0; {index} < BIND_PAD_COUNT; {index}++)\n        if (g_bind_pad'
        text = replace(text, marker, f'    host = mk8_filter_shortcut({slot}, {device}, host);\n' + marker)
        store_buttons = '    pad_buttons = b;' if slot == '0' else '    extra_buttons[i] = b;'
        physical = 'pad && !host_test' if slot == '0' else 'g'
        # Software-paired grips each supply only one member: their Plus/Minus are already independent.
        text = replace(text, store_buttons,
                       f'    if ({physical} && !mk8_partner[{slot}] && mk8_settings_shared_race()) b &= ~(uint64_t)mk8_pause_bindings(host);\n'
                       + store_buttons)
    for slot, handle in [('0', 'pad'), ('i+1', 'extra_pad[i]')]:
        close = f'mk8_rumble_close({slot}, {handle})'
        text = replace(text, close, f'mk8_reset_pause({slot}), ' + close, 2)
    marker = '    if (e->type == SDL_EVENT_WINDOW_FOCUS_LOST) mk8_rumble_focus(false);'
    text = replace(text, marker, '    if (e->type == SDL_EVENT_WINDOW_FOCUS_LOST)\n'
                   '        for (int i=0; i<=EXTRA_PADS; ++i) mk8_reset_pause(i);\n' + marker)
    text = replace(text, '    if (pad && !host_test) {', '''    if (mk8_partner[0] && !host_test) {
        host |= mk8_pair_buttons(pad, mk8_partner[0]);
    } else if (pad && !host_test) {''')
    text = replace(text, '    if (g) {\n        for (int k = 0; k < SDL_GAMEPAD_BUTTON_COUNT;', '''    if (mk8_partner[i+1]) {
        host = mk8_pair_buttons(g, mk8_partner[i+1]);
    } else if (g) {
        for (int k = 0; k < SDL_GAMEPAD_BUTTON_COUNT;''')
    for device, partner in [('pad', 'mk8_partner[0]'), ('g', 'mk8_partner[i+1]')]:
        for axis in ('LEFTX', 'LEFTY', 'RIGHTX', 'RIGHTY'):
            old = f'SDL_GetGamepadAxis({device}, SDL_GAMEPAD_AXIS_{axis})'
            text = replace(text, old, f'mk8_gamepad_axis({device}, {partner}, SDL_GAMEPAD_AXIS_{axis})')
    start = 'static void scan_gamepads(void) {'
    text = replace(text, start, (SOURCE / 'assign_gamepads.inc').read_text(encoding='utf-8') + '\n' + start)
    text = replace(text, '        for (int i = 0; i < n; i++) mk8_open_gamepad(ids[i]);', '''        static bool tried_layout;
        const char *layout = getenv("MK8_GAMEPAD_LAYOUT");
        if (!tried_layout && layout && *layout) {
            tried_layout = true;
            if (mk8_startup_layout(layout, ids, n)) { mk8_startup_preset = 1; SDL_free(ids); return; }
            mk8_startup_preset = -1;
            LOG("[input] invalid MK8_GAMEPAD_LAYOUT; using automatic assignment\\n");
            mk8_set_joycon_mode(g_settings.joycon_mode);
            SDL_free(ids);
            ids = SDL_GetGamepads(&n);
        }
        for (int i = 0; i < n; i++) mk8_open_gamepad(ids[i]);''')
    text = replace(text, 'void input_pads_reassign(void) {', '''void input_pads_reassign(void) {
    if (mk8_explicit_slots) { mk8_assign_gamepads(NULL); return; }
    for (unsigned p=0; p<=EXTRA_PADS; ++p) mk8_forget_seat(p);''')
    text = replace(text, '        mk8_open_gamepad(e->gdevice.which);',
                   '        if (!mk8_editor.rows) mk8_route_gamepad(e->gdevice.which);')
    marker = 'static void open_extra(SDL_JoystickID id) {'
    text = replace(text, marker, marker + '\n    for (int i=0; i<=EXTRA_PADS; ++i)\n'
                   '        if (mk8_partner[i] && SDL_GetGamepadID(mk8_partner[i]) == id) return;')
    text = replace(text, '        if (!extra_pad[i]) {\n            extra_pad[i] = mk8_rumble_open',
                   '        if (!extra_pad[i] && !(mk8_reserved_seats & (1u << (i+1))) && i+1 != mk8_assigned_keyboard()) {\n            extra_pad[i] = mk8_rumble_open')
    removed = text.index('    case SDL_EVENT_GAMEPAD_REMOVED:\n        if (pad')
    end = text.index('        break;\n', removed) + len('        break;\n')
    text = replace(text, text[removed:end], '    case SDL_EVENT_GAMEPAD_REMOVED:\n        mk8_remove_assigned(e->gdevice.which);\n        break;\n')
    text = replace(text, 'SDL_Gamepad *device = i ? extra_pad[i - 1] : pad;',
                   'SDL_Gamepad *device = mk8_partner[i] ? mk8_partner[i] : *mk8_primary_slot(i);')
    mark = '        memcpy(slot->pending[k], e->data, sizeof slot->pending[k]);'
    text = replace(text, mark, mark + '''
        if (mk8_partner[i]) {
            // SDL's single right Joy-Con rotates X/Z for sideways use. Restore its upright grip.
            slot->pending[k][0] = e->data[2];
            slot->pending[k][2] = -e->data[0];
        }''')
    # Keep discovery independently testable; use the same owner thread as SDL hotplug and rumble.
    marker = 'void input_handle_event(const SDL_Event *e) {'
    edit = (SOURCE / 'controller_edit.inc').read_text(encoding='utf-8')
    title = (SOURCE / 'title_input.inc').read_text(encoding='utf-8')
    tips = (SOURCE / 'controller_tips.inc').read_text(encoding='utf-8')
    marker_before_discovery = 'static void mk8_open_gamepad(SDL_JoystickID id) {'
    text = replace(text, marker_before_discovery, edit + '\n' + title + '\n' + tips + '\n' + marker_before_discovery)
    text = replace(text, marker, marker + '\n    mk8_edit_event(e);')
    shutil.copyfile(SOURCE.parent / 'support/controller_edit.h', runtime / 'controller_edit.h')
    shutil.copyfile(SOURCE.parent / 'support/controller_tips.h', runtime / 'controller_tips.h')
    # Route the existing key bindings and both sticks to the selected guest player.
    text = replace(text, 'key_buttons | pad_buttons | touch_buttons_dpad()',
                   'mk8_player_keys(0).buttons | pad_buttons | touch_buttons_dpad()', 2)
    marker = '    if (id >= 1 && id <= EXTRA_PADS) {  // players 2-4:'
    text = replace(text, marker, '    Mk8KeyState keys = mk8_player_keys((int)id);\n' + marker)
    text = replace(text, 'menu ? 0 : extra_buttons[i] |', 'menu ? 0 : keys.buttons | extra_buttons[i] | mk8_read_pause(id) |')
    for axis in ('lx', 'ly'):
        text = replace(text, f'clamp_axis(extra_{axis}[i] +', f'clamp_axis(keys.{axis} + extra_{axis}[i] +')
    for axis in ('rx', 'ry'):
        text = replace(text, f'menu ? 0 : extra_{axis}[i]', f'menu ? 0 : clamp_axis(keys.{axis} + extra_{axis}[i])')
    for axis in ('lx', 'ly', 'rx', 'ry'):
        text = replace(text, f'menu ? 0 : key_{axis} + pad_{axis}', f'menu ? 0 : keys.{axis} + pad_{axis}')
    text = replace(text, 'key_buttons | pad_buttons | touch_b)', 'keys.buttons | pad_buttons | touch_b | mk8_read_pause(0))')
    text = replace(text, 'key_buttons | pad_buttons | script_buttons;', 'keys.buttons | pad_buttons | script_buttons;')
    for indent in ('        ', '    '):
        marker = '\n' + indent + '*(uint64_t *)(s + 8) = b;'
        text = replace(text, marker, '\n' + indent + 'if (!menu && !mk8_settings_page_active()) mk8_title_hid(id, &b, &lx, &ly, &rx, &ry);' + marker)
    marker = 'RT_ON_RESET(input_reboot_reset) {'
    text = replace(text, marker, marker + ' mk8_title_input_active(false);')
    path.write_text(text, encoding='utf-8')
    device = runtime / 'nvn/vulkan/device.c'
    text = device.read_text(encoding='utf-8')
    marker = '        unsigned events = 0;'
    text = replace(text, marker, '        extern void mk8_title_pump(void);\n        mk8_title_pump();\n' + marker)
    marker = '        menu_apply_window_requests(window);'
    text = replace(text, marker, '        extern void mk8_edit_pump(void), mk8_tip_sync(void);\n'
                   '        mk8_edit_pump();\n        mk8_tip_sync();\n' + marker)
    marker = '            if (menu_handle_event(&e)) continue;'
    text = replace(text, marker, '            extern bool mk8_controller_layout_active(void), mk8_settings_page_active(void);\n'
                   '            extern bool mk8_title_event(const SDL_Event *, bool), menu_input_captured(void);\n'
                   '            if (mk8_title_event(&e,menu_input_captured() || mk8_settings_page_active() || mk8_controller_layout_active())) continue;\n'
                   '            extern void mk8_tip_event(const SDL_Event *, bool);\n'
                   '            mk8_tip_event(&e,mk8_settings_page_active() && !mk8_controller_layout_active());\n' + marker)
    marker = '    mk8_set_joycon_mode(g_settings.joycon_mode);'
    text = replace(text, marker, marker + '''
    const char *controller_layout = getenv("MK8_GAMEPAD_LAYOUT");
    extern bool mk8_title_manual_input(void), mk8_auto_lan_requested(void);
    if ((controller_layout && *controller_layout) || (mk8_title_manual_input() && !mk8_auto_lan_requested()))
        SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_HIDAPI_COMBINE_JOY_CONS, "0", SDL_HINT_OVERRIDE);''')
    device.write_text(text, encoding='utf-8')
