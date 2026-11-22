#!/bin/sh
# Cross-compile the FSR probes for native Windows x86-64 with mingw-w64.
#
# The probes load AMD's signed Vulkan DLLs at runtime, so nothing from AMD is linked or copied here.
# They are the same sources the Linux tests use (tools/fg-linux-upscaler/test_*.cpp); only the library
# loader differs (platform.h) and this script supplies a Vulkan import library for mingw.
#
# Requirements:
#   mingw-w64 (x86_64-w64-mingw32-gcc/g++/dlltool)
#   Vulkan headers (vulkan/ and vk_video/)
#   FidelityFX SDK 2.4.0 headers (api/include and upscalers/include)
#
# Env:
#   FFX_SDK          Kits/FidelityFX of the prepared SDK (default: the local sdk-copy used by the builds)
#   VULKAN_HEADERS   directory containing vulkan/ and vk_video/ (default: /usr/include)
#   WIN_FSR_BUILD    output directory (default: $HOME/.cache/mk8online/win-rtx/build)

set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
upscaler_dir=$here/../fg-linux-upscaler
mlfi_dir=$here/../mlfi-capture

: "${FFX_SDK:=$HOME/.cache/mk8online/fsr-build/sdk-copy/Kits/FidelityFX}"
: "${VULKAN_HEADERS:=/usr/include}"
: "${WIN_FSR_BUILD:=$HOME/.cache/mk8online/win-rtx/build}"

for header in "$FFX_SDK/api/include/ffx_api.h" \
              "$FFX_SDK/upscalers/include/ffx_upscale.h" \
              "$VULKAN_HEADERS/vulkan/vulkan.h"; do
    if [ ! -f "$header" ]; then
        echo "missing $header" >&2
        echo "set FFX_SDK / VULKAN_HEADERS" >&2
        exit 1
    fi
done

mkdir -p "$WIN_FSR_BUILD/vkinc/vulkan" "$WIN_FSR_BUILD/vkinc/vk_video"
cp -a "$VULKAN_HEADERS/vulkan/." "$WIN_FSR_BUILD/vkinc/vulkan/"
if [ -d "$VULKAN_HEADERS/vk_video" ]; then
    cp -a "$VULKAN_HEADERS/vk_video/." "$WIN_FSR_BUILD/vkinc/vk_video/"
fi

# MinGW has no Vulkan import library. Generate one for the entry points the probes call; the loader
# (vulkan-1.dll) is resolved at run time on the target machine.
cat > "$WIN_FSR_BUILD/vulkan-1.def" <<'EOF'
LIBRARY vulkan-1.dll
EXPORTS
vkCreateInstance
vkEnumeratePhysicalDevices
vkGetPhysicalDeviceProperties
vkGetPhysicalDeviceFeatures
vkGetPhysicalDeviceFeatures2
vkGetPhysicalDeviceMemoryProperties
vkGetPhysicalDeviceQueueFamilyProperties
vkEnumerateDeviceExtensionProperties
vkCreateDevice
vkGetDeviceProcAddr
vkGetDeviceQueue
vkCreateImage
vkGetImageMemoryRequirements
vkCreateImageView
vkCreateBuffer
vkGetBufferMemoryRequirements
vkBindBufferMemory
vkAllocateMemory
vkBindImageMemory
vkMapMemory
vkUnmapMemory
vkFreeMemory
vkDestroyImageView
vkDestroyImage
vkDestroyBuffer
vkCreateCommandPool
vkDestroyCommandPool
vkAllocateCommandBuffers
vkBeginCommandBuffer
vkEndCommandBuffer
vkCmdPipelineBarrier
vkCmdCopyBufferToImage
vkCmdCopyImageToBuffer
vkCreateFence
vkDestroyFence
vkResetFences
vkWaitForFences
vkQueueSubmit
vkDeviceWaitIdle
vkDestroyDevice
vkDestroyInstance
EOF
x86_64-w64-mingw32-dlltool -d "$WIN_FSR_BUILD/vulkan-1.def" \
    -l "$WIN_FSR_BUILD/libvulkan-1.a" -D vulkan-1.dll

cxxflags="-std=c++20 -O2 -static -DFFX_API_ENTRY= -I$WIN_FSR_BUILD/vkinc \
          -I$FFX_SDK/api/include -I$FFX_SDK/upscalers/include"
# shellcheck disable=SC2086
x86_64-w64-mingw32-g++ $cxxflags "$upscaler_dir/test_query.cpp" \
    -o "$WIN_FSR_BUILD/test_query.exe" -L"$WIN_FSR_BUILD" -lvulkan-1
# shellcheck disable=SC2086
x86_64-w64-mingw32-g++ $cxxflags "$upscaler_dir/test_context.cpp" \
    -o "$WIN_FSR_BUILD/test_context.exe" -L"$WIN_FSR_BUILD" -lvulkan-1

FFX_SDK="$FFX_SDK" VULKAN_HEADERS="$VULKAN_HEADERS" MLFI_CAPTURE_BUILD="$WIN_FSR_BUILD" \
    "$mlfi_dir/build.sh"

echo "built:"
ls -l "$WIN_FSR_BUILD/test_query.exe" "$WIN_FSR_BUILD/test_context.exe" "$WIN_FSR_BUILD/mlfi_capture.exe"
