"""Enable NGX extensions on request for the optional DLSS Super Resolution bridge."""



def _replace(text, old, new, what):
    if new in text:
        return text
    if text.count(old) != 1:
        raise ValueError(f'{what} changed upstream')
    return text.replace(old, new, 1)


def prepare_device(runtime):
    device = runtime / 'nvn/vulkan/device.c'
    text = device.read_text(encoding='utf-8')

    helpers = '''static atomic_bool ui_ready;
static bool mk8_dlss_requested(void) {
    const char *sr = getenv("MK8_DLSS");
    return sr && *sr && strcmp(sr, "0") != 0;
}

static bool mk8_dlss_listed(const char **list, uint32_t count, const char *name) {
    for (uint32_t i = 0; i < count; i++)
        if (!strcmp(list[i], name)) return true;
    return false;
}
'''
    text = _replace(text, 'static atomic_bool ui_ready;\n', helpers, 'DLSS helper anchor')

    instance = '''    if (mk8_dlss_requested()) {
        uint32_t ni = 0;
        vkEnumerateInstanceExtensionProperties(NULL, &ni, NULL);
        VkExtensionProperties *ip = ni ? calloc(ni, sizeof *ip) : NULL;
        bool props2 = false;
        if (ip) {
            vkEnumerateInstanceExtensionProperties(NULL, &ni, ip);
            for (uint32_t i = 0; i < ni; i++)
                if (!strcmp(ip[i].extensionName, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
                    props2 = true;
            free(ip);
        }
        if (!props2) {
            LOG("[dlss] VK_KHR_get_physical_device_properties2 is not advertised; NGX will fail closed\\n");
        } else if (!mk8_dlss_listed(ext, n_ext,
                                    VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME) &&
                   n_ext < 14) {
            ext[n_ext++] = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
            LOG("[dlss] VK_KHR_get_physical_device_properties2 enabled\\n");
        }
    }
'''
    # Insert before the validation extension so the fallback's enabledExtensionCount--
    # still removes the validation extension, which stays last.
    text = _replace(text, '    bool validate = getenv("LARECOMP_VK_VALIDATION") != NULL;',
                    instance + '    bool validate = getenv("LARECOMP_VK_VALIDATION") != NULL;',
                    'DLSS instance extension anchor')

    extensions = '''    if (mk8_dlss_requested()) {
        const char *missing = NULL;
        if (!has_ext(ep, ne, VK_NVX_BINARY_IMPORT_EXTENSION_NAME))
            missing = VK_NVX_BINARY_IMPORT_EXTENSION_NAME;
        else if (!has_ext(ep, ne, VK_NVX_IMAGE_VIEW_HANDLE_EXTENSION_NAME))
            missing = VK_NVX_IMAGE_VIEW_HANDLE_EXTENSION_NAME;
        else if (!has_ext(ep, ne, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME))
            missing = VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME;
        else if (!has_ext(ep, ne, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME))
            missing = VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME;
        if (missing) {
            LOG("[dlss] NGX device extension %s is not advertised; DLSS will fail closed\\n", missing);
        } else if (n_ext + 4u > sizeof ext / sizeof ext[0]) {
            LOG("[dlss] warning: no room for the NGX device extensions; DLSS will fail closed\\n");
        } else {
            const char *const ngx[] = {VK_NVX_BINARY_IMPORT_EXTENSION_NAME,
                                       VK_NVX_IMAGE_VIEW_HANDLE_EXTENSION_NAME,
                                       VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
                                       VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
            for (size_t i = 0; i < sizeof ngx / sizeof ngx[0]; i++)
                if (!mk8_dlss_listed(ext, n_ext, ngx[i])) ext[n_ext++] = ngx[i];
            LOG("[dlss] NGX device extensions enabled: VK_NVX_binary_import, "
                "VK_NVX_image_view_handle, VK_KHR_buffer_device_address, VK_KHR_push_descriptor\\n");
        }
    }
'''
    # Insert before free(ep), after toolchain/fsr.py's cooperative-matrix/float8 additions.
    text = _replace(text, '    free(ep);\n', extensions + '    free(ep);\n', 'DLSS device extension anchor')

    device.write_text(text, encoding='utf-8')


def prepare(runtime):
    prepare_device(runtime)
