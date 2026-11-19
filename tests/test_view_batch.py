"""The batched attachment-view spec matches vk_view's render branch (toolchain/view_batch.py).

The prepared textures.c/exec.c are checked for the thread-safe view cache and the prefetch call; vk_render_spec is
extracted and run in a native harness over 2D, 3D, format-view, depth and clamped cases."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from host import prepare_host_sources
import view_batch

HARNESS = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t VkFormat;
typedef uint32_t VkImageViewType;
typedef uint32_t VkImageAspectFlags;
typedef struct { uint32_t r, g, b, a; } VkComponentMapping;
typedef struct VkImage_T *VkImage;
#define VK_IMAGE_VIEW_TYPE_2D 1u
#define VK_IMAGE_VIEW_TYPE_3D 2u
#define VK_IMAGE_ASPECT_DEPTH_BIT 2u
#define VK_IMAGE_ASPECT_STENCIL_BIT 4u
typedef struct {
    VkImage image;
    VkFormat format;
    uint32_t levels, layers;
    VkImageAspectFlags aspect;
    VkImageViewType base_type;
    uint32_t depth;
} TexBackend;
typedef struct { TexBackend *backend; uint32_t format; } NvnTexture;
typedef struct { uint32_t setmask, base_level, num_levels, min_layer, num_layers, format; } NvnTextureView;
typedef struct { VkFormat vf; } VkFmt;
static VkFmt fmt_of(uint32_t f) { VkFmt r = {(VkFormat)(f + 100)}; return r; }
static VkFormat vk_format(uint32_t f) { return (VkFormat)f; }
static const VkComponentMapping IDENTITY = {0, 0, 0, 0};
typedef struct { TexBackend *b; VkImageViewType type; VkFormat fmt; uint32_t lv0, nlv, ly0, nly;
                 VkComponentMapping sw; VkImageAspectFlags aspect; } ViewSpec;
/*SPEC*/
static int check(TexBackend *b, NvnTextureView *v, bool has, VkImageViewType type, VkFormat fmt, uint32_t lv0,
                 uint32_t ly0, VkImageAspectFlags aspect) {
    NvnTexture t = {b, b->format};
    ViewSpec s;
    if (!vk_render_spec(&t, v, has, &s)) return 1;
    if (s.b != b || s.type != type || s.fmt != fmt || s.lv0 != lv0 || s.nlv != 1 || s.ly0 != ly0 || s.nly != 1 ||
        s.aspect != aspect || s.sw.r || s.sw.g || s.sw.b || s.sw.a) return 2;
    return 0;
}
int main(void) {
    TexBackend color = {(VkImage)1, 50, 4, 1, 1, VK_IMAGE_VIEW_TYPE_2D, 1};
    if (check(&color, NULL, false, VK_IMAGE_VIEW_TYPE_2D, 50, 0, 0, 1)) return 10;
    NvnTextureView view = {0};
    if (check(&color, &view, true, VK_IMAGE_VIEW_TYPE_2D, 50, 0, 0, 1)) return 11;
    view.setmask = 1 | 2; view.base_level = 1; view.num_levels = 2; view.min_layer = 0; view.num_layers = 1;
    if (check(&color, &view, true, VK_IMAGE_VIEW_TYPE_2D, 50, 1, 0, 1)) return 12;
    view.setmask = 4; view.format = 60;
    if (check(&color, &view, true, VK_IMAGE_VIEW_TYPE_2D, 160, 0, 0, 1)) return 13;
    view.setmask = 0;
    TexBackend depth = {(VkImage)1, 52, 1, 1, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                        VK_IMAGE_VIEW_TYPE_2D, 1};
    if (check(&depth, &view, true, VK_IMAGE_VIEW_TYPE_2D, 52, 0, 0,
              VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) return 14;
    TexBackend cube = {(VkImage)1, 50, 4, 6, 1, VK_IMAGE_VIEW_TYPE_3D, 8};
    view.setmask = 1 | 2; view.base_level = 0; view.num_levels = 1; view.min_layer = 2; view.num_layers = 1;
    if (check(&cube, &view, true, VK_IMAGE_VIEW_TYPE_2D, 50, 0, 2, 1)) return 15;
    view.base_level = 10; view.min_layer = 9;  // (clamped to the image's levels/layers)
    if (check(&cube, &view, true, VK_IMAGE_VIEW_TYPE_2D, 50, 3, 0, 1)) return 16;
    TexBackend none = {0};
    NvnTexture t = {&none, 50};
    ViewSpec s;
    if (vk_render_spec(&t, &view, true, &s)) return 17;  // (no image: no spec)
    return 0;
}
'''


class ViewBatchTests(unittest.TestCase):
    def test_boundaries_are_checked(self):
        with tempfile.TemporaryDirectory() as folder:
            vulkan = Path(folder)/'nvn/vulkan'
            vulkan.mkdir(parents=True)
            for name in ('textures.c', 'exec.c'):
                (vulkan/name).write_text('int x;\n', encoding='utf-8')
            with self.assertRaises(ValueError):
                view_batch.prepare(Path(folder))

    @unittest.skipUnless(shutil.which('cc'), 'Native C compiler required')
    def test_prepared_runtime_uses_the_pool_and_the_spec_is_exact(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', out/'host')
            textures = (runtime/'nvn/vulkan/textures.c').read_text(encoding='utf-8')
            execute = (runtime/'nvn/vulkan/exec.c').read_text(encoding='utf-8')
            self.assertEqual(textures.count('static pthread_mutex_t view_lock'), 1)
            self.assertEqual(textures.count('void mk8_views_prefetch'), 1)
            self.assertEqual(execute.count('mk8_views_prefetch'), 2)  # (prototype + call)
            self.assertEqual(execute.count('mk8_batch_views[i] ?'), 1)
            self.assertEqual(execute.count('mk8_batch_views[8] ?'), 1)
            view_batch.prepare(runtime)
            view_batch.prepare(runtime)
            self.assertEqual((runtime/'nvn/vulkan/textures.c').read_text(encoding='utf-8').count('void mk8_views_prefetch'), 1)
            start = textures.index('static bool vk_render_spec(')
            end = textures.index('\n}\n', start) + 3
            harness = out/'harness.c'
            harness.write_text(HARNESS.replace('/*SPEC*/', textures[start:end]), encoding='utf-8')
            exe = out/'harness'
            subprocess.run(['cc', '-std=gnu11', '-O1', str(harness), '-o', str(exe)], check=True, capture_output=True)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
