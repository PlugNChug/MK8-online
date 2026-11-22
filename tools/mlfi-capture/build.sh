#!/bin/sh
# Build the MLFI capture harness for Windows x86-64 under Wine.
#
# Requirements: mingw-w64, Vulkan headers, the FidelityFX SDK 2.4.0 headers.
# The Vulkan headers shipped by the distribution are platform-neutral, but using
# /usr/include directly with mingw picks up the host stdint.h. They are copied to a
# private include root instead.
#
# Env:
#   FFX_SDK              Kits/FidelityFX of the FSR SDK 2.4.0 archive
#                        (default: $HOME/.cache/mk8online/fg-sdk/Kits/FidelityFX)
#   VULKAN_HEADERS       directory containing vulkan/ and vk_video/
#                        (default: /usr/include)
#   MLFI_CAPTURE_BUILD   output directory (default: $HOME/.cache/mk8online/mlfi-capture/build)

set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
: "${FFX_SDK:=$HOME/.cache/mk8online/fg-sdk/Kits/FidelityFX}"
: "${VULKAN_HEADERS:=/usr/include}"
: "${MLFI_CAPTURE_BUILD:=$HOME/.cache/mk8online/mlfi-capture/build}"

for header in "$FFX_SDK/api/include/ffx_api.h" \
              "$FFX_SDK/framegeneration/include/ffx_framegeneration.h" \
              "$VULKAN_HEADERS/vulkan/vulkan.h"; do
    if [ ! -f "$header" ]; then
        echo "missing $header" >&2
        echo "set FFX_SDK / VULKAN_HEADERS" >&2
        exit 1
    fi
done

mkdir -p "$MLFI_CAPTURE_BUILD/vkinc/vulkan"
cp -a "$VULKAN_HEADERS/vulkan/." "$MLFI_CAPTURE_BUILD/vkinc/vulkan/"
if [ -d "$VULKAN_HEADERS/vk_video" ]; then
    mkdir -p "$MLFI_CAPTURE_BUILD/vkinc/vk_video"
    cp -a "$VULKAN_HEADERS/vk_video/." "$MLFI_CAPTURE_BUILD/vkinc/vk_video/"
fi

x86_64-w64-mingw32-gcc -O2 -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
    -Wno-unknown-pragmas -Wno-missing-field-initializers -Wno-cast-function-type \
    -Wno-format-truncation -Wno-stringop-truncation -Wno-unused-variable \
    -o "$MLFI_CAPTURE_BUILD/mlfi_capture.exe" "$here/mlfi_capture.c" \
    -I"$MLFI_CAPTURE_BUILD/vkinc" \
    -I"$FFX_SDK/api/include" \
    -I"$FFX_SDK/framegeneration/include" \
    -DFFX_API_ENTRY=

echo "built $MLFI_CAPTURE_BUILD/mlfi_capture.exe"
