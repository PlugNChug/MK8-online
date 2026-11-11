"""Preserva os metadados do readback durante a recriação da swapchain no present."""

BOUNDARIES = (
    ('    VkSpan shot = {0};\n    if (shot_path) save_window_image(cb, &shot);',
     '''    // O present pode recriar a swapchain; o readback continua com o tamanho/formato anterior.
    VkExtent2D shot_extent = g_vk.sc_extent;
    VkFormat shot_format = g_vk.sc_format;
    VkSpan shot = {0};
    if (shot_path) save_window_image(cb, &shot);'''),
    ('''        uint32_t w = g_vk.sc_extent.width, h = g_vk.sc_extent.height;
        uint8_t *px = shot.ptr;
        bool bgr = g_vk.sc_format == VK_FORMAT_B8G8R8A8_UNORM || g_vk.sc_format == VK_FORMAT_B8G8R8A8_SRGB;''',
     '''        uint32_t w = shot_extent.width, h = shot_extent.height;
        uint8_t *px = shot.ptr;
        bool bgr = shot_format == VK_FORMAT_B8G8R8A8_UNORM || shot_format == VK_FORMAT_B8G8R8A8_SRGB;'''),
)


def prepare(runtime):
    device = runtime/'nvn/vulkan/device.c'
    text = device.read_text(encoding='utf-8')
    for old, _ in BOUNDARIES:
        if text.count(old) != 1:
            raise ValueError('Screenshot readback boundary changed')
    for old, new in BOUNDARIES:
        text = text.replace(old, new)
    device.write_text(text, encoding='utf-8')
