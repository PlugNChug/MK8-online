#!/bin/sh
# Stage the native FSR 4.1.2 provider, loader and runtime assets next to a game
# executable.
#
# Usage: sh tools/stage_runtime.sh <destination-dir>
#
# The destination is typically the game's build output directory or a package
# staging directory. The provider looks for <dir>/fsr4-assets when
# FSR4_ASSET_DIR is not set, and the loader looks for the provider next to
# itself.

set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
DEST=${1:?usage: stage_runtime.sh <destination-dir>}

PROVIDER="$ROOT/build/native/libamd_fidelityfx_upscaler_vk.so"
LOADER="$ROOT/build/native/libamd_fidelityfx_loader_vk.so"
ASSETS="$ROOT/build/assets"
[ -f "$PROVIDER" ] || { echo "missing $PROVIDER; build the native tree first" >&2; exit 1; }
[ -f "$LOADER" ] || { echo "missing $LOADER" >&2; exit 1; }
[ -d "$ASSETS/shaders" ] || { echo "missing $ASSETS/shaders; run the extractor" >&2; exit 1; }

mkdir -p "$DEST/fsr4-assets"
cp "$PROVIDER" "$DEST/"
cp "$LOADER" "$DEST/"
rm -rf "$DEST/fsr4-assets/shaders" "$DEST/fsr4-assets/models"
python3 "$ROOT/tools/trim_assets.py" "$ASSETS" "$DEST/fsr4-assets" >/dev/null

echo "staged provider, loader and $(du -sh "$DEST/fsr4-assets" | cut -f1) of assets in $DEST"
