"""Five virtual SDL devices driving four logical players in the prepared runtime."""
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


@unittest.skipUnless(DEFAULT_COMPILER.is_file() and (DEPS / 'bin/SDL3.dll').is_file(), 'Native SDL toolchain required')
class MixedGamepadTests(unittest.TestCase):
    def test_four_players_including_paired_and_horizontal_joycons(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            runtime = prepare_host_sources(ROOT / 'upstream/smm2recomp/recomp', out / 'host')
            source = (runtime / 'hle/input.c').read_text(encoding='utf-8')
            bindings = (runtime / 'hle/bindings.c').read_text(encoding='utf-8')
            defaults = bindings[bindings.index('#define DEF_PAD'):bindings.index('static const char *const k_ini')]
            defaults += bindings[bindings.index('const char *bind_pad_label('):bindings.index('int bind_label_family(')]
            # Keep real discovery, event dispatch, sensors and HID vibration. The fixture supplies the surrounding UI.
            selection = source[source.index('enum {'):source.index('static _Atomic uint64_t script_buttons;')]
            style = source[source.index('static void style_changed(void) {'):source.index('pthread_mutex_lock(&g_klock);', source.index('static void style_changed(void) {'))]
            style += '++style_notifications; }\n'  # Stand in for the guest kernel's bound style event.
            vibration = source[source.index('// Vibration devices:'):source.index('HLE_FUNC("_ZN2nn3hid25InitializeVibrationDevice')]
            (out / 'mixed.inc').write_text(defaults + selection + style + vibration, encoding='utf-8')
            (out / 'guest_pad.inc').write_text(source[source.index('extern void touchpad_state('):source.index('#define FILL1')], encoding='utf-8')
            # Feed each queued SDL event through the same per-device chord tracker as the menu.
            menu=(runtime / 'ui/menu.mm').read_text(encoding='utf-8')
            start=menu.index('static bool mk8_settings_chord(')
            chord=menu[start:menu.index('\n}',start)+2]
            (out / 'chord.cpp').write_text('''#include <SDL3/SDL.h>
#include <map>
extern "C" bool mk8_settings_shared_race(void);
extern "C" int mk8_pause_owner(SDL_JoystickID id);
extern "C" void mk8_release_pause_button(SDL_JoystickID,unsigned);
''' + chord + '\nextern "C" bool settings_test_chord(const SDL_Event *e,bool captured) { return mk8_settings_chord(e,captured); }', encoding='utf-8')
            bridge=out / 'chord.o'
            build=subprocess.run([DEFAULT_COMPILER,'c++','-target','x86_64-windows-gnu','-O1','-I'+str(DEPS/'include'),
                                  '-c',out/'chord.cpp','-o',bridge],env=compiler_env(),capture_output=True,text=True,timeout=90)
            self.assertEqual(build.returncode,0,build.stderr)
            exe = out / 'mixed.exe'
            build = subprocess.run([DEFAULT_COMPILER, 'cc', '-target', 'x86_64-windows-gnu', '-O1', '-UNDEBUG',
                                    '-I' + str(out), '-I' + str(runtime), '-I' + str(DEPS / 'include'),
                                    '-DMK8_TEST_TITLE_INPUT', '-I' + str(ROOT / 'runtime/mk8/support'),
                                    ROOT / 'tests/steam_gamepads_harness.c', ROOT / 'tests/fixtures/mixed_gamepads.c', ROOT / 'runtime/mk8/support/joycon_modes.c',
                                    bridge, DEPS / 'lib/libSDL3.dll.a', '-lc++', '-o', exe],
                                   env=compiler_env(), capture_output=True, text=True, timeout=90)
            self.assertEqual(build.returncode, 0, build.stderr)
            env = dict(os.environ, PATH=str(DEPS / 'bin') + os.pathsep + os.environ['PATH'], LARECOMP_NO_USER_INPUT='0')
            env.pop('RECOMP_PAD_SCRIPT',None)
            for driver in ('JOYSTICK_HIDAPI', 'JOYSTICK_RAWINPUT', 'JOYSTICK_DIRECTINPUT', 'XINPUT_ENABLED',
                           'JOYSTICK_WGI', 'WINDOWS_GAMEINPUT', 'JOYSTICK_GAMEINPUT', 'JOYSTICK_GAMEINPUT_RAW'):
                env['SDL_' + driver] = '0'
            for order, layout, expected in (
                ('forward', None, 'title'),
                ('forward', None, 'title-hotplug'),
                ('forward', None, 'steam-title'),
                ('forward', None, 'all-reconnect'),
                ('forward', None, 'all-reconnect-stock'),
                ('forward', None, 'all-reconnect-edited'),
                ('forward', None, 'all-reconnect-identical'),
                ('forward', None, 'all-reconnect-one-serial'),
                ('forward', None, 'hotplug-open-failure'),
                ('forward', None, 'title-reconnect'),
                ('early-add', None, 'right-reconnect'),
                ('late-serial', None, 'right-reconnect'),
                ('forward', None, 'title-identical'),
                ('forward', None, 'title-manual'),
                ('forward', None, 'title-blocked'),
                ('forward', None, 'title-script'),
                ('forward', '1,2++3,4,5', 'title'),
                ('forward', None, 'editor'),
                ('forward', '1,2+3,4,5', 'mixed'),
                ('reverse', '5,4+3,2,1', 'mixed'),
                ('forward', '1,2+3,3,5', 'automatic'),
                ('forward', '1,2+3,4,99', 'automatic'),
                ('forward', '1,2++3,4,5', 'automatic'),
            ):
                with self.subTest(order=order, layout=layout, run=expected):
                    scenario = env.copy()
                    if expected=='steam-title':
                        scenario['SDL_VIDEO_DRIVER']='dummy'
                        scenario.pop('SteamAppId',None)
                        scenario.pop('SteamGameId',None)
                    if expected=='title-blocked':
                        scenario['LARECOMP_NO_USER_INPUT']='1'
                    if expected=='title-script':
                        scenario['RECOMP_PAD_SCRIPT']=str(out/'title.pad')
                    if layout is None:
                        scenario.pop('MK8_GAMEPAD_LAYOUT', None)
                    else:
                        scenario['MK8_GAMEPAD_LAYOUT'] = layout
                    run = subprocess.run([exe, order, expected], env=scenario,
                                         capture_output=True, text=True, timeout=20)
                    self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
