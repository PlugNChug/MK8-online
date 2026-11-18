"""NVN RGB565 stores red in the low five bits (Vulkan B5G6R5_PACK16)."""


def prepare(runtime):
    path = runtime / 'nvn/vulkan/textures.c'
    text = path.read_text(encoding='utf-8')
    old = '    case 0x3C: return (VkFmt){VK_FORMAT_R5G6B5_UNORM_PACK16};'
    new = ('    case 0x3C: return (VkFmt){VK_FORMAT_B5G6R5_UNORM_PACK16};'
           '  // MK8 RGB565: red occupies bits 0..4')
    if text.count(old) != 1:
        raise ValueError('NVN RGB565 format mapping changed upstream')
    path.write_text(text.replace(old, new), encoding='utf-8')
