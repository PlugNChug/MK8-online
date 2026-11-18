"""Read the level-0 texture sizes of a branching GLSL shader before its dispatch loop (#7).

Races ran at about 1.5 fps on a GTX 1070 (driver 580.196.07): one full-screen draw, the shadow mask, took 686 ms.
Its array lookup clamps the layer with textureSize inside the `switch (pc)` loop; after the SSY reconvergence the
Pascal compiler keeps the invocations in the loop until its 100000 guard, and the result is wrong as well. The same
query before the loop runs in a fraction of a millisecond and gives the expected values. Sizes are uniform per draw,
so the shader reads the same values; shaders without branches and MSL are unchanged.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

LOOP = '''        sp(&s, "  uint pc = 8u; uint ssy[16]; uint ssy_n = 0u; uint pbk[16]; uint pbk_n = 0u; uint pcnt[16]; uint pcnt_n = 0u;\\n");
'''
BOUNDARY = 'static ShaderInfo *glsl_finish(Ctx *c, bool any_branch, bool dbg) {'


def prepare(runtime):
    path = runtime/'nvn/maxwell/translate.c'
    text = path.read_text(encoding='utf-8')
    if text.count(LOOP) != 1 or text.count(BOUNDARY) != 1 or 'mk8_hoist_texture_sizes' in text:
        raise ValueError('Texture size hoist boundary changed')
    helper = (ROOT/'runtime/mk8/graphics/texture_size_hoist.inc').read_text(encoding='utf-8')
    text = text.replace(BOUNDARY, helper+'\n'+BOUNDARY)
    text = text.replace(LOOP, '        mk8_hoist_texture_sizes(&s, &body, info->ntex);\n'+LOOP)
    path.write_text(text, encoding='utf-8')
