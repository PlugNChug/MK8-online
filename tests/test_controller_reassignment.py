"""Reassignment must keep SDL delivering releases from real controller handles."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from host import prepare_host_sources
from native import DEFAULT_COMPILER, compiler_env


@unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
class ControllerReassignmentTests(unittest.TestCase):
    def test_reassigned_virtual_controller_keeps_button_and_axis_releases(self):
        deps = ROOT/'local/native-tools/deps/packages/ucrt64'
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', root/'host')
            full = (runtime/'hle/input.c').read_text(encoding='utf-8')
            source = full[full.index('enum {'):full.index('// Forget held keys')]
            source += full[full.index('// Events a test pushes'):full.index('// Controller family')]
            (root/'test.c').write_text(r'''
#include <SDL3/SDL.h>
#include <assert.h>
#include <stdatomic.h>
#include <string.h>
#include <stdlib.h>
#include "settings.h"
#include "input_bind.h"
#define LOG(...) ((void)0)
Settings g_settings;
int16_t g_bind_pad[BIND_PAD_COUNT], g_bind_key[BIND_KEY_COUNT];
bool padscript_active(void) { return false; }
''' + source + r'''
static void style_changed(void) {}
static void note_family(void) {}
static bool real_gamepad(SDL_JoystickID id) { return SDL_IsGamepad(id); }
static void released(SDL_Joystick *j, int button) {
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    assert(SDL_SetJoystickVirtualButton(j,button,false));
    assert(SDL_SetJoystickVirtualAxis(j,SDL_GAMEPAD_AXIS_LEFTY,0));
    SDL_UpdateJoysticks();
    SDL_Event e; bool up=false, neutral=false;
    while(SDL_PollEvent(&e)) {
        if(e.type==SDL_EVENT_GAMEPAD_BUTTON_UP && e.gbutton.button==button) up=true;
        if(e.type==SDL_EVENT_GAMEPAD_AXIS_MOTION && e.gaxis.axis==SDL_GAMEPAD_AXIS_LEFTY && !e.gaxis.value) neutral=true;
    }
    assert(up && neutral);
}
int main(void) {
    bool blocked=atoi(getenv("LARECOMP_NO_USER_INPUT"))!=0;
    assert(input_user_blocked(NULL)==blocked);
    SDL_Event e={0};e.type=SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    assert(input_user_blocked(&e)==blocked);
    e.gbutton.which=TEST_DEVICE;assert(!input_user_blocked(&e));
    assert(SDL_Init(SDL_INIT_GAMEPAD));
    SDL_VirtualJoystickDesc d; SDL_INIT_INTERFACE(&d);
    d.type=SDL_JOYSTICK_TYPE_GAMEPAD; d.naxes=SDL_GAMEPAD_AXIS_COUNT; d.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
    d.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1; d.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;
    d.name="Settings reassignment test";
    SDL_JoystickID id=SDL_AttachVirtualJoystick(&d); assert(id);
    SDL_Joystick *j=SDL_OpenJoystick(id); assert(j);
    scan_gamepads(); assert(SDL_GetGamepadFromID(id));
    for(int i=0;i<4;i++) {
        int b=i&1 ? SDL_GAMEPAD_BUTTON_DPAD_RIGHT : SDL_GAMEPAD_BUTTON_EAST;
        assert(SDL_SetJoystickVirtualButton(j,b,true));
        assert(SDL_SetJoystickVirtualAxis(j,SDL_GAMEPAD_AXIS_LEFTY,30000)); SDL_UpdateJoysticks();
        g_settings.pads_from_p2=!(i&1); input_pads_reassign();
        assert(scanned);
        assert(SDL_GetGamepadFromID(id));
        assert(g_settings.pads_from_p2 ? !pad && extra_pad[0] : pad!=NULL);
        released(j,b);
    }
    SDL_CloseJoystick(j); SDL_DetachVirtualJoystick(id); SDL_Quit();
}
''', encoding='utf-8')
            exe = root/'test.exe'
            result = subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu', '-O2', '-UNDEBUG',
                '-I'+str(runtime), '-I'+str(deps/'include'), str(root/'test.c'),
                str(deps/'lib/libSDL3.dll.a'), '-o', str(exe)], env=compiler_env(), capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr[-3000:])
            env = dict(os.environ, PATH=str(deps/'bin')+os.pathsep+os.environ.get('PATH',''))
            for blocked in ('0','1'):
                result = subprocess.run([str(exe)], env=dict(env,LARECOMP_NO_USER_INPUT=blocked), capture_output=True, text=True, timeout=15)
                self.assertEqual(result.returncode, 0, result.stderr[-3000:])


if __name__ == '__main__':
    unittest.main()
