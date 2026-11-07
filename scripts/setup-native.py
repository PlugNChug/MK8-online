"""Install the upstream-pinned Zig compiler locally, checking the archive hash."""
import hashlib
import os
from pathlib import Path
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
VERSION = '0.17.0'
SHA256 = 'b5663f69581dcf391293fbf16c06cb80d81d806545ce618b4d0bab7f0eb8c428'


def setup():
    folder = Path(os.environ.get('MK8_NATIVE_TOOLS') or ROOT / 'local/native-tools')
    folder.mkdir(parents=True, exist_ok=True)
    name = f'zig-x86_64-windows-{VERSION}'
    archive = folder / (name + '.zip')
    if not archive.exists():
        pending = archive.with_suffix('.pending')
        print('Downloading pinned Zig compiler...', flush=True)
        urllib.request.urlretrieve(f'https://ziglang.org/download/{VERSION}/{name}.zip', pending)
        pending.replace(archive)
    with archive.open('rb') as f:
        if hashlib.file_digest(f, 'sha256').hexdigest() != SHA256:
            raise ValueError('Zig archive hash mismatch')
    compiler = folder / name / 'zig.exe'
    if not compiler.exists():
        with zipfile.ZipFile(archive) as bundle:
            for entry in bundle.infolist():
                target = (folder / entry.filename).resolve()
                if not target.is_relative_to(folder.resolve()):
                    raise ValueError('Archive path escapes local tool directory')
            bundle.extractall(folder)
    print(compiler, flush=True)
    return compiler


if __name__ == '__main__':
    setup()
