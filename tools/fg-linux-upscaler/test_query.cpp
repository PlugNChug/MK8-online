// Verifies the provider's exported entry points and that ffxQuery reports the upscaler versions it offers.
//
// The native Linux provider advertises only 3.1.6. AMD's signed Windows DLL advertises 4.1.2, 3.1.6 and
// 2.3.4 on RDNA 4, but only 3.1.6 and 2.3.4 on other GPUs. An optional second argument is the version name
// the probe must find (`test_query <dll> 3.1.6`); without it any non-empty list passes.
#include "ffx_api.h"
#include "ffx_upscale.h"
#include "platform.h"
#include <cstdio>
#include <cstring>

typedef ffxReturnCode_t (*PfnQuery)(ffxContext*, ffxQueryDescHeader*);

int main(int argc, char** argv)
{
    const char* lib = argc > 1 ? argv[1] : "build/libamd_fidelityfx_upscaler_vk.so";
    const char* expect = argc > 2 ? argv[2] : nullptr;
    printf("library: %s\n", lib);
    void* h = mk8_lib_open(lib);
    if (!h) { printf("load failed: %s\n", mk8_lib_error()); return 1; }
    PfnQuery query = (PfnQuery)mk8_lib_symbol(h, "ffxQuery");
    if (!query) { printf("no ffxQuery export\n"); return 1; }

    ffxQueryDescGetVersions versions{};
    versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    uint64_t count = 0;
    versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
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
    bool found = expect == nullptr;
    for (uint64_t i = 0; i < count; ++i)
    {
        printf("version[%llu] id=0x%016llx name=%s\n", (unsigned long long)i,
               (unsigned long long)ids[i], names[i] ? names[i] : "(null)");
        if (expect && names[i] && strcmp(names[i], expect) == 0) found = true;
    }
    mk8_lib_close(h);
    printf("RESULT: %s\n", found ? "OK" : "MISSING");
    return found ? 0 : 3;
}
