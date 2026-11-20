"""Hit-test SDL mouse coordinates against the game image inside a high-DPI window."""
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
class ControllerPointerTests(unittest.TestCase):
    def test_scaled_window_and_letterbox(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            runtime = prepare_host_sources(ROOT / 'upstream/smm2recomp/recomp', out / 'host')
            touch = (runtime / 'hle/touch.c').read_text(encoding='utf-8')
            mapping = touch[touch.index('static pthread_mutex_t lock'):touch.index('// A two-finger tap')]
            adapter = touch[touch.index('// The presenter\'s drawable rectangle'):]
            src = out / 'pointer.c'
            src.write_text('#include <SDL3/SDL.h>\n#include <pthread.h>\n#include <assert.h>\n' + mapping + adapter + r'''
static int x, y, action;
bool mk8_controller_layout_active(void) { return true; }
bool mk8_controller_pointer(int gx, int gy, int kind) { x=gx; y=gy; action=kind; return true; }
int main(void) {
    assert(SDL_Init(SDL_INIT_VIDEO));
    SDL_Window *win=SDL_CreateWindow("pointer test",1000,400,SDL_WINDOW_HIDDEN);
    assert(win);
    SDL_Event e={.type=SDL_EVENT_MOUSE_MOTION};
    e.motion.windowID=SDL_GetWindowID(win);
    input_set_game_rect(360,40,1280,720,2000,800);
    e.motion.x=400; e.motion.y=323;
    assert(mk8_controller_pointer_event(&e) && x==440 && y==606 && action==0);
    e.motion.x=600;
    assert(mk8_controller_pointer_event(&e) && x==840 && y==606);
    e.motion.x=100;
    assert(mk8_controller_pointer_event(&e) && x==-1 && y==-1);
    e.type=SDL_EVENT_WINDOW_FOCUS_LOST;
    assert(!mk8_controller_pointer_event(&e) && action==3);
    e.type=SDL_EVENT_WINDOW_FOCUS_GAINED;
    assert(!mk8_controller_pointer_event(&e) && action==4);
    SDL_DestroyWindow(win);
    SDL_Quit();
}
''', encoding='utf-8')
            exe = out / 'pointer.exe'
            subprocess.run([DEFAULT_COMPILER, 'cc', '-target', 'x86_64-windows-gnu', '-O1', '-UNDEBUG',
                            '-I' + str(DEPS / 'include'), src, DEPS / 'lib/libSDL3.dll.a', '-o', exe],
                           env=compiler_env(), check=True, timeout=90)
            env = dict(os.environ, SDL_VIDEO_DRIVER='dummy', PATH=str(DEPS / 'bin') + os.pathsep + os.environ['PATH'])
            subprocess.run([exe], env=env, check=True, timeout=20)
