"""Give feedback snapshots the same bounded OOM recovery as normal textures."""

HELPER = '''static VkResult mk8_snapshot_allocate(const VkImageCreateInfo *ci, VmaAllocationCreateInfo *ai,
                                      VkImage *image, VmaAllocation *memory) {
    VkResult result = vmaCreateImage(g_vk.vma, ci, ai, image, memory, NULL);
    if (result != VK_ERROR_OUT_OF_DEVICE_MEMORY) return result;
    vk_collect_dead(); // Only GPU-completed deferred resources may be released.
    result = vmaCreateImage(g_vk.vma, ci, ai, image, memory, NULL);
    if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
        VmaAllocationCreateInfo host = *ai;
        host.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        result = vmaCreateImage(g_vk.vma, ci, &host, image, memory, NULL);
        LOG("[mk8:memory] feedback snapshot host-memory retry: result %d\\n", result);
    }
    return result;
}

'''


def prepare(runtime):
    path = runtime/'nvn/vulkan/exec.c'
    text = path.read_text(encoding='utf-8')
    start = 'static TexBackend *snapshot_image(NvnTexture *t, TexBackend *src) {'
    end = '// (debug frames) a depth image'
    if text.count(start) != 1 or text.count(end) != 1:
        raise ValueError('Snapshot allocation boundary changed')
    a, b = text.index(start), text.index(end)
    old = 'VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));'
    body = text[a:b]
    if a >= b or body.count(old) != 1:
        raise ValueError('Snapshot allocation call changed')
    body = body.replace(old, 'VK_CHECK(mk8_snapshot_allocate(&ci, &ai, &d->image, &d->mem));')
    path.write_text(text[:a]+HELPER+body+text[b:], encoding='utf-8')
