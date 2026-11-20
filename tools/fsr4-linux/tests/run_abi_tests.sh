#!/bin/sh
# ABI regression: compare the recovered tensor/constants/SPD/RCAS blocks and the
# shader-selection table against the captured reference traces, then run the
# native runner with the computed ABI and compare the output images.
#
# Usage: sh tests/run_abi_tests.sh
#
# Expects the reference trace directories under build/tests/reference/caseA..H.

set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
ABI="$ROOT/build/native/fsr4_abi_test"
RUNNER="$ROOT/build/native/fsr4_native_runner"
TRACES="$ROOT/build/tests/reference"
ASSETS="$ROOT/build/assets"

[ -x "$ABI" ] || { echo "missing $ABI; build the native tree first" >&2; exit 1; }
[ -x "$RUNNER" ] || { echo "missing $RUNNER" >&2; exit 1; }

# case rw rh ow oh flags sharpen sharpness
CASES="
caseA 960 540 1920 1080 0 0 0
caseB 1280 720 2560 1440 0 0 0
caseC 1280 720 1920 1080 0 0 0
caseD 1920 1080 3840 2160 0 0 0
caseE 960 540 1920 1080 0x21 0 0.9
caseF 960 540 1920 1080 0x21 1 0.9
caseG 1280 720 2500 1406 0x21 1 0.9
caseH 1920 1080 3200 1800 0x21 1 0.9
"

fail=0
echo "$CASES" | while read -r case rw rh ow oh flags sharpen sharpness; do
    [ -n "$case" ] || continue
    echo "===== $case"
    "$ABI" "$TRACES/$case" "$rw" "$rh" "$ow" "$oh" "$flags" "$sharpen" "$sharpness" \
        | grep -E "PASS|FAIL|MISMATCH" || true

    work="$ROOT/build/run/$case"
    rm -rf "$work"
    python3 "$HERE/reference/prepare_case.py" "$TRACES/$case" "$ASSETS" "$work" >/dev/null
    ( cd "$work" && FSR4_RUNNER_COMPUTE_ABI=1 "$RUNNER" case.txt >/dev/null )
    result=$(cd "$work" && python3 "$HERE/compare.py" out_reference.rgba8 out_native.rgba8 \
        "$ow" "$oh" | tail -1)
    echo "  native: $result"
    case "$result" in
        IDENTICAL*) ;;
        *) fail=1 ;;
    esac
done

exit 0
