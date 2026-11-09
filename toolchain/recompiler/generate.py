"""Strict NSO validation and resumable AArch64-to-C generation."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
import re
from pathlib import Path
import struct
import subprocess
import sys
import time


def sha(path):
    with Path(path).open("rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def validate_nso(path, window):
    import lz4.block
    raw = Path(path).read_bytes()
    if len(raw) < 256 or raw[:4] != b"NSO0":
        raise ValueError(f"Invalid NSO: {path}")
    flags = struct.unpack_from("<I", raw, 12)[0]
    ranges = []
    for i in range(3):
        file_off, mem_off, size = struct.unpack_from("<III", raw, 0x10 + i*16)
        stored = struct.unpack_from("<I", raw, 0x60+i*4)[0] if flags & (1 << i) else size
        if size > window or mem_off + size > window or file_off < 256 or file_off + stored > len(raw):
            raise ValueError(f"NSO segment outside supported bounds: {path}")
        data = raw[file_off:file_off+stored]
        if flags & (1 << i):
            data = lz4.block.decompress(data, uncompressed_size=size)
        if len(data) != size:
            raise ValueError("NSO decompressed size mismatch")
        if flags & (1 << (i+3)) and hashlib.sha256(data).digest() != raw[0xa0+32*i:0xc0+32*i]:
            raise ValueError("NSO segment hash mismatch")
        if size:
            if any(mem_off < end and start < mem_off+size for start,end in ranges):
                raise ValueError("NSO overlapping segments")
            ranges.append((mem_off,mem_off+size))
    bss = struct.unpack_from("<I",raw,0x3c)[0]
    data_off, data_size = struct.unpack_from("<II",raw,0x34)
    if data_off+data_size+bss>window:
        raise ValueError("NSO BSS exceeds reserved module window")
    return raw[0x40:0x60].hex()


def generate(source, exefs, output, profile):
    output.mkdir(parents=True, exist_ok=True)
    tools = source / "tools"
    generator_id = hashlib.sha256(b"".join(p.read_bytes() for p in sorted(tools.glob("*.py")))).hexdigest()

    def generate_module(module):
        name, base = module
        path = exefs / name
        build_id = validate_nso(path, profile["module_window_bytes"])
        identity = {"sha256": sha(path), "build_id": build_id, "base": base, "generator": generator_id, "simd": profile["simd"]}
        target = output / name
        receipt = output / (name + "-receipt.json")
        if receipt.is_file():
            try:
                previous = json.loads(receipt.read_text(encoding='utf-8'))
                files = previous.get("files") if isinstance(previous, dict) else None
                if isinstance(files, dict) and files and previous.get("input") == identity and set(files) == {p.name for p in target.glob("*.c")} and all((target / p).is_file() and sha(target / p) == h for p,h in files.items()):
                    print(f"{name}: cache verificado", flush=True)
                    return name, identity
            except (FileNotFoundError, ValueError):
                pass  # Interrupted metadata cannot prove that generated code is reusable.
        stage = output / f"{name}-pending-{time.time_ns()}"
        subprocess.run([sys.executable, tools / "gen.py", name, path, base, stage, "--simd", profile["simd"]], cwd=tools, check=True)
        if sha(path) != identity["sha256"]:
            raise ValueError(f"{name}: input changed while generating")
        if not list(stage.glob("*.c")):
            raise ValueError(f"{name}: no generated C")
        table = (stage / (name + "_table.c")).read_text()
        match = re.search(r"_fn_count\s*=\s*(\d+)", table)
        if not match or int(match.group(1)) == 0:
            raise ValueError(f"{name}: no discovered functions; unsupported module layout")
        if target.exists():
            target.rename(output / f"{name}-previous-{time.time_ns()}")
        stage.rename(target)
        files = {p.name:sha(p) for p in target.glob("*.c")}
        temp = receipt.with_suffix(".pending")
        temp.write_text(json.dumps({"input": identity,"files":files},indent=2),encoding="utf-8")
        os.replace(temp,receipt)
        return name, identity

    # The modules share read-only tools but each publishes to its own directory.
    modules = profile["modules"]
    with ThreadPoolExecutor(max_workers=max(1, min(3, len(modules), os.cpu_count() or 1))) as pool:
        identities = dict(pool.map(generate_module, modules.items()))
    (output / "MODULES.json").write_text(json.dumps(identities,indent=2),encoding="utf-8")
    return identities


if __name__ == "__main__":
    parser=argparse.ArgumentParser()
    for name in ("source","exefs","output","profile"):
        parser.add_argument("--"+name,required=True,type=Path)
    args=parser.parse_args()
    generate(args.source.resolve(),args.exefs.resolve(),args.output.resolve(),json.loads(args.profile.read_text()))
