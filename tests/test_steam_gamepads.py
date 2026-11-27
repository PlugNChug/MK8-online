"""Exercise the GameInput fallback through real SDL devices and mapped input."""
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


@unittest.skipUnless(DEFAULT_COMPILER.is_file() and (DEPS / 'bin/SDL3.dll').is_file(), 'Native SDL required')
class SteamGamepadTests(unittest.TestCase):
    def test_reader_recovers_a_silent_sensor_stream(self):
        with tempfile.TemporaryDirectory() as folder:
            exe = Path(folder) / 'reader-test.exe'
            subprocess.run([DEFAULT_COMPILER, 'cc', '-target', 'x86_64-windows-gnu', '-O1', '-UNDEBUG',
                            '-I' + str(DEPS / 'include'), '-I' + str(ROOT / 'runtime/mk8/support'),
                            ROOT / 'tests/steam_motion_reader_harness.c', DEPS / 'lib/libSDL3.dll.a',
                            '-lole32', '-loleaut32', '-luuid', '-o', exe],
                           env=compiler_env(), check=True)
            env = dict(os.environ, PATH=str(DEPS / 'bin') + os.pathsep + os.environ['PATH'],
                       SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS='1')
            run = subprocess.run([exe], env=env, capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stderr)

    def test_two_controllers_hotplug_and_native_precedence(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            runtime = prepare_host_sources(ROOT / 'upstream/smm2recomp/recomp', out / 'host')
            source = (runtime / 'hle/input.c').read_text(encoding='utf-8')
            start = source.index('static void mk8_open_gamepad(SDL_JoystickID id) {')
            end = source.index('void input_handle_event(', start)
            (out / 'slots.inc').write_text(source[start:end], encoding='utf-8')
            exe = out / 'test.exe'
            subprocess.run([DEFAULT_COMPILER, 'cc', '-target', 'x86_64-windows-gnu', '-O1', '-UNDEBUG',
                            '-I' + str(out), '-I' + str(DEPS / 'include'), '-I' + str(ROOT / 'runtime/mk8/support'),
                            ROOT / 'tests/steam_gamepads_harness.c', DEPS / 'lib/libSDL3.dll.a', '-o', exe],
                           env=compiler_env(), check=True)
            env = dict(os.environ, PATH=str(DEPS / 'bin') + os.pathsep + os.environ['PATH'],
                       SDL_VIDEO_DRIVER='dummy', SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS='1')
            env.pop('SteamAppId', None)
            env.pop('SteamGameId', None)
            run = subprocess.run([exe], env=env, capture_output=True, text=True, timeout=25)
            self.assertEqual(run.returncode, 0, run.stderr)
