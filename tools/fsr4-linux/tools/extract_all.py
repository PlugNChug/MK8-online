#!/usr/bin/env python3
"""Extract and catalog FSR 4.1.2 Vulkan assets from AMD's signed DLL.

Pipeline:
  1. extract_shaders.py   -> build/assets/shaders + index.json
  2. extract_models.py    -> build/assets/models + manifest.json
  3. analyze_shaders.py   -> build/assets/catalog_raw.json
  4. build_catalog.py     -> build/assets/catalog_final.json
  5. decode_selectors.py  -> build/assets/selector_tables.json
  6. verify_assets.py     -> PASS/FAIL

The signed DLL is read, never modified. Generated assets stay under build/ and are
not part of the source tree.
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
LINUX = TOOLS.parent
# Inside the SDK this is <sdk>/linux, so its parent is the SDK. The vendored copy
# lives in tools/fsr4-linux, where the SDK is elsewhere: set FSR4_SDK_ROOT.
SDK = Path(os.environ.get("FSR4_SDK_ROOT") or LINUX.parent)
DEFAULT_DLL = SDK / "Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_vk.dll"
DEFAULT_OUT = LINUX / "build/assets"


def run(cmd):
    print("+", " ".join(str(c) for c in cmd), flush=True)
    subprocess.run(cmd, check=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dll", type=Path, default=DEFAULT_DLL)
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    ap.add_argument("--skip-shaders", action="store_true")
    ap.add_argument("--skip-models", action="store_true")
    ap.add_argument("--skip-catalog", action="store_true")
    args = ap.parse_args()

    if not args.dll.is_file():
        raise SystemExit(f"signed DLL not found: {args.dll}")
    args.out.mkdir(parents=True, exist_ok=True)

    py = sys.executable
    if not args.skip_shaders:
        run([py, str(TOOLS / "extract_shaders.py"), str(args.dll), str(args.out)])
    if not args.skip_models:
        run([py, str(TOOLS / "extract_models.py"), str(args.dll), str(args.out / "models")])
    if not args.skip_catalog:
        run([py, str(TOOLS / "analyze_shaders.py"), str(args.out)])
        run([py, str(TOOLS / "build_catalog.py"), str(args.out)])
        run([py, str(TOOLS / "decode_selectors.py"), str(args.dll), str(args.out)])

    source_sha = hashlib.sha256(args.dll.read_bytes()).hexdigest()
    index = json.loads((args.out / "index.json").read_text())
    models = json.loads((args.out / "models/manifest.json").read_text())
    manifest = {
        "schema": 1,
        "source": str(args.dll),
        "source_sha256": source_sha,
        "shader_count": index["blob_count"],
        "shader_uncompressed_bytes": index["total_uncompressed"],
        "models": models["models"],
        "catalog_present": (args.out / "catalog_final.json").is_file(),
        "selectors_present": (args.out / "selector_tables.json").is_file(),
    }
    (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"wrote {args.out / 'manifest.json'}")

    verify = TOOLS / "verify_assets.py"
    if verify.is_file():
        run([py, str(verify), str(args.out)])


if __name__ == "__main__":
    main()
