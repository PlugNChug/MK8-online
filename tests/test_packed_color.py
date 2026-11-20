"""Execute the prepared NVN format mapping against known packed RGB565 colours."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'toolchain'))
from native import DEFAULT_COMPILER, TOOLS, compiler_env
import packed_color


class PackedColorTests(unittest.TestCase):
    def test_rejects_changed_mapping(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root/'nvn/vulkan').mkdir(parents=True)
            (root/'nvn/vulkan/textures.c').write_text('changed')
            with self.assertRaises(ValueError):
                packed_color.prepare(root)

    @unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
    def test_red_green_blue_and_gold_survive_the_vulkan_format_mapping(self):
        original = (ROOT/'upstream/smm2recomp/recomp/runtime/nvn/vulkan/textures.c').read_text(encoding='utf-8')
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            path = root/'nvn/vulkan/textures.c'
            path.parent.mkdir(parents=True)
            path.write_text(original, encoding='utf-8')
            packed_color.prepare(root)
            prepared = path.read_text(encoding='utf-8')
            preamble = '''
#undef NDEBUG
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>
typedef struct { VkFormat vf; uint8_t expand_rgb, alpha_one; } VkFmt;
static VkFormat depth24_stencil8(void) { abort(); }
'''
            harness = '''
int main(void) {
    uint16_t pixels[]={0x001f,0x07e0,0xf800,0x05bf}; // red, green, blue, gold; red in low bits
    unsigned expected[][3]={{31,0,0},{0,63,0},{0,0,31},{31,45,0}};
    VkFormat format=fmt_of(0x3c).vf;
    assert(!fmt_of(0x3c).expand_rgb && !fmt_of(0x3c).alpha_one);
    for (unsigned i=0;i<4;i++) {
        unsigned lo=pixels[i]&31, g=(pixels[i]>>5)&63, hi=(pixels[i]>>11)&31;
        // Vulkan packed formats name components from the most significant bits.
        unsigned r,b;
        if(format==VK_FORMAT_B5G6R5_UNORM_PACK16) { r=lo; b=hi; }
        else { assert(format==VK_FORMAT_R5G6B5_UNORM_PACK16); r=hi; b=lo; }
        if(r!=expected[i][0] || g!=expected[i][1] || b!=expected[i][2]) return 1;
    }
    assert(fmt_of(0x25).vf==VK_FORMAT_R8G8B8A8_UNORM);
    assert(fmt_of(0x48).vf==VK_FORMAT_BC3_SRGB_BLOCK);
    return 0;
}
'''
            for label, text, expected in [('before',original,1),('after',prepared,0)]:
                mapping = text[text.index('static VkFmt fmt_of('):text.index('bool renderer_format_is_depth')]
                source = root/(label+'.c')
                source.write_text(preamble+mapping+harness, encoding='utf-8')
                exe = root/(label+'.exe')
                result = subprocess.run([DEFAULT_COMPILER,'cc','-target','x86_64-windows-gnu','-O1',
                                         '-I'+str(TOOLS/'deps/packages/ucrt64/include'),source,'-o',exe],
                                        capture_output=True,text=True,env=compiler_env())
                self.assertEqual(result.returncode,0,result.stderr)
                result = subprocess.run([exe],capture_output=True,text=True,timeout=20)
                self.assertEqual(result.returncode,expected,result.stderr)


if __name__ == '__main__':
    unittest.main()
