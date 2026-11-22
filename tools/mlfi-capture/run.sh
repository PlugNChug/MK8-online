#!/bin/sh
# Run the MLFI capture harness under Wine and leave the whole capture in one directory
# (outside the repository). The signed DLL is copied there too; never commit that directory.
#
# Usage: run.sh [capture-dir] [-- extra mlfi_capture.exe args]
#   e.g. run.sh ~/.cache/mk8online/mlfi-capture/canonical-4k -- --width 1280 --height 720 --frames 2
#
# Env:
#   FFX_DLL              signed amd_fidelityfx_framegeneration_vk.dll (required)
#   WINEPREFIX           Wine prefix (default: $HOME/.cache/mk8online/mlfi-wine)
#   MLFI_CAPTURE_BUILD   build directory used by build.sh

set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
: "${FFX_DLL:?set FFX_DLL to the signed amd_fidelityfx_framegeneration_vk.dll}"
: "${WINEPREFIX:=$HOME/.cache/mk8online/mlfi-wine}"
: "${MLFI_CAPTURE_BUILD:=$HOME/.cache/mk8online/mlfi-capture/build}"

if [ ! -f "$FFX_DLL" ]; then
    echo "FFX_DLL not found: $FFX_DLL" >&2
    exit 1
fi

"$here/build.sh" >/dev/null

outdir=${1:-$HOME/.cache/mk8online/mlfi-capture/run}
if [ "$#" -gt 0 ]; then
    shift
fi
mkdir -p "$outdir"
if [ -e "$outdir/log.txt" ]; then
    echo "warning: $outdir already contains a capture; use a fresh directory" >&2
fi
cp -f "$FFX_DLL" "$outdir/amd_fidelityfx_framegeneration_vk.dll"
cp -f "$MLFI_CAPTURE_BUILD/mlfi_capture.exe" "$outdir/"
sha256sum "$outdir/amd_fidelityfx_framegeneration_vk.dll" > "$outdir/dll.sha256"

cd "$outdir"
export WINEPREFIX WINEDEBUG=-all
# Wine maps the current directory to the Windows current directory, so --out . is enough.
timeout 600 nice -n 10 wine ./mlfi_capture.exe --dll amd_fidelityfx_framegeneration_vk.dll --out . "$@"
