"""Preserva a ordem dos canais RGB565 ao mapear NVN para Vulkan."""


def prepare(runtime):
    textures = runtime/'nvn/vulkan/textures.c'
    text = textures.read_text(encoding='utf-8')
    old = '    case 0x3C: return (VkFmt){VK_FORMAT_R5G6B5_UNORM_PACK16};'
    # NVN RGB565 guarda R nos bits 0–4; Vulkan nomeia PACK16 dos bits altos aos baixos.
    new = '    case 0x3C: return (VkFmt){VK_FORMAT_B5G6R5_UNORM_PACK16};  // NVN: R nos bits 0–4'
    if text.count(old) != 1:
        raise ValueError('RGB565 format mapping boundary changed')
    textures.write_text(text.replace(old, new), encoding='utf-8')
