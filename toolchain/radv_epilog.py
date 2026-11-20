"""Work around a Mesa 27 RADV null-deref when a PS needs an epilog.

RADV initialises device->ps_epilogs only when one of the
VK_EXT_extended_dynamic_state3 features is enabled (radv_device.c, the
extendedDynamicState3ColorBlendEquation/RasterizationSamples/AlphaToCoverage guard). This
runtime uses the core dynamic VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE and its race shaders write
depth, so radv_shader_info sets ps.has_epilog through depth_output_needs_epilog. On the first
such draw radv_bind_ps_epilog -> lookup_ps_epilog -> radv_shader_part_cache_get uses the
uninitialised cache->ops and segfaults (call through a null function pointer) inside
vkCmdDrawIndexed. Mesa 26.2.6 does not hit it.

Enabling one extended-dynamic-state-3 feature is a no-op for this runtime's pipeline state
(it does not use dynamic colour blend) and makes RADV initialise the epilog cache. The
extension and feature are added only when the driver advertises them; every other device
state is unchanged. If the driver lacks either, the device is created exactly as before and
the log says so.
"""
from pathlib import Path


def _replace(text, old, new, what):
    if new in text:
        return text
    if text.count(old) != 1:
        raise ValueError(f'{what} changed upstream')
    return text.replace(old, new, 1)


def prepare(runtime):
    device = runtime / 'nvn/vulkan/device.c'
    text = device.read_text(encoding='utf-8')
    if 'mk8_ed3_workaround' in text:
        return
    text = _replace(text,
                    '    ext[n_ext++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;\n',
                    '    ext[n_ext++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;\n'
                    '    // mk8_ed3_workaround: see toolchain/radv_epilog.py. Enabling one extended-dynamic-\n'
                    '    // state-3 feature makes RADV initialise the PS-epilog cache; the runtime does not\n'
                    '    // use dynamic colour blend, so the pipeline state is unchanged.\n'
                    '    bool mk8_ed3 = has_ext(ep, ne, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);\n'
                    '    if (mk8_ed3) ext[n_ext++] = VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME;\n',
                    'Device extension')
    anchor = ('    VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,\n'
              '                                   .pNext = rob2 ? (void *)&r2 : fault ? (void *)&ff : (void *)&f11};')
    add = anchor + '\n' + (
        '    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT mk8_ed3_feat = {\n'
        '        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};\n'
        '    if (mk8_ed3) {\n'
        '        VkPhysicalDeviceFeatures2 mk8_ed3_query = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,\n'
        '                                                    .pNext = &mk8_ed3_feat};\n'
        '        vkGetPhysicalDeviceFeatures2(g_vk.phys, &mk8_ed3_query);\n'
        '        if (mk8_ed3_feat.extendedDynamicState3ColorBlendEquation) {\n'
        '            mk8_ed3_feat.pNext = f.pNext;\n'
        '            f.pNext = &mk8_ed3_feat;\n'
        '            LOG("[vk] Mesa 27 PS-epilog workaround: extendedDynamicState3ColorBlendEquation enabled\\n");\n'
        '        } else {\n'
        '            LOG("[vk] warning: extendedDynamicState3ColorBlendEquation unavailable; Mesa 27 may crash\\n");\n'
        '        }\n'
        '    }')
    text = _replace(text, anchor, add, 'Feature chain')
    device.write_text(text, encoding='utf-8')
