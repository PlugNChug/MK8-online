"""Feed real SDL sensor events from the runtime's existing player assignments into HID."""
from pathlib import Path


def prepare(runtime):
    path = runtime / 'hle/input.c'
    text = path.read_text(encoding='utf-8')
    sensors = (Path(__file__).resolve().parents[1] / 'runtime/mk8/services/controller_sensors.inc').read_text(encoding='utf-8')
    family = 'static void note_family(void) {'
    style = 'static void style_changed(void) {'
    event = 'void input_handle_event(const SDL_Event *e) {'
    block = '    case SDL_EVENT_GAMEPAD_ADDED: case SDL_EVENT_GAMEPAD_AXIS_MOTION:'
    for old, new in (
        (family, sensors + '\n' + family + '\n    mk8_sync_sensors();'),
        (style, style + '\n    mk8_sync_sensors();'),
        (block, '    case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:\n' + block),
        (event, event + '''
    if (e->type == SDL_EVENT_GAMEPAD_SENSOR_UPDATE) { mk8_sensor_event(&e->gsensor); return; }
    if (e->type == SDL_EVENT_WINDOW_FOCUS_LOST || e->type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
        mk8_sensor_focused = e->type == SDL_EVENT_WINDOW_FOCUS_GAINED;
        mk8_sync_sensors();
    }'''),
    ):
        if text.count(old) != 1:
            raise ValueError(f'Controller sensor boundary changed: {old}')
        text = text.replace(old, new)
    path.write_text(text, encoding='utf-8')
