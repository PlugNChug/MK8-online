#!/usr/bin/env python3
"""Copy only the shader blobs and model banks the native provider can select.

Usage: trim_assets.py <assets-dir> <out-dir>

Reads tools/data/fsr4_412_selector_map.json and the extractor catalogue, then
writes <out-dir>/shaders/<needed>.spv and <out-dir>/models/FP8_standard.bin.
The full extraction is ~102 MB; the selected set is a small fraction.
"""

import json
import os
import shutil
import sys


def collect_ids(map_path, catalog):
    with open(map_path, encoding="utf-8") as handle:
        data = json.load(handle)
    ids = set()

    def add(value):
        if value:
            ids.add(int(value))

    # Key-independent passes (skip prepass/postpass; covered by the key tables).
    for name, buckets in data["passes"].items():
        if name in ("prepass", "postpass", "main", "fsr_rcas_pass"):
            continue
        for entry in buckets.values():
            add(entry.get("id"))

    for bucket in data["prepass_key_table"].values():
        for value in bucket["key_to_id"].values():
            add(value)

    for bucket in data["postpass"].values():
        for value in bucket.values():
            add(value)

    add(329)  # SPD
    for value in (327, 330, 328, 324, 325):
        add(value)  # RCAS colour/exposure variants
    return ids


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    assets = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "..", "build", "assets")
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(here, "..", "build", "assets-runtime")
    catalog = json.load(open(os.path.join(assets, "catalog_final.json")))
    by_id = {c["id"]: c for c in catalog}
    ids = collect_ids(os.path.join(here, "data", "fsr4_412_selector_map.json"), catalog)

    shaders_out = os.path.join(out, "shaders")
    models_out = os.path.join(out, "models")
    os.makedirs(shaders_out, exist_ok=True)
    os.makedirs(models_out, exist_ok=True)
    total = 0
    copied = 0
    for blob_id in sorted(ids):
        entry = by_id.get(blob_id)
        if not entry:
            print(f"warning: blob {blob_id} not in catalog", file=sys.stderr)
            continue
        name = os.path.basename(entry["file"])
        src = os.path.join(assets, "shaders", name)
        if not os.path.exists(src):
            print(f"warning: missing {src}", file=sys.stderr)
            continue
        dst = os.path.join(shaders_out, name)
        if not os.path.exists(dst) or os.path.getsize(dst) != os.path.getsize(src):
            shutil.copyfile(src, dst)
        total += os.path.getsize(src)
        copied += 1

    model_src = os.path.join(assets, "models", "FP8_standard.bin")
    shutil.copyfile(model_src, os.path.join(models_out, "FP8_standard.bin"))
    total += os.path.getsize(model_src)

    print(f"copied {copied} shader blobs + FP8_standard.bin, {total / 1024 / 1024:.1f} MB -> {out}")


if __name__ == "__main__":
    main()
