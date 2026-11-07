"""A texture first rendered to while its guest memory is all zeros starts as zeros, not as undefined contents.

Upstream's first_render_use uploads the guest memory of a texture rendered to before it was ever sampled, but only
when that memory holds data; zeroed memory left the new Vulkan image undefined. MK8 4.0.1 renders 39 32x32 R11G11B10F
environment cubes and filters each from the previous frame's cube: unwritten mip levels decoded as Inf/NaN in some
runs (automated boot 83, frame 2316: only level 1 of those cubes non-finite) and the feedback kept them, blowing out
the whole race (boots 68, 71, 78). Clearing every new colour image instead (screen-sized scaled targets included)
made it worse (boots 80, 82); this applies only where the guest memory is known to be zero.
"""

HELPER = '''
// MK8: zeroed guest memory -> zeroed image (see toolchain/texture_init.py). Colour formats that can be drawn to only.
static void mk8_zero_render_image(TexBackend *b) {
    if (!b->image || b->aspect != VK_IMAGE_ASPECT_COLOR_BIT || !format_supports(b->format, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))
        return;
    VkCommandBuffer cb = vk_upload_cmd();
    VkClearColorValue zero = {0};
    VkImageSubresourceRange rg = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    vkCmdClearColorImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rg);
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}
'''

BOUNDARY = 'static void first_render_use(NvnTexture *t, TexBackend *b) {\n'
UPLOAD = '    if (data) upload_texture(t, b);\n}\n'
# (Test) MK8_FIRST_RENDER_ZERO=1: a texture first drawn to starts zeroed even when its memory holds data. GPU drawing
# stays in Vulkan images and never reaches guest memory, so that data may be what the CPU wrote there long before
# (boot 93: 0x00ffffff, white RGBA8 read as R11G11B10F, outside the HDR target's cleared rectangle).
ZERO = '''    static int mk8_first_zero = -1;
    if (mk8_first_zero < 0) mk8_first_zero = getenv("MK8_FIRST_RENDER_ZERO") != NULL;
    if (data && !mk8_first_zero) upload_texture(t, b);
    else mk8_zero_render_image(b);  /* the memory holds zeros: so does the image */
}
'''


def replace(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f'First render use boundary changed: {old[:90]}')
    return text.replace(old, new)


CREATED = '    vk_image_init_layout(vk_upload_cmd(), b->image, b->aspect);\n    if (depthfmt) {\n'
# (Test) MK8_ZERO_NEW_TARGETS=1: every new or re-created colour image that can be drawn to starts zeroed. Boot 93: the
# 1440x810 HDR target first used at frame 2322 held 0x00ffffff (non-finite) outside the scissor its clear covers.
CREATED_ZERO = '''    vk_image_init_layout(vk_upload_cmd(), b->image, b->aspect);
    {
        static int zero_new = -1;
        if (zero_new < 0) zero_new = getenv("MK8_ZERO_NEW_TARGETS") != NULL;
        // An upload belongs to the old image. GPU-owned HDR images have no guest copy to restore.
        b->uploaded = false;
        bool hdr_recreated = b->gpu_owned && b->scalable && b->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        if ((zero_new || hdr_recreated) && !depthfmt && (ci.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
            mk8_zero_render_image(b);
    }
    if (depthfmt) {
'''


def prepare(runtime):
    textures = runtime/'nvn/vulkan/textures.c'
    text = textures.read_text(encoding='utf-8')
    start = text.index(BOUNDARY) if text.count(BOUNDARY) == 1 else -1
    if start < 0 or text.count(UPLOAD) != 1 or text.index(UPLOAD) < start:
        raise ValueError('First render use boundary changed')
    # (Zeroing screen-sized scaled targets too did not change the blown-out runs: boots 98-104, 2 of 7. Not applied.)
    text = text.replace(BOUNDARY, HELPER + '\n' + BOUNDARY)
    text = replace(text, BOUNDARY + '    if (b->gpu_owned || b->uploaded || b->scalable', BOUNDARY + '''    bool reflection = t->target == 8 && t->width == 32 && t->height == 32 && b->levels > 1;
    if ((b->scalable || reflection) && !b->gpu_owned && !b->uploaded && b->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32) {
        // Reflection passes fill the cube in stages and can read mips still awaiting their first draw.
        mk8_zero_render_image(b);
        return;
    }
    if (b->gpu_owned || b->uploaded || b->scalable''')
    text = replace(text, UPLOAD, ZERO)
    text = replace(text, CREATED, CREATED_ZERO)
    # (image creation comes before first_render_use and its helper: declare the helper ahead of both)
    anchor = '// Images destroyed by the guest are kept until the submissions that may use them have completed.\n'
    text = replace(text, anchor, 'static void mk8_zero_render_image(TexBackend *b);\n' + anchor)
    textures.write_text(text, encoding='utf-8')
