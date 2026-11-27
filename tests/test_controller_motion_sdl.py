"""Exercise the prepared SDL adapter with a virtual gamepad that emits real SDL sensor events."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'toolchain'))
from host import DEPS, prepare_host_sources
from native import DEFAULT_COMPILER, compiler_env


HARNESS = r'''
#include <SDL3/SDL.h>
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#define EXTRA_PADS 3
static SDL_Gamepad *pad, *extra_pad[EXTRA_PADS], *mk8_partner[1 + EXTRA_PADS];
static SDL_Gamepad **mk8_primary_slot(unsigned player) { return player ? &extra_pad[player - 1] : &pad; }
static bool blocked;
static bool input_user_blocked(const SDL_Event *e) { return blocked; }
static bool legacy, settings, overlay, replay;
bool menu_input_captured(void) { return legacy; }
bool mk8_settings_page_active(void) { return settings; }
bool game_overlay_captured(void) { return overlay; }
bool padscript_active(void) { return replay; }
static void mk8_rumble_sensor(const SDL_GamepadSensorEvent *e) { (void)e; }
#include "adapter.inc"

static bool SDLCALL sensors_enabled(void *unused, bool enabled) { return true; }
static SDL_JoystickID attach(bool motion) {
    SDL_VirtualJoystickSensorDesc sensors[] = {{SDL_SENSOR_ACCEL, 200}, {SDL_SENSOR_GYRO, 200}};
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type=SDL_JOYSTICK_TYPE_GAMEPAD; d.naxes=SDL_GAMEPAD_AXIS_COUNT; d.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
    d.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1; d.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;
    d.nsensors=motion ? 2 : 0; d.sensors=sensors; d.SetSensorsEnabled=sensors_enabled;
    d.name="MK8 motion regression";
    SDL_JoystickID id=SDL_AttachVirtualJoystick(&d); assert(id);
    return id;
}

static void sample(SDL_Gamepad *p, SDL_SensorType type, Uint64 stamp, float value) {
    float data[3]={value, -value, 0.5f*value};
    assert(SDL_SendJoystickVirtualSensorData(SDL_GetGamepadJoystick(p),type,stamp,data,3));
    // SDL defers virtual input until its next joystick update, exactly like gamepad events in the runtime.
    SDL_UpdateJoysticks();
    SDL_Event e;
    while(SDL_PollEvent(&e)) motion_event(&e);
}

static uint64_t generation, stamp;
static float accel[3], gyro[3];
static bool read_slot(unsigned n) { return mk8_motion_read(n,&generation,&stamp,accel,gyro); }
static void expect(unsigned n, uint64_t when, float a, float g) {
    assert(read_slot(n) && stamp==when);
    assert(accel[0]==a && accel[1]==-a && accel[2]==0.5f*a);
    assert(gyro[0]==g && gyro[1]==-g && gyro[2]==0.5f*g);
}

int main(void) {
    assert(SDL_Init(SDL_INIT_GAMEPAD));
    SDL_JoystickID id=attach(true), plain=attach(false);
    pad=SDL_OpenGamepad(id); extra_pad[0]=SDL_OpenGamepad(plain); assert(pad && extra_pad[0]);
    mk8_sync_sensors();
    assert(SDL_GamepadSensorEnabled(pad,SDL_SENSOR_ACCEL) && SDL_GamepadSensorEnabled(pad,SDL_SENSOR_GYRO));
    assert(!read_slot(0) && !read_slot(1) && !read_slot(4));
    sample(pad,SDL_SENSOR_GYRO,1000000,2); assert(!read_slot(0));
    sample(pad,SDL_SENSOR_ACCEL,1000000,3); expect(0,1000000,3,2);
    uint64_t first=generation;
    sample(pad,SDL_SENSOR_ACCEL,1000000,9); sample(pad,SDL_SENSOR_GYRO,999999,8);
    expect(0,1000000,3,2);
    sample(pad,SDL_SENSOR_ACCEL,2000000,4); sample(pad,SDL_SENSOR_GYRO,2000000,-5);
    expect(0,2000000,4,-5); assert(generation==first);
    bool *captures[]={&legacy,&settings,&overlay,&replay};
    for(unsigned i=0;i<SDL_arraysize(captures);++i) {
        *captures[i]=true; assert(!read_slot(0)); *captures[i]=false;
        expect(0,2000000,4,-5);
    }

    SDL_Event focus={0};
    focus.type=SDL_EVENT_WINDOW_FOCUS_LOST; motion_event(&focus);
    assert(!read_slot(0) && !SDL_GamepadSensorEnabled(pad,SDL_SENSOR_GYRO));
    focus.type=SDL_EVENT_WINDOW_FOCUS_GAINED; motion_event(&focus);
    assert(!read_slot(0) && SDL_GamepadSensorEnabled(pad,SDL_SENSOR_GYRO));
    sample(pad,SDL_SENSOR_ACCEL,100,1); sample(pad,SDL_SENSOR_GYRO,100,6);
    expect(0,100,1,6); assert(generation!=first);

    // Move the same open pad to the third player. A partial or stale pair must never leak to another player.
    extra_pad[1]=pad; pad=NULL; mk8_sync_sensors();
    assert(!read_slot(0) && !read_slot(2));
    sample(extra_pad[1],SDL_SENSOR_ACCEL,10000000,3);
    sample(extra_pad[1],SDL_SENSOR_GYRO,60000000,5); assert(!read_slot(2));
    sample(extra_pad[1],SDL_SENSOR_ACCEL,61000000,7); expect(2,60000000,7,5);
    SDL_Delay(260); assert(!read_slot(2)); uint64_t previous=generation;
    sample(extra_pad[1],SDL_SENSOR_GYRO,10,8); sample(extra_pad[1],SDL_SENSOR_ACCEL,10,1);
    expect(2,10,1,8); assert(generation!=previous);
    blocked=true; mk8_sync_sensors(); assert(!read_slot(2));
    SDL_CloseGamepad(extra_pad[1]); extra_pad[1]=NULL; assert(SDL_DetachVirtualJoystick(id)); mk8_sync_sensors();
    blocked=false; id=attach(true); pad=SDL_OpenGamepad(id); assert(pad); mk8_sync_sensors();
    assert(!read_slot(0)); sample(pad,SDL_SENSOR_GYRO,1,-1); sample(pad,SDL_SENSOR_ACCEL,1,2); expect(0,1,2,-1);
    const float steam_accel[3]={5,-5,2.5f}, steam_gyro[3]={4,-4,2};
    mk8_motion_steam_sample(id, 10, steam_accel, steam_gyro);
    expect(0,1,2,-1); // The normal sensor pair takes precedence.
    mk8_motion_steam_sample(plain, 10, steam_accel, steam_gyro);
    assert(read_slot(1)); expect(1,stamp,5,4);
    uint64_t before_reconnect=generation;
    mk8_motion_steam_lost(plain); assert(!read_slot(1));
    mk8_motion_steam_sample(plain, 11, steam_accel, steam_gyro);
    assert(read_slot(1) && generation!=before_reconnect);
    focus.type=SDL_EVENT_WINDOW_FOCUS_LOST; motion_event(&focus);
    mk8_motion_steam_sample(plain, 11, steam_accel, steam_gyro);
    assert(!read_slot(1));
    focus.type=SDL_EVENT_WINDOW_FOCUS_GAINED; motion_event(&focus);
    assert(!read_slot(1));
    mk8_motion_steam_sample(plain, 11, steam_accel, steam_gyro);
    assert(read_slot(1)); expect(1,stamp,5,4);
    SDL_CloseGamepad(pad); SDL_CloseGamepad(extra_pad[0]); pad=NULL; extra_pad[0]=NULL; mk8_sync_sensors();
    assert(!read_slot(0)); SDL_DetachVirtualJoystick(id); SDL_DetachVirtualJoystick(plain); SDL_Quit();
}
'''


@unittest.skipUnless(DEFAULT_COMPILER.is_file() and (DEPS / 'bin/SDL3.dll').is_file(), 'Native SDL toolchain required')
class ControllerMotionSDLTests(unittest.TestCase):
    def test_virtual_motion_pair_assignment_focus_and_reconnection(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            runtime = prepare_host_sources(ROOT / 'upstream/smm2recomp/recomp', root / 'host')
            source = (runtime / 'hle/input.c').read_text(encoding='utf-8')
            start = source.index('// SDL\'s event thread owns')
            adapter = source[start:source.index('static void note_family(void) {', start)]
            start = source.index('    if (e->type == SDL_EVENT_GAMEPAD_SENSOR_UPDATE)')
            focus = source.index('    if (e->type == SDL_EVENT_WINDOW_FOCUS_LOST', start)
            focus_end = source.index('\n    }', focus) + len('\n    }')
            adapter += 'static void motion_event(const SDL_Event *e) {\n' + source[start:focus_end] + '\n}\n'
            (root / 'adapter.inc').write_text(adapter, encoding='utf-8')
            (root / 'test.c').write_text(HARNESS, encoding='utf-8')
            exe = root / 'test.exe'
            build = subprocess.run([DEFAULT_COMPILER, 'cc', '-target', 'x86_64-windows-gnu', '-O1', '-UNDEBUG',
                                    '-I' + str(DEPS / 'include'), root / 'test.c', DEPS / 'lib/libSDL3.dll.a',
                                    '-o', exe], env=compiler_env(), capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            env = dict(os.environ, PATH=str(DEPS / 'bin') + os.pathsep + os.environ['PATH'])
            run = subprocess.run([exe], env=env, capture_output=True, text=True, timeout=20)
            self.assertEqual(run.returncode, 0, run.stderr)
