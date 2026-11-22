"""Bind a depth image when a shadow sampler has no compatible input texture."""
from pathlib import Path

GRAPHICS = Path(__file__).resolve().parents[1] / 'runtime/mk8/graphics'


def replace(text, old, new):
    if text.count(old) != 1:
        raise ValueError('Shadow descriptor boundary changed: ' + old[:75])
    return text.replace(old, new)


def prepare(runtime):
    folder = runtime / 'nvn/vulkan'
    header = folder / 'vk.h'
    textures = folder / 'textures.c'
    executor = folder / 'exec.c'
    api = header.read_text(encoding='utf-8')
    code = textures.read_text(encoding='utf-8')
    draw = executor.read_text(encoding='utf-8')
    api = replace(api, 'VkImageView vk_dummy_view(int textype);',
                  'VkImageView vk_dummy_view(int textype);\nVkImageView vk_shadow_dummy_view(int textype);\n'
                  'bool vk_is_color_dummy_view(VkImageView view);\nVkSampler vk_shadow_default_sampler(void);')
    # Keep the eviction victim inside the lookup window even when every probe is occupied.
    code = replace(code, 'for (int probe = 0; probe < 16; probe++, slot = (slot + 1) & (VIEW_TAB - 1))',
                   'for (int probe = 1; ; probe++, slot = (slot + 1) & (VIEW_TAB - 1))')
    code = replace(code, '        if (!e->b) break;', '        if (!e->b || probe == 16) break;')
    first = 'VkImageView vk_dummy_view(int textype) {'
    last = '    return views[textype];\n}'
    if code.count(first) != 1 or code.count(last) != 1 or code.index(first) >= code.index(last):
        raise ValueError('Shadow dummy image boundary changed')
    code = code[:code.index(first)] + (GRAPHICS/'shadow_dummy.inc').read_text(encoding='utf-8') + code[code.index(last)+len(last):]
    draw = replace(draw, 'if (st_->shadow && st_->type == TEXTYPE_2D) {',
                   'if (st_->shadow && st_->type == TEXTYPE_2D && view && !vk_is_color_dummy_view(view)) {')
    draw = replace(draw, '    if (!view) view = vk_dummy_view(st_->type);\n    if (!smp) smp = vk_default_sampler(true);',
                   '''    if (st_->shadow && (!view || vk_is_color_dummy_view(view))) {
        view = vk_shadow_dummy_view(st_->type);
    } else if (!view) view = vk_dummy_view(st_->type);
    if (!smp) smp = st_->shadow ? vk_shadow_default_sampler() : vk_default_sampler(true);''')
    from shadow_array import adapt
    api, code, draw = adapt(api, code, draw, replace)
    header.write_text(api, encoding='utf-8')
    textures.write_text(code, encoding='utf-8')
    executor.write_text(draw, encoding='utf-8')
