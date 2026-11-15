"""Cópias guest não consomem duas vezes a referência criada por Initialize."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from host import prepare_host_sources
from native import DEFAULT_COMPILER, TOOLS


def between(text, start, end):
    return text[text.index(start):text.index(end, text.index(start))]


@unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
class TextureFinalizeTests(unittest.TestCase):
    def test_copied_owners_keep_descriptor_and_command_references(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', out/'prepared')
            objects = (runtime/'nvn/objects.c').read_text(encoding='utf-8')
            header = (runtime/'nvn/nvn.h').read_text(encoding='utf-8')
            commands = (runtime/'nvn/cmdbuf.c').read_text(encoding='utf-8')
            types = between(header, 'typedef struct NvnPool {', 'typedef struct NvnShaderStage {')
            layout = between(objects, 'static bool target_is_array(', 'static bool is_linear(')
            initialize = between(objects, 'static NvnTexture *texture_from_builder(', 'NVN(nvnTextureFinalize)')
            finalize = between(objects, 'NVN(nvnTextureFinalize)', 'NVN(nvnTextureSetDebugLabel)')
            compare = between(objects, 'NVN(nvnTextureCompare)', 'NVN(nvnTextureFlushTexels)')
            register = between(objects, 'static void register_texture(', 'NVN(nvnTexturePoolRegisterImage)')
            refs = objects[objects.index('NvnTexture *mk8_texture_retain('):]
            command_refs = between(commands, 'struct Mk8TextureRefs {', 'static void mk8_retire_command_buffer')
            prefix = r'''
#undef NDEBUG
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include "tegra.h"
#define LOG(...) ((void)0)
#define FATAL(...) do { fprintf(stderr, __VA_ARGS__); exit(86); } while (0)
typedef struct { uint64_t x[31]; } Cpu;
#define NVN(name) void name(Cpu *c)
#define A(i) (c->x[(i)])
#define AP(i) ((void *)(uintptr_t)c->x[(i)])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define HOST(T, guest) ((T *)(uintptr_t)(*(uint64_t *)(uintptr_t)(guest)))
#define SET_HOST(guest, p) (*(uint64_t *)(uintptr_t)(guest) = (uint64_t)(uintptr_t)(p))
#define G(T, base, off) (*(T *)((uint8_t *)(uintptr_t)(base) + (off)))
'''
            helpers = r'''
static unsigned initialized, finalized, destroyed;
static uint64_t next_debug_id = 1;
static void backend_texture_destroyed(NvnTexture *t) { assert(t); destroyed++; }
void game_texture_init(uint64_t addr, uint32_t w, uint32_t h, uint32_t fmt) {
    (void)addr; (void)w; (void)h; (void)fmt; initialized++;
}
void game_texture_final(uint64_t addr) { (void)addr; finalized++; }
static uint8_t mk8_texpool_cube_ids[1 << 16];
static bool mk8_texpool_trace_on(void) { return false; }
uint64_t renderer_present_count(void) { return 0; }
static void mk8_texpool_callers(Cpu *c) { (void)c; }
static void mk8_cube_note(NvnDescPool *d, int id, const NvnTexEntry *e) {
    (void)d; (void)id; (void)e; FATAL("Unexpected cube in 2D fixture\n");
}
NvnDescPool *mk8_descriptor_retain(NvnDescPool *d) { assert(!d); return d; }
void mk8_descriptor_release(NvnDescPool *d) { assert(!d); }
typedef struct { uint32_t op, size; struct Mk8TextureRefs *mk8_textures; } CmdRec;
#define CMD_END 0
'''
            behavior = r'''
typedef struct { uint64_t words[24]; } GuestTexture;
static void init(GuestTexture *object, uint64_t *builder) {
    Cpu c = {{(uintptr_t)object, (uintptr_t)builder}};
    nvnTextureInitialize(&c); assert(c.x[0] == 1);
}
static void finish(GuestTexture *object) {
    Cpu c = {{(uintptr_t)object}}; nvnTextureFinalize(&c);
    assert(object->words[0] == 0);
}
static void reg(NvnDescPool *pool, unsigned id, GuestTexture *object, NvnTextureView *view) {
    uint64_t gp = (uintptr_t)pool;
    Cpu c = {{(uintptr_t)&gp, id, (uintptr_t)object, (uintptr_t)view}};
    nvnTexturePoolRegisterTexture(&c);
}
int main(int argc, char **argv) {
    (void)argv;
    bool command = argc > 1;
    // Mesmo descarte assíncrono do runtime: a última referência aposenta até o tradutor drenar.
    atomic_store(&mk8_translator_running, true);
    uint64_t builder[12] = {0};
    G(uint32_t, (uintptr_t)builder, 0xc) = 1;
    G(uint32_t, (uintptr_t)builder, 0x10) = 4;
    G(uint32_t, (uintptr_t)builder, 0x14) = 4;
    G(uint32_t, (uintptr_t)builder, 0x18) = 1;
    G(uint32_t, (uintptr_t)builder, 0x1c) = 1;
    G(uint32_t, (uintptr_t)builder, 0x20) = 0x25;
    GuestTexture a = {{0}}, b, temporary = {{0}}, empty = {{0}};
    init(&a, builder);
    NvnTexture *t = HOST(NvnTexture, (uintptr_t)&a);
    uint64_t first_id = t->debug_id;
    NvnTexEntry entries[2] = {{0}};
    NvnDescPool pool = {.count=2, .entries=entries};
    reg(&pool, 0, &a, NULL);
    memcpy(&b, &a, sizeof b);
    init(&temporary, builder);
    Cpu cmp = {{(uintptr_t)&temporary, (uintptr_t)&b}};
    nvnTextureCompare(&cmp); assert(cmp.x[0] == 1);
    finish(&temporary); mk8_textures_drain();
    assert(destroyed == 1 && t->mk8_refs == 2);
    NvnTextureView view = {.setmask=8, .swizzle={2,3,4,1}};
    reg(&pool, 1, &b, &view);
    assert(t->mk8_refs == 3 && entries[0].tex == t && entries[1].tex == t);
    CmdRec record = {1, sizeof(CmdRec), NULL};
    if (command) mk8_command_texture(&record, (uintptr_t)t);
    GuestTexture borrowed = b;  // Só usada enquanto os descritores ainda retêm a textura.
    finish(&a); finish(&b); finish(&b);
    unsigned after_final = t->mk8_refs;
    reg(&pool, 0, &borrowed, &view);  // Reter antes de soltar permite regravar o mesmo HOST.
    unsigned after_same = t->mk8_refs;
    reg(&pool, 0, &empty, NULL);
    reg(&pool, 1, &empty, NULL);
    if (command) {
        assert(destroyed == 1);
        mk8_release_records((uint8_t *)&record, sizeof record);
    }
    mk8_textures_drain();
    assert(after_final == 2 + (unsigned)command && after_same == after_final);
    assert(!entries[0].tex && !entries[1].tex && destroyed == 2 && finalized == 2);
    init(&a, builder);  // Reutilizar o endereço guest cria outro dono, ainda não finalizado.
    t = HOST(NvnTexture, (uintptr_t)&a);
    assert(t->debug_id != first_id && t->mk8_refs == 1 && t->mk8_finalized == 0);
    finish(&a); finish(&a); mk8_textures_drain();
    assert(initialized == 3 && finalized == 3 && destroyed == 3);
    return 0;
}
'''
            old = finalize.replace('t && !__atomic_exchange_n(&t->mk8_finalized, 1, __ATOMIC_ACQ_REL)', 't')
            old = old.replace('game_texture_final(t->addr); mk8_texture_release(t);',
                              'game_texture_final(t->addr); __atomic_store_n(&t->mk8_finalized, 1, '
                              '__ATOMIC_RELAXED); mk8_texture_release(t);')
            self.assertNotEqual(old, finalize)
            env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(TOOLS/'cache'))
            env.pop('MK8_TRACE_TEXTURES', None)
            for name, final, expected in [('once', finalize, 0), ('old', old, 86)]:
                source = out/(name+'.c')
                source.write_text(prefix + types + helpers + refs + command_refs + layout + initialize + final +
                                  compare + register + behavior, encoding='utf-8')
                exe = source.with_suffix('.exe')
                build = subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu', '-O1',
                                        '-I'+str(runtime/'nvn'), str(source), str(runtime/'nvn/tegra.c'),
                                        str(runtime/'nvn/formats.c'), '-o', str(exe)], env=env,
                                       capture_output=True, text=True)
                self.assertEqual(build.returncode, 0, build.stderr[-3000:])
                for command in (False, True):
                    with self.subTest(implementation=name, command=command):
                        run = subprocess.run([str(exe)] + (['command'] if command else []), env=env,
                                             capture_output=True, text=True, timeout=30)
                        self.assertEqual(run.returncode, expected, run.stderr)
                        if expected:
                            self.assertIn('Texture reference underflow', run.stderr)


if __name__ == '__main__':
    unittest.main()
