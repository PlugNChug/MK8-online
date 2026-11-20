#!/bin/sh
# Prepare, run and compare one FSR 4.1.2 case.
#
# Usage: sh tests/run_case.sh <trace-dir> <assets-dir> <work-dir>
#
# <trace-dir> is a directory produced by tests/reference/fsr4_trace.c.
# The runner writes out_native.rgba8 in <work-dir>; compare.py checks it
# against the reference output copied there by prepare_case.py.

set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)

TRACE=${1:?usage: run_case.sh <trace-dir> <assets-dir> <work-dir>}
ASSETS=${2:?}
WORK=${3:?}

RUNNER="$ROOT/build/native/fsr4_native_runner"
[ -x "$RUNNER" ] || {
    echo "missing $RUNNER; build with: cmake -S $ROOT -B $ROOT/build/native && cmake --build $ROOT/build/native" >&2
    exit 1
}

python3 "$HERE/reference/prepare_case.py" "$TRACE" "$ASSETS" "$WORK"

SIZE=$(sed -n 's/^output \([0-9]*\) \([0-9]*\)$/\1 \2/p' "$WORK/case.txt")
[ -n "$SIZE" ] || { echo "cannot read output size from $WORK/case.txt" >&2; exit 1; }
W=$(echo "$SIZE" | cut -d' ' -f1)
H=$(echo "$SIZE" | cut -d' ' -f2)

cd "$WORK"
"$RUNNER" case.txt
python3 "$HERE/compare.py" out_reference.rgba8 out_native.rgba8 "$W" "$H"
