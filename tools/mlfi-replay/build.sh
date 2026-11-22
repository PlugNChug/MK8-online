#!/bin/sh
# Build the MLFI replay executable.
#
#   tools/mlfi-replay/build.sh [outdir]
#
# outdir defaults to $XDG_CACHE_HOME/mk8online/mlfi-replay/build (never the repository: the
# executable links no AMD content, but keeping generated files out of the tree is the rule here).
set -eu
cd "$(dirname "$0")"
out=${1:-${XDG_CACHE_HOME:-$HOME/.cache}/mk8online/mlfi-replay/build}
mkdir -p "$out"
${CC:-cc} -O2 -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
    -o "$out/mlfi_replay" mlfi_replay.c -lvulkan -lm
echo "built $out/mlfi_replay"
