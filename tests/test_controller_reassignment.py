"""Reassignment must keep SDL delivering releases from real controller handles."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from host import DEPS,prepare_host_sources
from native import DEFAULT_COMPILER, compiler_env


@unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
class ControllerReassignmentTests(unittest.TestCase):
    def test_settings_rescan_preserves_failed_device_and_duplicate_handling(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', root/'host')
            full = (runtime/'hle/input.c').read_text(encoding='utf-8')
            functions = []
            for declaration in ('static void mk8_open_gamepad(SDL_JoystickID id) {',
                                'static void scan_gamepads(void) {',
                                'void input_pads_reassign(void) {'):
                start = full.index(declaration)
                functions.append(full[start:full.index('\n}', start)+2])
            (root/'rescan.c').write_text(r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#define LOG(...) ((void)0)
#define EXTRA_PADS 3
typedef unsigned SDL_JoystickID;
typedef struct { unsigned id; } SDL_Gamepad;
static SDL_Gamepad devices[]={{0},{1},{2},{3}};
static SDL_Gamepad *pad, *extra_pad[EXTRA_PADS];
static bool scanned;
static bool mk8_explicit_slots;
static void mk8_forget_seat(unsigned player) { assert(player<=EXTRA_PADS); }
static int mk8_startup_preset;
static bool mk8_assign_gamepads(const SDL_JoystickID slots[4][2]) { abort(); }
static bool mk8_startup_layout(const char *layout,const SDL_JoystickID *ids,int n) { abort(); }
static unsigned opens,closes,enumerations;
static void mk8_reset_pause(unsigned player) { assert(player<=EXTRA_PADS); }
static bool real_gamepad(unsigned id) { return id!=0; }
static unsigned SDL_GetGamepadID(SDL_Gamepad *p) { return p->id; }
static SDL_Gamepad *SDL_OpenGamepad(unsigned id) { opens++;return id==1 ? NULL : &devices[id]; }
static void SDL_CloseGamepad(SDL_Gamepad *p) { assert(p==&devices[2] || p==&devices[3]);closes++; }
static SDL_Gamepad *mk8_rumble_open(int player, SDL_JoystickID id) {
    assert(player>=0 && player<=EXTRA_PADS);
    return SDL_OpenGamepad(id);
}
static void mk8_rumble_close(int player, SDL_Gamepad *p) {
    assert(player>=0 && player<=EXTRA_PADS);
    SDL_CloseGamepad(p);
}
static SDL_JoystickID *SDL_GetGamepads(int *count) {
    static SDL_JoystickID ids[]={0,1,2,3};*count=4;enumerations++;return ids;
}
static void SDL_free(void *p) { (void)p; }
static void open_extra(unsigned id) {
    for(unsigned i=0;i<EXTRA_PADS;i++) if(!extra_pad[i]) { extra_pad[i]=mk8_rumble_open(i+1,id);return; }
}
static void note_family(void) {}
static void recompute_pad(void) {}
static void recompute_extra(int i) { (void)i; }
static void style_changed(void) {}
''' + '\n'.join(functions) + r'''
int main(void) {
    scan_gamepads();
    assert(scanned && enumerations==1 && opens==3 && pad==&devices[2] && extra_pad[0]==&devices[3]);
    scan_gamepads();mk8_open_gamepad(2);mk8_open_gamepad(3);
    assert(enumerations==1 && opens==3);
    input_pads_reassign();
    assert(scanned && enumerations==2 && opens==6 && closes==2);
    assert(pad==&devices[2] && extra_pad[0]==&devices[3]);
    mk8_open_gamepad(2);mk8_open_gamepad(3);assert(opens==6);
    input_pads_reassign();
    assert(scanned && enumerations==3 && opens==9 && closes==4);
    assert(pad==&devices[2] && extra_pad[0]==&devices[3] && !extra_pad[1]);
    return 0;
}
''', encoding='utf-8')
            exe = root/'rescan.exe'
            result = subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu',
                                     '-O1', '-UNDEBUG', str(root/'rescan.c'), '-o', str(exe)],
                                    env=compiler_env(), capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr[-3000:])
            result = subprocess.run([str(exe)], env=dict(os.environ, MK8_GAMEPAD_LAYOUT=''),
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_reassigned_virtual_controller_keeps_button_and_axis_releases(self):
        deps = DEPS
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', root/'host')
            full = (runtime/'hle/input.c').read_text(encoding='utf-8')
            source = full[full.index('enum {'):full.index('// Forget held keys')]
            source += full[full.index('// Events a test pushes'):full.index('// Controller family')]
            start = full.index('static void mk8_open_gamepad(SDL_JoystickID id) {')
            discovery = full[start:full.index('\n}', start)+2]
            start = full.index('typedef struct {', full.index('// Vibration devices:'))
            lifecycle = full[start:full.index('static struct Mk8GuestPad *mk8_guest_rumble_pad(', start)]
            (root/'test.c').write_text(r'''
#include <pthread.h>
#include <SDL3/SDL.h>
#include <assert.h>
#include <math.h>
#include <stdatomic.h>
#include <string.h>
#include <stdlib.h>
#include "settings.h"
#include "input_bind.h"
#define LOG(...) ((void)0)
Settings g_settings;
int16_t g_bind_pad[BIND_PAD_COUNT], g_bind_key[BIND_KEY_COUNT];
bool padscript_active(void) { return false; }
bool mk8_settings_shared_race(void) { return false; }
''' + source + lifecycle + r'''
static void mk8_sync_sensors(void) {}
static void style_changed(void) {}
static void note_family(void) {}
static bool real_gamepad(SDL_JoystickID id) { return SDL_IsGamepad(id) && SDL_IsJoystickVirtual(id); }
''' + discovery + r'''
static bool SDLCALL rumble(void *userdata, Uint16 low, Uint16 high) {
    (void)userdata; (void)low; (void)high;
    return true;
}
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
    d.Rumble=rumble;
    SDL_JoystickID id=SDL_AttachVirtualJoystick(&d); assert(id);
    SDL_Joystick *j=SDL_OpenJoystick(id); assert(j);
    scan_gamepads(); assert(SDL_GetGamepadFromID(id));
    assert(mk8_rumble_pads[0].id==id);
    for(int i=0;i<4;i++) {
        int b=i&1 ? SDL_GAMEPAD_BUTTON_DPAD_RIGHT : SDL_GAMEPAD_BUTTON_EAST;
        assert(SDL_SetJoystickVirtualButton(j,b,true));
        assert(SDL_SetJoystickVirtualAxis(j,SDL_GAMEPAD_AXIS_LEFTY,30000)); SDL_UpdateJoysticks();
        g_settings.pads_from_p2=!(i&1); input_pads_reassign();
        assert(scanned);
        assert(SDL_GetGamepadFromID(id));
        assert(pad && !extra_pad[0]);
        assert(mk8_rumble_pads[0].id==id && !mk8_rumble_pads[1].id);
        released(j,b);
    }
    SDL_CloseJoystick(j); SDL_DetachVirtualJoystick(id); SDL_Quit();
}
''', encoding='utf-8')
            exe = root/'test.exe'
            result = subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu', '-O2', '-UNDEBUG',
                '-I'+str(runtime), '-I'+str(deps/'include'), str(root/'test.c'),
                str(ROOT/'runtime/mk8/support/joycon_modes.c'),
                str(deps/'lib/libSDL3.dll.a'), '-o', str(exe)], env=compiler_env(), capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr[-3000:])
            env = dict(os.environ, PATH=str(deps/'bin')+os.pathsep+os.environ.get('PATH',''))
            for blocked in ('0','1'):
                result = subprocess.run([str(exe)], env=dict(env,LARECOMP_NO_USER_INPUT=blocked), capture_output=True, text=True, timeout=15)
                self.assertEqual(result.returncode, 0, result.stderr[-3000:])


if __name__ == '__main__':
    unittest.main()
