#!/usr/bin/env python3
"""Prototype: locate FSR 4.1.2 model banks in the shipped Windows DLLs.

No hardcoded file offsets. Finds the four model classes' getWeights(preset)
dispatchers, reads their six {pointer,size} accessors, dedupes identical banks.

Usage: extract_fsr4_412.py <dll> <outdir>
"""
import hashlib
import json
import re
import struct
import sys
from pathlib import Path

IMAGE_BASE = 0x180000000
DISPATCH_RE = (
    b"\x41\x83\xf8\x05\x0f\x87....\x41\x8b\xc0\x4c\x8d\x05...."
    b"\x41\x8b\x8c\x80"
)
ACCESSOR_RE = b"\xb8....\x48\x8d\x0d....\x48\x89"


class PE:
    def __init__(self, path):
        self.data = Path(path).read_bytes()
        d = self.data
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        nsec = struct.unpack_from("<H", d, pe + 6)[0]
        opt = pe + 24
        optsz = struct.unpack_from("<H", d, pe + 20)[0]
        self.sections = []
        for i in range(nsec):
            off = opt + optsz + i * 40
            name = d[off : off + 8].rstrip(b"\0").decode(errors="replace")
            vsz, va, rsz, raw = struct.unpack_from("<4I", d, off + 8)
            self.sections.append((name, va, vsz, raw, rsz))

    def rva_to_raw(self, rva):
        for _, va, vsz, raw, rsz in self.sections:
            if va <= rva < va + rsz:
                return raw + (rva - va)
        return None

    def read_rva(self, rva, size):
        o = self.rva_to_raw(rva)
        if o is None or o + size > len(self.data):
            raise ValueError(f"unmapped RVA {rva:#x}")
        return self.data[o : o + size]


def find_dispatchers(pe):
    out = []
    for m in re.finditer(DISPATCH_RE, pe.data):
        # .text is the first section and is 1:1 mapped from file offset 0x400
        name, va, vsz, raw, rsz = pe.sections[0]
        if not (raw <= m.start() < raw + rsz):
            continue
        out.append(va + (m.start() - raw))
    return out


def parse_table(pe, disp_rva):
    raw = pe.rva_to_raw(disp_rva)
    i = pe.data.find(b"\x41\x8b\x8c\x80", raw, raw + 0x60)
    table_rva = struct.unpack_from("<i", pe.data, i + 4)[0]
    banks = []
    for k in range(6):
        fn_rva = struct.unpack_from("<i", pe.read_rva(table_rva + 4 * k, 4), 0)[0]
        fr = pe.rva_to_raw(fn_rva)
        size = struct.unpack_from("<I", pe.data, fr + 1)[0]
        disp = struct.unpack_from("<i", pe.data, fr + 8)[0]
        ptr_va = IMAGE_BASE + fn_rva + 12 + disp
        ptr_rva = ptr_va - IMAGE_BASE
        banks.append((ptr_rva, size))
    return banks


def main():
    dll, outdir = sys.argv[1], Path(sys.argv[2])
    pe = PE(dll)
    disps = find_dispatchers(pe)
    if len(disps) != 4:
        raise SystemExit(f"expected 4 model dispatchers, found {len(disps)}")

    # Model class attribution: stable order in the image (Lite, Int8, Navi2, FP8)
    # is derived from each dispatcher's code position relative to the accessor
    # groups; names are attached from vtable slot 1 in the full analysis.
    labels = ["LiteInt8", "Int8", "Int8Navi2", "FP8"]
    manifest = {"source": str(dll), "source_sha256": hashlib.sha256(pe.data).hexdigest(),
                "source_size": len(pe.data), "models": {}}
    outdir.mkdir(parents=True, exist_ok=True)

    for label, disp in zip(labels, sorted(disps)):
        banks = parse_table(pe, disp)
        seen = {}
        for preset, (rva, size) in enumerate(banks):
            blob = pe.read_rva(rva, size)
            digest = hashlib.sha256(blob).hexdigest()
            seen.setdefault(digest, {"rva": rva, "size": size, "presets": []})
            seen[digest]["presets"].append(preset)
        if len(seen) != 2:
            raise SystemExit(f"{label}: expected 2 unique banks, got {len(seen)}")
        entry = {"dispatcher_rva": disp, "banks": {}}
        for digest, rec in seen.items():
            name = ("ultraperf" if 5 in rec["presets"] else "standard")
            path = outdir / f"{label}_{name}.bin"
            path.write_bytes(pe.read_rva(rec["rva"], rec["size"]))
            entry["banks"][name] = {
                "rva": rec["rva"], "size": rec["size"],
                "presets": rec["presets"], "sha256": digest,
                "file": path.name,
            }
            print(f"{label:9} {name:8} rva={rec['rva']:#08x} size={rec['size']:#07x} "
                  f"presets={rec['presets']} sha256={digest[:16]}")
        manifest["models"][label] = entry
    (outdir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    main()
