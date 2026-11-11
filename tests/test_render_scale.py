"""Alvos de cor em array devem acompanhar a escala do depth, sem dados comerciais."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from host import DEPS, prepare_host_sources
from native import DEFAULT_COMPILER
import render_scale


@unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
class RenderScaleTests(unittest.TestCase):
    def test_screen_array_and_linear_depth_follow_2d_depth_scaling(self):
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', output)
            text = (runtime/'nvn/vulkan/textures.c').read_text(encoding='utf-8')
            function = text[text.index('static bool tex_scalable('):text.index('\nbool renderer_texture_first_write(')]
            source = output/'scale.c'
            source.write_text('''#include <assert.h>
#include <stdlib.h>
#include "nvn/nvn.h"
''' + function + r'''
int main(void) {
    _putenv("LARECOMP_NO_SCALE=");
    NvnTexture depth = {.target=1, .samples=1, .flags=8, .width=1920, .height=1080};
    NvnTexture color = depth;
    color.target = 4;
    assert(tex_scalable(&depth));
    if (!tex_scalable(&color)) return 1;
    color.depth = 2;
    assert(tex_scalable(&color));
    for (unsigned target=0; target<=10; target++) {
        color.target=target;
        assert(tex_scalable(&color) == (target==1 || target==4));
    }
    color.target=4; color.samples=4; assert(!tex_scalable(&color));
    color.samples=1; color.flags=0; assert(!tex_scalable(&color));
    color.flags=8; color.width=32; color.height=16; assert(!tex_scalable(&color));
    color.width=16; color.height=9; assert(!tex_scalable(&color));
    color.width=1024; color.height=1024; assert(!tex_scalable(&color));
    color.width=1920; color.height=1080;
    NvnTexture linear_depth = depth;
    linear_depth.format=0x05; linear_depth.flags=0;
    if (!tex_scalable(&linear_depth)) return 2;
    linear_depth.width=960; linear_depth.height=540;
    assert(tex_scalable(&linear_depth));
    linear_depth.flags=0x10; assert(!tex_scalable(&linear_depth));
    linear_depth.flags=2; assert(!tex_scalable(&linear_depth));
    linear_depth.flags=0;
    linear_depth.target=4; assert(!tex_scalable(&linear_depth));
    linear_depth.target=1; linear_depth.samples=4; assert(!tex_scalable(&linear_depth));
    linear_depth.samples=0; linear_depth.width=1024; linear_depth.height=1024;
    assert(!tex_scalable(&linear_depth));
    linear_depth.width=1920; linear_depth.height=1080;
    _putenv("LARECOMP_NO_SCALE=1");
    assert(!tex_scalable(&depth) && !tex_scalable(&color) && !tex_scalable(&linear_depth));
    return 0;
}
''', encoding='utf-8')
            exe = output/'scale.exe'
            result = subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu',
                                     '-O1', '-UNDEBUG', '-I'+str(runtime), '-I'+str(DEPS/'include'),
                                     str(source), '-o', str(exe)], capture_output=True, text=True, env=os.environ)
            self.assertEqual(result.returncode, 0, result.stderr[-3000:])
            subprocess.run([str(exe)], check=True, timeout=20)

    def test_changed_or_duplicate_eligibility_is_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            runtime = Path(folder)
            path = runtime/'nvn/vulkan/textures.c'
            path.parent.mkdir(parents=True)
            original = (ROOT/'upstream/smm2recomp/recomp/runtime/nvn/vulkan/textures.c').read_text(encoding='utf-8')
            line = next(line for line in original.splitlines() if 'if (t->target != 1 || t->samples > 1' in line)
            for text in ('changed upstream', (line+'\n')*2):
                path.write_text(text, encoding='utf-8')
                with self.assertRaisesRegex(ValueError, 'boundary changed'):
                    render_scale.prepare(runtime)
                self.assertEqual(path.read_text(encoding='utf-8'), text)


if __name__ == '__main__':
    unittest.main()
