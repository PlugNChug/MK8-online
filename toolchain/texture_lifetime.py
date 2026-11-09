"""Retain texture objects named by recorded commands in the pinned C runtime."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

COMMAND_RETIRE = '''// MK8: retain the existing graveyard delay; finalization retires metadata, never GPU images directly.
static void mk8_retire_command_buffer(NvnCmdBuf *cb) {
    pthread_mutex_lock(&list_lock);
    for (unsigned i = 0; i < 64; i++) {
        if (cb->calls_only[i]) bury(cb->calls_only[i]);
        cb->calls_only[i] = NULL;
    }
    pthread_mutex_unlock(&list_lock);
}

void mk8_commands_pool_finalized(uintptr_t pool, uint64_t storage, uint64_t size) {
    if (!storage || !size) return;
    if (storage < GUEST_LO || storage >= GUEST_HI || size > GUEST_HI - storage)
        FATAL("Command memory pool range outside guest memory\\n");
    pthread_mutex_lock(&list_lock);
    uint64_t before = grave_head;
    // Another pool may alias this storage. Only retire lists recorded from this pool object.
    uint64_t first = storage >> PAGE_SHIFT_L, last = (storage + size - 1) >> PAGE_SHIFT_L;
    for (uint64_t page = first; page <= last; page++)
        for (ListInfo **pp = &list_buckets[bucket(page)]; *pp;) {
            ListInfo *l = *pp;
            if ((l->start >> PAGE_SHIFT_L) == page && l->pool == pool) {
                *pp = l->next;
                bury(l->mem);
                free(l);
            } else pp = &l->next;
        }
    uint64_t n = grave_head - before;
    pthread_mutex_unlock(&list_lock);
    if (n && getenv("MK8_TRACE_VRAM"))
        LOG("[mk8:vram] command pool finalized %llx+%llx: %llu lists retired\\n",
            (unsigned long long)storage, (unsigned long long)size, (unsigned long long)n);
}

'''


def replace(text, old, new, count=1):
    if text.count(old) != count:
        raise ValueError(f'Texture ownership boundary changed: {old[:90]}')
    return text.replace(old, new)


def prepare(runtime):
    header = runtime/'nvn/nvn.h'
    text = header.read_text(encoding='utf-8')
    text = replace(text, '    char label[64];\n} NvnTexture;', '    char label[64];\n    unsigned mk8_refs;\n} NvnTexture;\nNvnTexture *mk8_texture_retain(NvnTexture *t);\nvoid mk8_texture_release(NvnTexture *t);')
    text = replace(text, '    uint8_t extra[];', '    struct Mk8TextureRefs *mk8_textures;\n    uint8_t extra[];')
    text = replace(text, '    uint8_t *calls_only[64];', '    uintptr_t mk8_cmd_pool;  // owner identity of command memory\n    uint8_t *calls_only[64];')
    header.write_text(text, encoding='utf-8')

    objects = runtime/'nvn/objects.c'
    text = objects.read_text(encoding='utf-8')
    text = replace(text, '    NvnTexture *t = calloc(1, sizeof *t);', '    NvnTexture *t = calloc(1, sizeof *t);\n    if (!t) FATAL("Texture allocation failed\\n");\n    t->mk8_refs = 1;')
    text = replace(text, 'backend_texture_destroyed(t); free(t);', 'mk8_texture_release(t);')
    text = replace(text, '    e->tex = HOST(NvnTexture, A(2));', '    NvnTexture *next = mk8_texture_retain(HOST(NvnTexture, A(2)));\n    mk8_texture_release(e->tex);\n    e->tex = next;')
    text = replace(text, '''NVN(nvnMemoryPoolFinalize) {
    NvnPool *p = HOST(NvnPool, A(0));
''', '''NVN(nvnMemoryPoolFinalize) {
    NvnPool *p = HOST(NvnPool, A(0));
    extern void mk8_commands_pool_finalized(uintptr_t pool, uint64_t storage, uint64_t size);
    if (p) mk8_commands_pool_finalized((uintptr_t)p, p->storage, p->size);
''')
    text += '\n'+(ROOT/'runtime/mk8/graphics/texture_refs.inc').read_text(encoding='utf-8')
    objects.write_text(text, encoding='utf-8')

    commands = runtime/'nvn/cmdbuf.c'
    text = commands.read_text(encoding='utf-8')
    text = replace(text, '#include "nvn.h"', '#include "nvn.h"\n'+(ROOT/'runtime/mk8/graphics/command_texture_refs.inc').read_text(encoding='utf-8'))
    text = replace(text, 'static NvnCmdBuf *cb_of(',
                   'static void mk8_retire_command_buffer(NvnCmdBuf *cb);\nstatic NvnCmdBuf *cb_of(')
    text = replace(text, '    if (cb) free(cb->buf);',
                   '    if (cb) { mk8_retire_command_buffer(cb); mk8_release_records(cb->buf, cb->buf_used); free(cb->buf); }')
    text = replace(text, "    free(cb);  // (its calls-only lists are left to the graveyard's limit: they may still be called)",
                   '    free(cb);  // its remaining lists were transferred to the graveyard')
    text = replace(text, 'free(graveyard[grave_tail++ % NGRAVE].mem)', 'mk8_free_list(graveyard[grave_tail++ % NGRAVE].mem)', 2)
    text = replace(text, 'static uint64_t est_used(', COMMAND_RETIRE + 'static uint64_t est_used(')
    text = replace(text, '    uint64_t start, end;  // guest command memory it used (estimate)',
                   '    uintptr_t pool;  // original NvnPool identity, not storage shared with another pool\n'
                   '    uint64_t start, end;  // guest command memory it used (estimate)')
    text = replace(text, '    cb->cmd_mem = HOST(NvnPool, A(1))->storage + A(2);',
                   '    cb->mk8_cmd_pool = (uintptr_t)HOST(NvnPool, A(1));\n'
                   '    cb->cmd_mem = HOST(NvnPool, A(1))->storage + A(2);')
    text = replace(text, '        l->start = start, l->end = end, l->mem = list;',
                   '        l->pool = cb->mk8_cmd_pool;\n        l->start = start, l->end = end, l->mem = list;')
    text = replace(text, '    cb->recording = true;\n    cb->buf_used = 0;', '    mk8_release_records(cb->buf, cb->buf_used);\n    cb->recording = true;\n    cb->buf_used = 0;')
    text = replace(text, '    uint8_t *list = malloc(cb->buf_used);', '    uint8_t *list = malloc(cb->buf_used);\n    if (!list) FATAL("nvn: out of host memory copying %llu command bytes\\n", (unsigned long long)cb->buf_used);')
    text = replace(text, '        ListInfo *l = malloc(sizeof *l);', '        ListInfo *l = malloc(sizeof *l);\n        if (!l) FATAL("nvn: out of host memory tracking a command list\\n");')
    text = replace(text, '    RET((uint64_t)(uintptr_t)list);', '    cb->buf_used = 0;  // references now belong to the immutable list\n    RET((uint64_t)(uintptr_t)list);')
    text = replace(text, '            memcpy(d, (void *)(uintptr_t)p, size);', '            memcpy(d, (void *)(uintptr_t)p, size);\n            mk8_copy_command_textures(d, r);')
    text = replace(text, '    memcpy(r->extra, &rt, sizeof rt);', '    for (int i = 0; i < n; i++) mk8_command_texture(r, rt.tex[i]);\n    mk8_command_texture(r, rt.depth);\n    memcpy(r->extra, &rt, sizeof rt);')
    text = replace(text, '    memcpy(r->extra, &ct, sizeof ct);', '    mk8_command_texture(r, ct.tex);\n    memcpy(r->extra, &ct, sizeof ct);')
    text = replace(text, '    memcpy(r->extra, &t, sizeof t);', '    mk8_command_texture(r, t.tex);\n    memcpy(r->extra, &t, sizeof t);', 2)
    text = replace(text, '    memcpy(r->extra, t, sizeof t);', '    mk8_command_texture(r, t[0].tex);\n    mk8_command_texture(r, t[1].tex);\n    memcpy(r->extra, t, sizeof t);', 3)
    commands.write_text(text, encoding='utf-8')

    execute = runtime/'nvn/vulkan/exec.c'
    text = execute.read_text(encoding='utf-8')
    text = replace(text, 'static void st_defaults(void) {', '''static void mk8_rt_retain(const RtState *rt) {
    for (int i = 0; i < rt->ncolor; i++) mk8_texture_retain(rt->color[i]);
    mk8_texture_retain(rt->depth);
}
static void mk8_rt_release(const RtState *rt) {
    for (int i = 0; i < rt->ncolor; i++) mk8_texture_release(rt->color[i]);
    mk8_texture_release(rt->depth);
}
static void st_defaults(void) {
    mk8_rt_release(&st.rt);''')
    text = replace(text, '            s->rt.has_dview = rt->has_dview;', '''            s->rt.has_dview = rt->has_dview;
            if (s == &st) {
                mk8_rt_retain(&s->rt);
                if (memcmp(&prev, &s->rt, sizeof prev)) vk_end_pass();
                mk8_rt_release(&prev);
            }''')
    text = replace(text, '''            if (!nomemo && memo[ms].tex == e->tex && memo[ms].vh == vh && memo[ms].frame == frame && memo[ms].gen == vk_view_gen &&
                memo[ms].type == st_->type && memo[ms].has_view == e->has_view) {''', '''            bool cached = !nomemo && memo[ms].tex == e->tex && memo[ms].vh == vh && memo[ms].frame == frame &&
                          memo[ms].gen == vk_view_gen && memo[ms].type == st_->type && memo[ms].has_view == e->has_view;
            // Hits still need uploads and rescaling. On misses vk_view already does this work.
            if (cached) vk_tex(naliases ? alias_read(e->tex) : e->tex, false);
            if (cached && memo[ms].gen == vk_view_gen) {''')
    text = replace(text, 'static VkImageView feedback_view[4];', 'static TexBackend *feedback_copy[4];')
    text = replace(text, 'static bool snap_ok[8];', 'static bool snap_ok[8];\nstatic VkImage mk8_snap_sources[8];')
    text = replace(text, '''    for (int i = 0; i < 8; i++) if ((!t || snap_tex[i] == t) && snap_ok[i]) { snap_ok[i] = false; snap_why[i] = snap_reason; }''', '''    TexBackend *b = t ? t->backend : NULL;
    for (int i = 0; i < 8; i++) {
        if (snap_ok[i] && (!t || snap_tex[i] == t || (b && b->image && mk8_snap_sources[i] == b->image))) {
            snap_ok[i] = false;
            snap_why[i] = snap_reason;
        }
    }''')
    text = replace(text, 'typedef struct { NvnTexture *key; TexBackend copy; } Snap;',
                   'typedef struct { NvnTexture *key; uint64_t key_id; VkImage source; TexBackend copy; } Snap;')
    text = replace(text, '    TexBackend *d = &s->copy;',
                   '    s->key_id = t->debug_id;\n    s->source = src->image;\n    TexBackend *d = &s->copy;')
    text = replace(text, '    if (d->image && (d->width != src->width || d->height != src->height || d->format != src->format || d->levels != nlv)) {',
                   '    if (d->image && (d->width != src->width || d->height != src->height || d->format != src->format ||\n'
                   '                     d->levels != nlv || d->samples != src->samples)) {')
    text = replace(text, '            if (!nosnap && si >= 0 && (snap_ok[si] || self_write) && existing) {',
                   '            if (!nosnap && si >= 0 && (snap_ok[si] || self_write) && existing &&\n'
                   '                existing->key_id == t->debug_id && existing->source == src->image) {')
    text = replace(text, '            snap_ok[si] = true;', '            mk8_snap_sources[si] = src->image;\n            snap_ok[si] = true;')
    text = replace(text, '''            VkImageAspectFlags sa = src->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : src->aspect;
            static const VkComponentMapping id = {0};
''', '')
    text = replace(text, '''                feedback_view[nfeedback++] = vk_image_view(&existing->copy, VK_IMAGE_VIEW_TYPE_2D, src->format, 0,
                                                           existing->copy.levels, 0, 1, id, sa);''',
                   '                feedback_copy[nfeedback++] = &existing->copy;')
    text = replace(text, '            feedback_view[nfeedback++] = vk_image_view(dst, VK_IMAGE_VIEW_TYPE_2D, src->format, 0, dst->levels, 0, 1, id, sa);',
                   '            feedback_copy[nfeedback++] = dst;')
    text = replace(text, '''                    view = feedback_view[k];
                    if (st_->type == TEXTYPE_2D_ARRAY) view = vk_dummy_view(TEXTYPE_2D_ARRAY);  // (2D snapshot as array: rare)''', '''                    if (st_->type == TEXTYPE_2D_ARRAY) {
                        view = vk_dummy_view(TEXTYPE_2D_ARRAY);
                    } else {
                        TexBackend *copy = feedback_copy[k];
                        // A bloom draw can sample one mip while rendering into another mip of the same texture.
                        bool mip_view = e->has_view && (e->view.setmask & 1);
                        uint32_t base = mip_view ? e->view.base_level : 0;
                        uint32_t count = mip_view ? e->view.num_levels : 0;
                        if (base >= copy->levels) base = copy->levels - 1;
                        if (!count || count > copy->levels - base) count = copy->levels - base;
                        VkImageAspectFlags aspect = copy->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : copy->aspect;
                        view = vk_image_view(copy, VK_IMAGE_VIEW_TYPE_2D, copy->format, base, count, 0, 1,
                                             (VkComponentMapping){0}, aspect);
                    }''')
    text = replace(text, 'static NvnTexture *g_pass_color[8];',
                   'static uint32_t mk8_pass_mip[8];\nstatic uint64_t mk8_pass_shader, mk8_pass_shader_hash;\n'
                   'static NvnTexture *g_pass_color[8];')
    text = replace(text, '    memcpy(g_pass_color, st.rt.color, sizeof g_pass_color);',
                   '    memcpy(g_pass_color, st.rt.color, sizeof g_pass_color);\n    mk8_pass_shader = mk8_pass_shader_hash = 0;\n'
                   '    if (g_debug_frame) memset(mk8_pass_mip, 0, sizeof mk8_pass_mip);')
    text = replace(text, '''        uint32_t lv = st.rt.has_cview[i] && (st.rt.cview[i].setmask & 1) ? st.rt.cview[i].base_level : 0;
        uint32_t ew''', '''        uint32_t lv = st.rt.has_cview[i] && (st.rt.cview[i].setmask & 1) ? st.rt.cview[i].base_level : 0;
        if (g_debug_frame) mk8_pass_mip[i] = lv < b->levels ? lv : b->levels - 1;
        uint32_t ew''')
    text = replace(text, '    stat_draws++;\n    cum_draws++;', '''    if (g_debug_frame) {
        mk8_pass_shader = st.program->stages[1].code_addr;
        mk8_pass_shader_hash = g_cur_fs_hash;
    }
    stat_draws++;
    cum_draws++;''')
    text = replace(text, '        if (scan < 0) scan = getenv("LARECOMP_DEBUG_NAN_SCAN") != NULL;', '''        if (scan < 0) {
            const char *mode = getenv("LARECOMP_DEBUG_NAN_SCAN");
            scan = mode ? (!strcmp(mode, "mip") ? 2 : 1) : 0;
        }''')
    text = replace(text, '''            for (uint32_t lv = 0; lv < b->levels; lv++)
                for (uint32_t ly = 0; ly < b->layers; ly++) {  // (every level and layer: cube faces, mips)''', '''            bool bound_only = scan == 2 && b->layers == 1;
            uint32_t first_lv = bound_only ? mk8_pass_mip[i] : 0;
            uint32_t end_lv = bound_only ? first_lv + 1 : b->levels;
            for (uint32_t lv = first_lv; lv < end_lv; lv++)
                for (uint32_t ly = 0; ly < b->layers; ly++) {''')
    text = replace(text, '''%llu non-finite texels:%s\\n", i, (void *)g_pass_color[i],
                                 w, h, lv, ly, (unsigned long long)bad, first);''', '''%llu non-finite texels (bound mip %u, last fs %llx):%s\\n", i, (void *)g_pass_color[i],
                                 w, h, lv, ly, (unsigned long long)bad, mk8_pass_mip[i],
                                 (unsigned long long)mk8_pass_shader, first);''')
    text = replace(text, '                    uint64_t bad = 0;\n                    char first[256]',
                   '                    uint64_t bad = 0;\n                    uint32_t max_r = 0, max_g = 0, max_b = 0;\n                    char first[256]')
    text = replace(text, '                            bad++;\n                        }\n                    }\n                    if (bad) LOG("[nan]',
                   '''                            bad++;
                        } else if (bound_only) {
                            // Unsigned packed floats sort by magnitude, so retain the exact channel encodings.
                            uint32_t r = v & 0x7ff, g = (v >> 11) & 0x7ff, blue = v >> 22;
                            if (r > max_r) max_r = r;
                            if (g > max_g) max_g = g;
                            if (blue > max_b) max_b = blue;
                        }
                    }
                    if (bound_only) LOG("[mk8:hdr] %p %ux%u mip %u fs %llx hash %016llx max %03x/%03x/%03x bad %llu\\n",
                                        (void *)g_pass_color[i], w, h, lv, (unsigned long long)mk8_pass_shader,
                                        (unsigned long long)mk8_pass_shader_hash, max_r, max_g, max_b, (unsigned long long)bad);
                    if (bad) LOG("[nan]''')
    text = replace(text, '''    VkRect2D rect = scissor_rect();
    if (!rect.extent.width || !rect.extent.height) return;
    if (index >= 0 && (index >= (int)pass_ncolor || pass_color[index] == VK_FORMAT_UNDEFINED)) return;''', '''    VkRect2D rect = scissor_rect();
    if (!rect.extent.width || !rect.extent.height) return;
    if (index >= 0 && (index >= (int)pass_ncolor || pass_color[index] == VK_FORMAT_UNDEFINED)) return;
    if (g_debug_frame && index >= 0 && pass_color[index] == VK_FORMAT_B10G11R11_UFLOAT_PACK32) {
        const NvnTexture *target = st.rt.color[index];
        LOG("[mk8:hdr-clear] tex %p guest %ux%u scissor %d %d %d %d -> %d %d %u %u of %ux%u\\n",
            (const void *)target, target->width, target->height, st.scissor[0][0], st.scissor[0][1],
            st.scissor[0][2], st.scissor[0][3], rect.offset.x, rect.offset.y,
            rect.extent.width, rect.extent.height, pass_w, pass_h);
    }''')
    execute.write_text(text, encoding='utf-8')
