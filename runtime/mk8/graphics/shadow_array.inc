// R32_FLOAT comparison arrays use the same stored float bits as D32_SFLOAT.
// The draw executor calls this before starting dynamic rendering. Upload commands
// would execute before the entire draw batch, including this shadow map's producer.
typedef struct {
    VkImage src;
    VkImage depth_image;
    VmaAllocation depth_memory;
    VkImageView view;
    VkImageSubresourceRange range;
    uint64_t seq;
    bool valid;
} ShadowArray;
static ShadowArray shadow_arrays[8];
static unsigned next_shadow_array;

static void release_shadow_array(ShadowArray *s) {
    if (s->view) vk_defer_view(s->view);
    if (s->depth_image) vk_defer_image(s->depth_image, s->depth_memory);
    *s = (ShadowArray){0};
}

static void forget_shadow_arrays(VkImage source) {
    if (!source) return;
    for (unsigned i = 0; i < 8; ++i)
        if (shadow_arrays[i].src == source) release_shadow_array(&shadow_arrays[i]);
}

void mk8_shadow_array_written(VkImage image) {
    for (unsigned i = 0; i < 8; ++i)
        if (!image || shadow_arrays[i].src == image) shadow_arrays[i].valid = false;
}

static VkImageView shadow_array_copy(TexBackend *b, VkFormat format, VkComponentMapping sw,
                                    VkImageSubresourceRange range, bool *failed) {
    if (!b->image || format != VK_FORMAT_R32_SFLOAT || b->format != format ||
        range.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT || b->samples != 1 ||
        !range.levelCount || range.baseMipLevel >= b->levels || range.levelCount > b->levels - range.baseMipLevel ||
        !range.layerCount || range.baseArrayLayer >= b->layers || range.layerCount > b->layers - range.baseArrayLayer)
        return VK_NULL_HANDLE;
    if (sw.r != VK_COMPONENT_SWIZZLE_IDENTITY && sw.r != VK_COMPONENT_SWIZZLE_R) {
        *failed = true;  // A raw R32 copy would compare the wrong component.
        return VK_NULL_HANDLE;
    }
    ShadowArray *s = NULL;
    for (unsigned i = 0; i < 8; ++i)
        if (shadow_arrays[i].src == b->image && !memcmp(&shadow_arrays[i].range, &range, sizeof range)) {
            s = &shadow_arrays[i];
            break;
        }
    if (s && s->valid && s->seq == vk_pending_seq()) return s->view;
    if (!s) {
        // Allocation failures may be transient. Retry once per submission for this source and range.
        static VkImage failed_image;
        static VkImageSubresourceRange failed_range;
        static uint64_t failed_seq;
        *failed = true;
        if (failed_image == b->image && failed_seq == vk_pending_seq() &&
            !memcmp(&range,&failed_range,sizeof range)) return VK_NULL_HANDLE;
        failed_image = b->image;
        failed_range = range;
        failed_seq = vk_pending_seq();
        uint32_t width = level_w(b, range.baseMipLevel), height = level_h(b, range.baseMipLevel);
        VkImageCreateInfo ci = {.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType=VK_IMAGE_TYPE_2D,
            .format=VK_FORMAT_D32_SFLOAT, .extent={width,height,1}, .mipLevels=range.levelCount,
            .arrayLayers=range.layerCount, .samples=VK_SAMPLE_COUNT_1_BIT, .tiling=VK_IMAGE_TILING_OPTIMAL,
            .usage=VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VkFormatProperties3 features = {.sType=VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
        VkFormatProperties2 props = {.sType=VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext=&features};
        VkImageFormatProperties limits;
        vkGetPhysicalDeviceFormatProperties2(g_vk.phys, ci.format, &props);
        if (!(features.optimalTilingFeatures & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_DEPTH_COMPARISON_BIT) ||
            vkGetPhysicalDeviceImageFormatProperties(g_vk.phys, ci.format, ci.imageType, ci.tiling, ci.usage, 0, &limits) != VK_SUCCESS ||
            limits.maxExtent.width < width || limits.maxExtent.height < height || limits.maxMipLevels < ci.mipLevels ||
            limits.maxArrayLayers < ci.arrayLayers || !(limits.sampleCounts & VK_SAMPLE_COUNT_1_BIT)) return VK_NULL_HANDLE;
        ShadowArray fresh = {.src=b->image, .range=range};
        VmaAllocationCreateInfo ai = {.usage=VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateImage(g_vk.vma, &ci, &ai, &fresh.depth_image, &fresh.depth_memory, NULL) != VK_SUCCESS) return VK_NULL_HANDLE;
        VkImageViewCreateInfo vi = {.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image=fresh.depth_image,
            .viewType=VK_IMAGE_VIEW_TYPE_2D_ARRAY, .format=ci.format,
            .subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT,0,ci.mipLevels,0,ci.arrayLayers}};
        if (vkCreateImageView(g_vk.dev, &vi, NULL, &fresh.view) != VK_SUCCESS) {
            vmaDestroyImage(g_vk.vma, fresh.depth_image, fresh.depth_memory);
            return VK_NULL_HANDLE;
        }
        s = &shadow_arrays[next_shadow_array++ % 8];
        release_shadow_array(s);
        *s = fresh;
        failed_image = VK_NULL_HANDLE;
        *failed = false;
        vk_end_pass();
        vk_image_init_layout(vk_cmd(), s->depth_image, VK_IMAGE_ASPECT_DEPTH_BIT);
    } else vk_end_pass();
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    for (uint32_t i = 0; i < range.levelCount; ++i) {
        uint32_t mip = range.baseMipLevel + i, width = level_w(b,mip), height = level_h(b,mip);
        VkSpan scratch = vk_transient((uint64_t)width * height * range.layerCount * 4, 16);
        VkBufferImageCopy region = {.bufferOffset=scratch.off, .imageExtent={width,height,1},
            .imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,mip,range.baseArrayLayer,range.layerCount}};
        vkCmdCopyImageToBuffer(cb,b->image,VK_IMAGE_LAYOUT_GENERAL,scratch.buf,1,&region);
        vk_full_barrier(cb);
        region.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_DEPTH_BIT,i,0,range.layerCount};
        vkCmdCopyBufferToImage(cb,scratch.buf,s->depth_image,VK_IMAGE_LAYOUT_GENERAL,1,&region);
    }
    vk_full_barrier(cb);
    s->seq = vk_pending_seq();
    s->valid = true;
    return s->view;
}
