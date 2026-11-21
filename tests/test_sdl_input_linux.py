"""Exercise the prepared SDL startup and gamepad routing on a native Linux process."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'toolchain'))
from host import prepare_host_sources


@unittest.skipUnless(sys.platform.startswith('linux') and shutil.which('cc') and shutil.which('pkg-config'),
                     'Linux C compiler and pkg-config required')
class LinuxInputTests(unittest.TestCase):
    def test_native_startup_gamepad_events_and_init_failure(self):
        if subprocess.run(['pkg-config', '--exists', 'sdl3']).returncode:
            self.skipTest('SDL3 development package required')
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            runtime = prepare_host_sources(ROOT / 'upstream/smm2recomp/recomp', out / 'host')
            device = (runtime / 'nvn/vulkan/device.c').read_text(encoding='utf-8')
            inputs = (runtime / 'hle/input.c').read_text(encoding='utf-8')
            start = device.index('#ifdef _WIN32', device.index('mk8_set_joycon_mode(g_settings.joycon_mode);'))
            init = device[start:device.index('    int win_w', start)]
            def windows_block(function):
                pos = device.index(function)
                return device[device.rindex('#ifdef _WIN32', 0, pos):device.index('#endif', pos) + len('#endif')]
            real_pad = inputs.index('static bool real_gamepad(SDL_JoystickID id) {')
            source = out / 'input.c'
            source.write_text('''#include <SDL3/SDL.h>
#include <assert.h>
#define FATAL(...) return 17
''' + inputs[real_pad:inputs.index('\n}', real_pad) + 2] + '''
int main(int argc, char **argv) {
''' + init + '''
    assert(SDL_WasInit(SDL_INIT_GAMEPAD) && SDL_WasInit(SDL_INIT_VIDEO));
    SDL_VirtualJoystickDesc desc = {0};
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    assert(id && real_gamepad(id));
    SDL_Gamepad *pad = SDL_OpenGamepad(id);
    assert(pad);
    SDL_Joystick *stick = SDL_GetGamepadJoystick(pad);
    assert(SDL_SetJoystickVirtualButton(stick, SDL_GAMEPAD_BUTTON_SOUTH, true));
    SDL_UpdateJoysticks();
    SDL_Event e;
    bool saw_button = false;
    while (SDL_PollEvent(&e)) {
''' + windows_block('extern bool mk8_steam_gamepad_event(') + '''
        if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e.gbutton.which == id)
            saw_button = true;
    }
''' + windows_block('extern void mk8_poll_steam_gamepads(') + '''
    assert(saw_button && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH));
    SDL_CloseGamepad(pad);
    SDL_DetachVirtualJoystick(id);
    SDL_Quit();
    return 0;
}
''', encoding='utf-8')
            flags = subprocess.check_output(['pkg-config', '--cflags', '--libs', 'sdl3'], text=True)
            binary = out / 'input'
            subprocess.run(['cc', source, *shlex.split(flags), '-o', binary], check=True)
            for video, expected in [('dummy', 0), ('mk8_missing_video_driver', 17)]:
                with self.subTest(video=video):
                    result = subprocess.run([binary], env=dict(os.environ, SDL_VIDEO_DRIVER=video),
                                            capture_output=True, timeout=10)
                    self.assertEqual(result.returncode, expected, result.stderr)
