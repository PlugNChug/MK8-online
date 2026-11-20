#!/usr/bin/env python3
"""Prepare the legalised 4K MLFI module set for mlfi_replay.

Reads the user's own extraction (tools/extract_mlfi.py output, normally
``$HOME/.cache/mk8online/mlfi``), copies the 191 ``mlfi_4k_*`` modules and the four preprocess
``main`` modules into a scratch directory under ``$HOME/.cache/mk8online/mlfi-replay`` and applies
``tools/radv-mlfi/fix_image_types.py`` to the four modules that RADV cannot compile unchanged
(pass151, pass152, pass190 and the 5,004-byte preprocess main).

Nothing is written into the repository, and no AMD byte is copied anywhere outside the user's
cache.  The replay consumes the resulting directory plus the extracted ``model_blob.bin``.

Usage:
    tools/mlfi-replay/prepare.py [--source DIR] [--out DIR] [--force]

Prints two lines that the test harness parses:

    MODULES <dir>
    MODEL <path>
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
FIXER = ROOT / 'tools' / 'radv-mlfi' / 'fix_image_types.py'

# The 4K MLFI context creates four preprocess pipelines, identified by module size (MLFI-CAPTURE.md
# §4/§5).  The 5,004-byte one is the module Mesa cannot parse (64-bit sampled image).
PREPROCESS_MAINS = {
    9008: ('dilate', False),
    4204: ('splat', False),
    5004: ('fwdmv', True),
    8980: ('watermark', False),
}

# Modules whose images need the RADV legalisation, by entry point.
LEGALISE_16BIT = {'mlfi_4k_pass151', 'mlfi_4k_pass152', 'mlfi_4k_pass190'}


def cache_root() -> Path:
    return Path(os.environ.get('XDG_CACHE_HOME') or Path.home() / '.cache')


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, 'rb') as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b''):
            digest.update(chunk)
    return digest.hexdigest()


def find_manifest(source: Path) -> Path:
    manifest = source / 'manifest.json'
    if manifest.is_file():
        return manifest
    raise SystemExit(f'{manifest} not found; run tools/extract_mlfi.py first')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--source', type=Path,
                        default=cache_root() / 'mk8online' / 'mlfi',
                        help='extraction root (default %(default)s)')
    parser.add_argument('--out', type=Path,
                        default=cache_root() / 'mk8online' / 'mlfi-replay',
                        help='scratch root (default %(default)s)')
    parser.add_argument('--force', action='store_true', help='replace an existing prepared set')
    args = parser.parse_args()

    source = args.source.expanduser().resolve()
    out = args.out.expanduser().resolve()
    if ROOT in out.parents or out == ROOT:
        raise SystemExit('refusing to write inside the repository; use a cache directory')

    manifest_path = find_manifest(source)
    manifest = json.loads(manifest_path.read_text())
    modules = manifest['modules']
    by_entry = {m['entry']: m for m in modules if m['kind'] == 'mlfi_4k'}
    if len(by_entry) != 191:
        raise SystemExit(f'expected 191 mlfi_4k modules, manifest has {len(by_entry)}')

    mains = [m for m in modules if m['entry'] == 'main']
    by_size = {}
    for module in mains:
        by_size.setdefault(module['size'], []).append(module)
    selected_mains = []
    for size, (label, _) in PREPROCESS_MAINS.items():
        candidates = by_size.get(size) or []
        if len(candidates) != 1:
            raise SystemExit(f'expected exactly one {size}-byte main module, found {len(candidates)}')
        selected_mains.append((label, candidates[0]))

    models = sorted((source / 'model').glob('model_blob.bin'))
    if len(models) != 1:
        raise SystemExit(f'expected one model_blob.bin under {source / "model"}, found {len(models)}')
    model = models[0]

    module_dir = out / 'modules'
    if module_dir.exists():
        if not args.force:
            raise SystemExit(f'{module_dir} exists; pass --force to rebuild it')
        shutil.rmtree(module_dir)
    module_dir.mkdir(parents=True)

    if not FIXER.is_file():
        raise SystemExit(f'{FIXER} not found')
    if not shutil.which('spirv-dis') or not shutil.which('spirv-as'):
        raise SystemExit('spirv-dis/spirv-as not found (install spirv-tools)')

    extracted_by_hash = {}
    for candidate in sorted((source / 'modules').glob('*.spv')):
        extracted_by_hash.setdefault(sha256(candidate), candidate)

    def module_path(module: dict) -> Path:
        candidate = extracted_by_hash.get(module['sha256'])
        if candidate is None:
            raise SystemExit(f'no extracted file with sha256 {module["sha256"]} for {module["entry"]}')
        return candidate

    records = []
    for entry, module in sorted(by_entry.items()):
        src = module_path(module)
        dst = module_dir / src.name
        if entry in LEGALISE_16BIT:
            subprocess.run([sys.executable, str(FIXER), '--in', str(src), '--out', str(dst)],
                           check=True, capture_output=True, text=True)
            transform = '16-bit-image-types'
        else:
            shutil.copy2(src, dst)
            transform = 'none'
        records.append({'entry': entry, 'file': dst.name, 'source_sha256': sha256(src),
                        'output_sha256': sha256(dst), 'transform': transform})

    for label, module in selected_mains:
        src = module_path(module)
        dst = module_dir / src.name
        _, needs_i64 = PREPROCESS_MAINS[module['size']]
        if needs_i64:
            subprocess.run([sys.executable, str(FIXER), '--in', str(src), '--out', str(dst),
                            '--i64-sampled-to-storage'], check=True, capture_output=True, text=True)
            transform = 'i64-sampled-to-storage'
        else:
            shutil.copy2(src, dst)
            transform = 'none'
        records.append({'entry': 'main', 'role': label, 'file': dst.name,
                        'source_sha256': sha256(src), 'output_sha256': sha256(dst),
                        'transform': transform})

    files = sorted(p.name for p in module_dir.glob('*.spv'))
    if len(files) != 195:
        raise SystemExit(f'prepared module set has {len(files)} files, expected 195')

    summary = {
        'schema': 'mk8online.mlfi-replay-prepare/1',
        'source_manifest': str(manifest_path),
        'source_content_root': manifest.get('content_root'),
        'modules': records,
        'module_count': len(records),
        'model': {'path': str(model), 'size': model.stat().st_size, 'sha256': sha256(model)},
    }
    (out / 'prepare.json').write_text(json.dumps(summary, indent=1) + '\n')

    print(f'prepared {len(records)} modules in {module_dir}')
    print(f'MODULES {module_dir}')
    print(f'MODEL {model}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
