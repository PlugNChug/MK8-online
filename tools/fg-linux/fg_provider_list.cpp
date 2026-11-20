// Provider registry for the FidelityFX frame-generation module.
//
// The SDK generates this file per effect at build time; the published SDK source does not ship it. We register the two
// providers that make up the shipped `amd_fidelityfx_framegeneration_vk` module:
//   - ffxProvider_Fsr3FrameGeneration          (the frame-interpolation effect: 3.1.x)
//   - ffxProvider_Fsr3FrameGenerationSwapChain (the Vulkan swapchain replacement: 3.1.7)
#include "ffx_provider.h"
#include "ffx_provider_fsr3framegeneration.h"
#include "ffx_provider_fsr3framegenerationswapchain.h"

static ffxProvider* g_ffxProviders[] = {
    &ffxProvider_Fsr3FrameGeneration::GetInstance(),
    &ffxProvider_Fsr3FrameGenerationSwapChain::GetInstance(),
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
