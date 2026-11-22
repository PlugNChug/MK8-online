#!/usr/bin/env python3
"""Verify extracted FSR 4.1.2 assets against known sizes and hashes."""
import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("build/assets")

KNOWN_MODEL_HASHES = {
    "FP8_standard.bin": "ce1bd5f19d4fc14f857c5fc810def7f45d5a935a62aa2ca5b5a59829a1d6c868",
    "FP8_ultraperf.bin": "101d91f69e3f6121c2a6eb477ee5cbf6e7bf25f26b65c0a4dcd1ac04e57fe2e8",
    "Int8_standard.bin": "a54e552ff69a3f7199861417f7ae461d1f67f636ab532c6ec9d8ea5d2963735a",
    "Int8_ultraperf.bin": "3c42fc6eabfa9b5b1e489df0e9b2f58e3f9fa22048c584f6839bb27d32a0640e",
    "Int8Navi2_standard.bin": "a54e552ff69a3f7199861417f7ae461d1f67f636ab532c6ec9d8ea5d2963735a",
    "Int8Navi2_ultraperf.bin": "3c42fc6eabfa9b5b1e489df0e9b2f58e3f9fa22048c584f6839bb27d32a0640e",
}
EXPECTED_SHADERS = 1778
EXPECTED_SHADER_BYTES = 102181900
SPV_MAGIC = b"\x03\x02\x23\x07"
FAIL = []


def check(cond, msg):
    print(("PASS  " if cond else "FAIL  ") + msg)
    if not cond:
        FAIL.append(msg)


def main():
    index_path = ROOT / "index.json"
    check(index_path.is_file(), f"index.json exists ({index_path})")
    if not index_path.is_file():
        raise SystemExit(1)
    index = json.loads(index_path.read_text())
    check(index["blob_count"] == EXPECTED_SHADERS,
          f"shader count {index['blob_count']} == {EXPECTED_SHADERS}")
    check(index["total_uncompressed"] == EXPECTED_SHADER_BYTES,
          f"shader bytes {index['total_uncompressed']} == {EXPECTED_SHADER_BYTES}")

    bad_magic = []
    for b in index["blobs"]:
        p = ROOT / b["file"]
        if not p.is_file() or p.read_bytes()[:4] != SPV_MAGIC:
            bad_magic.append(b["file"])
    check(not bad_magic, f"all shader files start with SPIR-V magic ({len(bad_magic)} bad)")

    models = json.loads((ROOT / "models/manifest.json").read_text())
    by_file = {}
    for model in models["models"].values():
        for bank in model["banks"].values():
            by_file[bank["file"]] = bank
    for name, want in KNOWN_MODEL_HASHES.items():
        entry = by_file.get(name)
        check(entry is not None and entry["sha256"] == want,
              f"model hash {name}")
    for name in ("FP8_standard.bin", "FP8_ultraperf.bin",
                 "Int8_standard.bin", "Int8_ultraperf.bin",
                 "Int8Navi2_standard.bin", "Int8Navi2_ultraperf.bin"):
        p = ROOT / "models" / name
        check(p.is_file() and p.stat().st_size == 0x20000,
              f"model size {name} == 0x20000")
    for name in ("LiteInt8_standard.bin", "LiteInt8_ultraperf.bin"):
        p = ROOT / "models" / name
        check(p.is_file() and p.stat().st_size == 0x6B00,
              f"model size {name} == 0x6b00")

    cat_path = ROOT / "catalog_final.json"
    check(cat_path.is_file(), "catalog_final.json exists")
    if cat_path.is_file():
        cat = json.loads(cat_path.read_text())
        check(len(cat) == EXPECTED_SHADERS, f"catalog entries {len(cat)} == {EXPECTED_SHADERS}")
        check(any(x["pass"] == "prepass" for x in cat), "catalog has prepass modules")
        check(any(x["pass"] == "postpass" for x in cat), "catalog has postpass modules")
        check(any(x["pass"] == "pass12" for x in cat), "catalog has pass12 modules")

    sel_path = ROOT / "selector_tables.json"
    check(sel_path.is_file(), "selector_tables.json exists")
    if sel_path.is_file():
        tables = json.loads(sel_path.read_text())
        check(len(tables) == 324, f"selector tables {len(tables)} == 324")
        prepass = [t for t in tables
                   if t["entry_names"] == ["fsr4_model_v07_fp8_no_scale_prepass"]
                   and len(t["distinct_blobs"]) == 30]
        postpass = [t for t in tables
                    if t["entry_names"] == ["fsr4_model_v07_fp8_no_scale_postpass"]
                    and len(t["distinct_blobs"]) == 16]
        check(len(prepass) == 12, f"canonical prepass selectors {len(prepass)} == 12")
        check(len(postpass) == 12, f"canonical postpass selectors {len(postpass)} == 12")
        full = sum(1 for t in tables if len(t["key_to_blob"]) == 64)
        check(full >= 13, f"selector tables with 64/64 keys >= 13 ({full})")

    print()
    if FAIL:
        print(f"FAILED ({len(FAIL)} checks)")
        raise SystemExit(1)
    print("ALL CHECKS PASSED")


if __name__ == "__main__":
    main()
