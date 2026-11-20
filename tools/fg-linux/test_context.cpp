// Headless Vulkan validation for the native FSR frame-generation provider.
//
// Creates a compute-capable Vulkan device, then asks the provider to create a frame-generation context. That runs the
// full code path used in-game: backend setup, optical-flow + frame-interpolation contexts, every pipeline from the
// generated shader blobs, and all internal resource allocation. A blob/layout mismatch fails here.
#include "ffx_api.h"
#include "vk/ffx_api_vk.h"
#include "ffx_framegeneration.h"
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include <vector>

typedef ffxReturnCode_t (*PfnCreate)(ffxContext*, ffxCreateContextDescHeader*, const ffxAllocationCallbacks*);
typedef ffxReturnCode_t (*PfnDestroy)(ffxContext*, const ffxAllocationCallbacks*);

static const char* rc_name(ffxReturnCode_t rc)
{
    switch (rc)
    {
    case FFX_API_RETURN_OK: return "OK";
    case FFX_API_RETURN_ERROR: return "ERROR";
    case FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE: return "UNKNOWN_DESCTYPE";
    case FFX_API_RETURN_ERROR_PARAMETER: return "PARAMETER";
    case FFX_API_RETURN_NO_PROVIDER: return "NO_PROVIDER";
    case FFX_API_RETURN_ERROR_MEMORY: return "MEMORY";
    case FFX_API_RETURN_ERROR_RUNTIME_ERROR: return "RUNTIME_ERROR";
    default: return "?";
    }
}

int main(int argc, char** argv)
{
    const char* lib = argc > 1 ? argv[1] : "build/libamd_fidelityfx_framegeneration_vk.so";
    void* h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
    if (!h) { printf("dlopen: %s\n", dlerror()); return 1; }
    PfnCreate create = (PfnCreate)dlsym(h, "ffxCreateContext");
    PfnDestroy destroy = (PfnDestroy)dlsym(h, "ffxDestroyContext");
    if (!create || !destroy) { printf("missing exports\n"); return 1; }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) { printf("vkCreateInstance failed\n"); return 1; }

    uint32_t gpu_count = 0;
    vkEnumeratePhysicalDevices(instance, &gpu_count, nullptr);
    if (!gpu_count) { printf("no Vulkan devices\n"); return 1; }
    std::vector<VkPhysicalDevice> gpus(gpu_count);
    vkEnumeratePhysicalDevices(instance, &gpu_count, gpus.data());
    VkPhysicalDevice gpu = gpus[0];
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(gpu, &props);
    printf("device: %s (api %u.%u.%u)\n", props.deviceName,
           VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));

    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &family_count, families.data());
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < family_count; ++i)
        if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { family = i; break; }
    if (family == UINT32_MAX) { printf("no compute queue\n"); return 1; }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    VkDevice device = VK_NULL_HANDLE;
    if (vkCreateDevice(gpu, &dci, nullptr, &device) != VK_SUCCESS) { printf("vkCreateDevice failed\n"); return 1; }

    ffxCreateBackendVKDesc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backend.deviceContext.vkDevice = device;
    backend.deviceContext.vkPhysicalDevice = gpu;
    backend.deviceContext.vkDeviceProcAddr = vkGetDeviceProcAddr;

    ffxCreateContextDescFrameGeneration fg{};
    fg.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    fg.header.pNext = &backend.header;
    fg.flags = FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS;
    fg.displaySize = {1920, 1080};
    fg.maxRenderSize = {1920, 1080};
    fg.backBufferFormat = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;

    ffxContext ctx = nullptr;
    ffxReturnCode_t rc = create(&ctx, &fg.header, nullptr);
    printf("ffxCreateContext(FG 1920x1080) rc=%d (%s) ctx=%p\n", (int)rc, rc_name(rc), (void*)ctx);
    if (rc == FFX_API_RETURN_OK && ctx)
    {
        ffxReturnCode_t drc = destroy(&ctx, nullptr);
        printf("ffxDestroyContext rc=%d (%s)\n", (int)drc, rc_name(drc));
    }

    vkDeviceWaitIdle(device);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    dlclose(h);
    return rc == FFX_API_RETURN_OK ? 0 : 2;
}
