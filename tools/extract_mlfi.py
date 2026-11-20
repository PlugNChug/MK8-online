#!/usr/bin/env python3
"""Extract the ML frame-interpolation (FSR 4) shader and model data from AMD's signed frame-generation DLL.

AMD's `amd_fidelityfx_framegeneration_vk.dll` is one provider that carries both the FSR 3.1.7 optical-flow
frame generation and the FSR 4 ML frame interpolation (`mlfi_4k_*` / `mlfi_8k_*`). The SDK source has no
MLFI implementation, so the shipped DLL is the only copy of the ML pass set and of the model that those
passes read from `InitializerBuffer`.

This tool extracts, from the user's own copy of the DLL:

* the unpacked SPIR-V modules (no FFXZ/FXZF compression in this provider), classified by entry point;
* the MLFI pass-name pool (191 names per resolution) and the provider version string;
* the embedded model region (fp16/fp32 tensor data) split at its low-density boundaries.

It is read-only with respect to the DLL and fails closed:

* the pinned build (FSR SDK 2.4.0) is identified by SHA-256, and the module counts, pass pool and model
  hash are checked against the pin; any difference aborts;
* an unknown build is rejected unless `--allow-unknown-layout` is given, and is then marked `unpinned`
  in the manifest instead of silently producing files that claim to be the pinned build;
* output is refused inside the repository (AMD content must never be committed).

Nothing from AMD is shipped with this tool: the user downloads the official SDK archive and points the
tool at the DLL.

Usage::

    python3 tools/extract_mlfi.py --dll /path/to/amd_fidelityfx_framegeneration_vk.dll
    python3 tools/extract_mlfi.py --dll DIR/amd_fidelityfx_framegeneration_vk.dll --dry-run
    python3 tools/extract_mlfi.py --dll ... --allow-unknown-layout --out ~/.cache/mk8online/mlfi

Exit codes: 0 success, 3 unpinned build, 4 unsafe output path, 5 unexpected layout or failed validation.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

TOOL_VERSION = 1
SCHEMA = "mk8online.mlfi-extract/1"

# The FSR SDK 2.4.0 signed framegeneration DLL (tag v2.4.0, published 2026-10-28). Anything else is
# rejected by default: the extraction offsets are only meaningful for the build they were derived from.
PINNED = {
    "size": 36_806_928,
    "sha256": "d002dabff8c5e71eaa715fc99438cd4dc81230262a7897b16bc402efec3498b4",
    "provider_version": "4.0.2",
    "modules": {"total": 416, "mlfi_4k": 191, "mlfi_8k": 191, "cs": 23, "other": 11},
    "pass_pool": {"mlfi_4k": 191, "mlfi_8k": 191, "other": 14},
    "model": {
        "offset": 0xDF7468,
        "size": 10_720_312,
        "sha256": "43a77c641f8ba3cd6ee08939661578097bbf0d9f43007939ccb64c161349da90",
    },
}

RESOURCE_NAMES = (
    "CsFrameInterpolationConstants",
    "CsTensorSizes",
    "r_input_rgb0",
    "r_input_rgb1",
    "ScratchBuffer",
    "g_sampler_linear",
    "InitializerBuffer",
    "rw_output_flow_0to05",
    "rw_output_flow_1to05",
    "r_input_packed_splatted0to05",
    "r_input_packed_splatted1to05",
    "r_hudless_ui",
    "rw_prediction",
)

SPIRV_MAGIC = b"\x03\x02#\x07"
SPIRV_MAGIC_WORD = 0x07230203
OP_NAME = 5
OP_ENTRY_POINT = 15
OP_TYPE_ARRAY = 28
OP_CONSTANT = 43
OP_SPEC_CONSTANT = 50
OP_DECORATE = 71

PASS_NAME_RE = re.compile(r"mlfi_(4k|8k)_pass(\d+)$")
MLFI_ENTRY_RE = re.compile(r"mlfi_(4k|8k)_pass\d+$")
PRINTABLE_RE = re.compile(rb"[\x20-\x7e]+$")

MIN_HEURISTIC_BYTES = 1 << 20
WINDOW = 1 << 16          # fp16 heuristic window
SEGMENT_WINDOW = 1 << 12  # low-density boundary detection inside the model bank


class LayoutError(RuntimeError):
    """The DLL does not look like a build this tool understands."""


class UnpinnedError(RuntimeError):
    """The DLL is not the pinned build and --allow-unknown-layout was not given."""


class OutputGuardError(RuntimeError):
    """Refusing to write extracted AMD content there."""


@dataclass(frozen=True)
class Section:
    name: str
    rva: int
    virtual_size: int
    raw: int
    raw_size: int

    @property
    def raw_end(self) -> int:
        return self.raw + self.raw_size


@dataclass(frozen=True)
class Module:
    offset: int
    size: int
    sha256: str
    entry: str
    kind: str
    functions: int


@dataclass(frozen=True)
class PassPool:
    offset: int
    end: int
    names: tuple[str, ...]
    version: str

    def count(self, prefix: str) -> int:
        return sum(1 for name in self.names if name.startswith(prefix))


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while True:
            chunk = handle.read(4 << 20)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def parse_pe(data: bytes) -> tuple[str, list[Section]]:
    """Read the section table of a PE32+ image. Only what the extractor needs, validated."""
    if len(data) < 0x40 or data[:2] != b"MZ":
        raise LayoutError("not a PE image (missing MZ header)")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if pe + 24 > len(data) or data[pe:pe + 4] != b"PE\0\0":
        raise LayoutError("not a PE image (missing PE signature)")
    machine, nsections = struct.unpack_from("<HH", data, pe + 4)
    if machine != 0x8664:
        raise LayoutError(f"unexpected PE machine {machine:#x}, expected x86-64")
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    if optional_size < 0x70:
        raise LayoutError("optional header too small for PE32+")
    magic = struct.unpack_from("<H", data, pe + 24)[0]
    if magic != 0x20B:
        raise LayoutError(f"unexpected optional header magic {magic:#x}, expected PE32+")
    table = pe + 24 + optional_size
    if table + nsections * 40 > len(data):
        raise LayoutError("section table runs past end of file")
    sections: list[Section] = []
    for index in range(nsections):
        base = table + index * 40
        raw_name = data[base:base + 8].split(b"\0", 1)[0]
        virtual_size, rva, raw_size, raw = struct.unpack_from("<IIII", data, base + 8)
        if raw_size and raw + raw_size > len(data):
            raise LayoutError(f"section {raw_name!r} points past end of file")
        sections.append(Section(raw_name.decode("ascii", "replace"), rva, virtual_size, raw, raw_size))
    if not any(section.name == ".rdata" for section in sections):
        raise LayoutError("PE image has no .rdata section")
    return "pei-x86-64", sections


def spirv_entry_point(module: bytes) -> str:
    words = len(module) // 4
    at = 5
    while at + 1 <= words:
        word = struct.unpack_from("<I", module, at * 4)[0]
        count, opcode = word >> 16, word & 0xFFFF
        if count == 0:
            break
        if opcode == OP_ENTRY_POINT:
            start = (at + 3) * 4
            end = module.find(b"\0", start)
            if end < 0:
                raise LayoutError("unterminated OpEntryPoint name")
            name = module[start:end]
            if not PRINTABLE_RE.match(name):
                raise LayoutError("non-printable OpEntryPoint name")
            return name.decode("ascii")
        at += count
    raise LayoutError("SPIR-V module has no OpEntryPoint")


def scan_spirv(data: bytes) -> list[Module]:
    """Find every SPIR-V module by magic and validate the instruction stream structurally."""
    found: list[Module] = []
    seen: set[str] = set()
    at = 0
    while True:
        start = data.find(SPIRV_MAGIC, at)
        if start < 0:
            break
        at = start + 4
        if start + 20 > len(data):
            continue
        version, _generator, bound, schema = struct.unpack_from("<4I", data, start + 4)
        if version >> 16 not in (1,) or bound == 0 or bound > (1 << 24) or schema != 0:
            continue
        cursor = start + 20
        entries = functions = ends = 0
        depth = 0
        candidate_end = 0
        ok = True
        while cursor + 4 <= len(data):
            word = struct.unpack_from("<I", data, cursor)[0]
            if word == 0:
                break
            count, opcode = word >> 16, word & 0xFFFF
            if count == 0 or cursor + count * 4 > len(data):
                ok = False
                break
            if opcode == OP_ENTRY_POINT:
                entries += 1
            elif opcode == 54:  # OpFunction
                functions += 1
                depth += 1
            elif opcode == 56:  # OpFunctionEnd
                ends += 1
                depth -= 1
                if depth < 0:
                    ok = False
                    break
                if depth == 0:
                    candidate_end = cursor + count * 4
                    # A module ends with its last function. The next word is either a new SPIR-V
                    # module (back-to-back kernels in the packed section) or a descriptor table;
                    # continuing to walk there would swallow the neighbouring module, which is how
                    # two concatenated kernels used to be extracted as one oversized blob.
                    nxt = candidate_end
                    if nxt + 4 > len(data):
                        break
                    following = struct.unpack_from("<I", data, nxt)[0]
                    fcount, fopcode = following >> 16, following & 0xFFFF
                    # Every global declaration and decoration precedes the functions, so after a
                    # function only another OpFunction may follow inside the same module.
                    if fcount == 0 or fopcode != 54 or nxt + fcount * 4 > len(data):
                        cursor = candidate_end
                        break
            cursor += count * 4
        if not ok or entries == 0 or functions == 0 or ends != functions:
            continue
        if depth != 0 or candidate_end == 0:
            continue
        size = candidate_end - start
        blob = data[start:start + size]
        digest = sha256_bytes(blob)
        if digest in seen:
            continue
        seen.add(digest)
        entry = spirv_entry_point(blob)
        if MLFI_ENTRY_RE.match(entry):
            kind = "mlfi_4k" if entry.startswith("mlfi_4k") else "mlfi_8k"
        elif entry in ("main", "mainPS", "mainVS"):
            kind = "ui"
        elif entry == "CS":
            kind = "fsr3fg"
        else:
            kind = "other"
        found.append(Module(start, size, digest, entry, kind, functions))
    found.sort(key=lambda module: module.offset)
    if not found:
        raise LayoutError("no SPIR-V modules found")
    return found


def read_cstring(data: bytes, offset: int, limit: int = 96) -> str:
    if offset < 0 or offset >= len(data):
        raise LayoutError("string offset out of range")
    end = data.find(b"\0", offset, min(len(data), offset + limit))
    if end < 0:
        raise LayoutError("unterminated string")
    raw = data[offset:end]
    if not raw or not PRINTABLE_RE.match(raw):
        raise LayoutError(f"non-printable string at {offset:#x}")
    return raw.decode("ascii")


def find_pass_pool(data: bytes, sections: list[Section]) -> PassPool:
    """Locate the contiguous MLFI name pool (191 4k names, 191 8k names, resources, version)."""
    rdata = next((section for section in sections if section.name == ".rdata"), None)
    if rdata is None:
        raise LayoutError("no .rdata section")
    # `mlfi_4k_pass0` also appears in SPIR-V debug strings; only the name pool is followed by the
    # resource names and the 8k sequence, so every candidate is validated and the first real pool wins.
    anchor = data.find(b"mlfi_4k_pass0\0", rdata.raw, rdata.raw_end)
    failures: list[str] = []
    while anchor >= 0:
        cursor = anchor
        entries: list[tuple[str, int]] = []
        while cursor < rdata.raw_end:
            end = data.find(b"\0", cursor, min(rdata.raw_end, cursor + 96))
            if end < 0:
                break
            raw = data[cursor:end]
            if not raw or not PRINTABLE_RE.match(raw):
                break
            name = raw.decode("ascii")
            if not (PASS_NAME_RE.match(name) or name in RESOURCE_NAMES
                    or re.fullmatch(r"\d+\.\d+\.\d+", name)):
                break
            entries.append((name, end + 1))
            cursor = end + 1
            while cursor < rdata.raw_end and data[cursor] == 0 and cursor - end < 16:
                cursor += 1  # names are 16-byte aligned in the pool
            if len(entries) > 512:
                break
        if entries:
            try:
                return _validate_pass_pool(anchor, entries)
            except LayoutError as error:
                failures.append(str(error))
        anchor = data.find(b"mlfi_4k_pass0\0", anchor + 1, rdata.raw_end)
    raise LayoutError(f"MLFI pass-name pool not found ({failures[0] if failures else 'no candidates'})")


def _validate_pass_pool(anchor: int, entries: list[tuple[str, int]]) -> PassPool:
    versions = [index for index, (name, _end) in enumerate(entries)
                if re.fullmatch(r"\d+\.\d+\.\d+", name)]
    if len(versions) != 1:
        raise LayoutError(f"pool has no single version string: {len(versions)}")
    entries = entries[:versions[0] + 1]
    names = tuple(name for name, _end in entries)
    unknown = [name for name in names if not PASS_NAME_RE.match(name)
               and name not in RESOURCE_NAMES and not re.fullmatch(r"\d+\.\d+\.\d+", name)]
    if unknown:
        raise LayoutError(f"pool contains unexpected names: {unknown[:4]}")
    for resolution in ("4k", "8k"):
        sequence = [name for name in names if name.startswith(f"mlfi_{resolution}_pass")]
        expected = [f"mlfi_{resolution}_pass{index}" for index in range(len(sequence))]
        if sequence != expected:
            raise LayoutError(f"{resolution} pass names are not a contiguous 0..{len(sequence) - 1} range")
    resources = [name for name in names if not PASS_NAME_RE.match(name)]
    missing = [name for name in RESOURCE_NAMES if name not in resources]
    if missing:
        raise LayoutError(f"pool is missing resource names: {missing}")
    return PassPool(anchor, entries[-1][1], names, resources[-1])


def _fp16_exponent(word: int) -> int:
    return (word >> 10) & 0x1F


def fp16_histogram(data: bytes, offset: int, size: int) -> dict[str, object]:
    counts = [0] * 32
    zeros = 0
    end = offset + (size & ~1)
    for at in range(offset, end, 2):
        word = struct.unpack_from("<H", data, at)[0]
        if word == 0:
            zeros += 1
            continue
        counts[_fp16_exponent(word)] += 1
    total = sum(counts) + zeros
    return {
        "words": total,
        "zero_words": zeros,
        "exponents": {str(exponent - 15): counts[exponent] for exponent in range(32) if counts[exponent]},
    }


def fp16_signature(data: bytes, offset: int, size: int) -> float:
    """Fraction of 16-bit words that look like normal fp16 values in a plausible weight range."""
    words = size // 2
    if words == 0:
        return 0.0
    good = 0
    for at in range(offset, offset + words * 2, 2):
        exponent = _fp16_exponent(struct.unpack_from("<H", data, at)[0])
        if 1 <= exponent <= 20:
            good += 1
    return good / words


def find_model_region_pinned(data: bytes) -> tuple[int, int]:
    offset = PINNED["model"]["offset"]
    size = PINNED["model"]["size"]
    if offset + size > len(data):
        raise LayoutError("pinned model region runs past end of file")
    digest = sha256_bytes(data[offset:offset + size])
    if digest != PINNED["model"]["sha256"]:
        raise LayoutError(
            "pinned model region hash mismatch: expected "
            f"{PINNED['model']['sha256']}, got {digest}"
        )
    return offset, offset + size


def find_model_region_heuristic(data: bytes, modules: list[Module]) -> tuple[int, int]:
    """For an unknown build: the largest fp16-like run between two SPIR-V modules.

    The model bank is dense tensor data, so most 16-bit words are normal fp16 values with a small
    exponent; code, tables and SPIR-V do not have that property. The run is expanded to the enclosing
    module boundaries so the bank's low-density header/tailer tables stay with it.
    """
    covered = bytearray(len(data))
    for module in modules:
        covered[module.offset:module.offset + module.size] = b"\1" * module.size
    best: tuple[int, int] | None = None
    best_len = 0
    run_start = run_end = None
    for start in range(0, len(data) - WINDOW, WINDOW):
        free = not any(covered[start:start + WINDOW])
        dense = free and fp16_signature(data, start, WINDOW) >= 0.995
        if dense:
            if run_start is None:
                run_start = start
            run_end = start + WINDOW
        elif run_start is not None:
            if run_end - run_start > best_len:
                best, best_len = (run_start, run_end), run_end - run_start
            run_start = run_end = None
    if run_start is not None and run_end - run_start > best_len:
        best, best_len = (run_start, run_end), run_end - run_start
    if best is None or best_len < MIN_HEURISTIC_BYTES:
        raise LayoutError("no dense fp16 region large enough to be the MLFI model bank")
    start, end = best
    before = [module for module in modules if module.offset + module.size <= start]
    after = [module for module in modules if module.offset >= end]
    if before:
        start = max(start, max(module.offset + module.size for module in before))
    if after:
        end = min(end, min(module.offset for module in after))
    return start, end


def model_segments(data: bytes, start: int, end: int) -> list[tuple[int, int]]:
    """Split a bank at its low-density (descriptor/scale table) boundaries, deterministically."""
    boundaries = {start, end}
    run: tuple[int, int] | None = None
    for offset in range(start, end - SEGMENT_WINDOW + 1, SEGMENT_WINDOW):
        zeros = data[offset:offset + SEGMENT_WINDOW].count(0) / SEGMENT_WINDOW
        if zeros > 0.4:
            if run and run[1] == offset:
                run = (run[0], offset + SEGMENT_WINDOW)
            else:
                if run:
                    boundaries.update(run)
                run = (offset, offset + SEGMENT_WINDOW)
        elif run:
            boundaries.update(run)
            run = None
    if run:
        boundaries.update(run)
    ordered = sorted(boundaries)
    return [(a, b) for a, b in zip(ordered, ordered[1:]) if b > a]


def module_bindings(module: bytes) -> dict[str, object]:
    """Descriptor set/binding of each named resource, plus the CsTensorSizes table length.

    The MLFI passes all bind set 0: weights as a ByteAddressBuffer (0), a RW scratch buffer (128),
    two CBVs (256/257) and a sampler (320). Recording it per module makes a future provider's
    descriptor layout auditable from the manifest instead of from the shader text.
    """
    words = len(module) // 4
    names: dict[int, str] = {}
    decorations: dict[int, tuple[int | None, int | None]] = {}
    constants: dict[int, int] = {}
    types: dict[int, tuple] = {}
    variables: dict[int, int] = {}
    at = 5
    while at + 1 <= words:
        word = struct.unpack_from("<I", module, at * 4)[0]
        count, opcode = word >> 16, word & 0xFFFF
        if count == 0:
            break
        if opcode == OP_NAME and count >= 3:
            target = struct.unpack_from("<I", module, (at + 1) * 4)[0]
            begin = (at + 2) * 4
            finish = module.find(b"\0", begin)
            if finish > begin:
                names[target] = module[begin:finish].decode("utf-8", "replace")
        elif opcode == OP_DECORATE and count >= 4:
            target, kind, value = struct.unpack_from("<III", module, (at + 1) * 4)
            set_id, binding = decorations.get(target, (None, None))
            if kind == 34:      # Decoration::DescriptorSet
                set_id = value
            elif kind == 33:    # Decoration::Binding
                binding = value
            decorations[target] = (set_id, binding)
        elif opcode in (OP_CONSTANT, OP_SPEC_CONSTANT) and count >= 4:
            target = struct.unpack_from("<I", module, (at + 2) * 4)[0]
            constants[target] = struct.unpack_from("<I", module, (at + 3) * 4)[0]
        elif opcode == OP_TYPE_ARRAY and count == 4:
            target, element, length = struct.unpack_from("<III", module, (at + 1) * 4)
            types[target] = ("array", element, constants.get(length, 0))
        elif opcode == 30 and count >= 3:      # OpTypeStruct
            target = struct.unpack_from("<I", module, (at + 1) * 4)[0]
            types[target] = ("struct", [struct.unpack_from("<I", module, (at + 2 + index) * 4)[0]
                                         for index in range(count - 2)])
        elif opcode == 32 and count == 4:      # OpTypePointer
            target, _storage, pointee = struct.unpack_from("<III", module, (at + 1) * 4)
            types[target] = ("pointer", pointee)
        elif opcode == 59 and count >= 4:      # OpVariable
            result_type, result_id = struct.unpack_from("<II", module, (at + 1) * 4)
            variables[result_id] = result_type
        at += count
    resources: dict[str, list[int]] = {}
    for target, (set_id, binding) in decorations.items():
        if set_id is None or binding is None:
            continue
        name = names.get(target)
        if name:
            resources[name] = [set_id, binding]
    # CsTensorSizes is a uniform block whose single member is uint4[n]; n is what the host fills
    # for every dispatch, so it is the clearest single number to carry in the manifest.
    tensor_sizes = 0
    for target, name in names.items():
        if name != "CsTensorSizes" or target not in variables:
            continue
        pointee = types.get(variables[target], ())
        if pointee[:1] != ("pointer",):
            continue
        block = types.get(pointee[1], ())
        if block[:1] != ("struct",) or not block[1]:
            continue
        member = types.get(block[1][0], ())
        if member[:1] == ("array",):
            tensor_sizes = member[2]
    return {"resources": dict(sorted(resources.items())), "tensor_sizes_entries": tensor_sizes}


def module_kind_counts(modules: list[Module]) -> dict[str, int]:
    counts = {"total": len(modules), "mlfi_4k": 0, "mlfi_8k": 0, "cs": 0, "other": 0}
    for module in modules:
        if module.kind == "mlfi_4k":
            counts["mlfi_4k"] += 1
        elif module.kind == "mlfi_8k":
            counts["mlfi_8k"] += 1
        elif module.kind == "fsr3fg":
            counts["cs"] += 1
        else:
            counts["other"] += 1
    return counts


def safe_name(entry: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]", "_", entry)


def check_output_path(out: Path) -> Path:
    resolved = out.expanduser().resolve()
    repo = Path(__file__).resolve().parents[1]
    if resolved == repo or repo in resolved.parents:
        raise OutputGuardError(
            f"refusing to write AMD-extracted data inside the repository ({resolved}); "
            "use $HOME/.cache/mk8online/mlfi or another path outside the checkout"
        )
    return resolved


def build_manifest(dll: Path, data: bytes, sections: list[Section], modules: list[Module],
                   pool: PassPool, region: tuple[int, int], pinned: bool) -> dict[str, object]:
    records = []
    for module in modules:
        record = {
            "entry": module.entry,
            "kind": module.kind,
            "offset": module.offset,
            "size": module.size,
            "sha256": module.sha256,
            "functions": module.functions,
        }
        if module.kind in ("mlfi_4k", "mlfi_8k"):
            record.update(module_bindings(data[module.offset:module.offset + module.size]))
        records.append(record)
    start, end = region
    segments = [
        {"index": index, "offset": a, "size": b - a, "sha256": sha256_bytes(data[a:b])}
        for index, (a, b) in enumerate(model_segments(data, start, end))
    ]
    return {
        "schema": SCHEMA,
        "tool_version": TOOL_VERSION,
        "pinned": pinned,
        "source": {
            "file": dll.name,
            "size": len(data),
            "sha256": sha256_bytes(data),
            "format": "PE32+ x86-64",
        },
        "provider_version": pool.version,
        "sections": [
            {"name": section.name, "rva": hex(section.rva), "raw": hex(section.raw),
             "virtual_size": section.virtual_size, "raw_size": section.raw_size}
            for section in sections
        ],
        "modules": records,
        "module_counts": module_kind_counts(modules),
        "pass_pool": {
            "offset": pool.offset,
            "size": pool.end - pool.offset,
            "counts": {
                "mlfi_4k": pool.count("mlfi_4k_pass"),
                "mlfi_8k": pool.count("mlfi_8k_pass"),
                "other": len(pool.names) - pool.count("mlfi_4k_pass") - pool.count("mlfi_8k_pass"),
            },
            "names": list(pool.names),
        },
        "model": {
            "offset": start,
            "size": end - start,
            "sha256": sha256_bytes(data[start:end]),
            "fp16": fp16_histogram(data, start, end - start),
            "segments": segments,
        },
        "notes": [
            "Extracted from the user's own copy of AMD's signed DLL; not for redistribution.",
            "FSR 3.1.7 optical-flow passes are the 'fsr3fg' modules; 'mlfi_*' are the FSR 4 ML passes.",
            "Model tensors are consumed through InitializerBuffer; the host computes CsTensorSizes.",
        ],
    }


def write_outputs(out: Path, data: bytes, manifest: dict[str, object], modules: list[Module],
                  region: tuple[int, int], force: bool) -> str:
    if (out / "manifest.json").exists() and not force:
        raise OutputGuardError(
            f"{out} already holds an extraction; pass --force to replace it (stale modules from an "
            "older DLL would otherwise mix with the new set)"
        )
    out.mkdir(parents=True, exist_ok=True)
    modules_dir = out / "modules"
    model_dir = out / "model"
    modules_dir.mkdir(exist_ok=True)
    model_dir.mkdir(exist_ok=True)
    lines = []
    used: set[str] = set()
    for index, module in enumerate(modules):
        stem = f"{index:04d}_{safe_name(module.entry)}"
        while stem in used:
            stem = f"{index:04d}_{safe_name(module.entry)}_{len(used)}"
        used.add(stem)
        name = f"modules/{stem}.spv"
        (out / name).write_bytes(data[module.offset:module.offset + module.size])
        lines.append(f"{name}\t{module.sha256}\t{module.size}\t{module.entry}\t{module.kind}\n")
    start, end = region
    (model_dir / "model_blob.bin").write_bytes(data[start:end])
    lines.append(f"model/model_blob.bin\t{manifest['model']['sha256']}\t{end - start}\t-\tmodel\n")
    segments = model_segments(data, start, end)
    for index, (a, b) in enumerate(segments):
        name = f"model/segment_{index:02d}_{a:#09x}.bin"
        (out / name).write_bytes(data[a:b])
        lines.append(f"{name}\t{sha256_bytes(data[a:b])}\t{b - a}\t-\tmodel-segment\n")
    (out / "index.tsv").write_text("".join(lines), encoding="utf-8")
    root = hashlib.sha256()
    for line in lines:
        root.update(line.encode("utf-8"))
    manifest["content_root"] = root.hexdigest()
    manifest["files"] = len(lines)
    (out / "manifest.json").write_text(json.dumps(manifest, indent=1) + "\n", encoding="utf-8")
    notice = out / "NOTICE.txt"
    notice.write_text(
        "Extracted from AMD's signed FidelityFX frame-generation DLL by tools/extract_mlfi.py.\n"
        "The DLL is published in the AMD FSR SDK 2.4.0 archive and is MIT-listed in\n"
        "Kits/FidelityFX/docs/license.md of that archive. Do not redistribute these files.\n"
        "Official archive: https://gpuopen.com/download-fsr-sdk/ (tag v2.4.0)\n",
        encoding="utf-8",
    )
    return root.hexdigest()


def extract(dll: Path, out: Path, allow_unknown: bool, dry_run: bool,
            validate_spirv: bool, force: bool) -> dict[str, object]:
    data = dll.read_bytes()
    digest = sha256_bytes(data)
    pinned = digest == PINNED["sha256"] and len(data) == PINNED["size"]
    if not pinned and not allow_unknown:
        raise UnpinnedError(
            f"{dll.name} is not the pinned FSR SDK 2.4.0 build (sha256 {digest}, {len(data)} bytes); "
            "pass --allow-unknown-layout to extract an unverified build"
        )
    _format, sections = parse_pe(data)
    modules = scan_spirv(data)
    pool = find_pass_pool(data, sections)
    counts = module_kind_counts(modules)
    if pinned:
        if counts != PINNED["modules"]:
            raise LayoutError(f"module counts {counts} do not match the pinned build {PINNED['modules']}")
        if pool.count("mlfi_4k_pass") != PINNED["pass_pool"]["mlfi_4k"] or \
                pool.count("mlfi_8k_pass") != PINNED["pass_pool"]["mlfi_8k"]:
            raise LayoutError("pass pool counts do not match the pinned build")
        if pool.version != PINNED["provider_version"]:
            raise LayoutError(f"provider version {pool.version!r} does not match the pinned build")
        region = find_model_region_pinned(data)
    else:
        region = find_model_region_heuristic(data, modules)
    manifest = build_manifest(dll, data, sections, modules, pool, region, pinned)
    if validate_spirv:
        import shutil
        import subprocess
        tool = shutil.which("spirv-val")
        if tool is None:
            raise LayoutError("--validate-spirv requested but spirv-val is not installed")
        # AMD's compiler emits OpFConvert on cooperative-matrix types in a way spirv-val rejects
        # ("Expected Use of Matrix type and Result Type to be identical"). That rule is recorded but
        # not fatal; anything else means the extracted bytes are wrong and aborts.
        matrix_rule = "Expected Use of Matrix type and Result Type to be identical"
        ok = 0
        known, structural = [], []
        for module, record in zip(modules, manifest["modules"]):
            result = subprocess.run([tool, "-"], input=data[module.offset:module.offset + module.size],
                                    capture_output=True)
            if result.returncode == 0:
                ok += 1
                continue
            message = result.stderr.decode("utf-8", "replace").strip()
            if matrix_rule in message:
                known.append(record["entry"])
            else:
                structural.append((record["entry"], message))
        manifest["spirv_val"] = {"checked": len(modules), "ok": ok,
                                 "known_matrix_rule": known, "other": structural}
        if structural:
            raise LayoutError(
                f"spirv-val rejected {len(structural)} module(s) for reasons other than the known "
                f"cooperative-matrix rule: {structural[:3]}"
            )
        if known and not dry_run:
            print(f"warning: spirv-val rejects {len(known)} module(s) on the known cooperative-matrix "
                  "conversion rule; recorded in the manifest", file=sys.stderr)
    if dry_run:
        return manifest
    out = check_output_path(out)
    write_outputs(out, data, manifest, modules, region, force)
    return manifest


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--dll", required=True, type=Path, help="amd_fidelityfx_framegeneration_vk.dll")
    parser.add_argument("--out", type=Path,
                        default=Path.home() / ".cache" / "mk8online" / "mlfi",
                        help="output directory (must stay outside the repository)")
    parser.add_argument("--allow-unknown-layout", action="store_true",
                        help="extract a DLL that is not the pinned SDK 2.4.0 build (marked unpinned)")
    parser.add_argument("--dry-run", action="store_true", help="analyse and print the manifest only")
    parser.add_argument("--validate-spirv", action="store_true", help="run spirv-val on every module")
    parser.add_argument("--force", action="store_true", help="replace an existing extraction")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args(argv)
    try:
        manifest = extract(args.dll, args.out, args.allow_unknown_layout, args.dry_run,
                           args.validate_spirv, args.force)
    except UnpinnedError as error:
        print(f"error: {error}", file=sys.stderr)
        return 3
    except OutputGuardError as error:
        print(f"error: {error}", file=sys.stderr)
        return 4
    except LayoutError as error:
        print(f"error: {error}", file=sys.stderr)
        return 5
    if args.dry_run:
        print(json.dumps(manifest, indent=1))
        return 0
    if not args.quiet:
        counts = manifest["module_counts"]
        model = manifest["model"]
        print(f"{'pinned' if manifest['pinned'] else 'UNPINNED'} provider {manifest['provider_version']}")
        print(f"modules: {counts['total']} (4k {counts['mlfi_4k']}, 8k {counts['mlfi_8k']}, "
              f"fsr3 {counts['cs']}, other {counts['other']})")
        print(f"passes:  {manifest['pass_pool']['counts']['mlfi_4k']} 4k + "
              f"{manifest['pass_pool']['counts']['mlfi_8k']} 8k names")
        print(f"model:   {model['size']} bytes at {model['offset']:#x}, {len(model['segments'])} segments")
        print(f"output:  {args.out} (content_root {manifest['content_root']})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
