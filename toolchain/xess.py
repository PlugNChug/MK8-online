"""XeSS device requirements: VK_EXT_mutable_descriptor_type and its feature.

XeSS 2.2/3.x on Vulkan asks for VK_EXT_mutable_descriptor_type and the
mutableDescriptorType feature, plus shaderStorageImageWriteWithoutFormat in the
core feature set. The device must be created with them; enabling them after
vkCreateDevice is not possible. This hook is gated on MK8_XESS, queries only
what the driver advertises, logs the exact missing item, and otherwise leaves
the device extension/feature lists untouched (fail-closed).
"""
from pathlib import Path


def _replace(text, old, new, what):
    if new in text:
        return text
    if text.count(old) != 1:
        raise ValueError(f'{what} changed upstream')
    return text.replace(old, new, 1)


def prepare(runtime: Path):
    device = runtime/'nvn/vulkan/device.c'
    text = device.read_text(encoding='utf-8')
    if 'mk8_xess_extension' in text:
        return
    # Keep the string in a marker variable so a second preparation is idempotent.
    extension = '''    const char *mk8_xess_extension = getenv("MK8_XESS");
    if (mk8_xess_extension && *mk8_xess_extension && strcmp(mk8_xess_extension, "0") != 0) {
        if (!has_ext(ep, ne, VK_EXT_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME)) {
            LOG("[xess] VK_EXT_mutable_descriptor_type is not advertised; XeSS will fail closed\\n");
        } else if ((size_t)n_ext < sizeof ext / sizeof ext[0]) {
            ext[n_ext++] = VK_EXT_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME;
            LOG("[xess] VK_EXT_mutable_descriptor_type enabled\\n");
        } else {
            LOG("[xess] warning: no room for VK_EXT_mutable_descriptor_type; XeSS will fail closed\\n");
        }
    }
'''
    text = _replace(text, '    free(ep);\n', extension + '    free(ep);\n', 'XeSS device extension anchor')

    feature = '''    const char *mk8_xess_feature = getenv("MK8_XESS");
    VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT mk8_xess_mutable = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT};
    if (mk8_xess_feature && *mk8_xess_feature && strcmp(mk8_xess_feature, "0") != 0) {
        VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT have_mk8_xess = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT};
        VkPhysicalDeviceFeatures2 mk8_xess_query = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &have_mk8_xess};
        vkGetPhysicalDeviceFeatures2(g_vk.phys, &mk8_xess_query);
        if (!have_mk8_xess.mutableDescriptorType) {
            LOG("[xess] mutableDescriptorType is not supported; XeSS will fail closed\\n");
        } else {
            mk8_xess_mutable.mutableDescriptorType = VK_TRUE;
            mk8_xess_mutable.pNext = f.pNext;
            f.pNext = &mk8_xess_mutable;
            LOG("[xess] mutableDescriptorType feature enabled\\n");
        }
        b->shaderStorageImageWriteWithoutFormat = h->shaderStorageImageWriteWithoutFormat;
        LOG("[xess] shaderStorageImageWriteWithoutFormat: %d\\n",
            (int)b->shaderStorageImageWriteWithoutFormat);
        if (!b->shaderStorageImageWriteWithoutFormat)
            LOG("[xess] warning: shaderStorageImageWriteWithoutFormat is not supported; XeSS will fail closed\\n");
    }
'''
    text = _replace(text, '    VkDeviceCreateInfo di = {', feature + '    VkDeviceCreateInfo di = {',
                    'XeSS feature-chain anchor')
    device.write_text(text, encoding='utf-8')
