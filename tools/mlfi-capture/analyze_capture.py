#!/usr/bin/env python3
"""Turn an mlfi_capture run into derived MLFI layout metadata.

The capture directory (written under $HOME by run.sh) contains the raw log, event table,
tensor-table dumps and buffer dumps. This tool reads it, checks the capture against the
extracted model bank, and writes a compact JSON/TSV description. It never writes shader or
model bytes: only pass names, dispatch geometry, dimensions and offsets, which are the
metadata docs/en/reports/MLFI-CAPTURE.md is allowed to carry.

Usage:
  python3 tools/mlfi-capture/analyze_capture.py --capture DIR
  python3 tools/mlfi-capture/analyze_capture.py --capture DIR --derived tools/mlfi-capture/derived
  python3 tools/mlfi-capture/analyze_capture.py --capture DIR --extracted ~/.cache/mk8online/mlfi
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import sys
from pathlib import Path

SPV_MAGIC = 0x07230203
OP_TYPE_INT = 21
OP_CONSTANT = 43


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(4 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_manifest(extracted: Path) -> dict:
    manifest_path = extracted / "manifest.json"
    if not manifest_path.exists():
        return {}
    return json.loads(manifest_path.read_text())


def parse_log(capture: Path) -> dict:
    log = capture / "log.txt"
    info: dict = {"providers": [], "pipelines": [], "substituted": [], "frames": []}
    if not log.exists():
        return info
    header = re.compile(r"# mlfi_capture: size=(\d+)x(\d+) frames=(\d+) substitute=(\d+)")
    provider = re.compile(r"\[ffx\] version\[\d+\] id=([0-9a-f]+) name=(.+)")
    pipeline = re.compile(r"\[pipeline\] entry=(\S+) module_size=(\d+) module_hash=([0-9a-f]+) result=(-?\d+) pipe=(\S+)")
    substitute = re.compile(r"\[pipeline\] SUBSTITUTE entry=(\S+) module_size=(\d+) module_hash=([0-9a-f]+)")
    frame = re.compile(r"\[frame (\d+)\] (configure|prepare|framegen|submit)=(-?\d+)")
    context = re.compile(r"\[ffx\] ffxCreateContext=(-?\d+)")
    for line in log.read_text(errors="replace").splitlines():
        match = header.search(line)
        if match:
            info["width"] = int(match.group(1))
            info["height"] = int(match.group(2))
            info["frames_requested"] = int(match.group(3))
            info["substitute"] = bool(int(match.group(4)))
        match = provider.search(line)
        if match:
            info["providers"].append({"id": match.group(1), "name": match.group(2).strip()})
        match = pipeline.search(line)
        if match:
            info["pipelines"].append({
                "entry": match.group(1), "module_size": int(match.group(2)),
                "module_hash": match.group(3), "pipeline": match.group(5),
            })
        match = substitute.search(line)
        if match:
            info["substituted"].append({
                "entry": match.group(1), "module_size": int(match.group(2)),
                "module_hash": match.group(3),
            })
        match = frame.search(line)
        if match:
            info["frames"].append({"frame": int(match.group(1)), "call": match.group(2), "rc": int(match.group(3))})
        match = context.search(line)
        if match:
            info["context_rc"] = int(match.group(1))
    return info


def parse_passes(capture: Path) -> list[dict]:
    path = capture / "passes.tsv"
    if not path.exists():
        return []
    rows = []
    lines = path.read_text().splitlines()
    if not lines:
        return rows
    header = lines[0].split("\t")
    for line in lines[1:]:
        values = line.split("\t")
        if len(values) != len(header):
            continue
        row = dict(zip(header, values))
        for key in ("index", "substituted", "dispatch_x", "dispatch_y", "dispatch_z", "tensor_table"):
            row[key] = int(row[key])
        for key in ("weight_offset", "weight_range", "tensor_offset", "constants_offset"):
            row[key] = int(row.get(key, 0))
        rows.append(row)
    return rows


def parse_tensors(capture: Path) -> list[dict]:
    path = capture / "tensors.tsv"
    tables = []
    if not path.exists():
        return tables
    lines = path.read_text().splitlines()[1:]
    for line in lines:
        values = line.split("\t")
        if len(values) < 3:
            continue
        table_id = int(values[0])
        blob = capture / f"tensor_{table_id:03d}.bin"
        if not blob.exists():
            continue
        data = blob.read_bytes()
        rows = [list(struct.unpack_from("<4I", data, i * 16)) for i in range(len(data) // 16)]
        tables.append({
            "id": table_id,
            "entry_first_seen": values[1],
            "fnv1a64": values[2],
            "sha256": hashlib.sha256(data).hexdigest(),
            "rows": rows,
        })
    return tables


def find_model_copy(capture: Path) -> dict:
    raw = capture / "raw"
    candidates = sorted(raw.glob("*copy_*.bin"), key=lambda p: p.stat().st_size, reverse=True)
    if not candidates:
        return {}
    path = candidates[0]
    data_hash = sha256_file(path)
    match = re.search(r"copy_(\d+)_(\d+)\.bin$", path.name)
    return {
        "file": path.name,
        "length": path.stat().st_size,
        "sha256": data_hash,
        "dst_offset": int(match.group(1)) if match else None,
        "length_from_name": int(match.group(2)) if match else None,
    }


def compare_model(copy: dict, extracted: Path) -> dict:
    model_path = extracted / "model" / "model_blob.bin"
    if not copy or not model_path.exists():
        return {}
    model = model_path.read_bytes()
    data = (Path(copy.get("_dir", ".")) / "raw" / copy["file"]).read_bytes() if "_dir" in copy else b""
    result = {
        "bank_size": len(model),
        "bank_sha256": hashlib.sha256(model).hexdigest(),
        "upload_length": copy["length"],
        "upload_sha256": copy["sha256"],
    }
    if not data:
        return result
    # The upload starts at the first byte of the model bank's tensor payload. Find the 4 KiB
    # prefix, then verify the whole range.
    probe = data[:4096]
    offset = model.find(probe)
    if offset < 0:
        result["match"] = False
        return result
    exact = model[offset:offset + len(data)] == data
    result.update({
        "match": exact,
        "bank_offset": offset,
        "upload_end": offset + len(data),
        "unuploaded_tail": len(model) - (offset + len(data)),
        "first_bytes": data[:16].hex(),
    })
    return result


def spirv_constants(module: bytes, limit: int) -> list[int]:
    if len(module) < 20:
        return []
    words = struct.unpack("<%dI" % (len(module) // 4), module[: len(module) // 4 * 4])
    if words[0] != SPV_MAGIC:
        return []
    types: dict[int, int] = {}
    values: set[int] = set()
    at = 5
    while at + 1 <= len(words):
        count, opcode = words[at] >> 16, words[at] & 0xFFFF
        if count == 0:
            break
        if opcode == OP_TYPE_INT and at + 2 < len(words):
            types[words[at + 1]] = words[at + 2]
        elif opcode == OP_CONSTANT and count >= 4 and types.get(words[at + 1]) == 32:
            value = words[at + 3]
            if 100_000 <= value < limit:
                values.add(value)
        at += count
    return sorted(values)


def shader_offsets(extracted: Path, kind: str, limit: int) -> dict[int, list[int]]:
    index = extracted / "index.tsv"
    if not index.exists():
        return {}
    result: dict[int, list[int]] = {}
    prefix = f"{kind}_pass"
    for line in index.read_text().splitlines():
        parts = line.split("\t")
        if len(parts) < 4 or not parts[3].startswith(prefix):
            continue
        try:
            number = int(parts[3][len(prefix):])
        except ValueError:
            continue
        module = extracted / parts[0]
        if module.exists():
            result[number] = spirv_constants(module.read_bytes(), limit)
    return result



def module_identity(capture: Path, extracted: Path) -> dict:
    manifest = parse_manifest(extracted)
    if not manifest:
        return {}
    known = {record["sha256"]: record["entry"] for record in manifest.get("modules", [])}
    checked = identical = 0
    unmatched = []
    for path in sorted((capture / "raw").glob("*_module_*.bin")):
        digest = sha256_file(path)
        checked += 1
        if digest in known:
            identical += 1
        else:
            unmatched.append({"file": path.name, "sha256": digest})
    return {"checked": checked, "identical": identical, "unmatched": unmatched}

def build_layout(capture: Path, extracted: Path) -> dict:
    log = parse_log(capture)
    passes = parse_passes(capture)
    tensors = parse_tensors(capture)
    model_copy = find_model_copy(capture)
    if model_copy:
        model_copy["_dir"] = str(capture)
    model = compare_model(model_copy, extracted)
    if model_copy:
        model_copy.pop("_dir", None)

    prepare = [p for p in passes if p["stage"] == "prepare"]
    generation = [p for p in passes if p["stage"] == "framegen"]
    kind = "mlfi_8k" if generation and generation[0]["entry"].startswith("mlfi_8k") else "mlfi_4k"
    limit = model.get("upload_length") or 10_657_152
    offsets = shader_offsets(extracted, kind, limit)

    for row in generation:
        entry = row["entry"]
        try:
            number = int(entry.rsplit("pass", 1)[1])
        except (ValueError, IndexError):
            number = -1
        row["model_range_constants"] = offsets.get(number, [])

    return {
        "capture": {
            "dir": str(capture),
            "width": log.get("width"),
            "height": log.get("height"),
            "frames_requested": log.get("frames_requested"),
            "frames_recorded": len({frame["frame"] for frame in log.get("frames", [])}),
            "dll_sha256": (capture / "dll.sha256").read_text().split()[0] if (capture / "dll.sha256").exists() else None,
        },
        "providers": log.get("providers", []),
        "context_rc": log.get("context_rc"),
        "module_identity": module_identity(capture, extracted),
        "pipelines": {
            "created": len(log.get("pipelines", [])) + len(log.get("substituted", [])),
            "created_ok": len(log.get("pipelines", [])),
            "substituted": log.get("substituted", []),
        },
        "prepare": prepare,
        "framegen": generation,
        "tensors": {
            "unique_tables": len(tensors),
            "tables": tensors,
        },
        "model_upload": model,
        "model_range_constants": {
            "kind": kind,
            "passes_with_constants": sum(1 for values in offsets.values() if values),
            "min": min((v for values in offsets.values() for v in values), default=None),
            "max": max((v for values in offsets.values() for v in values), default=None),
            "note": "heuristic: 32-bit OpConstant values in [100000, uploaded model size); these are candidates for model-buffer addressing, not proven weight offsets; the 8k modules also contain constants above the upload size",
        },
    }


def write_derived(layout: dict, out: Path) -> None:
    out.mkdir(parents=True, exist_ok=True)
    width = layout["capture"].get("width")
    height = layout["capture"].get("height")
    kind = layout["model_range_constants"]["kind"]
    stem = f"{kind}_{width}x{height}"

    with (out / f"passes_{stem}.tsv").open("w") as handle:
        handle.write("index\tstage\tentry\tgroups\tsubstituted\tweight_offset\tweight_range\ttensor_offset\tconstants_offset\ttensor_table\tmodel_range_constants\n")
        for row in layout["prepare"] + layout["framegen"]:
            groups = f'{row["dispatch_x"]}x{row["dispatch_y"]}x{row["dispatch_z"]}'
            constants = ",".join(str(v) for v in row.get("shader_constants", []))
            line = (f'{row["index"]}\t{row["stage"]}\t{row["entry"]}\t{groups}\t{row["substituted"]}\t'
                    f'{row["weight_offset"]}\t{row["weight_range"]}\t{row["tensor_offset"]}\t'
                    f'{row["constants_offset"]}\t{row["tensor_table"]}\t{constants}')
            handle.write(line.rstrip() + "\n")

    for table in layout["tensors"]["tables"]:
        tag = "4k" if kind == "mlfi_4k" else "8k"
        with (out / f"tensor_{tag}_{width}x{height}.tsv").open("w") as handle:
            handle.write("index\tx\ty\tz\tw\n")
            for index, row in enumerate(table["rows"]):
                handle.write(f"{index}\t{row[0]}\t{row[1]}\t{row[2]}\t{row[3]}\n")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--capture", required=True, type=Path)
    parser.add_argument("--extracted", type=Path, default=Path.home() / ".cache" / "mk8online" / "mlfi")
    parser.add_argument("--json", type=Path, help="write the layout JSON here (default: <capture>/layout.json)")
    parser.add_argument("--derived", type=Path, help="write pass/tensor TSVs into this directory")
    args = parser.parse_args(argv)

    capture = args.capture.expanduser().resolve()
    if not (capture / "log.txt").exists() or not (capture / "passes.tsv").exists():
        print(f"{capture} does not look like an mlfi_capture run", file=sys.stderr)
        return 1

    layout = build_layout(capture, args.extracted.expanduser())
    json_path = args.json or (capture / "layout.json")
    json_path.write_text(json.dumps(layout, indent=1) + "\n")

    model = layout["model_upload"]
    print(f"capture: {layout['capture']['width']}x{layout['capture']['height']} "
          f"frames={layout['capture']['frames_recorded']} dll={layout['capture']['dll_sha256']}")
    print(f"provider: {', '.join(p['name'] + ' (' + p['id'] + ')' for p in layout['providers'])}")
    identity = layout.get("module_identity") or {}
    if identity.get("checked"):
        print(f"shader identity: {identity['identical']}/{identity['checked']} runtime modules byte-identical to the extracted set")
    print(f"pipelines: {layout['pipelines']['created']} created "
          f"({len(layout['pipelines']['substituted'])} substituted for the known RADV crash)")
    print(f"dispatches: {len(layout['prepare'])} prepare + {len(layout['framegen'])} frame-generation")
    if layout["framegen"]:
        first, last = layout["framegen"][0], layout["framegen"][-1]
        print(f"order: {first['entry']} .. {last['entry']}")
    print(f"tensor tables: {layout['tensors']['unique_tables']} unique")
    if model:
        print(f"model upload: {model['upload_length']} bytes sha256={model['upload_sha256']}")
        if "bank_offset" in model:
            print(f"  bank offset={model['bank_offset']} match={model['match']} "
                  f"unuploaded_tail={model['unuploaded_tail']}")
    constants = layout["model_range_constants"]
    print(f"model-range constants (heuristic): {constants['passes_with_constants']}/191 passes, "
          f"range {constants['min']}..{constants['max']}")

    if args.derived:
        write_derived(layout, args.derived.expanduser())
        print(f"derived TSVs: {args.derived}")
    print(f"layout JSON: {json_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
