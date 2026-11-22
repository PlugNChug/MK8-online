"""Prepare real R32 array shadow descriptors outside the active rendering pass."""
from pathlib import Path


def adapt(api, code, draw, replace):
    impl = (Path(__file__).resolve().parents[1] / 'runtime/mk8/graphics/shadow_array.inc').read_text(encoding='utf-8')
    header = 'VkImageView vk_shadow_view(NvnTexture *t);'
    api = replace(api, header, header + '''
typedef struct { VkImageView depth; bool failed; } Mk8ShadowArrayResult;
Mk8ShadowArrayResult mk8_shadow_array_prepare(NvnTexture *t, const NvnTextureView *v, bool has);
void mk8_shadow_array_written(VkImage src);''')
    boundary = 'static void destroy_image(TexBackend *b) {'
    code = replace(code, boundary, impl + '\n' + boundary + '\n    forget_shadow_arrays(b->image);')
    upload = '    b->data_hash = h;\n    b->uploaded = true;'
    code = replace(code, upload, '    if (b->format == VK_FORMAT_R32_SFLOAT) mk8_shadow_array_written(b->image);\n' + upload)
    entry = 'VkImageView vk_view(NvnTexture *t, const NvnTextureView *v, bool has_view, bool render, int want_textype)'
    code = replace(code, entry + ' {', entry.replace('VkImageView vk_view', 'static VkImageView resolve_view')[:-1] +
                   ', Mk8ShadowArrayResult *shadow) {')
    # Use the exact range chosen by vk_view, including its one-layer 2D-as-array branch.
    line = '        return vk_image_view(b, want, fmt, lv0, nlv, ly0, nly, sw, aspect);'
    code = replace(code, line, '''        if (shadow && want == VK_IMAGE_VIEW_TYPE_2D_ARRAY) {
            shadow->depth = shadow_array_copy(b,fmt,sw,(VkImageSubresourceRange){aspect,lv0,nlv,ly0,nly},&shadow->failed);
            return shadow->depth;
        }
''' + line)
    array = '''    if (want == VK_IMAGE_VIEW_TYPE_2D_ARRAY && (have == VK_IMAGE_VIEW_TYPE_2D || have == VK_IMAGE_VIEW_TYPE_CUBE))
        return vk_image_view(b, want, fmt, lv0, nlv, ly0, have == VK_IMAGE_VIEW_TYPE_CUBE ? nly : 1, sw, aspect);'''
    code = replace(code, array, '''    if (want == VK_IMAGE_VIEW_TYPE_2D_ARRAY && (have == VK_IMAGE_VIEW_TYPE_2D || have == VK_IMAGE_VIEW_TYPE_CUBE)) {
        if (have == VK_IMAGE_VIEW_TYPE_2D) nly = 1;
        if (shadow) {
            shadow->depth = shadow_array_copy(b,fmt,sw,(VkImageSubresourceRange){aspect,lv0,nlv,ly0,nly},&shadow->failed);
            return shadow->depth;
        }
        return vk_image_view(b, want, fmt, lv0, nlv, ly0, nly, sw, aspect);
    }''')
    next_fn = '// A colour texture sampled with depth comparison'
    code = replace(code, next_fn, entry + ''' {
    return resolve_view(t,v,has_view,render,want_textype,NULL);
}
Mk8ShadowArrayResult mk8_shadow_array_prepare(NvnTexture *t, const NvnTextureView *v, bool has) {
    Mk8ShadowArrayResult result = {0};
    resolve_view(t,v,has,false,TEXTYPE_2D_ARRAY,&result);
    return result;
}

''' + next_fn)
    invalid = 'static void snapshots_invalidate(NvnTexture *t) {\n    TexBackend *b = t ? t->backend : NULL;'
    draw = replace(draw, invalid, invalid + '''
    if (!t || (b && b->format == VK_FORMAT_R32_SFLOAT)) mk8_shadow_array_written(b ? b->image : VK_NULL_HANDLE);''')
    # Build the complete fragment descriptor immediately from the copy made for this draw.
    # Resolving the color view again can return a new handle for the same image and range.
    entry = 'static void texture_info(int stage, ShaderInfo *si, int i, VkDescriptorImageInfo *out, float sc[2], uint32_t wh[2])'
    draw = replace(draw, entry + ' {', entry.replace('texture_info(', 'resolved_texture_info(')[:-1] +
                   ', VkImageView prepared_depth) {')
    memo = '            // Per-frame memo:'
    draw = replace(draw, memo, '''            if (prepared_depth) {
                view = prepared_depth;
            } else {
''' + memo)
    scale = '            renderer_texture_scale(e->tex, &sc[0], &sc[1]);'
    draw = replace(draw, scale, '            }\n' + scale)
    end = '// Writes the descriptor set of one stage.'
    draw = replace(draw, end, entry + ''' {
    resolved_texture_info(stage,si,i,out,sc,wh,VK_NULL_HANDLE);
}

''' + end)
    draw = replace(draw, '    VkDescriptorImageInfo fii[64];', '')
    anchor = '    alias_draw_check();\n    resolve_feedback(pr);'
    draw = replace(draw, anchor, anchor + '''
    VkDescriptorImageInfo fii[64];
    uint64_t prepared_arrays = 0;
    for (int i = 0; i < pr->fi->ntex && i < 64; ++i) {
        ShaderTexture *slot = &pr->fi->tex[i];
        if (!slot->shadow || slot->type != TEXTYPE_2D_ARRAY) continue;
        int offset = ((int)slot->handle_offset - 0x20) / 8;
        if (offset < 0 || offset >= 32 || !st.tpool) continue;
        uint64_t handle = st.tex[1][offset];
        uint32_t tic = handle & 0xfffff;
        if (!handle || tic >= (uint32_t)st.tpool->count) continue;
        const NvnTexEntry *e = mk8_tex_entry(st.tpool,tic,slot->type);
        if (!e->tex || is_attachment(e->tex)) continue;
        NvnTexture *src = naliases ? alias_read(e->tex) : e->tex;
        if (is_attachment(src) || vk_format(src->format) != VK_FORMAT_R32_SFLOAT) continue;
        Mk8ShadowArrayResult result = mk8_shadow_array_prepare(src,&e->view,e->has_view);
        if (result.failed) {
            static bool warned;
            if (!warned) LOG("[vk] unable to create the selected R32 shadow-array depth copy; skipping draw\\n");
            warned = true;
            SKIP_DRAW();
        }
        if (result.depth) {
            resolved_texture_info(1,pr->fi,i,&fii[i],tex_sc[1][i],tex_wh[1][i],result.depth);
            prepared_arrays |= UINT64_C(1) << i;
        }
    }''')
    lookup = '        texture_info(1, pr->fi, i, &fii[i], sc, wh);'
    draw = replace(draw, lookup, '        if (prepared_arrays & (UINT64_C(1) << i)) continue;\n' + lookup)
    return api, code, draw
