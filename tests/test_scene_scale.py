"""Exercise the prepared native texture-scaling decision with scene/resource fixtures."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from native import DEFAULT_COMPILER, compiler_env
import scene_scale


class SceneScaleTests(unittest.TestCase):
    def test_rejects_changed_boundary(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            path = root/'nvn/vulkan/textures.c'
            path.parent.mkdir(parents=True)
            path.write_text('changed')
            with self.assertRaises(ValueError):
                scene_scale.prepare(root)

    @unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
    def test_scene_buffers_scale_together_without_scaling_unrelated_resources(self):
        original = (ROOT/'upstream/smm2recomp/recomp/runtime/nvn/vulkan/textures.c').read_text(encoding='utf-8')
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            path = root/'nvn/vulkan/textures.c'
            path.parent.mkdir(parents=True)
            path.write_text(original, encoding='utf-8')
            scene_scale.prepare(root)
            harness = r'''
int main(int argc, char **argv) {
    (void)argv;
    NvnTexture scene={1,0,8,1920,1080,1,1,0x34};
    NvnTexture copy={1,0,0,1920,1080,1,1,0x05};
    NvnTexture light={4,0,8,1920,1080,2,1,0x3f};
    if(argc>1) return tex_scalable(&scene)||tex_scalable(&copy)||tex_scalable(&light);
    assert(tex_scalable(&scene));
    if(!tex_scalable(&copy)||!tex_scalable(&light)) return 1;
    NvnTexture t=light; t.target=8; assert(!tex_scalable(&t));
    t=light; t.depth=6; assert(!tex_scalable(&t));
    t=light; t.format=0x25; assert(!tex_scalable(&t));
    t=light; t.levels=2; assert(!tex_scalable(&t));
    t=light; t.samples=4; assert(!tex_scalable(&t));
    t=light; t.width=128; t.height=72; assert(!tex_scalable(&t));
    t=copy; t.format=0x25; assert(!tex_scalable(&t));
    t=copy; t.levels=2; assert(!tex_scalable(&t));
    t=copy; t.samples=4; assert(!tex_scalable(&t));
    t=copy; t.width=960; assert(!tex_scalable(&t));
    t=scene; t.width=960; t.height=1080; assert(!tex_scalable(&t));
    return 0;
}
'''
            for label, text, expected in [('before',original,1),('after',path.read_text(encoding='utf-8'),0)]:
                decision=text[text.index('static bool tex_scalable('):text.index('bool renderer_texture_first_write(')]
                source=root/(label+'.c')
                source.write_text('''
#undef NDEBUG
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
typedef struct { unsigned target,samples,flags,width,height,depth,levels,format; } NvnTexture;
'''+decision+harness, encoding='utf-8')
                exe=root/(label+'.exe')
                result=subprocess.run([DEFAULT_COMPILER,'cc','-target','x86_64-windows-gnu','-O1',source,'-o',exe],
                                      capture_output=True,text=True,env=compiler_env())
                self.assertEqual(result.returncode,0,result.stderr)
                env={k:v for k,v in os.environ.items() if k!='LARECOMP_NO_SCALE'}
                result=subprocess.run([exe],env=env,capture_output=True,text=True,timeout=20)
                self.assertEqual(result.returncode,expected,result.stderr)
                subprocess.run([exe,'disabled'],env=dict(env,LARECOMP_NO_SCALE='1'),check=True,timeout=20)


if __name__ == '__main__':
    unittest.main()
