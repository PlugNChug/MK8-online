#!/usr/bin/env python3
"""Scan a PE image for raw SPIR-V modules (the FSR3 frame-generation VK provider ships them uncompressed).

The upscaler DLL uses FFXZ-compressed blobs (linux/tools/extract_shaders.py); this provider does not, so
modules are found by the SPIR-V magic and validated structurally: header, instruction word counts, one or
more OpEntryPoint, and OpFunctionEnd terminating every function.

Module boundaries matter here: the packed section stores kernels back to back, sometimes separated only by
a descriptor record. Walking past the first function end swallowed the following kernels, so the scanner
stops at a top-level OpFunctionEnd unless the next instruction is another OpFunction. That rule lives in
`extract_mlfi.scan_spirv`; this tool is the generic, name-agnostic front end for it.

Only read-only scanning; nothing is executed.
"""
import argparse
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

import extract_mlfi


def scan(data: bytes):
    return [
        {"offset": module.offset, "size": module.size, "sha256": module.sha256,
         "entry_points": 1, "functions": module.functions, "entry": module.entry}
        for module in extract_mlfi.scan_spirv(data)
    ]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dll")
    ap.add_argument("out")
    args = ap.parse_args()
    data = Path(args.dll).read_bytes()
    mods = scan(data)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    for k, m in enumerate(mods):
        (out / f"fg_{k:04d}_{m['offset']:08x}.spv").write_bytes(data[m["offset"]:m["offset"] + m["size"]])
    (out / "index.json").write_text(json.dumps(mods, indent=1), encoding="utf-8")
    (out / "index.tsv").write_text("".join(f"{k}\t{m['offset']:#x}\t{m['size']}\t{m['entry']}\t{m['sha256']}\n"
                                           for k, m in enumerate(mods)), encoding="utf-8")
    print(f"{len(mods)} modules, {sum(m['size'] for m in mods)} bytes, "
          f"{len(set(m['sha256'] for m in mods))} unique")


if __name__ == "__main__":
    main()
