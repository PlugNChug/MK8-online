"""Log the selected Vulkan driver and the instance layers before the device is created.

Reports #94 and #111 end with an access violation between the GPU choice and vkCreateDevice, on NVIDIA drivers that
report Vulkan 1.4.357; earlier drivers and other GPUs continue. The log did not name the driver build or the implicit
layers (Steam's Fossilize layer is visible in #94 only through its own messages), so a report could not separate a
driver regression from an injected layer. This only reads properties; device creation is unchanged.
"""


def replace(text, old, new):
    if text.count(old) != 1:
        raise ValueError('Vulkan driver log boundary changed: ' + old[:80])
    return text.replace(old, new)


LOG_DRIVER = r'''    LOG("[vk] using %s\n", g_vk.props.deviceName);
    {  // (toolchain/vulkan_driver_log.py)
        VkPhysicalDeviceDriverProperties driver = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &driver};
        vkGetPhysicalDeviceProperties2(g_vk.phys, &properties);
        driver.driverName[sizeof driver.driverName - 1] = 0;
        driver.driverInfo[sizeof driver.driverInfo - 1] = 0;
        LOG("[vk] driver %s %s (id %u, raw version %#x, device %04x:%04x)\n", driver.driverName, driver.driverInfo,
            (unsigned)driver.driverID, g_vk.props.driverVersion, g_vk.props.vendorID, g_vk.props.deviceID);
        uint32_t count = 0;
        if (vkEnumerateInstanceLayerProperties(&count, NULL) == VK_SUCCESS && count) {
            VkLayerProperties layers[32];
            if (count > 32) count = 32;
            VkResult listed = vkEnumerateInstanceLayerProperties(&count, layers);
            if (listed == VK_SUCCESS || listed == VK_INCOMPLETE)
                for (uint32_t i = 0; i < count; i++) {
                    layers[i].layerName[sizeof layers[i].layerName - 1] = 0;
                    LOG("[vk] instance layer available: %s (spec %u.%u.%u, version %u)\n", layers[i].layerName,
                        VK_API_VERSION_MAJOR(layers[i].specVersion), VK_API_VERSION_MINOR(layers[i].specVersion),
                        VK_API_VERSION_PATCH(layers[i].specVersion), layers[i].implementationVersion);
                }
        }
    }
'''


def prepare(runtime):
    device = runtime/'nvn/vulkan/device.c'
    text = device.read_text(encoding='utf-8')
    text = replace(text, '    LOG("[vk] using %s\\n", g_vk.props.deviceName);\n', LOG_DRIVER)
    device.write_text(text, encoding='utf-8')
