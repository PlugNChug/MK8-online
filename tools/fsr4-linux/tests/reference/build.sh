#!/bin/sh
# Build the Windows-PE reference tracer with mingw.
#
# Usage: sh build.sh [output-dir]
#
# Environment:
#   FSR4_SDK_ROOT    FidelityFX SDK 2.4.0 tree (default: ../../.. from this file)
#   FSR4_SIGNEDBIN   directory containing the signed loader/upscaler DLLs
#                    (default: $FSR4_SDK_ROOT/Kits/FidelityFX/signedbin)
#
# The output directory gets fsr4_trace.exe plus the two DLLs it loads. Nothing
# here is committed; the DLLs are the user's own AMD-signed copies.

set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
LINUX_ROOT=$(cd "$HERE/../.." && pwd)
SDK_ROOT=${FSR4_SDK_ROOT:-$(cd "$LINUX_ROOT/.." && pwd)}
SIGNEDBIN=${FSR4_SIGNEDBIN:-$SDK_ROOT/Kits/FidelityFX/signedbin}
OUT=${1:-$LINUX_ROOT/build/tests/reference}

CXX=${CXX:-x86_64-w64-mingw32-g++}
CC=${CC:-x86_64-w64-mingw32-gcc}

command -v "$CXX" >/dev/null 2>&1 || {
    echo "missing $CXX (install mingw-w64)" >&2
    exit 1
}

VK_INC="$LINUX_ROOT/build/tests/reference/vk-include"
mkdir -p "$VK_INC"
# The Khronos headers are platform-neutral, but the mingw sysroot has no
# Vulkan headers. Copy them into a dedicated include root so mingw never sees
# the host libc headers.
rm -rf "$VK_INC/vulkan" "$VK_INC/vk_video"
cp -r /usr/include/vulkan "$VK_INC/vulkan"
if [ -d /usr/include/vk_video ]; then
    cp -r /usr/include/vk_video "$VK_INC/vk_video"
fi

mkdir -p "$OUT"
"$CXX" -x c++ -O2 -std=c++17 \
    -I"$SDK_ROOT/Kits/FidelityFX/api/include" \
    -I"$SDK_ROOT/Kits/FidelityFX/upscalers/include" \
    -I"$VK_INC" \
    -o "$OUT/fsr4_trace.exe" "$HERE/fsr4_trace.c"

for dll in amd_fidelityfx_loader_vk.dll amd_fidelityfx_upscaler_vk.dll; do
    if [ -f "$SIGNEDBIN/$dll" ]; then
        cp "$SIGNEDBIN/$dll" "$OUT/"
    elif [ -f "$OUT/$dll" ]; then
        :
    else
        echo "warning: $dll not found in $SIGNEDBIN" >&2
    fi
done

echo "built $OUT/fsr4_trace.exe"
