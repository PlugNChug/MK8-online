"""DLSS SR/FG host hooks: enable the NGX device extensions before vkCreateDevice.

NGX's Vulkan backend requires four device extensions and one instance extension that the
runtime does not otherwise enable:

    instance: VK_KHR_get_physical_device_properties2
    device:   VK_NVX_binary_import, VK_NVX_image_view_handle,
              VK_KHR_buffer_device_address, VK_KHR_push_descriptor

This step runs after toolchain/fsr.py and toolchain/fsr_fg.py and before
toolchain/dlss_fg.py, so the FSR FG/MLFI extension lists are already in place and are not
reordered. It is inert when neither MK8_DLSS nor MK8_DLSS_FG is on: with both unset (or
"0"), the instance/device extension lists and the created objects are the same as before.
FSR frame generation wins when MK8_FSR_FG is also set, matching the bridge's own refusal
and toolchain/dlss_fg.py's helper.

Every extension is added only when advertised. A missing required extension logs one
exact line naming it and adds none of the NGX set; the bridge then fails closed at Init
and the existing FSR/bilinear path runs.
"""
from pathlib import Path


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
// DLSS SR/FG: true only when a DLSS mode was requested; FSR frame generation wins.
static bool mk8_dlss_requested(void) {
    const char *sr = getenv("MK8_DLSS");
    const char *fg = getenv("MK8_DLSS_FG");
    const char *fsr = getenv("MK8_FSR_FG");
    bool sr_on = sr && *sr && strcmp(sr, "0") != 0;
    bool fg_on = fg && *fg && strcmp(fg, "0") != 0;
    bool fsr_on = fsr && *fsr && strcmp(fsr, "0") != 0;
    return (sr_on || fg_on) && !fsr_on;
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
