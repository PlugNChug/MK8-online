#!/usr/bin/env python3
"""Scan a PE image for raw SPIR-V modules (the FSR3 frame-generation VK provider ships them uncompressed).

The upscaler DLL uses FFXZ-compressed blobs (linux/tools/extract_shaders.py); this provider does not, so modules are
found by the SPIR-V magic and validated structurally: header, instruction word counts, one or more OpEntryPoint, and
OpFunctionEnd terminating every function. Only read-only scanning; nothing is executed.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

MAGIC = b"\x03\x02\x23\x07"
MAGIC_WORD = 0x07230203
OP_ENTRY_POINT = 15
OP_FUNCTION = 54
OP_FUNCTION_END = 56


def scan(data: bytes):
    out = []
    i = 0
    n = len(data)
    while True:
        j = data.find(MAGIC, i)
        if j < 0:
            break
        i = j + 4
        if j + 20 > n:
            continue
        version, generator, bound, schema = struct.unpack_from("<4I", data, j + 4)
        if version >> 16 not in (1,) or bound == 0 or bound > (1 << 24) or schema != 0:
            continue
        p, entries, funcs, ends, words = j + 20, 0, 0, 0, 0
        ok = True
        while p + 4 <= n:
            w = struct.unpack_from("<I", data, p)[0]
            if w == 0:
                break
            wc, op = w >> 16, w & 0xFFFF
            if wc == 0 or p + wc * 4 > n:
                ok = False
                break
            if op == OP_ENTRY_POINT:
                entries += 1
            elif op == OP_FUNCTION:
                funcs += 1
            elif op == OP_FUNCTION_END:
                ends += 1
            p += wc * 4
            words += 1
            if words > 4_000_000:
                ok = False
                break
        if not ok or entries == 0 or funcs == 0 or ends != funcs:
            continue
        size = p - j
        # trim trailing zero padding so the module is exactly the instruction stream
        while size > 20 and data[j + size - 4:j + size] == b"\0\0\0\0" and not (data[j + size - 8:j + size - 4] == b"\0\0\0\0"):
            size -= 4
        blob = data[j:j + size]
        out.append({"offset": j, "size": size, "sha256": hashlib.sha256(blob).hexdigest(),
                    "entry_points": entries, "functions": funcs})
    # deduplicate by content, keep the first offset
    seen, uniq = set(), []
    for m in out:
        if m["sha256"] in seen:
            continue
        seen.add(m["sha256"])
        uniq.append(m)
    return uniq


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
    (out / "index.tsv").write_text("".join(f"{k}\t{m['offset']:#x}\t{m['size']}\t{m['entry_points']}\t{m['sha256']}\n"
                                           for k, m in enumerate(mods)), encoding="utf-8")
    print(f"{len(mods)} modules, {sum(m['size'] for m in mods)} bytes, "
          f"{len(set(m['sha256'] for m in mods))} unique")


if __name__ == "__main__":
    main()
