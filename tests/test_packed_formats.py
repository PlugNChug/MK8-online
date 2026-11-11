"""Ordem dos canais RGB565: fixtures autorais, sem texturas comerciais."""
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
import packed_formats


@unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
class PackedFormatTests(unittest.TestCase):
    def test_rgb565_uses_red_in_low_bits_without_changing_other_formats(self):
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', output)
            text = (runtime/'nvn/vulkan/textures.c').read_text(encoding='utf-8')
            mapping = text[text.index('static VkFmt fmt_of('):text.index('\nbool renderer_format_is_depth(')]
            source = output/'packed.c'
            source.write_text('''#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>
#define LOG(...) ((void)0)
typedef struct { VkFormat vf; uint8_t expand_rgb, alpha_one; } VkFmt;
static VkFormat depth24_stencil8(void) { return VK_FORMAT_D24_UNORM_S8_UINT; }
''' + mapping + r'''
// Vulkan PACK16 nomeia os componentes dos bits altos para os baixos.
static unsigned red(VkFormat f, uint16_t pixel) {
    return f == VK_FORMAT_B5G6R5_UNORM_PACK16 ? pixel & 31 : pixel >> 11;
}
static unsigned blue(VkFormat f, uint16_t pixel) {
    return f == VK_FORMAT_B5G6R5_UNORM_PACK16 ? pixel >> 11 : pixel & 31;
}
int main(void) {
    VkFormat format = fmt_of(0x3c).vf;
    if (format != VK_FORMAT_B5G6R5_UNORM_PACK16) return 1;
    assert(red(format, 0x001f) == 31 && blue(format, 0x001f) == 0);
    assert(red(format, 0xf800) == 0 && blue(format, 0xf800) == 31);
    assert(red(format, 0x07ff) == 31 && blue(format, 0x07ff) == 0);
    assert(fmt_of(0x25).vf == VK_FORMAT_R8G8B8A8_UNORM);
    assert(fmt_of(0x38).vf == VK_FORMAT_R8G8B8A8_SRGB);
    assert(fmt_of(0x44).vf == VK_FORMAT_BC3_UNORM_BLOCK);
    assert(fmt_of(0x99).vf == VK_FORMAT_B8G8R8A8_UNORM);
    return 0;
}
''', encoding='utf-8')
            exe = output/'packed.exe'
            build = subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu',
                                    '-O1', '-UNDEBUG', '-I'+str(DEPS/'include'),
                                    str(source), '-o', str(exe)],
                                   capture_output=True, text=True, env=os.environ)
            self.assertEqual(build.returncode, 0, build.stderr[-3000:])
            subprocess.run([str(exe)], check=True, timeout=20)

    def test_unknown_or_duplicated_layout_is_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            runtime = Path(folder)
            path = runtime/'nvn/vulkan/textures.c'
            path.parent.mkdir(parents=True)
            for text in ('changed upstream',
                         '    case 0x3C: return (VkFmt){VK_FORMAT_R5G6B5_UNORM_PACK16};\n'*2):
                path.write_text(text)
                with self.assertRaisesRegex(ValueError, 'boundary changed'):
                    packed_formats.prepare(runtime)
                self.assertEqual(path.read_text(), text)


if __name__ == '__main__':
    unittest.main()
