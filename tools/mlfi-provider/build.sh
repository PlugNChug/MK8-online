#!/bin/sh
# Build the MK8-online MLFI provider and its headless validation program.
#
#   tools/mlfi-provider/build.sh [outdir]
#
# outdir defaults to $XDG_CACHE_HOME/mk8online/mlfi-provider/build.  Generated files (the graph
# header from the committed capture tables, the shared library and the test executable) stay out
# of the repository.
#
# Requirements: a C compiler, the Vulkan loader/headers, and python3.  The provider reads the
# user's own MLFI extraction at runtime (MK8_MLFI_MODULES / MK8_MLFI_MODEL, see README.md); this
# build does not read any AMD byte.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out=${1:-${XDG_CACHE_HOME:-$HOME/.cache}/mk8online/mlfi-provider/build}
mkdir -p "$out"

python3 "$here/gen_graph_header.py" --out "$out/mlfi_graph.h" >/dev/null

CC=${CC:-cc}
CFLAGS=${CFLAGS:--O2 -g}
WARN="-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-unknown-pragmas -Wno-deprecated-declarations"
INC="-I$out -I$here -I$root/runtime/mk8/graphics"

$CC $CFLAGS -std=gnu11 $WARN $INC -fPIC -shared \
    -o "$out/libmk8_mlfi_framegeneration_vk.so" "$here/mlfi_provider.c" -lvulkan -lm

$CC $CFLAGS -std=gnu11 $WARN $INC \
    -o "$out/mlfi_provider_test" "$here/mlfi_provider_test.c" \
    -L"$out" -lmk8_mlfi_framegeneration_vk -Wl,-rpath,'$ORIGIN' -lvulkan -lm

echo "built $out/libmk8_mlfi_framegeneration_vk.so"
echo "built $out/mlfi_provider_test"
