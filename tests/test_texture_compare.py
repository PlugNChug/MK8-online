"""Recriar uma textura equivalente preserva o descritor que o jogo já usa."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from native import DEFAULT_COMPILER, TOOLS
import texture_pool


@unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
class TextureCompareTests(unittest.TestCase):
    def test_semantics_preserve_descriptor_and_reject_address_only_mutation(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            source = out/'compare.c'
            fixture = r'''
#undef NDEBUG
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct { uint8_t bpb, bw, bh, kind; const char *name; } FormatInfo;
typedef struct {
    uint32_t width, height, depth, layers, levels;
    const FormatInfo *fmt;
    uint32_t block_height_log2, block_depth_log2;
} TexLayout;
typedef struct { uint64_t storage, size; uint32_t flags; } NvnPool;
typedef struct {
    TexLayout layout;
    uint32_t target, format, flags, width, height, depth, levels, samples, ds_mode;
    uint8_t swizzle[4];
    uint64_t stride, addr;
    NvnPool *pool;
    uint64_t pool_offset;
    void *backend;
    uint64_t debug_id;
    char label[64];
    unsigned mk8_refs;
} NvnTexture;
typedef struct { uint64_t x[31]; } Cpu;
#define NVN(name) void name(Cpu *c)
#define A(i) (c->x[i])
#define RET(value) (c->x[0] = (uint64_t)(value))
#define HOST(T, guest) ((T *)(uintptr_t)(*(uint64_t *)(uintptr_t)(guest)))
'''
            behavior = r'''
static uint64_t compare(uint64_t a, uint64_t b) {
    Cpu c = {{0}}; c.x[0] = a; c.x[1] = b;
    nvnTextureCompare(&c);
    return c.x[0];
}
typedef struct { uint64_t object; unsigned descriptor; } Slot;
static unsigned released, registered, discarded;
// Modelo da decisão do chamador: a igualdade descarta só o temporário, sem trocar o descritor.
static void replace_slot(Slot *slot, uint64_t *temporary) {
    if (compare((uintptr_t)temporary, (uintptr_t)&slot->object)) {
        discarded++;
    } else {
        released++;
        slot->object = *temporary;
        slot->descriptor = 100 + ++registered;
    }
    *temporary = 0;
}
int main(void) {
    FormatInfo fmt = {4, 1, 1, 0, "B10G11R11"};
    NvnPool pool = {0x100000, 0x100000, 3}, other_pool = pool;
    NvnTexture a = {.target=8, .format=0x3f, .flags=0, .width=32, .height=32, .depth=6,
        .levels=5, .samples=0, .ds_mode=0, .swizzle={2,3,4,1}, .addr=0x10c000,
        .pool=&pool, .pool_offset=0xc000, .layout={32,32,1,6,5,&fmt,2,0}};
    NvnTexture b = a;
    uint64_t ga = (uintptr_t)&a, gb = (uintptr_t)&b;
    Slot slot = {ga, 37};
    uint64_t temporary = gb;
    replace_slot(&slot, &temporary);
    // O mutante antigo precisa falhar exatamente nesta decisão, sem depender de abort().
    if (slot.descriptor != 37 || slot.object != ga || released || registered || discarded != 1 || temporary)
        return 23;
    b.target = 1; b.depth = 1; b.layout.layers = 1;
    temporary = gb;
    replace_slot(&slot, &temporary);
    assert(slot.descriptor == 101 && slot.object == gb && released == 1 && registered == 1 && discarded == 1);
    assert(temporary == 0);
    b = a;
    b.backend = &pool; b.debug_id = 19; b.mk8_refs = 7; strcpy(b.label, "mutable");
    assert(compare((uintptr_t)&ga, (uintptr_t)&gb) == 1);
    assert(compare((uintptr_t)&gb, (uintptr_t)&ga) == 1);
    assert(compare((uintptr_t)&ga, (uintptr_t)&ga) == 1);
    uint64_t copied = ga;
    assert(compare((uintptr_t)&ga, (uintptr_t)&copied) == 1);
    // Cada propriedade imutável pode diferenciar duas descrições sobre o mesmo armazenamento.
#define CHANGED(field) do { b = a; b.field++; assert(compare((uintptr_t)&ga, (uintptr_t)&gb) == 0); } while (0)
    CHANGED(target); CHANGED(format); CHANGED(flags); CHANGED(width); CHANGED(height); CHANGED(depth);
    CHANGED(levels); CHANGED(samples); CHANGED(ds_mode); CHANGED(stride); CHANGED(addr); CHANGED(pool_offset);
    CHANGED(layout.layers); CHANGED(layout.block_height_log2); CHANGED(layout.block_depth_log2);
    for (unsigned i = 0; i < 4; i++) { CHANGED(swizzle[i]); }
#undef CHANGED
    b = a; b.pool = &other_pool;
    assert(compare((uintptr_t)&ga, (uintptr_t)&gb) == 0);
    // A textura pode sobreviver ao pool; a comparação nunca desreferencia esse ponteiro.
    a.pool = (NvnPool *)(uintptr_t)1; b = a;
    assert(compare((uintptr_t)&ga, (uintptr_t)&gb) == 1);
    b.pool = (NvnPool *)(uintptr_t)2;
    assert(compare((uintptr_t)&ga, (uintptr_t)&gb) == 0);
    uint64_t empty = 0, other_empty = 0;
    assert(compare((uintptr_t)&ga, (uintptr_t)&empty) == 0);
    assert(compare((uintptr_t)&empty, (uintptr_t)&ga) == 0);
    assert(compare((uintptr_t)&empty, (uintptr_t)&empty) == 1);
    assert(compare((uintptr_t)&empty, (uintptr_t)&other_empty) == 0);
    assert(compare(0, (uintptr_t)&empty) == 0);
    assert(compare((uintptr_t)&empty, 0) == 0);
    assert(compare(0, (uintptr_t)&ga) == 0);
    assert(compare((uintptr_t)&ga, 0) == 0);
    assert(compare(0, 0) == 1);
    return 0;
}
'''
            env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(TOOLS/'cache'))
            for name, implementation, expected in [('semantic', texture_pool.COMPARE, 0),
                                                    ('address_only', texture_pool.COMPARE_OLD, 23)]:
                with self.subTest(implementation=name):
                    source.write_text(fixture + implementation + behavior, encoding='utf-8')
                    exe = out/(name+'.exe')
                    build = subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu', '-O2',
                                            '-Wall', '-Wextra', '-Werror', str(source), '-o', str(exe)],
                                           env=env, capture_output=True, text=True)
                    self.assertEqual(build.returncode, 0, build.stderr[-3000:])
                    run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
                    self.assertEqual(run.returncode, expected, run.stderr)


if __name__ == '__main__':
    unittest.main()
