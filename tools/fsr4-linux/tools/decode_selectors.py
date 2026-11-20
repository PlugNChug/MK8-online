#!/usr/bin/env python3
"""Decode all 324 FSR 4.1.2 VK selector tables into (table, key) -> blob."""
import argparse
import collections
import json
import re
import struct
from pathlib import Path

ap = argparse.ArgumentParser()
ap.add_argument("dll", type=Path)
ap.add_argument("root", type=Path)
args = ap.parse_args()

DLL = args.dll
ROOT = args.root
STRIDE = 0x130
KEYS = 64

data = open(DLL, "rb").read()
meta = json.load(open(f"{ROOT}/index.json"))
cat = {r["id"]: r for r in json.load(open(f"{ROOT}/catalog_raw.json"))}
ib = meta["image_base"]
sections = meta["sections"]
by_rva = {b["rva"]: b["id"] for b in meta["blobs"]}


def rva2off(rva):
    for s in sections:
        if s["rva"] <= rva < s["rva"] + s["rawsize"]:
            return s["raw"] + rva - s["rva"]
    return None


def u32(rva):
    o = rva2off(rva)
    return struct.unpack_from("<I", data, o)[0] if o is not None else None


def is_blob_va(va):
    return 0x180000000 <= va < 0x182600000 and (va - ib) in by_rva


text = [s for s in sections if s["name"] == ".text"][0]
tstart, tend, trva = text["raw"], text["raw"] + text["rawsize"], text["rva"]

pairs = {}
for m in re.finditer(rb"\x48\x69\xc0\x30\x01\x00\x00", data[tstart:tend]):
    o = tstart + m.start()
    insn_rva = trva + (o - tstart)
    if data[o + 7:o + 10] != b"\x48\x8d\x0d":
        continue
    disp = struct.unpack_from("<i", data, o + 10)[0]
    rec_rva = insn_rva + 14 + disp
    pre = data[max(tstart, o - 96):o]
    j = pre.rfind(b"\x48\x8d\x0d")
    if j < 0:
        continue
    o2 = max(tstart, o - 96) + j
    disp2 = struct.unpack_from("<i", data, o2 + 3)[0]
    idx_rva = trva + (o2 - tstart) + 7 + disp2
    pairs[(idx_rva, rec_rva)] = pairs.get((idx_rva, rec_rva), 0) + 1

tables = []
for (idx_rva, rec_rva), refs in sorted(pairs.items()):
    idx = [u32(idx_rva + 4 * k) for k in range(KEYS)]
    recs = {}
    for i in range(512):
        o = rva2off(rec_rva + i * STRIDE)
        if o is None:
            break
        size, pad, ptr = struct.unpack_from("<IIQ", data, o)
        if is_blob_va(ptr):
            recs[i] = (size, by_rva[ptr - ib])
    keymap = {}
    for k, ix in enumerate(idx):
        if ix in recs:
            keymap[k] = recs[ix][1]
    names = sorted({cat[b]["entry_names"][0] for b in set(keymap.values())})
    caps = sorted({tuple(cat[b]["capabilities"]) for b in set(keymap.values())})
    exts = sorted({tuple(cat[b]["extensions"]) for b in set(keymap.values())})
    tables.append({
        "index_table_rva": idx_rva,
        "record_table_rva": rec_rva,
        "refs_per_table": refs,
        "index_values": idx,
        "record_count": len(recs),
        "key_to_blob": {str(k): v for k, v in keymap.items()},
        "distinct_blobs": sorted(set(keymap.values())),
        "entry_names": names,
        "capability_sets": caps,
        "extension_sets": exts,
    })

json.dump(tables, open(f"{ROOT}/selector_tables.json", "w"), indent=1)

print("tables:", len(tables))
print("refs per table:", collections.Counter(t["refs_per_table"] for t in tables))
print("64/64 keys resolved:", sum(1 for t in tables if len(t["key_to_blob"]) == 64))
print("distinct blobs per table:", collections.Counter(len(t["distinct_blobs"]) for t in tables).most_common())
print("\nentry-name sets:")
for names, c in collections.Counter(tuple(t["entry_names"]) for t in tables).most_common(40):
    print(f"  {c:4d} {names}")
print("\ntotal distinct blobs referenced:", len({b for t in tables for b in t['distinct_blobs']}))
