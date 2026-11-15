"""A cube sampler keeps reading the cube map last registered under a pool ID that now holds a 2D texture."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from host import DEPS, prepare_host_sources
from native import DEFAULT_COMPILER, compiler_env
import texture_pool


def between(text, start, end):
    return text[text.index(start):text.index(end, text.index(start))]


@unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
class TexturePoolTests(unittest.TestCase):
    def test_draw_probe_selector_only_ends_requested_debug_draws(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', out/'host')
            execute = (runtime/'nvn/vulkan/exec.c').read_text(encoding='utf-8')
            selector = between(execute, '    // LARECOMP_DEBUG_PROBE_DRAWS=', '    if (g_gpuprof_frame && gp_n) {')
            source = out/'probe.c'
            source.write_text(r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef struct { struct { uint64_t code_addr; } stages[2]; } Program;
static Program program;
static struct { Program *program; struct { int ncolor; } rt; bool rt_dirty; } st = { .program = &program };
static bool g_debug_frame;
static int ended;
static void vk_end_pass(void) { ended++; }
static void after_draw(void) {
''' + selector + r'''
}
int main(int argc, char **argv) {
    if (argc != 2) return 100;
    unsigned mode = (unsigned)atoi(argv[1]);
    enum { MULTI = 1, SHADER = 2, ALL = 4 };
    const struct { bool debug; int targets; uint64_t shader; unsigned modes; } cases[] = {
        { false, 0, 0x123, 0 }, { false, 1, 0x123, 0 }, { false, 2, 0x123, 0 },
        { false, 0, 0x456, 0 }, { false, 1, 0x456, 0 }, { false, 2, 0x456, 0 },
        { true, 0, 0x456, ALL }, { true, 0, 0x123, ALL | SHADER },
        { true, 1, 0x456, ALL }, { true, 1, 0x123, ALL | SHADER },
        { true, 2, 0x456, ALL | MULTI }, { true, 2, 0x123, ALL | MULTI | SHADER },
    };
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        g_debug_frame = cases[i].debug;
        st.rt.ncolor = cases[i].targets;
        program.stages[1].code_addr = cases[i].shader;
        st.rt_dirty = false;
        ended = 0;
        after_draw();
        bool expected = (cases[i].modes & mode) != 0;
        if (ended != (int)expected || st.rt_dirty != expected) return (int)i + 1;
    }
    return 0;
}
''', encoding='utf-8')
            exe = out/'probe.exe'
            build = subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu', '-O2',
                                    str(source), '-o', str(exe)], env=compiler_env(), capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr[-3000:])
            for value, mode in ((None, 0), ('1', 1), ('123', 2), ('all', 4)):
                with self.subTest(selector=value):
                    env = dict(os.environ)
                    env.pop('LARECOMP_DEBUG_PROBE_DRAWS', None)
                    if value is not None:
                        env['LARECOMP_DEBUG_PROBE_DRAWS'] = value
                    result = subprocess.run([str(exe), str(mode)], env=env, capture_output=True, text=True, timeout=30)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_cube_sampler_reads_last_cube_of_reused_id(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', out)
            objects = (runtime/'nvn/objects.c').read_text(encoding='utf-8')
            execute = (runtime/'nvn/vulkan/exec.c').read_text(encoding='utf-8')
            support = between(objects, '// MK8 (toolchain/texture_pool.py): the last cube map', 'static void register_texture(Cpu *c) {')
            register = between(objects, 'static void register_texture(Cpu *c) {', 'NVN(nvnTexturePoolRegisterTexture)')
            lookup = between(execute, '// MK8 (toolchain/texture_pool.py): the entry a sampler reads', 'static NvnTexture *shader_texture_object(')
            refs = (ROOT/'runtime/mk8/graphics/texture_refs.inc').read_text(encoding='utf-8')
            source = out/'pool.c'
            source.write_text(r'''
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define LOG(...) ((void)0)
#define FATAL(...) abort()
#define A(i) (c->x[(i)])
#define AP(i) ((void *)(uintptr_t)c->x[(i)])
#define HOST(T, guest) ((T *)(uintptr_t)(*(uint64_t *)(uintptr_t)(guest)))
enum { TEXTYPE_1D, TEXTYPE_2D, TEXTYPE_3D, TEXTYPE_CUBE, TEXTYPE_2D_ARRAY, TEXTYPE_CUBE_ARRAY };
typedef struct { uint64_t x[31]; } Cpu;
typedef struct { uint8_t raw[0x28]; } NvnTextureView;
typedef struct NvnTexture { uint32_t target, width, height; unsigned mk8_refs; } NvnTexture;
typedef struct { NvnTexture *tex; NvnTextureView view; bool has_view; } NvnTexEntry;
typedef struct NvnDescPool { uint64_t addr; int count; void *entries; } NvnDescPool;
static int freed;
static void backend_texture_destroyed(NvnTexture *t) { (void)t; freed++; }
uint64_t renderer_present_count(void) { return 0; }
''' + refs + support + register + lookup + r'''
static NvnTexture *texture(uint32_t target, uint32_t w, uint32_t h) {
    NvnTexture *t = calloc(1, sizeof *t);
    t->target = target; t->width = w; t->height = h; t->mk8_refs = 1;
    return t;
}
static void reg(NvnDescPool *pool, int id, NvnTexture *t) {  /* nvnTexturePoolRegisterTexture(pool, id, t, NULL) */
    uint64_t pool_obj = (uint64_t)(uintptr_t)pool, tex_obj = (uint64_t)(uintptr_t)t;
    Cpu c = {{0}};
    c.x[0] = (uint64_t)(uintptr_t)&pool_obj; c.x[1] = (uint64_t)id; c.x[2] = (uint64_t)(uintptr_t)&tex_obj;
    register_texture(&c);
}
int main(int argc, char **argv) {
    NvnTexEntry entries[16] = {{0}};
    NvnDescPool pool = {0, 16, entries};
    NvnTexture *cube = texture(8, 32, 32), *cube2 = texture(8, 32, 32), *target = texture(1, 1920, 1080);
    reg(&pool, 5, cube);
    if (mk8_tex_entry(&pool, 5, TEXTYPE_CUBE)->tex != cube) return 1;
    reg(&pool, 5, target);  /* the slot handed to a render target */
    if (mk8_tex_entry(&pool, 5, TEXTYPE_2D)->tex != target) return 2;
    const NvnTexture *seen = mk8_tex_entry(&pool, 5, TEXTYPE_CUBE)->tex;
    if (argc > 1) return seen == target ? 0 : 3;  /* MK8_TEXPOOL_CUBE_HISTORY=0: the ID's own registration */
    if (seen != cube || mk8_tex_entry(&pool, 5, TEXTYPE_CUBE_ARRAY)->tex != cube) return 4;
    if (mk8_tex_entry(&pool, 6, TEXTYPE_CUBE)->tex != NULL) return 5;  /* never a cube there: unchanged */
    if (cube->mk8_refs != 2) return 6;  /* the game's reference and the history's */
    reg(&pool, 5, cube2);  /* a new cube under the ID replaces the remembered one */
    if (cube->mk8_refs != 1 || cube2->mk8_refs != 3 || target->mk8_refs != 1 || freed) return 7;
    reg(&pool, 5, target);
    return mk8_tex_entry(&pool, 5, TEXTYPE_CUBE)->tex == cube2 ? 0 : 8;
}
''', encoding='utf-8')
            exe = out/'pool.exe'
            env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(ROOT/'local/native-tools/cache'))
            build = subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu', '-O2', '-I'+str(DEPS/'include'),
                                    str(source), '-o', str(exe)], env=env, capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr[-3000:])
            subprocess.run([str(exe)], check=True, timeout=30)
            off = dict(os.environ, MK8_TEXPOOL_CUBE_HISTORY='0')
            subprocess.run([str(exe), 'off'], check=True, timeout=30, env=off)

    def test_boundaries_are_checked(self):
        with tempfile.TemporaryDirectory() as folder:
            runtime = Path(folder)
            (runtime/'nvn/vulkan').mkdir(parents=True)
            (runtime/'nvn/objects.c').write_text('static void register_texture(Cpu *c) {\n}\n')
            (runtime/'nvn/cmdbuf.c').write_text('int x;\n')
            (runtime/'nvn/vulkan/exec.c').write_text('int y;\n')
            with self.assertRaises(ValueError):
                texture_pool.prepare(runtime)


if __name__ == '__main__':
    unittest.main()
