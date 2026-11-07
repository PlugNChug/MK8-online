import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from native import DEFAULT_COMPILER


@unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
class FrameCaptureTests(unittest.TestCase):
    def test_capture_is_opt_in_bounded_and_preserves_pending_request(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)
            code=root/'capture.c'
            code.write_text('''#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
char g_window_shot_request[256];
#include "graphics/frame_capture.inc"
int main(void) {
 _putenv_s("MK8_CAPTURE_DIR", "");mk8_capture_frame(60);assert(!*g_window_shot_request);
 _putenv_s("MK8_CAPTURE_DIR", "C:/local-test");
 mk8_capture_frame(0);mk8_capture_frame(59);mk8_capture_frame(660);assert(!*g_window_shot_request);
 mk8_capture_frame(60);assert(!strcmp(g_window_shot_request,"C:/local-test/mk8-frame-60.png"));
 mk8_capture_frame(120);assert(strstr(g_window_shot_request,"frame-60.png"));
 g_window_shot_request[0]=0;mk8_capture_frame(600);assert(strstr(g_window_shot_request,"frame-600.png"));
 char large[300];memset(large,'x',299);large[299]=0;_putenv_s("MK8_CAPTURE_DIR",large);
 g_window_shot_request[0]=0;mk8_capture_frame(60);assert(!*g_window_shot_request);
 return 0;
}
''')
            exe=root/'capture.exe'
            env=dict(os.environ,ZIG_GLOBAL_CACHE_DIR=str(ROOT/'local/native-tools/cache'))
            subprocess.run([str(DEFAULT_COMPILER),'cc','-target','x86_64-windows-gnu','-O2', '-UNDEBUG','-I'+str(ROOT/'runtime/mk8'),str(code),'-o',str(exe)],check=True,env=env)
            subprocess.run([str(exe)],check=True)
