// Thin FFX loader that forwards to libamd_fidelityfx_upscaler_vk.so.
//
// The game dlopens libamd_fidelityfx_loader_vk.so; this loader finds the effect
// next to itself (or on LD_LIBRARY_PATH), keeps it loaded for the process
// lifetime, and forwards the five FFX entry points.
//
// SPDX-License-Identifier: GPL-3.0-only

#include <dlfcn.h>

#include <cstdio>
#include <string>

#if !defined(_WIN32)
#define __declspec(x)
#endif

#include "ffx_api.h"

namespace {

void* g_effect = nullptr;
PfnFfxCreateContext g_create = nullptr;
PfnFfxDestroyContext g_destroy = nullptr;
ffxReturnCode_t (*g_configure)(ffxContext*, const ffxConfigureDescHeader*) = nullptr;
ffxReturnCode_t (*g_query)(ffxContext*, ffxQueryDescHeader*) = nullptr;
ffxReturnCode_t (*g_dispatch)(ffxContext*, const ffxDispatchDescHeader*) = nullptr;

std::string moduleDir() {
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&moduleDir), &info) && info.dli_fname) {
        const std::string path = info.dli_fname;
        const auto slash = path.find_last_of('/');
        if (slash != std::string::npos) return path.substr(0, slash);
    }
    return ".";
}

bool loadEffect() {
    if (g_effect) return true;
    const std::string local = moduleDir() + "/libamd_fidelityfx_upscaler_vk.so";
    g_effect = dlopen(local.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!g_effect) g_effect = dlopen("libamd_fidelityfx_upscaler_vk.so", RTLD_NOW | RTLD_LOCAL);
    if (!g_effect) {
        std::fprintf(stderr, "fsr4 loader: cannot load provider: %s\n", dlerror());
        return false;
    }
    g_create = reinterpret_cast<PfnFfxCreateContext>(dlsym(g_effect, "ffxCreateContext"));
    g_destroy = reinterpret_cast<PfnFfxDestroyContext>(dlsym(g_effect, "ffxDestroyContext"));
    g_configure = reinterpret_cast<ffxReturnCode_t (*)(ffxContext*, const ffxConfigureDescHeader*)>(
        dlsym(g_effect, "ffxConfigure"));
    g_query = reinterpret_cast<ffxReturnCode_t (*)(ffxContext*, ffxQueryDescHeader*)>(
        dlsym(g_effect, "ffxQuery"));
    g_dispatch = reinterpret_cast<ffxReturnCode_t (*)(ffxContext*, const ffxDispatchDescHeader*)>(
        dlsym(g_effect, "ffxDispatch"));
    if (!g_create || !g_destroy || !g_configure || !g_query || !g_dispatch) {
        std::fprintf(stderr, "fsr4 loader: provider is missing FFX exports\n");
        return false;
    }
    return true;
}

} // namespace

#define FSR4_EXPORT extern "C" __attribute__((visibility("default")))

FSR4_EXPORT ffxReturnCode_t ffxCreateContext(ffxContext* context,
                                             ffxCreateContextDescHeader* desc,
                                             const ffxAllocationCallbacks* mem_cb) {
    if (!loadEffect()) return FFX_API_RETURN_NO_PROVIDER;
    return g_create(context, desc, mem_cb);
}

FSR4_EXPORT ffxReturnCode_t ffxDestroyContext(ffxContext* context,
                                              const ffxAllocationCallbacks* mem_cb) {
    if (!loadEffect()) return FFX_API_RETURN_NO_PROVIDER;
    return g_destroy(context, mem_cb);
}

FSR4_EXPORT ffxReturnCode_t ffxConfigure(ffxContext* context,
                                         const ffxConfigureDescHeader* desc) {
    if (!loadEffect()) return FFX_API_RETURN_NO_PROVIDER;
    return g_configure(context, desc);
}

FSR4_EXPORT ffxReturnCode_t ffxQuery(ffxContext* context, ffxQueryDescHeader* desc) {
    if (!loadEffect()) return FFX_API_RETURN_NO_PROVIDER;
    return g_query(context, desc);
}

FSR4_EXPORT ffxReturnCode_t ffxDispatch(ffxContext* context,
                                        const ffxDispatchDescHeader* desc) {
    if (!loadEffect()) return FFX_API_RETURN_NO_PROVIDER;
    return g_dispatch(context, desc);
}
