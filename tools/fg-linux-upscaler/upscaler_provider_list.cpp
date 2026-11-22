// Provider registry for the FidelityFX upscaler module.
//
// The SDK generates this file per effect at build time; the published SDK source does not ship it. We register the
// provider that makes up the shipped `amd_fidelityfx_upscaler_vk` module:
//   - ffxProvider_FSR3Upscale (the FSR 3.1.6 upscaler effect)
//
// The older FSR 2 effect (ffxProvider_FSR2) is not built: the SDK does not publish its generated shader blob tables
// either, and the Linux bridge targets FSR 3.1.6 only.
#include "ffx_provider.h"
#include "ffx_provider_fsr3upscale.h"

static ffxProvider* g_ffxProviders[] = {
    &ffxProvider_FSR3Upscale::GetInstance(),
};

static std::span<ffxProvider* const> ProviderSpan()
{
    return std::span<ffxProvider* const>(g_ffxProviders, sizeof(g_ffxProviders) / sizeof(g_ffxProviders[0]));
}

ffxProvider* GetProvider(ffxStructType_t descType, uint64_t overrideId, void* device, std::optional<ffxProviderExternal>& extProviderSlot)
{
    return GetProvider(descType, overrideId, device, extProviderSlot, ProviderSpan());
}

uint64_t GetProviderVersions(ffxStructType_t descType, void* device, uint64_t capacity, uint64_t* versionIds, const char** versionNames, std::optional<ffxProviderExternal>& extProviderSlot)
{
    return GetProviderVersions(descType, device, capacity, versionIds, versionNames, extProviderSlot, ProviderSpan());
}
