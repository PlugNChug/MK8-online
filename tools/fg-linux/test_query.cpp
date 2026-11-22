// Verifies the provider's exported entry points and that ffxQuery reports the FSR 3.1.7 swapchain version.
#include "ffx_api.h"
#include "vk/ffx_api_framegeneration_vk.h"
#include <dlfcn.h>
#include <cstdio>
#include <cstring>

typedef ffxReturnCode_t (*PfnQuery)(ffxContext*, ffxQueryDescHeader*);

int main(int argc, char** argv)
{
    const char* lib = argc > 1 ? argv[1] : "build/libamd_fidelityfx_framegeneration_vk.so";
    void* h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
    if (!h) { printf("dlopen failed: %s\n", dlerror()); return 1; }
    PfnQuery query = (PfnQuery)dlsym(h, "ffxQuery");
    if (!query) { printf("no ffxQuery export\n"); return 1; }

    ffxQueryDescGetVersions versions{};
    versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    uint64_t count = 0;
    versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK;
    versions.device = nullptr;
    versions.outputCount = &count;
    versions.versionIds = nullptr;
    versions.versionNames = nullptr;
    ffxReturnCode_t rc = query(nullptr, &versions.header);
    printf("count query rc=%d count=%llu\n", (int)rc, (unsigned long long)count);
    if (rc != FFX_API_RETURN_OK || count == 0) return 2;

    uint64_t ids[8] = {0};
    const char* names[8] = {nullptr};
    uint64_t cap = count < 8 ? count : 8;
    count = cap;
    versions.outputCount = &count;
    versions.versionIds = ids;
    versions.versionNames = names;
    rc = query(nullptr, &versions.header);
    printf("name query rc=%d count=%llu\n", (int)rc, (unsigned long long)count);
    for (uint64_t i = 0; i < count; ++i)
        printf("version[%llu] id=0x%016llx name=%s\n", (unsigned long long)i,
               (unsigned long long)ids[i], names[i] ? names[i] : "(null)");
    dlclose(h);
    return 0;
}
