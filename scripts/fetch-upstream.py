"""Fetch the pinned upstream sources into upstream/<name> and verify every file against its manifest.

The public repository keeps only upstream/manifests; each machine downloads the pinned commits here.
"""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def verified(target, manifest):
    return all((target/rel).is_file() and digest(target/rel) == sha for rel, sha in manifest['files'].items())


def fetch(name):
    manifest = json.loads((ROOT/'upstream/manifests'/f'{name}.json').read_text(encoding='utf-8'))
    target = ROOT/'upstream'/name
    if target.is_dir() and verified(target, manifest):
        print(f'{name}: already at {manifest["commit"][:12]}')
        return
    with tempfile.TemporaryDirectory(dir=ROOT/'upstream') as folder:
        work = Path(folder)/'src'
        for args in (['init', '-q', str(work)], ['-C', str(work), 'remote', 'add', 'origin', manifest['repository']],
                     ['-C', str(work), 'fetch', '-q', '--depth', '1', 'origin', manifest['commit']],
                     ['-C', str(work), 'checkout', '-q', 'FETCH_HEAD']):
            subprocess.run(['git', *args], check=True)
        staged = Path(folder)/'staged'
        for rel in manifest['files']:
            (staged/rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(work/rel, staged/rel)
        if not verified(staged, manifest):
            raise SystemExit(f'{name}: downloaded files do not match the manifest')
        if target.exists():
            raise SystemExit(f'{name}: {target} exists with other contents; inspect it before replacing')
        staged.rename(target)
    print(f'{name}: fetched {manifest["commit"][:12]}')


if __name__ == '__main__':
    for name in sys.argv[1:] or ['smm2recomp', 'LASRecomp']:
        fetch(name)
