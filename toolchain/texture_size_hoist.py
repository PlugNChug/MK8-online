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
KEY = '    uint32_t vs_out_mask;   // fragment: varyings the vertex shader provides'
DIALECT = '    vkey->glsl = fkey->glsl = 1;'
AHEAD_KEY = '    f.flip_y = fkey->flip_y, f.depth_minus_one = fkey->depth_minus_one;'
DEVICE_CHECK = '''static inline bool mk8_use_texture_size_hoist(uint32_t vendor, uint32_t device) {
    return vendor == 0x10de && device == 0x1b81;  // GeForce GTX 1070 desktop (#7)
}
'''


def prepare(runtime):
    path = runtime/'nvn/maxwell/translate.c'
    header = runtime/'nvn/maxwell/shader.h'
    executor = runtime/'nvn/vulkan/exec.c'
    ahead = runtime/'nvn/vulkan/ahead.c'
    text = path.read_text(encoding='utf-8')
    keys = header.read_text(encoding='utf-8')
    exec_text = executor.read_text(encoding='utf-8')
    ahead_text = ahead.read_text(encoding='utf-8')
    if text.count(LOOP) != 1 or text.count(BOUNDARY) != 1 or 'mk8_hoist_texture_sizes' in text:
        raise ValueError('Texture size hoist boundary changed')
    if keys.count(KEY) != 1 or exec_text.count(DIALECT) != 1 or ahead_text.count(AHEAD_KEY) != 1 or 'texture_size_hoist' in keys:
        raise ValueError('Texture size hoist key boundary changed')
    helper = (ROOT/'runtime/mk8/graphics/texture_size_hoist.inc').read_text(encoding='utf-8')
    text = text.replace(BOUNDARY, helper+'\n'+BOUNDARY)
    text = text.replace(LOOP, '        if (key && key->texture_size_hoist) mk8_hoist_texture_sizes(&s, &body, info->ntex);\n'+LOOP)
    path.write_text(text, encoding='utf-8')
    # Use the byte before the aligned mask so the host key retains its layout.
    keys = keys.replace(KEY, '    bool texture_size_hoist;  // fragment GLSL workaround for the selected Vulkan device\n'+KEY)
    header.write_text(keys+'\n'+DEVICE_CHECK, encoding='utf-8')
    executor.write_text(exec_text.replace(DIALECT, DIALECT+'\n'
        '    fkey->texture_size_hoist = mk8_use_texture_size_hoist(g_vk.props.vendorID, g_vk.props.deviceID);'),
        encoding='utf-8')
    ahead.write_text(ahead_text.replace(AHEAD_KEY, AHEAD_KEY+'\n'
        '    f.texture_size_hoist = fkey->texture_size_hoist;'), encoding='utf-8')
