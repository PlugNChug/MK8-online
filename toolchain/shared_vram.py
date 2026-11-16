"""Keep the guest heap out of shared GPU memory on integrated GPUs with a separate system heap.

The pinned runtime skips the 3.25 GB guest heap import only when the largest host-visible heap is device-local
(Intel iGPUs). AMD APUs on Windows expose a small device-local carve-out plus a host-visible system heap that is not
flagged device-local, so the import still landed in the memory textures spill into. A 0.2.0 report on an
"AMD Radeon(TM) Graphics" iGPU then failed image creation with VK_ERROR_OUT_OF_DEVICE_MEMORY (textures.c and
exec.c) one to two minutes into play. Integrated GPUs now use the same size limit whatever the heap flag says.
Discrete GPUs are unchanged. Every heap is logged so later reports show the budget.
"""

OLD = '''    bool low_vram = host_heap >= 0 && (mem_props.memoryHeaps[host_heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) &&
                    mem_props.memoryHeaps[host_heap].size < HEAP_MAX + (3ull << 30);
'''

NEW = '''    for (uint32_t i = 0; i < mem_props.memoryHeapCount; i++)  // MK8 (toolchain/shared_vram.py)
        LOG("[vk] memory heap %u: %llu MB%s\\n", i, (unsigned long long)(mem_props.memoryHeaps[i].size >> 20),
            (mem_props.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? " device-local" : "");
    bool low_vram = mk8_shared_heap_too_small(&mem_props, host_heap, g_vk.props.deviceType);
'''

HELPER_BOUNDARY = 'static void pick_device(void) {\n'

HELPER = '''// MK8 (toolchain/shared_vram.py): the guest heap import would leave too little shared memory for images. Integrated
// GPUs place images in system memory whatever the heap flag says (AMD APUs: carve-out + host-visible system heap).
static bool mk8_shared_heap_too_small(const VkPhysicalDeviceMemoryProperties *mem, int host_heap, VkPhysicalDeviceType type) {
    if (host_heap < 0 || (uint32_t)host_heap >= mem->memoryHeapCount) return false;
    bool shared = (mem->memoryHeaps[host_heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ||
                  type == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    return shared && mem->memoryHeaps[host_heap].size < HEAP_MAX + (3ull << 30);
}

'''


def prepare(runtime):
    device = runtime/'nvn/vulkan/device.c'
    text = device.read_text(encoding='utf-8')
    for boundary in (OLD, HELPER_BOUNDARY):
        if text.count(boundary) != 1:
            raise ValueError(f'Shared VRAM boundary changed: {boundary[:80]}')
    text = text.replace(OLD, NEW).replace(HELPER_BOUNDARY, HELPER + HELPER_BOUNDARY)
    device.write_text(text, encoding='utf-8')
