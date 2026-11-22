#!/bin/sh
# Build the RADV MLFI repro/verification harness.
#
#   tools/radv-mlfi/build.sh
#
# Needs only the Vulkan loader headers (vulkan-headers) and a C compiler.
set -eu
cd "$(dirname "$0")"
${CC:-cc} -O2 -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -o repro repro.c -lvulkan
echo "built $(pwd)/repro"
