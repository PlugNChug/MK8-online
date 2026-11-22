#!/usr/bin/env python3
"""Extract FFXZ-compressed shader blobs from a PE image without loading it.

Format (Kits/FidelityFX/api/internal/ffx_blob_compression.h):
    u32 magic = 0x465A5846   (file bytes: 46 58 5A 46)
    u32 uncompressedSize
    u8  payload[]            zlib stream (0x78 0x9c / 0x78 0xda)

No offsets are hardcoded: every byte position in the image is scanned for the
magic, then each candidate is validated structurally (sane size, clean zlib
termination, exact output length, SPIR-V magic).
"""
import argparse
import hashlib
import json
import re
import struct
import zlib
from pathlib import Path

MAGIC = 0x465A5846
MAGIC_BYTES = struct.pack("<I", MAGIC)  # b'FXZF' in file order
SPV_MAGIC = 0x07230203
MIN_SIZE = 64
MAX_SIZE = 64 * 1024 * 1024


def parse_pe(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("not a PE image")
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe + 20)[0]
    opt = pe + 24
    image_base = struct.unpack_from("<Q", data, opt + 24)[0]
    sections = []
    for i in range(nsec):
        hdr = opt + opt_size + i * 40
        name = data[hdr:hdr + 8].rstrip(b"\0").decode("ascii", "replace")
        vsize, rva, rawsize, raw = struct.unpack_from("<4I", data, hdr + 8)
        sections.append({"name": name, "rva": rva, "vsize": vsize,
                         "raw": raw, "rawsize": rawsize})
    return {"image_base": image_base, "sections": sections}


def section_for_offset(pe, off):
    for s in pe["sections"]:
        if s["raw"] <= off < s["raw"] + s["rawsize"]:
            return s
    return None


def rva_for_offset(pe, off):
    s = section_for_offset(pe, off)
    if not s:
        return None
    return s["rva"] + off - s["raw"]


def extract(data, pe):
    blobs = []
    seen = set()
    for m in re.finditer(re.escape(MAGIC_BYTES), data):
        off = m.start()
        if off in seen:
            continue
        if off + 8 > len(data):
            continue
        magic, usize = struct.unpack_from("<II", data, off)
        if magic != MAGIC or not (MIN_SIZE <= usize <= MAX_SIZE):
            continue
        # zlib stream must end cleanly and inflate to exactly usize bytes
        z = zlib.decompressobj()
        feed = data[off + 8:off + 8 + min(len(data) - off - 8, usize * 4 + 4096)]
        try:
            out = z.decompress(feed)
            out += z.flush()
        except zlib.error:
            continue
        if not z.eof or len(out) != usize:
            continue
        if struct.unpack_from("<I", out, 0)[0] != SPV_MAGIC:
            continue
        consumed = len(feed) - len(z.unused_data)
        blobs.append({
            "file_offset": off,
            "rva": rva_for_offset(pe, off),
            "section": (section_for_offset(pe, off) or {}).get("name"),
            "uncompressed_size": usize,
            "compressed_size": consumed,
            "total_size": 8 + consumed,
            "sha256": hashlib.sha256(out).hexdigest(),
            "spirv": out,
        })
        seen.add(off)
    blobs.sort(key=lambda b: b["file_offset"])
    return blobs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dll", type=Path)
    ap.add_argument("out", type=Path)
    args = ap.parse_args()

    data = args.dll.read_bytes()
    pe = parse_pe(data)
    blobs = extract(data, pe)
    shaders = args.out / "shaders"
    shaders.mkdir(parents=True, exist_ok=True)

    index = []
    for i, b in enumerate(blobs):
        name = f"blob_{i:04d}_{b['file_offset']:08x}.spv"
        (shaders / name).write_bytes(b["spirv"])
        index.append({
            "id": i,
            "file": f"shaders/{name}",
            "file_offset": b["file_offset"],
            "file_offset_hex": f"0x{b['file_offset']:x}",
            "rva": b["rva"],
            "rva_hex": f"0x{b['rva']:x}" if b["rva"] is not None else None,
            "section": b["section"],
            "uncompressed_size": b["uncompressed_size"],
            "compressed_size": b["compressed_size"],
            "sha256": b["sha256"],
        })

    meta = {
        "schema": 1,
        "source": str(args.dll),
        "source_size": len(data),
        "source_sha256": hashlib.sha256(data).hexdigest(),
        "image_base": pe["image_base"],
        "sections": pe["sections"],
        "magic": f"0x{MAGIC:08X}",
        "blob_count": len(index),
        "total_uncompressed": sum(i["uncompressed_size"] for i in index),
        "total_compressed": sum(i["compressed_size"] for i in index),
        "blobs": index,
    }
    (args.out / "index.json").write_text(json.dumps(meta, indent=1) + "\n")
    rows = ["id\tfile_offset\trva\tsection\tcompressed\tuncompressed\tsha256\tfile"]
    for i in index:
        rows.append("\t".join(str(i[k]) for k in
                    ("id", "file_offset_hex", "rva_hex", "section",
                     "compressed_size", "uncompressed_size", "sha256", "file")))
    (args.out / "index.tsv").write_text("\n".join(rows) + "\n")
    print(f"{len(index)} blobs, {meta['total_uncompressed']} bytes uncompressed")


if __name__ == "__main__":
    main()
