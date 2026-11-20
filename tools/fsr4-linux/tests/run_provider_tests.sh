#!/bin/sh
# End-to-end provider regression: create an FFX context through the loader,
# dispatch one frame from the captured inputs, and compare with the reference.
#
# Usage: sh tests/run_provider_tests.sh
#
# Requires the native build (provider, loader, provider test) and the reference
# traces under build/tests/reference/caseA..H.

set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
TEST="$ROOT/build/native/fsr4_provider_test"
LOADER="$ROOT/build/native/libamd_fidelityfx_loader_vk.so"
TRACES="$ROOT/build/tests/reference"
ASSETS="${FSR4_ASSETS:-$ROOT/build/assets}"

[ -x "$TEST" ] || { echo "missing $TEST; build the native tree first" >&2; exit 1; }
[ -f "$LOADER" ] || { echo "missing $LOADER" >&2; exit 1; }

# case rw rh ow oh flags sharpen sharpness dispatch_flags
CASES="
caseA 960 540 1920 1080 0 0 0 0
caseB 1280 720 2560 1440 0 0 0 0
caseC 1280 720 1920 1080 0 0 0 0
caseD 1920 1080 3840 2160 0 0 0 0
caseE 960 540 1920 1080 0x21 0 0.9 0
caseF 960 540 1920 1080 0x21 1 0.9 0
caseG 1280 720 2500 1406 0x21 1 0.9 0
caseH 1920 1080 3200 1800 0x21 1 0.9 0
caseI 960 540 1920 1080 0x21 0 0.9 2
caseJ 1280 720 2560 1440 0x21 0 0.9 2
caseK 3840 2160 7680 4320 0x21 0 0.9 2
"

echo "$CASES" | while read -r case rw rh ow oh flags sharpen sharpness dispatch_flags; do
    [ -n "$case" ] || continue
    work="$ROOT/build/run/provider_$case"
    rm -rf "$work"
    mkdir -p "$work"
    ( cd "$work" && FSR4_ASSET_DIR="$ASSETS" "$TEST" "$TRACES/$case" "$LOADER" \
        "$rw" "$rh" "$ow" "$oh" "$flags" "$sharpen" "$sharpness" "${dispatch_flags:-0}" \
        >/dev/null )
    result=$(cd "$work" && python3 "$HERE/compare.py" "$TRACES/$case/out_color.rgba8" \
        out_provider.rgba8 "$ow" "$oh" | tail -1)
    echo "$case: $result"
done
