#!/usr/bin/env python3
"""Build the FSR 4.1.2 VK asset catalog from extracted SPIR-V metadata."""
import collections
import json
import struct
import sys
from pathlib import Path

import spv

ROOT = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(".")
cat = json.load(open(f"{ROOT}/catalog_raw.json"))

TIERS = {15392: "1080", 30752: "2160", 61472: "4320"}
SRGB = {0.04045, 0.055, 0.077399, 0.003131, 12.92, 1.055}
PQ = {0.8359375, 18.8515625, 18.6875, 0.159302, 78.84375, 10000.0}
GAMMA = {2.2, 0.4545, 0.454545, 1.0 / 2.2}


def scan(module):
    """Return (tier, color_space, uses_jitter_member, ubo_members)."""
    data = module.data
    n = len(data) // 4
    w = struct.unpack_from(f"<{n}I", data, 0)
    ftypes = {}
    itypes = {}
    ivals = {}
    i = 5
    while i < n:
        wc = w[i] >> 16
        op = w[i] & 0xFFFF
        if op == 21:
            itypes[w[i + 1]] = w[i + 2]
        elif op == 22:
            ftypes[w[i + 1]] = w[i + 2]
        elif op == 43 and wc >= 4 and itypes.get(w[i + 1]) == 32:
            ivals[w[i + 2]] = w[i + 3]
        i += wc

    i = 5
    fconst = set()
    tier = set()
    chains = []
    while i < n:
        wc = w[i] >> 16
        op = w[i] & 0xFFFF
        if op == 43 and wc >= 4 and ftypes.get(w[i + 1]) == 32:
            fconst.add(round(struct.unpack("<f", struct.pack("<I", w[i + 3]))[0], 6))
        elif op == 43 and wc >= 4 and itypes.get(w[i + 1]) == 32:
            if w[i + 3] in TIERS:
                tier.add(TIERS[w[i + 3]])
        elif op == 65 and wc >= 4:  # OpAccessChain
            chains.append(list(w[i + 3:i + wc]))
        i += wc

    if len(fconst & SRGB) >= 2:
        color = "srgb"
    elif len(fconst & PQ) >= 2:
        color = "pq"
    elif fconst & {8.51788, 150.0, 0.1174}:
        color = "linear"
    else:
        color = "nonlinear"

    # jitter-cancellation path: access chain into MLSR UBO at member 7
    ubo_ids = set()
    for b in module.descriptor_bindings():
        if b["name"] == "MLSR_Optimized_Constants":
            ubo_ids.add(b["id"])
    uses_jitter = False
    for ch in chains:
        base = ch[0]
        if base in ubo_ids and len(ch) >= 2 and ivals.get(ch[1]) == 7:
            uses_jitter = True
            break
    return sorted(tier), color, uses_jitter


def model_of(r):
    if "SPV_EXT_float8" in r["extensions"]:
        return "fp8-wmma"
    if 39 in r["capability_ids"]:
        return "int8"
    if "SPV_KHR_compute_shader_derivatives" in r["extensions"]:
        return "int8-deriv"
    return "fp8"


out = []
for r in cat:
    m = spv.Module(open(f"{ROOT}/{r['file']}", "rb").read())
    tiers, color, jitter = scan(m)
    bn = [b["name"] for b in r["bindings"]]
    entries = r["entry_names"]
    e = entries[0]
    if "prepass" in e:
        family = "prepass"
    elif "postpass" in e:
        family = "postpass"
    elif e.startswith("fsr4_model"):
        family = "pass" + e.split("no_scale_pass")[1]
    elif e == "fsr_rcas_pass":
        family = "rcas"
    elif e == "CS":
        family = "fsr3-cs"
    elif e == "main":
        family = "debug/utility"
    else:
        family = e
    out.append({
        "id": r["id"],
        "file": r["file"],
        "file_offset": r["file_offset"],
        "rva": r["rva"],
        "sha256": r["sha256"],
        "spirv_size": r["size"],
        "entry_points": entries,
        "pass": family,
        "model": model_of(r),
        "tier": tiers[0] if len(tiers) == 1 else ("|".join(tiers) if tiers else None),
        "color_space": color,
        "depth": "r_depth" in bn,
        "exposure_external": "r_input_exposure" in bn,
        "exposure_auto": "r_auto_exposure_texture" in bn,
        "debug": ("rw_debug_visualization" in bn or "r_debug_visualization" in bn),
        "tensor_sizes": "CsTensorSizes" in bn,
        "jitter_cancel": jitter,
        "capabilities": r["capability_ids"],
        "extensions": r["extensions"],
        "bindings": [
            {"name": b["name"], "set": b["set"], "binding": b["binding"],
             "class": b["storage_class"], "type": b["type"]}
            for b in r["bindings"]
        ],
    })

json.dump(out, open(f"{ROOT}/catalog_final.json", "w"), indent=1)

print("modules:", len(out))
print("\npass family counts:")
for k, v in collections.Counter(x["pass"] for x in out).most_common():
    print(f"  {v:5d} {k}")
print("\nmodel counts (fsr4 only):")
fsr4 = [x for x in out if x["pass"] not in ("fsr3-cs", "rcas", "debug/utility")]
for k, v in collections.Counter(x["model"] for x in fsr4).most_common():
    print(f"  {v:5d} {k}")
print("\ncolor counts (fsr4):")
for k, v in collections.Counter(x["color_space"] for x in fsr4).most_common():
    print(f"  {v:5d} {k}")
print("\ntier coverage (fsr4):", collections.Counter(x["tier"] for x in fsr4))
print("\njitter:", collections.Counter(x["jitter_cancel"] for x in fsr4))
print("\nprepass matrix (tier x model x color x depth x expo):")
c = collections.Counter()
for x in out:
    if x["pass"] == "prepass":
        c[(x["tier"], x["model"], x["color_space"], x["depth"],
           "ext" if x["exposure_external"] else ("auto" if x["exposure_auto"] else "none"))] += 1
for k, v in sorted(c.items()):
    print(f"  {v:4d} {k}")
