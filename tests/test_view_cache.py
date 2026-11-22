"""Repeat a real Vulkan view-cache lookup with every probe occupied."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'toolchain'))
from host import DEPS, prepare_host_sources
from native import DEFAULT_COMPILER, compiler_env


@unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
class ViewCacheTests(unittest.TestCase):
    def test_hit_after_collision_window_fills(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', out)
            textures = (runtime/'nvn/vulkan/textures.c').read_text(encoding='utf-8')
            start = textures.index('VkImageView vk_image_view(')
            function = textures[start:textures.index('\n}', start) + 2]
            source = out/'view.c'
            source.write_text(r'''
#include <vulkan/vulkan.h>
#include <assert.h>
typedef struct { VkImage image; } TexBackend;
typedef struct { TexBackend *b; uint64_t k1,k2; VkImageView view; } ViewEnt;
#define VIEW_TAB 32
static ViewEnt view_tab[VIEW_TAB];
static struct { VkDevice dev; } g_vk;
static uint64_t prof_views_created;
static unsigned vk_view_gen, retired;
typedef struct { VkImageView view; } Dead;
static void defer(Dead d) { assert(d.view); ++retired; }
#define LOG(...) ((void)0)
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice dev, const VkImageViewCreateInfo *info,
        const VkAllocationCallbacks *allocator, VkImageView *view) {
    *view = (VkImageView)(uintptr_t)(prof_views_created + 1);
    return VK_SUCCESS;
}
''' + function + r'''
int main(void) {
    TexBackend other = {0}, texture = {0};
    for (unsigned i=0; i<VIEW_TAB; ++i)
        view_tab[i] = (ViewEnt){.b=&other, .view=(VkImageView)(uintptr_t)1};
    VkImageView requested[2];
    for (int i=0; i<2; ++i)
        requested[i] = vk_image_view(&texture,VK_IMAGE_VIEW_TYPE_2D_ARRAY,VK_FORMAT_R32_SFLOAT,
            0,1,0,1,(VkComponentMapping){0},VK_IMAGE_ASPECT_COLOR_BIT);
    return !(requested[0] == requested[1] && retired == 1 && vk_view_gen == 1 && prof_views_created == 1);
}
''')
            exe = out/'view.exe'
            subprocess.run([str(DEFAULT_COMPILER), 'cc', '-I'+str(DEPS/'include'), str(source),
                            '-o', str(exe)], check=True, timeout=90, env=compiler_env())
            subprocess.run([str(exe)], check=True, timeout=10)
