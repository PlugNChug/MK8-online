"""Keep the first bloom blur contiguous when one guest texel spans several host texels."""
import json
from pathlib import Path
import shutil

GRAPHICS = Path(__file__).resolve().parents[1]/'runtime/mk8/graphics'
INCLUDE = '#include "../castle_blur.h"\n'


def replace(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f'Castle blur preparation boundary changed: {old}')
    return text.replace(old, new)


def prepare(runtime):
    nvn = runtime/'nvn'
    translate = nvn/'maxwell/translate.c'
    executor = nvn/'vulkan/exec.c'
    textures = nvn/'vulkan/textures.c'
    header = nvn/'vulkan/vk.h'
    code = translate.read_text(encoding='utf-8')
    draw = executor.read_text(encoding='utf-8')
    sampler = textures.read_text(encoding='utf-8')
    api = header.read_text(encoding='utf-8')
    body = (GRAPHICS/'castle_blur.glsl').read_text(encoding='utf-8')
    body = replace(body, 'HORIZONTAL', '%s')
    down = (GRAPHICS/'bloom_first_down.glsl').read_text(encoding='utf-8')

    code = replace(code, '    bool glsl;', '    unsigned mk8_blur_program;\n    bool glsl;')
    finish = '    if (c->glsl) return glsl_finish('
    code = replace(code, finish,
                   '    if (c->glsl && stage == STAGE_FRAGMENT && !dbg && !c->uses_rscale && key->rt_kind[0] == 0) {\n'
                   '        c->mk8_blur_program = mk8_blur_program(blob, max_size, end);\n'
                   '        if (c->mk8_blur_program == MK8_BLUR_REDUCTION) c->uses_rscale = true;\n    }\n'+finish)
    main = r'    sp(&s, castle ? "void mk8_castle_sample(vec4 v0) {\n" : "void main() {\n");'
    code = replace(code, main,
                   '    if (c->mk8_blur_program == MK8_BLUR_HORIZONTAL || c->mk8_blur_program == MK8_BLUR_VERTICAL) {\n'
                   '        info->uses_rscale = true;\n'
                   f'        sp(&s, {json.dumps(body)}, c->mk8_blur_program == MK8_BLUR_HORIZONTAL ? "true" : "false");\n'
                   r'        sp(&s, "void main() {\n  if (mk8_scaled_blur()) return;\n");'+'\n'
                   '    } else {\n'
                   f'        if (c->mk8_blur_program == MK8_BLUR_REDUCTION) sp(&s, "%s", {json.dumps(down)});\n'
                   '    '+main+'\n    }')

    state = 'static uint64_t g_cur_fs_hash;'
    draw = replace(draw, state, state+'\nstatic Mk8BlurSequence mk8_blur_sequence;\n'
                   'static unsigned mk8_blur_target;\nstatic int mk8_blur_lod;')
    reset = 'void vk_exec_reset(void) {'
    draw = replace(draw, reset, reset+'\n    mk8_blur_sequence = (Mk8BlurSequence){0};')
    begin = 'static void draw_(CmdRec *r, bool indexed) {'
    draw = replace(draw, begin, begin+'\n    Mk8BlurSequence pending_blur = mk8_blur_begin(&mk8_blur_sequence);')
    fs = '        g_cur_fs_hash = last_hash;'
    draw = replace(draw, fs, fs+'\n        mk8_blur_lod = -1;\n'
                   '        mk8_blur_target = mk8_blur_draw(&pending_blur, renderer_present_count(), last_hash);')
    issued = '        else vkCmdDrawIndexed(cb, icount, run_n[ri], 0, base_vertex, base_inst + run_at[ri]);'
    draw = replace(draw, issued, issued+'\n        if (run_n[ri] && (use_index ? icount : count)) mk8_blur_sequence = pending_blur;')
    desc = 'vk_sampler(&((NvnSamplerDesc *)st.spool->entries)[tsc])'
    draw = replace(draw, desc, 'vk_sampler(&((NvnSamplerDesc *)st.spool->entries)[tsc],\n'
                   '        stage == 1 && i == 0 && mk8_blur_target && si->ntex == 1 && si->uses_rscale &&\n'
                   '        (mk8_blur_target != MK8_BLUR_REDUCTION || !((NvnSamplerDesc *)st.spool->entries)[tsc].compare_mode) ? &mk8_blur_lod : NULL)')
    scale = '    rs[65][0] = g_mirror_w;'
    draw = replace(draw, scale, scale+'\n    if (stage == 1 && pre_ii && mk8_blur_target && mk8_blur_lod >= 0) {\n'
                   '        rs[1][2] = 1; rs[1][3] = (float)mk8_blur_lod;\n    }')

    # Read the effective descriptor, including the normal renderer's LOD overrides,
    # at this bind. No sampler handles or per-device lifetime state are retained.
    signature = 'VkSampler vk_sampler(NvnSamplerDesc *d)'
    extended = 'VkSampler vk_sampler(NvnSamplerDesc *d, int *locked_lod)'
    api = replace(api, signature, extended)
    sampler = replace(sampler, signature, extended)
    cached = '    if (d->backend) return (VkSampler)d->backend;'
    sampler = replace(sampler, cached, cached.replace('d->backend)', 'd->backend && !locked_lod)'))
    compare = '    if (d->compare_mode) {'
    sampler = replace(sampler, compare,
                      '    if (locked_lod) {\n'
                      '        *locked_lod = mk8_blur_locked_lod(si.minLod, si.maxLod,\n'
                      '            si.minFilter == VK_FILTER_LINEAR && si.magFilter == VK_FILTER_LINEAR);\n'
                      '        if (d->backend) return (VkSampler)d->backend;\n    }\n'+compare)
    shutil.copyfile(GRAPHICS/'castle_blur.h', nvn/'castle_blur.h')
    for path, contents in ((translate, INCLUDE+code), (executor, INCLUDE+draw),
                           (textures, INCLUDE+sampler), (header, api)):
        path.write_text(contents, encoding='utf-8')
