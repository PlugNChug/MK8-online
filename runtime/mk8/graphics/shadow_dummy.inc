// These views belong to the device for as long as the dummy images do.
// They must not be evicted from the ordinary texture-view cache.
static VkImageView dummy_views[2][10];

bool vk_is_color_dummy_view(VkImageView view) {
    if (!view) return false;
    for (unsigned i = 0; i < 10; i++)
        if (view == dummy_views[0][i]) return true;
    return false;
}

static VkFormat dummy_depth_format(const VkImageCreateInfo *ci) {
    const VkFormat candidates[] = { VK_FORMAT_D16_UNORM, VK_FORMAT_D32_SFLOAT };
    for (unsigned i = 0; i < 2; i++) {
        VkFormatProperties3 features = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
        VkFormatProperties2 props = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &features};
        vkGetPhysicalDeviceFormatProperties2(g_vk.phys, candidates[i], &props);
        if (!(features.optimalTilingFeatures & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_DEPTH_COMPARISON_BIT)) continue;
        VkImageFormatProperties image;
        if (vkGetPhysicalDeviceImageFormatProperties(g_vk.phys, candidates[i], ci->imageType, ci->tiling,
                ci->usage, ci->flags, &image) == VK_SUCCESS && image.maxArrayLayers >= ci->arrayLayers)
            return candidates[i];
    }
    FATAL("[vk] no sampled depth-comparison format for dummy image type %u flags %#x\n", ci->imageType, ci->flags);
    return VK_FORMAT_UNDEFINED;
}

static VkImageView dummy_view(int textype, bool shadow) {
    static TexBackend dummies[2][10];
    if (textype < 0 || textype >= 10) textype = TEXTYPE_2D;
    VkImageView *view = &dummy_views[shadow][textype];
    if (*view) return *view;
    TexBackend *b = &dummies[shadow][textype];
    VkImageViewType vt = view_type_for(textype);
    bool cube = vt == VK_IMAGE_VIEW_TYPE_CUBE || vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
                            .imageType = vt == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D
                                       : vt == VK_IMAGE_VIEW_TYPE_1D || vt == VK_IMAGE_VIEW_TYPE_1D_ARRAY ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {1, 1, 1}, .mipLevels = 1, .arrayLayers = cube ? 6 : 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    if (shadow) ci.format = dummy_depth_format(&ci);
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &b->image, &b->mem, NULL));
    VkCommandBuffer cb = vk_upload_cmd();
    VkImageSubresourceRange all = {shadow ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, ci.arrayLayers};
    vk_image_init_layout(cb, b->image, all.aspectMask);
    if (shadow) {
        VkClearDepthStencilValue zero = {0, 0};
        vkCmdClearDepthStencilImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
    } else {
        VkClearColorValue zero = {{0, 0, 0, 0}};
        vkCmdClearColorImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
    }
    vk_full_barrier(cb);
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = b->image,
                                .viewType = vt, .format = ci.format, .subresourceRange = all};
    VK_CHECK(vkCreateImageView(g_vk.dev, &vi, NULL, view));
    return *view;
}

VkImageView vk_dummy_view(int textype) { return dummy_view(textype, false); }
VkImageView vk_shadow_dummy_view(int textype) { return dummy_view(textype, true); }

VkSampler vk_shadow_default_sampler(void) {
    static VkSampler sampler;
    if (!sampler) {
        // NVN's default comparison function is LEQUAL. A bound sampler keeps its own state.
        VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR, .compareEnable = VK_TRUE,
            .compareOp = VK_COMPARE_OP_LESS_OR_EQUAL, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
        VK_CHECK(vkCreateSampler(g_vk.dev, &si, NULL, &sampler));
    }
    return sampler;
}
