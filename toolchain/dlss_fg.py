"""DLSS Frame Generation host hooks: low-latency device/swapchain gating.

MK8_DLSS_FG=1 uses the normal window swapchain (DLSS-G does not replace it). This step only
does what must happen before Vulkan objects are created:

* enable VK_NV_low_latency2 on the device when it is requested and exposed (the extension has
  no feature struct; it depends on VK_KHR_present_id/present_id2, so one of those is enabled
  too), and add the swapchain latency mode;
* add VK_IMAGE_USAGE_SAMPLED_BIT to the swapchain, because DLSS-G samples the final image;
* chain VkSwapchainLatencyCreateInfoNV when the extension is enabled.

Everything is behind mk8_dlss_fg_requested(), which is false when MK8_DLSS_FG is unset/0 or when
MK8_FSR_FG is active. With DLSS-G off, the device and swapchain are the same as before. The
per-frame two-image present lives in runtime/mk8/graphics/fsr_fg.cpp; this script also adds the
device.c accessor it needs and returns the real image to PRESENT_SRC on the DLSS-G path.
"""
from pathlib import Path


def _replace(text, old, new, what):
    if new in text:
        return text
    if text.count(old) != 1:
        raise ValueError(f'{what} changed upstream')
    return text.replace(old, new, 1)


def _mk8_dlss_fg_enabled(expr='getenv("MK8_DLSS_FG")'):
    return f'({expr} && strcmp({expr}, "0") != 0)'


def prepare_exec(runtime):
    exec_c = runtime / 'nvn/vulkan/exec.c'
    text = exec_c.read_text(encoding='utf-8')
    gate = _mk8_dlss_fg_enabled()
    text = _replace(
        text,
        'if ((getenv("MK8_FSR4_CAPTURE") || getenv("MK8_FSR_FG")) && g_pass_depth',
        f'if ((getenv("MK8_FSR4_CAPTURE") || getenv("MK8_FSR_FG") || {gate}) && g_pass_depth',
        'DLSS-G depth capture condition')
    text = _replace(
        text,
        'if (getenv("MK8_FSR4") || getenv("MK8_FSR_FG")) {',
        f'if (getenv("MK8_FSR4") || getenv("MK8_FSR_FG") || {gate}) {{',
        'DLSS-G main-draw matrix scan condition')
    exec_c.write_text(text, encoding='utf-8')


def prepare_device(runtime):
    device = runtime / 'nvn/vulkan/device.c'
    text = device.read_text(encoding='utf-8')

    # VK_NV_low_latency2 adds two extensions (low_latency2 + a present-id dependency); give the
    # existing list headroom. This is a stack array, not a device/swapchain change.
    text = _replace(
        text,
        '    // room for the swapchain/WSI extensions plus the fsr4 MLFI set\n'
        '    const char *ext[16];',
        '    // room for the swapchain/WSI extensions, the fsr4 MLFI set and DLSS-G\n'
        '    const char *ext[20];',
        'DLSS-G extension array size')

    # Request helper (false unless DLSS-G is the only frame-generation mode) and the flag that
    # says VK_NV_low_latency2 was actually enabled at device creation.
    text = _replace(
        text,
        '// frame generation: family of the present/acquire queues (UINT32_MAX: none)\n'
        'uint32_t g_vk_fg_present_family = UINT32_MAX;\n',
        '// frame generation: family of the present/acquire queues (UINT32_MAX: none)\n'
        'uint32_t g_vk_fg_present_family = UINT32_MAX;\n'
        '// DLSS-G: VK_NV_low_latency2 was enabled on the created device.\n'
        'static bool mk8_dlss_fg_low_latency;\n'
        'static bool mk8_dlss_fg_requested(void) {\n'
        '    const char *value = getenv("MK8_DLSS_FG");\n'
        '    const char *fsr = getenv("MK8_FSR_FG");\n'
        '    return value && *value && strcmp(value, "0") != 0 &&\n'
        '           !(fsr && *fsr && strcmp(fsr, "0") != 0);\n'
        '}\n',
        'DLSS-G globals')

    # Enable the extension before vkCreateDevice. It has no feature struct. VK_NV_low_latency2
    # declares a dependency on VK_KHR_present_id (or present_id2), so enable one of those too;
    # vkSetLatencySleepModeNV fails if the dependency is not available.
    text = _replace(
        text,
        '    }\n'
        '    // LARECOMP_VK_ROBUST=1: out-of-range image reads',
        '    }\n'
        '    if (mk8_dlss_fg_requested()) {\n'
        '#if defined(VK_KHR_present_id) || defined(VK_KHR_present_id2)\n'
        '        bool present_id = false;\n'
        '#if defined(VK_KHR_present_id)\n'
        '        if (has_ext(ep, ne, VK_KHR_PRESENT_ID_EXTENSION_NAME) && n_ext < 20) {\n'
        '            ext[n_ext++] = VK_KHR_PRESENT_ID_EXTENSION_NAME;\n'
        '            present_id = true;\n'
        '            LOG("[dlss_fg] VK_KHR_present_id enabled\\n");\n'
        '        }\n'
        '#endif\n'
        '#if defined(VK_KHR_present_id2)\n'
        '        if (!present_id && has_ext(ep, ne, VK_KHR_PRESENT_ID_2_EXTENSION_NAME) && n_ext < 20) {\n'
        '            ext[n_ext++] = VK_KHR_PRESENT_ID_2_EXTENSION_NAME;\n'
        '            present_id = true;\n'
        '            LOG("[dlss_fg] VK_KHR_present_id2 enabled\\n");\n'
        '        }\n'
        '#endif\n'
        '        if (!present_id)\n'
        '            LOG("[dlss_fg] warning: no VK_KHR_present_id/present_id2; VK_NV_low_latency2 "\n'
        '                "pacing may be unavailable\\n");\n'
        '#endif\n'
        '#if defined(VK_NV_low_latency2)\n'
        '        if (has_ext(ep, ne, VK_NV_LOW_LATENCY_2_EXTENSION_NAME) && n_ext < 20) {\n'
        '            ext[n_ext++] = VK_NV_LOW_LATENCY_2_EXTENSION_NAME;\n'
        '            mk8_dlss_fg_low_latency = true;\n'
        '            LOG("[dlss_fg] VK_NV_low_latency2 enabled\\n");\n'
        '        } else {\n'
        '            LOG("[dlss_fg] warning: VK_NV_low_latency2 is unavailable; DLSS-G will fail closed\\n");\n'
        '        }\n'
        '#else\n'
        '        LOG("[dlss_fg] warning: VK_NV_low_latency2 is not in the Vulkan headers; DLSS-G will fail closed\\n");\n'
        '#endif\n'
        '    }\n'
        '    // LARECOMP_VK_ROBUST=1: out-of-range image reads',
        'DLSS-G device extension')

    # The final window image is sampled by DLSS-G (the swapchain otherwise only has colour
    # attachment/transfer usage), and the latency mode is declared on the swapchain.
    text = _replace(
        text,
        '        .oldSwapchain = old};\n'
        '    fg_was = mk8_fsr_fg_active();\n',
        '        .oldSwapchain = old};\n'
        '    if (mk8_dlss_fg_requested()) {\n'
        '        if (caps.supportedUsageFlags & VK_IMAGE_USAGE_SAMPLED_BIT) {\n'
        '            ci.imageUsage |= VK_IMAGE_USAGE_SAMPLED_BIT;  // DLSS-G samples the final image\n'
        '        } else {\n'
        '            LOG("[dlss_fg] warning: the swapchain cannot be sampled; DLSS-G will fail closed\\n");\n'
        '        }\n'
        '#if defined(VK_NV_low_latency2)\n'
        '        if (mk8_dlss_fg_low_latency) {\n'
        '            VkSwapchainLatencyCreateInfoNV dlss_fg_latency = {\n'
        '                .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV,\n'
        '                .latencyModeEnable = VK_TRUE};\n'
        '            dlss_fg_latency.pNext = (void *)ci.pNext;\n'
        '            ci.pNext = &dlss_fg_latency;\n'
        '        }\n'
        '#endif\n'
        '    }\n'
        '    fg_was = mk8_fsr_fg_active();\n',
        'DLSS-G swapchain usage/latency')

    # The bridge prototypes are next to the other fsr_fg entries; the image index is read by the
    # two-image present (device.c keeps cur_frame private).
    text = _replace(
        text,
        'VkResult mk8_fsr_fg_present(VkQueue queue, const VkPresentInfoKHR *info);\n',
        'VkResult mk8_fsr_fg_present(VkQueue queue, const VkPresentInfoKHR *info);\n'
        'bool mk8_dlss_fg_active(void);\n'
        'uint32_t mk8_present_image_index(void);\n'
        'uint64_t mk8_dlss_fg_pending_seq(void);\n',
        'DLSS-G bridge prototypes')
    text = _replace(
        text,
        'static Frame cur_frame;\n',
        'static Frame cur_frame;\n'
        'uint32_t mk8_present_image_index(void) { return cur_frame.image; }\n'
        'uint64_t mk8_dlss_fg_pending_seq(void) { return vk_pending_seq(); }\n',
        'DLSS-G present image accessor')

    # DLSS-G presents the final window image, so it must stay in PRESENT_SRC_KHR. The FSR FG
    # proxy keeps its SHADER_READ_ONLY layout; with DLSS-G off this expression is unchanged.
    text = _replace(
        text,
        'mk8_fsr_fg_active() ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);',
        '(mk8_fsr_fg_active() && !mk8_dlss_fg_active()) ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL\n'
        '                                                                : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);',
        'DLSS-G present image layout')

    device.write_text(text, encoding='utf-8')


def prepare(runtime):
    prepare_exec(runtime)
    prepare_device(runtime)
