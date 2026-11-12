"""Preserva o readback no present; recorte opcional reduz o custo do diagnóstico."""

CAPTURE_RECT = '''#include <errno.h>
// Coordenadas da swapchain, somente para as capturas solicitadas pelo teste.
static VkRect2D mk8_capture_rect(VkExtent2D extent) {
    const char *p = getenv("MK8_CAPTURE_REGION");
    if (!p || !*p) return (VkRect2D){.extent = extent};
    uint32_t v[4];
    for (int i = 0; i < 4; i++) {
        if (*p < '0' || *p > '9') FATAL("Invalid MK8_CAPTURE_REGION\\n");
        char *end;
        errno = 0;
        unsigned long long n = strtoull(p, &end, 10);
        if (errno || n > UINT32_MAX || *end != (i < 3 ? ',' : '\\0'))
            FATAL("Invalid MK8_CAPTURE_REGION\\n");
        v[i] = (uint32_t)n;
        p = i < 3 ? end + 1 : end;
    }
    if (!v[2] || !v[3] || v[0] > INT32_MAX || v[1] > INT32_MAX ||
        v[0] > extent.width || v[1] > extent.height ||
        v[2] > extent.width - v[0] || v[3] > extent.height - v[1])
        FATAL("MK8_CAPTURE_REGION outside the current swapchain\\n");
    return (VkRect2D){{(int32_t)v[0], (int32_t)v[1]}, {v[2], v[3]}};
}

'''

BOUNDARIES = (
    ('''static void save_window_image(VkCommandBuffer cb, VkSpan *out) {
    uint32_t w = g_vk.sc_extent.width, h = g_vk.sc_extent.height;''',
     CAPTURE_RECT + '''static void save_window_image(VkCommandBuffer cb, VkRect2D rect, VkSpan *out) {
    uint32_t w = rect.extent.width, h = rect.extent.height;'''),
    ('''VkBufferImageCopy rg = {.bufferOffset = out->off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .imageExtent = {w, h, 1}};''',
     '''VkBufferImageCopy rg = {.bufferOffset = out->off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .imageOffset = {rect.offset.x, rect.offset.y, 0}, .imageExtent = {w, h, 1}};'''),
    ('    VkSpan shot = {0};\n    if (shot_path) save_window_image(cb, &shot);',
     '''    // O present pode recriar a swapchain; o readback continua com o tamanho/formato anterior.
    VkRect2D shot_rect = {0};
    VkFormat shot_format = g_vk.sc_format;
    VkSpan shot = {0};
    if (shot_path) {
        shot_rect = mk8_capture_rect(g_vk.sc_extent);
        save_window_image(cb, shot_rect, &shot);
    }'''),
    ('''        uint32_t w = g_vk.sc_extent.width, h = g_vk.sc_extent.height;
        uint8_t *px = shot.ptr;
        bool bgr = g_vk.sc_format == VK_FORMAT_B8G8R8A8_UNORM || g_vk.sc_format == VK_FORMAT_B8G8R8A8_SRGB;''',
     '''        uint32_t w = shot_rect.extent.width, h = shot_rect.extent.height;
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
