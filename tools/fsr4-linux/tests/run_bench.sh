#!/bin/sh
# Timestamped GPU benchmark of the native FSR 4.1.2 chain.
#
# Usage: sh tests/run_bench.sh [frames]
#
# Runs the provider test with FSR4_BENCH_FRAMES on the verified cases and prints
# GPU ms and CPU record time per frame.

set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
TEST="$ROOT/build/native/fsr4_provider_test"
LOADER="$ROOT/build/native/libamd_fidelityfx_loader_vk.so"
TRACES="$ROOT/build/tests/reference"
ASSETS="${FSR4_ASSETS:-$ROOT/build/assets-runtime}"
FRAMES=${1:-20}

[ -x "$TEST" ] || { echo "missing $TEST; build the native tree first" >&2; exit 1; }
[ -f "$LOADER" ] || { echo "missing $LOADER" >&2; exit 1; }

# case rw rh ow oh flags sharpen sharpness dispatch_flags
CASES="
caseA 960 540 1920 1080 0x21 1 0.9 2
caseJ 1280 720 2560 1440 0x21 0 0.9 2
caseD 1920 1080 3840 2160 0 0 0 0
"

echo "$CASES" | while read -r case rw rh ow oh flags sharpen sharpness dispatch_flags; do
    [ -n "$case" ] || continue
    work="$ROOT/build/run/bench_$case"
    rm -rf "$work"
    mkdir -p "$work"
    ( cd "$work" && FSR4_ASSET_DIR="$ASSETS" FSR4_BENCH_FRAMES="$FRAMES" "$TEST" \
        "$TRACES/$case" "$LOADER" "$rw" "$rh" "$ow" "$oh" "$flags" "$sharpen" \
        "$sharpness" "${dispatch_flags:-0}" 2>/dev/null | grep -E "^bench:" |
        sed "s/^/$case: /" )
done
