/* Probe whether this Vulkan driver accepts the MLFI compute modules extracted from AMD's signed
 * frame-generation DLL.
 *
 * The modules are FSR 4 ML frame-interpolation passes: SPIR-V 1.5 with
 * VK_KHR_cooperative_matrix, VK_NV_cooperative_matrix2, VK_EXT_shader_float8 (float8 cooperative
 * matrix), shaderFloat16/Int8, the Vulkan memory model and variable pointers. Creating the compute
 * pipeline is the cheapest check that the driver's SPIR-V front end and feature set cover them.
 *
 * Usage: mlfi_probe [--device N] module.spv [module.spv ...]
 *
 * Build: cc -O2 -o mlfi_probe tools/mlfi_probe.c -lvulkan
 *
 * This program only creates pipelines; it never dispatches, so it needs no buffers and no model.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define MAX_BINDINGS 32
#define MAX_MODULES 4096

typedef struct {
    uint32_t set;
    uint32_t binding;
    VkDescriptorType type;
} Binding;

typedef struct {
    uint32_t count;
    Binding items[MAX_BINDINGS];
    char entry[64];
    uint32_t execution_model;
} Layout;

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Minimal SPIR-V walk: only decorations, variable types and the type declarations they need. */
static int parse_layout(const uint8_t *code, size_t size, Layout *layout) {
    if (size < 20 || rd32(code) != 0x07230203) return -1;
    uint32_t words = (uint32_t)(size / 4);
    layout->count = 0;
    layout->entry[0] = 0;
    layout->execution_model = 0;
    /* type id -> kind, with image "sampled" operand when relevant */
    static enum { K_UNKNOWN, K_SAMPLER, K_IMAGE, K_STRUCT, K_POINTER, K_OTHER } kind[65536];
    static uint32_t image_sampled[65536];
    static uint32_t struct_buffer_block[65536];
    static uint32_t pointer_pointee[65536];
    static uint32_t var_type[65536];
    static uint32_t var_storage[65536];
    static uint32_t binding_of[65536], set_of[65536];
    memset(kind, 0, sizeof(kind));
    memset(image_sampled, 0, sizeof(image_sampled));
    memset(struct_buffer_block, 0, sizeof(struct_buffer_block));
    memset(var_type, 0, sizeof(var_type));
    memset(binding_of, 0xff, sizeof(binding_of));
    memset(set_of, 0, sizeof(set_of));

    uint32_t at = 5;
    while (at + 1 <= words) {
        uint32_t word = rd32(code + at * 4);
        uint32_t count = word >> 16, op = word & 0xffff;
        if (!count) break;
        if (op == 15 && count >= 4) { /* OpEntryPoint */
            const char *name = (const char *)(code + (at + 3) * 4);
            if (layout->entry[0] == 0) {
                strncpy(layout->entry, name, sizeof(layout->entry) - 1);
                layout->execution_model = rd32(code + (at + 1) * 4);
            }
        } else if (op == 5 && count >= 3) { /* OpName: not needed */
        } else if (op == 71 && count >= 3) { /* OpDecorate (Block/BufferBlock have no literal) */
            uint32_t target = rd32(code + (at + 1) * 4);
            uint32_t dec = rd32(code + (at + 2) * 4);
            uint32_t value = count >= 4 ? rd32(code + (at + 3) * 4) : 0;
            if (dec == 33) binding_of[target & 0xffff] = value;
            else if (dec == 34) set_of[target & 0xffff] = value;
            else if (dec == 3) struct_buffer_block[target & 0xffff] = 3; /* BufferBlock */
            else if (dec == 2) struct_buffer_block[target & 0xffff] = 2; /* Block */
        } else if (op == 25 && count >= 9) { /* OpTypeImage */
            uint32_t id = rd32(code + (at + 1) * 4);
            kind[id & 0xffff] = K_IMAGE;
            image_sampled[id & 0xffff] = rd32(code + (at + 7) * 4);
        } else if (op == 26) { /* OpTypeSampler */
            kind[rd32(code + (at + 1) * 4) & 0xffff] = K_SAMPLER;
        } else if (op == 30) { /* OpTypeStruct */
            kind[rd32(code + (at + 1) * 4) & 0xffff] = K_STRUCT;
        } else if (op == 32 && count == 4) { /* OpTypePointer */
            uint32_t id = rd32(code + (at + 1) * 4) & 0xffff;
            kind[id] = K_POINTER;
            pointer_pointee[id] = rd32(code + (at + 3) * 4) & 0xffff;
        } else if (op == 59 && count >= 4) { /* OpVariable */
            uint32_t type = rd32(code + (at + 1) * 4) & 0xffff;
            uint32_t id = rd32(code + (at + 2) * 4) & 0xffff;
            var_type[id] = type;
            var_storage[id] = rd32(code + (at + 3) * 4);
        }
        at += count;
    }

    for (uint32_t id = 0; id < 65536; ++id) {
        if (!var_type[id] || binding_of[id] == 0xffff) continue;
        uint32_t storage = var_storage[id];
        if (storage != 0 /* UniformConstant */ && storage != 2 /* Uniform */
            && storage != 12 /* StorageBuffer */) continue;
        uint32_t ptr = var_type[id];
        if (kind[ptr] != K_POINTER) continue;
        uint32_t pointee = pointer_pointee[ptr];
        VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
        if (kind[pointee] == K_SAMPLER) {
            type = VK_DESCRIPTOR_TYPE_SAMPLER;
        } else if (kind[pointee] == K_IMAGE) {
            type = image_sampled[pointee] == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                               : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        } else if (kind[pointee] == K_STRUCT) {
            if (struct_buffer_block[pointee] == 2 && storage == 2) type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            else type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        }
        if (type == VK_DESCRIPTOR_TYPE_MAX_ENUM) continue;
        if (layout->count >= MAX_BINDINGS) return -1;
        layout->items[layout->count].set = set_of[id];
        layout->items[layout->count].binding = binding_of[id];
        layout->items[layout->count].type = type;
        layout->count++;
    }
    return 0;
}

static VkDevice device;
static VkPhysicalDevice physical;

static int init_vulkan(int device_index) {
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo instance_info = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    instance_info.pApplicationInfo = &app;
    VkInstance instance;
    if (vkCreateInstance(&instance_info, NULL, &instance) != VK_SUCCESS) return -1;

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, NULL);
    if (!count) return -1;
    VkPhysicalDevice *devices = calloc(count, sizeof(*devices));
    vkEnumeratePhysicalDevices(instance, &count, devices);
    physical = devices[device_index < (int)count ? device_index : 0];
    free(devices);

    VkPhysicalDeviceVulkan12Properties props12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES };
    VkPhysicalDeviceProperties2 props2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    props2.pNext = &props12;
    vkGetPhysicalDeviceProperties2(physical, &props2);
    const VkPhysicalDeviceProperties *props = &props2.properties;
    printf("device: %s (api %u.%u.%u, driver %u)\n", props->deviceName,
           VK_API_VERSION_MAJOR(props->apiVersion), VK_API_VERSION_MINOR(props->apiVersion),
           VK_API_VERSION_PATCH(props->apiVersion), props->driverVersion);

    uint32_t ext_count = 0;
    vkEnumerateDeviceExtensionProperties(physical, NULL, &ext_count, NULL);
    VkExtensionProperties *exts = calloc(ext_count, sizeof(*exts));
    vkEnumerateDeviceExtensionProperties(physical, NULL, &ext_count, exts);
    int has_cm = 0, has_cm2 = 0, has_f8 = 0, has_vmm = 0, has_varptr = 0, has_cm_maint1 = 0;
    int has_img_atomic64 = 0;
    for (uint32_t i = 0; i < ext_count; ++i) {
        const char *name = exts[i].extensionName;
        if (!strcmp(name, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME)) has_cm = 1;
        else if (!strcmp(name, VK_NV_COOPERATIVE_MATRIX_2_EXTENSION_NAME)) has_cm2 = 1;
        else if (!strcmp(name, VK_EXT_SHADER_FLOAT8_EXTENSION_NAME)) has_f8 = 1;
        else if (!strcmp(name, VK_EXT_SHADER_IMAGE_ATOMIC_INT64_EXTENSION_NAME)) has_img_atomic64 = 1;
        else if (!strcmp(name, VK_KHR_VULKAN_MEMORY_MODEL_EXTENSION_NAME)) has_vmm = 1;
        else if (!strcmp(name, VK_KHR_VARIABLE_POINTERS_EXTENSION_NAME)) has_varptr = 1;
        else if (!strncmp(name, "VK_NV_cooperative_matrix", 24)) has_cm2 = 1;
        if (!strcmp(name, VK_EXT_COOPERATIVE_MATRIX_MAINTENANCE_1_EXTENSION_NAME)) has_cm_maint1 = 1;
    }
    free(exts);
    printf("extensions: cooperative_matrix=%d cooperative_matrix2=%d maintenance1=%d "
           "shader_float8=%d vulkan_memory_model=%d variable_pointers=%d\n",
           has_cm, has_cm2, has_cm_maint1, has_f8, has_vmm, has_varptr);

    VkPhysicalDeviceVulkanMemoryModelFeatures vmm = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES };
    VkPhysicalDeviceVariablePointersFeatures varptr = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES };
    VkPhysicalDeviceShaderFloat16Int8Features f16 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES };
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR cm = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR };
    VkPhysicalDeviceCooperativeMatrixMaintenance1FeaturesEXT cm2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_MAINTENANCE_1_FEATURES_EXT };
    VkPhysicalDeviceShaderFloat8FeaturesEXT f8 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT };
    VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT img64 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_ATOMIC_INT64_FEATURES_EXT };

    void **tail = &f16.pNext;
    *tail = &vmm; tail = &vmm.pNext;
    *tail = &varptr; tail = &varptr.pNext;
    *tail = &cm; tail = &cm.pNext;
    *tail = &cm2; tail = &cm2.pNext;
    *tail = &f8; tail = &f8.pNext;
    *tail = &img64;
    VkPhysicalDeviceFeatures2 query = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    query.pNext = &f16;
    vkGetPhysicalDeviceFeatures2(physical, &query);
    printf("features: float16=%d int8=%d memory_model=%d varptr=%d coopmat=%d "
           "coopmat_conversions=%d per_element=%d reductions=%d float8=%d float8_coopmat=%d\n",
           f16.shaderFloat16, f16.shaderInt8, vmm.vulkanMemoryModel,
           varptr.variablePointersStorageBuffer, cm.cooperativeMatrix,
           cm2.cooperativeMatrixConversions, cm2.cooperativeMatrixPerElementOperations,
           cm2.cooperativeMatrixReductions, f8.shaderFloat8, f8.shaderFloat8CooperativeMatrix);

    /* Attribution switches: drop one feature at a time to see whether the driver rejects the module
     * cleanly instead of crashing while compiling it. */
    if (getenv("MLFI_PROBE_NO_F8")) { has_f8 = 0; f8.shaderFloat8 = f8.shaderFloat8CooperativeMatrix = VK_FALSE; }
    if (getenv("MLFI_PROBE_NO_CM2")) { has_cm_maint1 = 0; }
    if (getenv("MLFI_PROBE_NO_CM")) { has_cm = 0; cm.cooperativeMatrix = VK_FALSE; }
    /* Ask for exactly what the extracted modules use, and only where the driver reports support. */
    varptr.variablePointers = varptr.variablePointersStorageBuffer;
    vmm.vulkanMemoryModelDeviceScope = vmm.vulkanMemoryModel;
    f16.shaderFloat16 = f16.shaderFloat16;
    f8.shaderFloat8 = f8.shaderFloat8 && has_f8;
    f8.shaderFloat8CooperativeMatrix = f8.shaderFloat8CooperativeMatrix && has_f8;
    cm.cooperativeMatrix = cm.cooperativeMatrix && has_cm;
    if (!has_cm_maint1) {
        cm2.cooperativeMatrixProperties2 = VK_FALSE;
        cm2.cooperativeMatrixReductions = VK_FALSE;
        cm2.cooperativeMatrixConversions = VK_FALSE;
        cm2.cooperativeMatrixPerElementOperations = VK_FALSE;
        cm2.cooperativeMatrixGetCoordinate = VK_FALSE;
    }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkPhysicalDeviceFeatures core = { 0 };
    vkGetPhysicalDeviceFeatures(physical, &core);
    printf("int64 image atomics: %d (extension %d)\n", img64.shaderImageInt64Atomics, has_img_atomic64);
    printf("core: int64=%d storage_image_read_without_format=%d write_without_format=%d\n",
           core.shaderInt64, core.shaderStorageImageReadWithoutFormat,
           core.shaderStorageImageWriteWithoutFormat);
    VkDeviceCreateInfo device_info = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.pNext = &f16;
    device_info.pEnabledFeatures = &core;
    const char *used[7];
    uint32_t used_count = 0;
    if (has_cm) used[used_count++] = VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME;
    if (has_cm2) used[used_count++] = VK_NV_COOPERATIVE_MATRIX_2_EXTENSION_NAME;
    if (has_cm_maint1) used[used_count++] = VK_EXT_COOPERATIVE_MATRIX_MAINTENANCE_1_EXTENSION_NAME;
    if (has_f8) used[used_count++] = VK_EXT_SHADER_FLOAT8_EXTENSION_NAME;
    if (has_img_atomic64) used[used_count++] = VK_EXT_SHADER_IMAGE_ATOMIC_INT64_EXTENSION_NAME;
    if (has_vmm) used[used_count++] = VK_KHR_VULKAN_MEMORY_MODEL_EXTENSION_NAME;
    if (has_varptr) used[used_count++] = VK_KHR_VARIABLE_POINTERS_EXTENSION_NAME;
    device_info.enabledExtensionCount = used_count;
    device_info.ppEnabledExtensionNames = used;
    if (vkCreateDevice(physical, &device_info, NULL, &device) != VK_SUCCESS) {
        printf("vkCreateDevice failed\n");
        return -1;
    }
    return 0;
}

static int try_module(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) { printf("FAIL %s: cannot open\n", path); return 1; }
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    uint8_t *code = malloc((size_t)length);
    if (fread(code, 1, (size_t)length, file) != (size_t)length) { fclose(file); free(code); return 1; }
    fclose(file);

    Layout layout;
    if (parse_layout(code, (size_t)length, &layout) != 0) {
        printf("FAIL %s: not SPIR-V\n", path);
        free(code);
        return 1;
    }

    VkShaderModuleCreateInfo module_info = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    module_info.codeSize = (size_t)length;
    module_info.pCode = (const uint32_t *)code;
    VkShaderModule module;
    if (getenv("MLFI_PROBE_EMPTY_LAYOUT")) layout.count = 0;
    if (layout.execution_model != 5 /* GLCompute */) {
        printf("SKIP %s: execution model %u is not GLCompute\n", path, layout.execution_model);
        free(code);
        return 0;
    }
    printf("... %s\n", path);
    printf("    creating shader module (%u bindings)\n", layout.count);
    for (uint32_t i = 0; i < layout.count; ++i)
        printf("      set %u binding %u type %d\n", layout.items[i].set, layout.items[i].binding, layout.items[i].type);
    printf("      entry %s\n", layout.entry);
    VkResult result = vkCreateShaderModule(device, &module_info, NULL, &module);
    if (result != VK_SUCCESS) {
        printf("FAIL %s: vkCreateShaderModule %d\n", path, result);
        free(code);
        return 1;
    }

    VkDescriptorSetLayoutBinding bindings[MAX_BINDINGS];
    for (uint32_t i = 0; i < layout.count; ++i) {
        bindings[i].binding = layout.items[i].binding;
        bindings[i].descriptorType = layout.items[i].type;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[i].pImmutableSamplers = NULL;
    }
    VkDescriptorSetLayoutCreateInfo set_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    set_info.bindingCount = layout.count;
    set_info.pBindings = bindings;
    VkDescriptorSetLayout set_layout;
    if (vkCreateDescriptorSetLayout(device, &set_info, NULL, &set_layout) != VK_SUCCESS) {
        printf("FAIL %s: vkCreateDescriptorSetLayout\n", path);
        vkDestroyShaderModule(device, module, NULL);
        free(code);
        return 1;
    }
    VkPipelineLayoutCreateInfo layout_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &set_layout;
    VkPipelineLayout pipeline_layout;
    if (vkCreatePipelineLayout(device, &layout_info, NULL, &pipeline_layout) != VK_SUCCESS) {
        printf("FAIL %s: vkCreatePipelineLayout\n", path);
        return 1;
    }

    VkComputePipelineCreateInfo pipeline_info = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline_info.stage.module = module;
    pipeline_info.stage.pName = layout.entry;
    pipeline_info.layout = pipeline_layout;
    VkPipeline pipeline;
    printf("    creating compute pipeline\n");
    result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pipeline);
    if (result != VK_SUCCESS) {
        printf("FAIL %s: vkCreateComputePipelines %d (bindings %u)\n", path, result, layout.count);
        vkDestroyPipelineLayout(device, pipeline_layout, NULL);
        vkDestroyDescriptorSetLayout(device, set_layout, NULL);
        vkDestroyShaderModule(device, module, NULL);
        free(code);
        return 1;
    }
    printf("PASS %s (%ld bytes, %u bindings)\n", path, length, layout.count);
    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyPipelineLayout(device, pipeline_layout, NULL);
    vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    vkDestroyShaderModule(device, module, NULL);
    free(code);
    return 0;
}

int main(int argc, char **argv) {
    int device_index = 0;
    int first = 1;
    while (first < argc && !strncmp(argv[first], "--device", 8)) {
        if (!strcmp(argv[first], "--device") && first + 1 < argc) {
            device_index = atoi(argv[first + 1]);
            first += 2;
        } else {
            ++first;
        }
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    if (first >= argc) {
        fprintf(stderr, "usage: %s [--device N] module.spv [...]\n", argv[0]);
        return 2;
    }
    if (init_vulkan(device_index) != 0) return 2;
    int failures = 0;
    for (int i = first; i < argc; ++i) failures += try_module(argv[i]);
    vkDestroyDevice(device, NULL);
    printf("%d/%d modules accepted\n", argc - first - failures, argc - first);
    return failures ? 1 : 0;
}
