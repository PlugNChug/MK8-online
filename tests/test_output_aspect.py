"""Run the prepared output-scale calculation for wide, tall and 16:9 windows."""
import subprocess
import tempfile
import unittest
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from host import prepare_host_sources
from native import DEFAULT_COMPILER, compiler_env


@unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
class OutputAspectTests(unittest.TestCase):
    def test_fill_and_fit_preserve_camera_and_internal_resolution(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', root/'host')
            source = (runtime/'nvn/output_scale.c').read_text(encoding='utf-8')
            source = source.replace('#include "../hle.h"', '#define LOG(...) ((void)0)')
            source = source.replace('#include "../settings.h"', '#include "settings.h"\nSettings g_settings;')
            source = source.replace('#include "output_scale.h"', '#include "nvn/output_scale.h"')
            source += r'''
#include <assert.h>
double mk8_ultrawide_present_mode(void) { return 0; }
int main(void) {
    double sizes[][2]={{3440,1440},{1280,1024},{1920,1080}};
    for(int i=0;i<3;i++) {
        double w=sizes[i][0],h=sizes[i][1]; OutScale fit,fill;
        g_settings.aspect=ASPECT_ORIGINAL;
        output_scale(w,h,false,0,-1,&fit);
        g_settings.aspect=ASPECT_UNLOCKED;
        output_scale(w,h,false,0,-1,&fill);
        assert(fill.vw==w && fill.vh==h && fill.vx==0 && fill.vy==0);
        assert(fabs(fit.vw/fit.vh-16.0/9.0)<0.0001);
        assert(fill.sx==fit.sx && fill.sy==fit.sy && fill.shape==1);
    }
    return 0;
}
'''
            c, exe = root/'aspect.c', root/'aspect.exe'
            c.write_text(source, encoding='utf-8')
            built = subprocess.run([str(DEFAULT_COMPILER),'cc','-target','x86_64-windows-gnu','-O1','-UNDEBUG',
                '-I'+str(runtime),str(c),'-o',str(exe)],env=compiler_env(),capture_output=True,text=True)
            self.assertEqual(built.returncode,0,built.stderr)
            ran = subprocess.run([str(exe)],capture_output=True,text=True,timeout=15)
            self.assertEqual(ran.returncode,0,ran.stdout+ran.stderr)
