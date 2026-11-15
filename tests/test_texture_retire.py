"""Which idle texture images give their memory back: the prepared textures.c's own decision, case by case.

mk8_retire_image is taken from the prepared runtime and run in a native harness over backend records; Vulkan, VMA and
the copy-alias / snapshot hooks are stand-ins that record what they are asked."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from host import prepare_host_sources
from native import DEFAULT_COMPILER, compiler_env
import texture_retire

HARNESS = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct Img *VkImage;
typedef struct Mem *VmaAllocation;
typedef struct { uint64_t size; } VmaAllocationInfo;
typedef struct TexBackend {
    VkImage image;
    VmaAllocation mem;
    bool gpu_owned, uploaded, presented;
    void *pending_rgba;
    uint64_t mk8_used, mk8_superseded;
} TexBackend;
static struct { int vma; } g_vk;
static int ends, destroyed, forgotten;
static TexBackend *aliased;
static void vk_end_pass(void) { ends++; }
static void vmaGetAllocationInfo(int vma, VmaAllocation m, VmaAllocationInfo *info) { (void)vma; (void)m; info->size = 8u << 20; }
static void destroy_image(TexBackend *b) { destroyed++; b->image = NULL; b->mem = NULL; }
bool mk8_copy_alias_uses(const TexBackend *b) { return b == aliased; }
void mk8_snapshots_forget(VkImage image) { (void)image; forgotten++; }
/*RETIRE*/
static TexBackend make(bool gpu, uint64_t used, uint64_t superseded) {
    TexBackend b = {(VkImage)(uintptr_t)0x10, (VmaAllocation)(uintptr_t)0x20, gpu, !gpu, false, NULL, used, superseded};
    return b;
}
#define NOW 10000ull
#define IDLE 3600ull
#define GRACE 120ull
static int retire(TexBackend *b, bool *ended, uint64_t *bytes, int *targets) {
    return mk8_retire_image(b, NOW, IDLE, GRACE, ended, bytes, targets);
}
int main(void) {
    bool ended = false;
    uint64_t bytes = 0;
    int targets = 0;
    TexBackend cpu_idle = make(false, NOW - IDLE, 0), cpu_recent = make(false, NOW - IDLE + 1, 0);
    TexBackend target_idle = make(true, 100, 0), target_superseded = make(true, 100, 200);
    TexBackend target_reused = make(true, 300, 200);  // (superseded, then its shape drawn again: still current)
    TexBackend presented = make(false, 100, 0), video = make(false, 100, 0), copied = make(false, 100, 0);
    TexBackend future = make(false, NOW + 5, 0);
    presented.presented = true;
    video.pending_rgba = &video;
    aliased = &copied;
    if (retire(&cpu_idle, &ended, &bytes, &targets) != 1 || cpu_idle.image || cpu_idle.uploaded) return 1;
    if (retire(&cpu_recent, &ended, &bytes, &targets) != 0 || !cpu_recent.image) return 2;
    if (retire(&target_idle, &ended, &bytes, &targets) != 0 || !target_idle.image) return 3;
    if (retire(&target_superseded, &ended, &bytes, &targets) != 1 || target_superseded.image) return 4;
    if (target_superseded.gpu_owned || target_superseded.mk8_superseded) return 5;
    if (retire(&target_reused, &ended, &bytes, &targets) != 0) return 6;
    if (retire(&presented, &ended, &bytes, &targets) != 0) return 7;
    if (retire(&video, &ended, &bytes, &targets) != 0) return 8;
    if (retire(&copied, &ended, &bytes, &targets) != 0) return 9;
    if (retire(&future, &ended, &bytes, &targets) != 0) return 10;
    if (retire(&cpu_idle, &ended, &bytes, &targets) != 0) return 11;  // (already retired: no image)
    if (ends != 1 || !ended) return 12;  // (the pass ends once, before the first)
    if (destroyed != 2 || forgotten != 2 || bytes != (16u << 20) || targets != 1) return 13;
    // A target the game made again at another size (dynamic resolution) goes after the short grace, not the minute:
    TexBackend resized = make(true, NOW - GRACE, NOW - GRACE + 1), resized_recent = make(true, NOW - GRACE + 1, NOW);
    TexBackend current = make(true, NOW - GRACE, 0);  // (not superseded: kept however long it waits)
    if (retire(&resized, &ended, &bytes, &targets) != 1 || resized.image) return 14;
    if (retire(&resized_recent, &ended, &bytes, &targets) != 0 || !resized_recent.image) return 15;
    if (retire(&current, &ended, &bytes, &targets) != 0 || !current.image) return 16;
    if (targets != 2 || destroyed != 3) return 17;
    return 0;
}
'''


class TextureRetireTests(unittest.TestCase):
    def test_boundaries_are_checked(self):
        with tempfile.TemporaryDirectory() as folder:
            vulkan = Path(folder)/'nvn/vulkan'
            vulkan.mkdir(parents=True)
            for name in ('vk.h', 'textures.c', 'device.c', 'exec.c'):
                (vulkan/name).write_text('int x;\n', encoding='utf-8')
            with self.assertRaises(ValueError):
                texture_retire.prepare(Path(folder))

    @unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
    def test_only_images_whose_contents_can_be_had_again_are_retired(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', out/'host')
            vulkan = runtime/'nvn/vulkan'
            textures = (vulkan/'textures.c').read_text(encoding='utf-8')
            # uses stamped always; a backend made now is not idle; another shape over the storage marks the old one
            self.assertIn('        b->mk8_used = renderer_present_count();  // MK8 (toolchain/vram_trace.py, texture_retire.py)\n', textures)
            self.assertNotIn('if (mk8_vram_traced()) b->mk8_used', textures)
            self.assertIn('b->mk8_used = renderer_present_count();  // MK8 (toolchain/texture_retire.py): made now', textures)
            self.assertIn('if (a->addr == t->addr && !alias_match(a, t)) a->b->mk8_superseded = renderer_present_count();', textures)
            # the existing re-creation of an image-less backend still comes first in tex_backend
            lookup = textures[textures.index('static TexBackend *tex_backend(NvnTexture *t) {'):]
            self.assertLess(lookup.index('if (!b->image) {'), lookup.index('return b;'))
            self.assertIn('mk8_textures_retire_tick();', (vulkan/'device.c').read_text(encoding='utf-8'))
            start = textures.index('static int mk8_retire_image(')
            retire = textures[start:textures.index('\n}\n', start) + 3]
            harness = out/'harness.c'
            harness.write_text(HARNESS.replace('/*RETIRE*/', retire), encoding='utf-8')
            exe = out/'harness.exe'
            subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu', '-O1', str(harness), '-o', str(exe)],
                           env=compiler_env(), check=True, capture_output=True)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout)


if __name__ == '__main__':
    unittest.main()
