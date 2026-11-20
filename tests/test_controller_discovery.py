"""Native execution of the prepared SDL slot assignment with failed and duplicate devices."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'toolchain'))
from host import prepare_host_sources
from native import DEFAULT_COMPILER, compiler_env

HARNESS = r'''
#include <stdbool.h>
#include <assert.h>
#include <stddef.h>
#define LOG(...) ((void)0)
#define EXTRA_PADS 3
typedef unsigned SDL_JoystickID;
typedef struct { unsigned id; } SDL_Gamepad;
static SDL_Gamepad devices[]={{0},{1},{2},{3},{4}};
static SDL_Gamepad *pad, *extra_pad[EXTRA_PADS];
static bool p2;
static unsigned opens, refreshes, styles;
static bool real_gamepad(unsigned id) { return id != 0; }
static unsigned SDL_GetGamepadID(SDL_Gamepad *p) { return p->id; }
static SDL_Gamepad *SDL_OpenGamepad(unsigned id) { opens++; return id==1 ? NULL : &devices[id]; }
static SDL_Gamepad *mk8_rumble_open(int player, unsigned id) { assert(player==0); return SDL_OpenGamepad(id); }
static bool pads_from_p2(void) { return p2; }
static void open_extra(unsigned id) {
    for (unsigned i=0;i<EXTRA_PADS;i++) if (!extra_pad[i]) { extra_pad[i]=SDL_OpenGamepad(id); return; }
}
static void note_family(void) {}
static void recompute_pad(void) { refreshes++; }
static void style_changed(void) { styles++; }
#include "discovery.inc"
int main(void) {
    for (unsigned id=0;id<=3;id++) mk8_open_gamepad(id);
    assert(pad==&devices[2] && extra_pad[0]==&devices[3]);
    assert(opens==3 && refreshes==1 && styles==1);
    mk8_open_gamepad(2); mk8_open_gamepad(3);
    assert(opens==3); // startup scan plus ADDED must not open a second handle
    pad=NULL; // player one disconnects: never steal player two on a duplicate event
    mk8_open_gamepad(3); assert(!pad && opens==3);
    mk8_open_gamepad(4); assert(pad==&devices[4]);
    pad=NULL; extra_pad[0]=NULL; p2=true;
    mk8_open_gamepad(2); assert(!pad && extra_pad[0]==&devices[2]);
    return 0;
}
'''


class ControllerDiscoveryTests(unittest.TestCase):
    @unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
    def test_failed_first_device_duplicate_events_and_player_two_mode(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            runtime = prepare_host_sources(ROOT / 'upstream/smm2recomp/recomp', out / 'host')
            text = (runtime / 'hle/input.c').read_text(encoding='utf-8')
            start = text.index('static void mk8_open_gamepad(SDL_JoystickID id) {')
            end = text.index('void input_handle_event(', start)
            (out / 'discovery.inc').write_text(text[start:end], encoding='utf-8')
            (out / 'test.c').write_text(HARNESS, encoding='utf-8')
            exe = out / 'test.exe'
            result = subprocess.run([DEFAULT_COMPILER, 'cc', '-target', 'x86_64-windows-gnu',
                                     '-O1', '-UNDEBUG', out / 'test.c', '-o', exe],
                                    capture_output=True, text=True, env=compiler_env())
            self.assertEqual(result.returncode, 0, result.stderr)
            run = subprocess.run([exe], capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stderr)


if __name__ == '__main__':
    unittest.main()
