"""Draws into a block-compressed texture go to an uncompressed image of its blocks, then are copied into it.

MK8 4.0.1 builds its specular reflection cube array (128x128, 8 mips, 5 cubes, NVN BC3) at race start by encoding BC3
blocks in a fragment shader: each level and layer is a colour target through an NVN texture view with an uncompressed
128-bit format, one texel per 4x4 block (viewport 32x32 at level 0). Upstream made that view on the BC3 Vulkan image
itself: an image without COLOR_ATTACHMENT usage or BLOCK_TEXEL_VIEW_COMPATIBLE, so what the driver wrote is undefined
(AMD accepts the view silently). The cube array is written only then and lights every reflective surface for the whole
race: blown-out asphalt, walls and karts in about half of the Grand Prix runs, depending on what the image's memory held.

Such a target is drawn into a shadow image of the view's format sized to the level's block grid, and when the pass ends
the result is copied into that level and layer of the compressed image (vkCmdCopyImage between size-compatible formats:
the extent is in source texels, one per block). MK8_BC_RENDER_DIRECT=1 restores upstream's view.
"""

HELPERS = '''// MK8 (toolchain/bc_render.py): colour targets that are views of block-compressed images
static bool mk8_bc_direct(void) {
    static int on = -1;
    if (on < 0) on = getenv("MK8_BC_RENDER_DIRECT") != NULL;
    return on;
}
static uint32_t mk8_bc_block_bytes(VkFormat f) {
    if (f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC1_RGBA_SRGB_BLOCK) return 8;
    if (f == VK_FORMAT_BC4_UNORM_BLOCK || f == VK_FORMAT_BC4_SNORM_BLOCK) return 8;
    if (f >= VK_FORMAT_BC2_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK) return 16;
    return 0;
}
static uint32_t mk8_bc_texel_bytes(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT: case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
    case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32_SFLOAT:
    case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_SNORM: return 8;
    default: return 0;
    }
}
typedef struct { VkFormat fmt; uint32_t w, h; VkImage image; VmaAllocation mem; VkImageView view; } Mk8BcShadow;
static Mk8BcShadow mk8_bc_shadows[32];
static int mk8_bc_nshadows;
static struct { TexBackend *dst; VkImage src; uint32_t level, layer, w, h; } mk8_bc_copies[8];
static int mk8_bc_ncopies;
// The shadow image for a level of w x h blocks in the view's format (made once, kept: they are small).
static Mk8BcShadow *mk8_bc_shadow(VkCommandBuffer cb, VkFormat fmt, uint32_t w, uint32_t h) {
    for (int i = 0; i < mk8_bc_nshadows; i++)
        if (mk8_bc_shadows[i].fmt == fmt && mk8_bc_shadows[i].w == w && mk8_bc_shadows[i].h == h) return &mk8_bc_shadows[i];
    if (mk8_bc_nshadows == 32) return NULL;
    Mk8BcShadow *s = &mk8_bc_shadows[mk8_bc_nshadows];
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(g_vk.vma, &ci, &ai, &s->image, &s->mem, NULL) != VK_SUCCESS) return NULL;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = s->image,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(g_vk.dev, &vi, NULL, &s->view) != VK_SUCCESS) {
        vmaDestroyImage(g_vk.vma, s->image, s->mem);
        return NULL;
    }
    vk_image_init_layout(cb, s->image, VK_IMAGE_ASPECT_COLOR_BIT);
    s->fmt = fmt; s->w = w; s->h = h;
    mk8_bc_nshadows++;
    return s;
}
// Colour target i of the pass being begun: a block-compressed image drawn through an uncompressed view of its blocks
// is drawn into a shadow image instead (its view returned, its extent in *w x *h), copied in when the pass ends.
static VkImageView mk8_bc_target(VkCommandBuffer cb, int i, TexBackend *b, VkImageView v, uint32_t *w, uint32_t *h) {
    uint32_t block = mk8_bc_block_bytes(b->format);
    if (!block || mk8_bc_direct() || !st.rt.has_cview[i] || !(st.rt.cview[i].setmask & 4)) return v;
    VkFormat vf = vk_format(st.rt.cview[i].format);
    uint32_t texel = mk8_bc_texel_bytes(vf);
    static int nlog;
    if (texel != block) {
        if (nlog++ < 8) LOG("[mk8:bc] colour target over BC format %d through format %d: not size-compatible, left as is\\n", b->format, vf);
        return v;
    }
    uint32_t lv = st.rt.cview[i].setmask & 1 ? st.rt.cview[i].base_level : 0;
    uint32_t ly = st.rt.cview[i].setmask & 2 ? st.rt.cview[i].min_layer : 0;
    if (lv >= b->levels || ly >= b->layers || mk8_bc_ncopies == 8) return v;
    uint32_t bw = ((b->width >> lv ? b->width >> lv : 1) + 3) / 4, bh = ((b->height >> lv ? b->height >> lv : 1) + 3) / 4;
    Mk8BcShadow *s = mk8_bc_shadow(cb, vf, bw, bh);
    if (!s) return v;
    if (nlog++ < 8) LOG("[mk8:bc] BC format %d level %u layer %u drawn as %ux%u format %d, then copied in\\n", b->format, lv, ly, bw, bh, vf);
    mk8_bc_copies[mk8_bc_ncopies++] = (typeof(mk8_bc_copies[0])){b, s->image, lv, ly, bw, bh};
    *w = bw; *h = bh;
    return s->view;
}
// After the pass: its shadow images into their compressed levels.
static void mk8_bc_copy_back(VkCommandBuffer cb) {
    if (!mk8_bc_ncopies) return;
    vk_full_barrier(cb);
    for (int k = 0; k < mk8_bc_ncopies; k++) {
        VkImageCopy c = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                         .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mk8_bc_copies[k].level, mk8_bc_copies[k].layer, 1},
                         .extent = {mk8_bc_copies[k].w, mk8_bc_copies[k].h, 1}};
        vkCmdCopyImage(cb, mk8_bc_copies[k].src, VK_IMAGE_LAYOUT_GENERAL, mk8_bc_copies[k].dst->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    }
    vk_full_barrier(cb);
    mk8_bc_ncopies = 0;
}

'''
END_PASS = ('    vkCmdEndRendering(vk_cmd());\n    in_pass = false;\n',
            '    vkCmdEndRendering(vk_cmd());\n    in_pass = false;\n'
            '    mk8_bc_copy_back(vk_cmd());  // (toolchain/bc_render.py)\n')
TARGET = ('        TexBackend *b = t->backend;\n        if (!v) continue;\n        ca[i].imageView = v;\n',
          '        TexBackend *b = t->backend;\n        if (!v) continue;\n'
          '        uint32_t mk8_bcw = 0, mk8_bch = 0;  // (toolchain/bc_render.py)\n'
          '        v = mk8_bc_target(cb, i, b, v, &mk8_bcw, &mk8_bch);\n'
          '        ca[i].imageView = v;\n')
EXTENT = ('        if (g_debug_frame) mk8_pass_mip[i] = lv < b->levels ? lv : b->levels - 1;\n'
          '        uint32_t ew = level_extent(b->width, lv), eh = level_extent(b->height, lv);\n',
          '        if (g_debug_frame) mk8_pass_mip[i] = lv < b->levels ? lv : b->levels - 1;\n'
          '        uint32_t ew = mk8_bcw ? mk8_bcw : level_extent(b->width, lv), eh = mk8_bch ? mk8_bch : level_extent(b->height, lv);\n')
BOUNDARY = 'static void begin_pass(void) {\n'


def replace(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f'Block-compressed target boundary changed: {old.strip()[:80]}')
    return text.replace(old, new)


def prepare(runtime):
    execute = runtime/'nvn/vulkan/exec.c'
    text = execute.read_text(encoding='utf-8')
    for old, new in (TARGET, EXTENT, END_PASS):
        text = replace(text, old, new)
    # (vk_end_pass_at comes before begin_pass: the helpers go ahead of both)
    anchor = 'static void vk_end_pass_at(int line) {\n'
    text = replace(text, anchor, HELPERS + anchor)
    execute.write_text(text, encoding='utf-8')
