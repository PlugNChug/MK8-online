"""Preserve coverage of moving highlights in the Castle bloom extraction."""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def prepare(runtime):
    translate = runtime/'nvn/maxwell/translate.c'
    executor = runtime/'nvn/vulkan/exec.c'
    body = translate.read_text(encoding='utf-8')
    renderer = executor.read_text(encoding='utf-8')
    header = 'static ShaderInfo *glsl_finish(Ctx *c, bool any_branch, bool dbg) {'
    main = r'    sp(&s, "void main() {\n");'
    tint = '        if (tint)\n            sp(&s, "  if (ltint[0].w > 0.0f) { vec3'
    emit = 'if (c->glsl) return glsl_finish(c, any_branch, dbg);'
    scale = '    rs[65][0] = g_mirror_w;'
    ctx = '    bool glsl;'
    hash_state = 'static uint64_t g_cur_fs_hash;'
    bind = '    if (h && st.spool && tsc < (uint32_t)st.spool->count) smp ='
    if any(body.count(s) != 1 for s in (header, main, tint, emit, ctx)) or any(renderer.count(s) != 1 for s in (scale, hash_state, bind)):
        raise ValueError('Castle bloom shader boundary changed')
    helper = (ROOT/'runtime/mk8/graphics/castle_bloom.inc').read_text(encoding='utf-8')
    body = body.replace(header, helper+'\n'+header.replace('bool dbg)', 'bool dbg, bool castle)')+
                        '\n    if (castle) c->uses_rscale = true;')
    body = body.replace(emit, 'if (c->glsl && stage == STAGE_FRAGMENT && !dbg && key->rt_kind[0] == 0) {\n'
                        '        c->mk8_bloom_reduce = mk8_castle_reduce(blob, max_size, end);\n'
                        '        if (c->mk8_bloom_reduce) c->uses_rscale = true;\n    }\n'
                        '    if (c->glsl) return glsl_finish(c, any_branch, dbg, stage == STAGE_FRAGMENT && !dbg &&\n'
                        '        key->rt_kind[0] == 0 && mk8_castle_seed(blob, max_size, end));')
    body = body.replace(main, r'    sp(&s, castle ? "void mk8_castle_sample(vec4 v0) {\n" : "void main() {\n");')
    body = body.replace(tint, '        if (castle) mk8_castle_resolve(&s);\n'+tint)
    reduce = (ROOT/'runtime/mk8/graphics/bloom_reduce.glsl').read_text(encoding='utf-8')
    body = body.replace(ctx, ctx+'\n    bool mk8_bloom_reduce;')
    reduce_main = r'    sp(&s, castle ? "void mk8_castle_sample(vec4 v0) {\n" : "void main() {\n");'
    body = body.replace(reduce_main, '    if (c->mk8_bloom_reduce) sp(&s, "%s", '+json.dumps(reduce)+');\n'+reduce_main)
    renderer = renderer.replace(hash_state, hash_state+'\nstatic bool mk8_reduce_linear;')
    renderer = renderer.replace(bind, '    if (stage == 1 && i == 0 && g_cur_fs_hash == 0x9ad68b90ed7e6d80ull) {\n'
        '        NvnSamplerDesc *d = h && st.spool && tsc < (uint32_t)st.spool->count ? &((NvnSamplerDesc *)st.spool->entries)[tsc] : NULL;\n'
        '        mk8_reduce_linear = d && d->mag_filter == 1 && (d->min_filter == 1 || d->min_filter == 3 || d->min_filter == 5) && !d->compare_mode;\n'
        '    }\n'+bind)
    # Restrict the override to the GPU program recorded in the race, including its header and padding.
    renderer = renderer.replace(scale, scale+'\n    if (stage == 1 && (g_cur_fs_hash == 0x051303030c68828full ||\n'
        '        (g_cur_fs_hash == 0x9ad68b90ed7e6d80ull && pre_ii && mk8_reduce_linear))) rs[1][2] = 1;')
    translate.write_text(body, encoding='utf-8')
    executor.write_text(renderer, encoding='utf-8')
