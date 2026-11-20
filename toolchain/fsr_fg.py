"""Native FSR 3 / FSR 4 frame generation hooks (runtime/mk8/graphics/fsr_fg.cpp).

MK8_FSR_FG=fsr3 replaces the window swapchain with the FFX frame-interpolation swapchain. The
provider is optional and loaded at runtime; these hooks add the extra present/acquire queues, route
the swapchain calls through the bridge, and record the prepare dispatch at the end of each frame.
MK8_FSR_FG=fsr4 uses the same swapchain but the MLFI 4.0.2 effect provider, whose captured modules
need cooperative matrices (maintenance1), float8 cooperative matrices, 64-bit image atomics, the
Vulkan memory model and subgroup-size control. Cooperative matrix/float8 are already enabled by
toolchain/fsr.py; the remaining device extensions/features are enabled only in fsr4. No guest code
or ABI changes.

The depth capture and the main-draw matrix scan already exist for the upscaler (toolchain/fsr.py);
this script extends both to MK8_FSR_FG so the frame-generation prepare has real inputs when the game
provides them. That is why fsr.prepare runs before fsr_fg.prepare in toolchain/host.py.
"""
from pathlib import Path


def _replace(text, old, new, what):
    if new in text:
        return text
    if text.count(old) != 1:
        raise ValueError(f'{what} changed upstream')
    return text.replace(old, new, 1)


def prepare_exec(runtime):
    exec_c = runtime / 'nvn/vulkan/exec.c'
    text = exec_c.read_text(encoding='utf-8')
    text = _replace(text,
                    'if (getenv("MK8_FSR4_CAPTURE") && g_pass_depth',
                    'if ((getenv("MK8_FSR4_CAPTURE") || getenv("MK8_FSR_FG")) && g_pass_depth',
                    'Depth capture condition')
    text = _replace(text,
                    'if (getenv("MK8_FSR4") || getenv("MK8_DLSS")) {',
                    'if (getenv("MK8_FSR4") || getenv("MK8_DLSS") || getenv("MK8_FSR_FG")) {',
                    'Main-draw matrix scan condition')
    exec_c.write_text(text, encoding='utf-8')


def prepare_device(runtime):
    device = runtime / 'nvn/vulkan/device.c'
    text = device.read_text(encoding='utf-8')

    # The extra queue family and the bridge's queue handles are process-wide.
    text = _replace(text,
                    'static atomic_bool ui_ready;\nstatic VkDebugUtilsMessengerEXT messenger;',
                    'static atomic_bool ui_ready;\nstatic VkDebugUtilsMessengerEXT messenger;\n'
                    '// frame generation: family of the present/acquire queues (UINT32_MAX: none)\n'
                    'uint32_t g_vk_fg_present_family = UINT32_MAX;',
                    'Global declarations')

    # Find a second present-capable family for the FFX swapchain's own queues. The provider requires
    # the present and acquire queues to be distinct from the game queue and from each other.
    text = _replace(text,
                    '    free(qp);\n'
                    '    if (g_vk.qfam == UINT32_MAX) FATAL("no graphics+present queue\\n");',
                    '    g_vk_fg_present_family = UINT32_MAX;\n'
                    '    if (g_vk.qfam != UINT32_MAX && getenv("MK8_FSR_FG")) {\n'
                    '        for (uint32_t i = 0; i < nq; i++) {\n'
                    '            VkBool32 present = VK_FALSE;\n'
                    '            vkGetPhysicalDeviceSurfaceSupportKHR(g_vk.phys, i, g_vk.surface, &present);\n'
                    '            if (i != g_vk.qfam && present && qp[i].queueCount >= 2 &&\n'
                    '                (qp[i].queueFlags & (VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT))) {\n'
                    '                g_vk_fg_present_family = i;\n'
                    '                LOG("[fsr_fg] present/acquire queue family %u (%u queues)\\n", i, qp[i].queueCount);\n'
                    '                break;\n'
                    '            }\n'
                    '        }\n'
                    '        if (g_vk_fg_present_family == UINT32_MAX)\n'
                    '            LOG("[fsr_fg] disabled: no separate present queue family with two queues\\n");\n'
                    '    }\n'
                    '    free(qp);\n'
                    '    if (g_vk.qfam == UINT32_MAX) FATAL("no graphics+present queue\\n");',
                    'Queue family selection')

    # Request the extra queues from the same device (two: present and image acquire).
    text = _replace(text,
                    '    float prio = 1.0f;\n'
                    '    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = g_vk.qfam,\n'
                    '                                  .queueCount = 1, .pQueuePriorities = &prio};\n'
                    '    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f, .queueCreateInfoCount = 1,\n'
                    '                             .pQueueCreateInfos = &qi, .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext};',
                    '    float prio = 1.0f;\n'
                    '    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = g_vk.qfam,\n'
                    '                                  .queueCount = 1, .pQueuePriorities = &prio};\n'
                    '    const float fg_prio[2] = {1.0f, 1.0f};\n'
                    '    VkDeviceQueueCreateInfo fg_qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,\n'
                    '                                     .queueFamilyIndex = g_vk_fg_present_family, .queueCount = 2,\n'
                    '                                     .pQueuePriorities = fg_prio};\n'
                    '    bool fg_queues = g_vk_fg_present_family != UINT32_MAX;\n'
                    '    VkDeviceQueueCreateInfo qis[2] = {qi, fg_qi};\n'
                    '    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f,\n'
                    '                             .queueCreateInfoCount = fg_queues ? 2 : 1, .pQueueCreateInfos = qis,\n'
                    '                             .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext};',
                    'Device queue creation')

    # The generated frame-interpolation blobs and the MLFI captured modules declare storage images
    # with format Unknown and (for MLFI) storage features that need device extensions.
    text = _replace(text,
                    '    VkExtensionProperties *ep = calloc(ne, sizeof *ep);\n'
                    '    vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ne, ep);\n'
                    '    const char *ext[8];',
                    '    VkExtensionProperties *ep = calloc(ne, sizeof *ep);\n'
                    '    vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ne, ep);\n'
                    '    // room for the swapchain/WSI extensions plus the fsr4 MLFI set\n'
                    '    const char *ext[16];',
                    'Device extension array')

    # Query and enable the matching feature structs. These are only chained into the device when the
    # extensions above were enabled; the provider refuses creation with its own log line if a
    # feature it needs is false. Inserted before VkPhysicalDeviceFeatures2 f so the pNext chain
    # built below can reference them.
    text = _replace(text,
                    '    VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,\n'
                    '                                   .pNext = rob2 ? (void *)&r2 : fault ? (void *)&ff : (void *)&f11};',
                    '    // fsr4 (MK8_FSR_FG=fsr4): the MLFI feature set, enabled on top of the backend\'s own\n'
                    '    // features. Subgroup-size control and the memory-model/variable-pointer features are\n'
                    '    // taken from the Vulkan 1.1/1.2/1.3 queries; the maintenance1 and image-atomic\n'
                    '    // structs are queried separately (cooperative matrix/float8 are already queried by\n'
                    '    // toolchain/fsr.py).\n'
                    '    VkPhysicalDeviceCooperativeMatrixMaintenance1FeaturesEXT mlfi_cm2 = {\n'
                    '        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_MAINTENANCE_1_FEATURES_EXT};\n'
                    '    VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT mlfi_img64 = {\n'
                    '        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_ATOMIC_INT64_FEATURES_EXT};\n'
                    '    VkPhysicalDeviceVariablePointersFeatures mlfi_varptr = {\n'
                    '        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES};\n'
                    '    if (mlfi) {\n'
                    '        VkPhysicalDeviceFeatures2 mlfi_query = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};\n'
                    '        mlfi_cm2.pNext = &mlfi_img64;\n'
                    '        mlfi_img64.pNext = &mlfi_varptr;\n'
                    '        mlfi_query.pNext = &mlfi_cm2;\n'
                    '        vkGetPhysicalDeviceFeatures2(g_vk.phys, &mlfi_query);\n'
                    '        f13.subgroupSizeControl = have13.subgroupSizeControl;\n'
                    '        f12.shaderFloat16 = have12.shaderFloat16;\n'
                    '        f12.vulkanMemoryModel = have12.vulkanMemoryModel;\n'
                    '        f11.variablePointersStorageBuffer = mlfi_varptr.variablePointersStorageBuffer;\n'
                    '        f11.variablePointers = mlfi_varptr.variablePointers;\n'
                    '        if (!have_cm.cooperativeMatrix || !mlfi_cm2.cooperativeMatrixConversions ||\n'
                    '            !mlfi_cm2.cooperativeMatrixPerElementOperations || !mlfi_cm2.cooperativeMatrixReductions ||\n'
                    '            !have_f8.shaderFloat8CooperativeMatrix || !mlfi_img64.shaderImageInt64Atomics ||\n'
                    '            !have12.shaderFloat16 || !have12.vulkanMemoryModel || !have13.subgroupSizeControl ||\n'
                    '            !mlfi_varptr.variablePointersStorageBuffer)\n'
                    '            LOG("[fsr_fg] warning: the device lacks part of the MLFI feature set; the fsr4 "\n'
                    '                "context creation will be refused\\n");\n'
                    '    }\n'
                    '    VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,\n'
                    '                                   .pNext = rob2 ? (void *)&r2 : fault ? (void *)&ff : (void *)&f11};',
                    'MLFI device features')

    # The generated shader blobs declare their storage images with format Unknown
    # (tools/gen_fg_blobs.py): the SDK creates each image with a format chosen for the effect
    # (R16G16_SFLOAT, R8_UINT, R16G16_SINT, the swapchain format, ...), so the format must come
    # from the image view. Only when frame generation is requested, so the disabled path creates
    # the device exactly as before.
    text = _replace(text,
                    '    b->shaderClipDistance = h->shaderClipDistance;',
                    '    b->shaderClipDistance = h->shaderClipDistance;\n'
                    '    if (getenv("MK8_FSR_FG")) {\n'
                    '        b->shaderStorageImageWriteWithoutFormat = h->shaderStorageImageWriteWithoutFormat;\n'
                    '        b->shaderStorageImageReadWithoutFormat = h->shaderStorageImageReadWithoutFormat;\n'
                    '        LOG("[fsr_fg] formatless storage images: write %d read %d\\n",\n'
                    '            (int)b->shaderStorageImageWriteWithoutFormat, (int)b->shaderStorageImageReadWithoutFormat);\n'
                    '        if (!b->shaderStorageImageWriteWithoutFormat)\n'
                    '            LOG("[fsr_fg] warning: no shaderStorageImageWriteWithoutFormat; frame generation cannot run\\n");\n'
                    '    }',
                    'Device features')

    # The generated frame-interpolation shaders and the 3.1 SPD pyramid shaders used by the FSR
    # upscaler declare ComputeDerivativeGroupLinearKHR (Mesa/RADV: VK_KHR_compute_shader_derivatives).
    # Enable the extension and its feature only when FSR/FG is requested; the no-FSR device is
    # unchanged, and a missing extension/feature is skipped with one warning.
    text = _replace(text,
                    '    g_fault_ext = fault;',
                    '    g_fault_ext = fault;\n'
                    '    bool fsr_derivatives = false;\n'
                    '    if ((getenv("MK8_FSR_FG") || getenv("MK8_FSR4")) &&\n'
                    '        has_ext(ep, ne, VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME)) {\n'
                    '        fsr_derivatives = true;\n'
                    '        ext[n_ext++] = VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME;\n'
                    '    }',
                    'Compute shader derivatives extension')
    text = _replace(text,
                    '    VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,\n'
                    '                                   .pNext = rob2 ? (void *)&r2 : fault ? (void *)&ff : (void *)&f11};',
                    '    VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,\n'
                    '                                   .pNext = rob2 ? (void *)&r2 : fault ? (void *)&ff : (void *)&f11};\n'
                    '    VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR fsr_deriv_feat = {\n'
                    '        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR};\n'
                    '    if (fsr_derivatives) {\n'
                    '        VkPhysicalDeviceFeatures2 fsr_deriv_query = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,\n'
                    '                                                    .pNext = &fsr_deriv_feat};\n'
                    '        vkGetPhysicalDeviceFeatures2(g_vk.phys, &fsr_deriv_query);\n'
                    '        if (fsr_deriv_feat.computeDerivativeGroupLinear) {\n'
                    '            fsr_deriv_feat.pNext = f.pNext;\n'
                    '            f.pNext = &fsr_deriv_feat;\n'
                    '        } else {\n'
                    '            LOG("[fsr_fg] warning: no computeDerivativeGroupLinear; FSR upscaling/frame generation cannot run\\n");\n'
                    '        }\n'
                    '    }',
                    'Compute shader derivatives feature')

    # fsr4: the MLFI 4.0.2 provider's captured modules additionally need cooperative matrix
    # maintenance1 and 64-bit image atomics (cooperative matrix/float8 are already enabled by
    # toolchain/fsr.py for the upscaler). Only MK8_FSR_FG=fsr4 requests them; fsr3 and the FG-off
    # device are unchanged, and a missing extension disables fsr4 with one warning instead of
    # creating a device the provider refuses.
    # (Anchored on the tail of the derivatives block: both insert after the same writer.)
    text = _replace(text,
                    '    if ((getenv("MK8_FSR_FG") || getenv("MK8_FSR4")) &&\n'
                    '        has_ext(ep, ne, VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME)) {\n'
                    '        fsr_derivatives = true;\n'
                    '        ext[n_ext++] = VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME;\n'
                    '    }',
                    '    if ((getenv("MK8_FSR_FG") || getenv("MK8_FSR4")) &&\n'
                    '        has_ext(ep, ne, VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME)) {\n'
                    '        fsr_derivatives = true;\n'
                    '        ext[n_ext++] = VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME;\n'
                    '    }\n'
                    '    bool mlfi = getenv("MK8_FSR_FG") && !strcmp(getenv("MK8_FSR_FG"), "fsr4");\n'
                    '    if (mlfi) {\n'
                    '        if (has_ext(ep, ne, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME) &&\n'
                    '            has_ext(ep, ne, VK_EXT_SHADER_FLOAT8_EXTENSION_NAME) &&\n'
                    '            has_ext(ep, ne, VK_EXT_COOPERATIVE_MATRIX_MAINTENANCE_1_EXTENSION_NAME) &&\n'
                    '            has_ext(ep, ne, VK_EXT_SHADER_IMAGE_ATOMIC_INT64_EXTENSION_NAME)) {\n'
                    '            ext[n_ext++] = VK_EXT_COOPERATIVE_MATRIX_MAINTENANCE_1_EXTENSION_NAME;\n'
                    '            ext[n_ext++] = VK_EXT_SHADER_IMAGE_ATOMIC_INT64_EXTENSION_NAME;\n'
                    '            LOG("[fsr_fg] MLFI device extensions enabled (cooperative_matrix_maintenance1, image_int64_atomics)\\n");\n'
                    '        } else {\n'
                    '            mlfi = false;\n'
                    '            LOG("[fsr_fg] warning: the MLFI extensions are unavailable (cooperative_matrix, "\n'
                    '                "cooperative_matrix_maintenance1, shader_float8, shader_image_atomic_int64)\\n");\n'
                    '        }\n'
                    '    }',
                    'MLFI device extensions')

    text = _replace(text,
                    '        if (fsr_deriv_feat.computeDerivativeGroupLinear) {\n'
                    '            fsr_deriv_feat.pNext = f.pNext;\n'
                    '            f.pNext = &fsr_deriv_feat;\n'
                    '        } else {\n'
                    '            LOG("[fsr_fg] warning: no computeDerivativeGroupLinear; FSR upscaling/frame generation cannot run\\n");\n'
                    '        }\n'
                    '    }',
                    '        if (fsr_deriv_feat.computeDerivativeGroupLinear) {\n'
                    '            fsr_deriv_feat.pNext = f.pNext;\n'
                    '            f.pNext = &fsr_deriv_feat;\n'
                    '        } else {\n'
                    '            LOG("[fsr_fg] warning: no computeDerivativeGroupLinear; FSR upscaling/frame generation cannot run\\n");\n'
                    '        }\n'
                    '    }\n'
                    '    if (mlfi) {\n'
                    '        // maintenance1 learns about FrameGeneration, image atomics about the splat module.\n'
                    '        // The cooperative-matrix/float8 structs are already in the chain (toolchain/fsr.py).\n'
                    '        mlfi_img64.pNext = f13.pNext;\n'
                    '        f13.pNext = &mlfi_img64;\n'
                    '        mlfi_cm2.pNext = f13.pNext;\n'
                    '        f13.pNext = &mlfi_cm2;\n'
                    '    }',
                    'MLFI feature chain')

    text = _replace(text,
                    'void vk_queue_lock(void) { pthread_mutex_lock(&queue_lock); }\n'
                    'void vk_queue_unlock(void) { pthread_mutex_unlock(&queue_lock); }',
                    'void vk_queue_lock(void) { pthread_mutex_lock(&queue_lock); }\n'
                    'void vk_queue_unlock(void) { pthread_mutex_unlock(&queue_lock); }\n'
                    '// C-linkage wrappers for the frame-generation bridge (vk.h declares these in C++).\n'
                    'void mk8_fsr_fg_queue_lock(void) { vk_queue_lock(); }\n'
                    'void mk8_fsr_fg_queue_unlock(void) { vk_queue_unlock(); }',
                    'Queue lock helpers')

    # The bridge entry points, declared where the swapchain code lives (as toolchain/fsr.py does for
    # the upscaler: device.c is C and has no header of its own for these). Anchored on the section
    # comment because toolchain/present_mode.py inserts its helper before present_mode().
    text = _replace(text,
                    '// ================================================================== swapchain\n',
                    '// ================================================================== swapchain\n'
                    '// Native FSR frame generation (runtime/mk8/graphics/fsr_fg.cpp); each one falls back to\n'
                    '// the plain Vulkan call when the bridge is not active.\n'
                    'bool mk8_fsr_fg_create_swapchain(const VkSwapchainCreateInfoKHR *ci, VkSwapchainKHR *swapchain);\n'
                    'VkResult mk8_fsr_fg_get_images(VkDevice device, VkSwapchainKHR swapchain, uint32_t *count, VkImage *images);\n'
                    'VkResult mk8_fsr_fg_acquire(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore,\n'
                    '                            VkFence fence, uint32_t *index);\n'
                    'VkResult mk8_fsr_fg_present(VkQueue queue, const VkPresentInfoKHR *info);\n'
                    'bool mk8_fsr_fg_active(void);\n'
                    'void mk8_fsr_fg_prepare(VkCommandBuffer cb, uint32_t width, uint32_t height);\n'
                    'void mk8_fsr_fg_destroy_swapchain(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks *allocator);\n',
                    'Swapchain section')

    text = _replace(text,
                    'static void create_swapchain(void) {\n    VkSurfaceCapabilitiesKHR caps;',
                    'static void create_swapchain(void) {\n    bool fg_sc = false, fg_was = false;\n    VkSurfaceCapabilitiesKHR caps;',
                    'Swapchain creation start')

    text = _replace(text,
                    '    cr = vkCreateSwapchainKHR(g_vk.dev, &ci, NULL, &g_vk.swapchain);',
                    '    fg_was = mk8_fsr_fg_active();\n'
                    '    fg_sc = mk8_fsr_fg_create_swapchain(&ci, &g_vk.swapchain);\n'
                    '    if (fg_sc) {\n'
                    '        cr = VK_SUCCESS;\n'
                    '    } else {\n'
                    '        ci.oldSwapchain = g_vk.swapchain;  // (a frame-generation proxy destroyed on failure is gone)\n'
                    '        cr = vkCreateSwapchainKHR(g_vk.dev, &ci, NULL, &g_vk.swapchain);\n'
                    '    }',
                    'Swapchain creation call')

    text = _replace(text,
                    '    if (old) vkDestroySwapchainKHR(g_vk.dev, old, NULL);',
                    '    if (old && !fg_sc && !fg_was) vkDestroySwapchainKHR(g_vk.dev, old, NULL);',
                    'Old swapchain destruction')

    text = _replace(text,
                    '    g_vk.sc_count = 8;\n'
                    '    vkGetSwapchainImagesKHR(g_vk.dev, g_vk.swapchain, &g_vk.sc_count, g_vk.sc_images);',
                    '    g_vk.sc_count = 8;\n'
                    '    if (fg_sc) mk8_fsr_fg_get_images(g_vk.dev, g_vk.swapchain, &g_vk.sc_count, g_vk.sc_images);\n'
                    '    else vkGetSwapchainImagesKHR(g_vk.dev, g_vk.swapchain, &g_vk.sc_count, g_vk.sc_images);',
                    'Swapchain image query')

    for timeout in ('UINT64_MAX', 'mk8_acquire_timeout_ns()'):
        acquire = f'vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, {timeout}, acquire_sem[slot], VK_NULL_HANDLE,'
        if acquire in text:
            if text.count(acquire) != 2:
                raise ValueError('Swapchain acquire changed upstream')
            text = text.replace(acquire, acquire.replace('vkAcquireNextImageKHR', 'mk8_fsr_fg_acquire'))

    # The proxy present must run without the game's queue lock: it may wait for the provider's
    # present thread, whose submissions take that lock. When the bridge is inactive it makes the
    # plain vkQueuePresentKHR call, under the lock as before.
    if 'mk8_fsr_fg_present(g_vk.queue, &pi)' not in text:
        if text.count('        vk_queue_lock();') != 2 or text.count('        vk_queue_unlock();') != 2 or \
                text.count('        VkResult r = vkQueuePresentKHR(g_vk.queue, &pi);') != 2:
            raise ValueError('Present calls changed upstream')
        text = text.replace('        vk_queue_lock();', '        if (!mk8_fsr_fg_active()) vk_queue_lock();')
        text = text.replace('        vk_queue_unlock();', '        if (!mk8_fsr_fg_active()) vk_queue_unlock();')
        text = text.replace('        VkResult r = vkQueuePresentKHR(g_vk.queue, &pi);',
                            '        VkResult r = mk8_fsr_fg_present(g_vk.queue, &pi);')

    text = _replace(text,
                    'static void frame_end(const char *shot_path, uint64_t present_at) {\n'
                    '    if (!cur_frame.ok || cur_frame.off) return;',
                    'static void frame_end(const char *shot_path, uint64_t present_at) {\n'
                    '    if (!cur_frame.ok || cur_frame.off) return;\n'
                    '    if (present_at && mk8_fsr_fg_active()) {\n'
                    '        static int said;\n'
                    '        if (!said++) LOG("[fsr_fg] native frame generation takes the present path; the game\'s 120 Hz image interpolation is ignored\\n");\n'
                    '        present_at = 0;\n'
                    '    }',
                    'Frame end start')

    text = _replace(text,
                    '    VkCommandBuffer cb = vk_cmd();\n'
                    '    vkCmdEndRendering(cb);\n'
                    '    VkSpan shot = {0};',
                    '    VkCommandBuffer cb = vk_cmd();\n'
                    '    vkCmdEndRendering(cb);\n'
                    '    if (mk8_fsr_fg_active()) mk8_fsr_fg_prepare(cb, g_vk.sc_extent.width, g_vk.sc_extent.height);\n'
                    '    VkSpan shot = {0};',
                    'Frame end record point')

    text = _replace(text,
                    '    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);',
                    '    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,\n'
                    '                  mk8_fsr_fg_active() ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);',
                    'Window image present barrier')

    text = _replace(text,
                    '    if (g_vk.swapchain) vkDestroySwapchainKHR(g_vk.dev, g_vk.swapchain, NULL);',
                    '    if (g_vk.swapchain) mk8_fsr_fg_destroy_swapchain(g_vk.dev, g_vk.swapchain, NULL);',
                    'Swapchain destruction')

    device.write_text(text, encoding='utf-8')


def prepare(runtime):
    prepare_exec(runtime)
    prepare_device(runtime)
