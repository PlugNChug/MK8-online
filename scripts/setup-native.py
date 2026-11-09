"""Install the upstream-pinned Zig compiler locally, checking the archive hash.

Once extracted and checked, the archive (96 MB) is deleted: `zig-<version>.done` keeps the archive's SHA-256 and the
compiler's own, which is checked on every run (a changed or damaged zig.exe is installed again). The C library headers
of other systems (Linux, macOS, BSD, WASI: ~55 MB) are removed too; the build only targets x86_64-windows-gnu.
"""
import hashlib
import os
from pathlib import Path
import shutil
import zipfile

from downloads import fetch

ROOT = Path(__file__).resolve().parents[1]
VERSION = '0.17.0'
SHA256 = 'b5663f69581dcf391293fbf16c06cb80d81d806545ce618b4d0bab7f0eb8c428'
# What the Windows (MinGW) target reads from lib/libc: everything else is for other systems.
KEEP_LIBC = {'include', 'mingw'}
KEEP_LIBC_INCLUDE = {'any-windows-any'}


def sha256(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def prune(compiler_dir):
    """Removes the other systems' C library files from an extracted Zig (idempotent). Returns the bytes freed."""
    libc = compiler_dir / 'lib/libc'
    freed = 0
    victims = [p for p in libc.iterdir() if p.is_dir() and p.name not in KEEP_LIBC] if libc.is_dir() else []
    include = libc / 'include'
    if include.is_dir():
        victims += [p for p in include.iterdir() if p.is_dir() and p.name not in KEEP_LIBC_INCLUDE]
    for path in victims:
        if path.is_symlink() or os.path.isjunction(path):
            continue
        freed += sum(f.stat().st_size for f in path.rglob('*') if f.is_file())
        shutil.rmtree(path)
    return freed


def setup():
    folder = Path(os.environ.get('MK8_NATIVE_TOOLS') or ROOT / 'local/native-tools')
    folder.mkdir(parents=True, exist_ok=True)
    name = f'zig-x86_64-windows-{VERSION}'
    archive = folder / (name + '.zip')
    marker = folder / (name + '.done')
    compiler = folder / name / 'zig.exe'
    recorded = marker.read_text(encoding='ascii').split() if marker.is_file() else []
    if not (len(recorded) == 2 and recorded[0] == SHA256 and compiler.is_file() and sha256(compiler) == recorded[1]):
        print('Checking/downloading pinned Zig compiler...', flush=True)
        fetch(f'https://ziglang.org/download/{VERSION}/{name}.zip', archive, parts=8, sha256=SHA256)
        if sha256(archive) != SHA256:
            raise ValueError('Zig archive hash mismatch')
        with zipfile.ZipFile(archive) as bundle:
            for entry in bundle.infolist():
                target = (folder / entry.filename).resolve()
                if not target.is_relative_to(folder.resolve()):
                    raise ValueError('Archive path escapes local tool directory')
            if compiler.parent.exists():
                shutil.rmtree(compiler.parent)
            bundle.extractall(folder)
        marker.write_text(f'{SHA256} {sha256(compiler)}\n', encoding='ascii')
    freed = prune(compiler.parent)
    if archive.exists():
        freed += archive.stat().st_size
        archive.unlink()
    if freed:
        print(f'Zig: {freed / 1024**2:.0f} MB de arquivos não usados removidos.', flush=True)
    print(compiler, flush=True)
    return compiler


if __name__ == '__main__':
    setup()
