// CPU texture reads must fetch the rendered Vulkan image, not its stale guest allocation.
void mk8_texture_readback(NvnTexture *texture, const NvnTextureView *view, const int32_t *region,
                          uint64_t destination, int64_t row_stride, int64_t image_stride) {
    if (!texture || !region || row_stride < 0 || image_stride < 0)
        FATAL("[mk8:readback] invalid texture read arguments\n");
    const FormatInfo *format = nvn_format_info(texture->format);
    if (!format || format->bw != 1 || format->bh != 1 || format->bpb != 4 ||
        renderer_format_is_depth(texture->format) || texture->samples > 1 || texture->depth > 1 ||
        (view && ((view->setmask & 1 && view->base_level) || (view->setmask & 2 && view->min_layer))))
        FATAL("[mk8:readback] unsupported texture format/view %u\n", texture->format);
    if (region[0] < 0 || region[1] < 0 || region[2] || region[3] <= 0 || region[4] <= 0 || region[5] != 1 ||
        (uint64_t)region[0] + region[3] > texture->width || (uint64_t)region[1] + region[4] > texture->height)
        FATAL("[mk8:readback] texture region outside level zero\n");
    uint64_t packed = (uint64_t)region[3] * 4;
    uint64_t row = row_stride ? (uint64_t)row_stride : packed;
    if (row < packed || row % 4 || row > UINT32_MAX || row > UINT64_MAX / (uint32_t)region[4])
        FATAL("[mk8:readback] invalid row stride\n");
    uint64_t bytes = row * (uint32_t)region[4];
    uint64_t image = image_stride ? (uint64_t)image_stride : bytes;
    if (image < bytes || image > 256ull * 1024 * 1024 || destination < HEAP_BASE ||
        destination >= GUEST_HI || image > GUEST_HI - destination)
        FATAL("[mk8:readback] invalid destination/image stride\n");
    TexRefRec reference = {.tex = (uint64_t)(uintptr_t)texture};
    if (view) { reference.view = *view; reference.has_view = 1; }
    memcpy(reference.region, region, sizeof reference.region);
    uint64_t old_row = st.copy_row_stride, old_image = st.copy_image_stride;
    st.copy_row_stride = row; st.copy_image_stride = image;
    LOG("[mk8:readback] %dx%d format=%u row=%llu image=%llu\n", region[3], region[4], texture->format,
        (unsigned long long)row, (unsigned long long)image);
    copy_texture_to_buffer(&reference, destination);
    st.copy_row_stride = old_row; st.copy_image_stride = old_image;
}
