"""Resolve Windows runtime DLL imports from the official MSYS2 repository index.

Only required DLLs are copied. Archives are checked against repository SHA256 metadata.
The index and resolved package/hash list are retained locally for audit and reuse.

The DLLs taken from a checked archive are kept in `deps/dlls` with their SHA-256 and the archive's in `index.json`;
the archive is then deleted. A cached DLL is used only while both hashes still match (the DLL file and the index entry of
the package that owns it), so a build after the Tools step downloads nothing. `--prefetch` resolves the graphics and
FFmpeg libraries the runtime links (the same ones toolchain/host.py copies) ahead of the build.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import tarfile
import tempfile

import pefile
import zstandard

from downloads import fetch, fetch_all, msys2

ROOT = Path(__file__).resolve().parents[1]
DEPS = Path(os.environ.get('MK8_NATIVE_TOOLS') or ROOT/'local/native-tools')/'deps'
CACHE = DEPS/'dlls'
# The libraries toolchain/host.py copies next to the runtime executable.
LINKED = ['SDL3.dll', 'libshaderc_shared.dll', 'avcodec-*.dll', 'avformat-*.dll', 'avutil-*.dll', 'swscale-*.dll']


def digest(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def cached(cache, name, filename, expected):
    """The cached DLL, if it came from this exact archive and is unchanged."""
    entry = cache.get(name.lower())
    path = CACHE/name.lower()
    if entry and entry['package'] == filename and entry['package_sha256'] == expected and path.is_file() \
            and digest(path) == entry['sha256']:
        return path
    return None


def unpack(filename, expected, name, cache):
    """Checks one downloaded package archive and keeps one of its DLLs in the cache. The archive is deleted by
    `resolve` once every DLL it provides has been taken."""
    package = DEPS/filename
    if digest(package) != expected:
        raise ValueError('Package hash mismatch: '+filename)
    CACHE.mkdir(parents=True, exist_ok=True)
    with package.open('rb') as f, zstandard.ZstdDecompressor().stream_reader(f) as reader, tarfile.open(fileobj=reader, mode='r|') as archive:
        for member in archive:
            if member.isfile() and member.name.lower() == 'ucrt64/bin/'+name.lower():
                data = archive.extractfile(member).read()
                (CACHE/name.lower()).write_bytes(data)
                cache[name.lower()] = {'sha256': hashlib.sha256(data).hexdigest(), 'package': filename, 'package_sha256': expected}
                break
    (CACHE/'index.json').write_text(json.dumps(cache, indent=2, sort_keys=True), encoding='utf-8')


def resolve(folder):
    index = DEPS/'ucrt64.files'
    if not index.exists():
        fetch(msys2('ucrt64.files'), index)
    records = {}
    with index.open('rb') as compressed, zstandard.ZstdDecompressor().stream_reader(compressed) as reader, tarfile.open(fileobj=reader, mode='r|') as archive:
        for member in archive:
            if member.name.endswith(('/desc', '/files')):
                raw = archive.extractfile(member).read().decode('utf-8')
                record = records.setdefault(member.name.split('/')[0], {})
                for section in raw.strip().split('\n\n'):
                    lines = section.splitlines()
                    if lines and lines[0].startswith('%'):
                        record[lines[0].strip('%')] = lines[1:]
    owners = {}
    for record in records.values():
        for name in record.get('FILES', []):
            if name.startswith('ucrt64/bin/') and name.lower().endswith('.dll'):
                owners[Path(name).name.lower()] = record
    cache_index = CACHE/'index.json'
    cache = json.loads(cache_index.read_text(encoding='utf-8')) if cache_index.is_file() else {}
    lock = {}
    seen = set()
    opened = set()
    queue = [*folder.glob('*.exe'), *folder.glob('*.dll')]
    # One dependency level at a time: the archives a level needs are downloaded together.
    while queue:
        level, queue, missing = queue, [], {}
        for path in level:
            if path.name.lower() in seen:
                continue
            seen.add(path.name.lower())
            pe = pefile.PE(str(path), fast_load=True)
            if pe.FILE_HEADER.Machine != 0x8664 or pe.OPTIONAL_HEADER.SizeOfImage >= 0x80000000:
                pe.close()
                raise ValueError('Imagem Windows x64 inválida ou >= 2 GiB: '+path.name)
            pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT']])
            imports = [entry.dll.decode('ascii') for entry in getattr(pe, 'DIRECTORY_ENTRY_IMPORT', [])]
            pe.close()
            for name in imports:
                dll = folder/name
                if dll.exists():
                    queue.append(dll); continue
                system = (Path(os.environ['SystemRoot'])/'System32'/name).is_file()
                if name.lower() in missing or name.lower().startswith(('api-ms-', 'ext-ms-')) or system:
                    continue
                record = owners.get(name.lower())
                if not record:
                    raise ValueError(f'DLL sem fornecedor conhecido: {name} (importada por {path.name})')
                filename, expected = record['FILENAME'][0], record['SHA256SUM'][0]
                if Path(filename).name != filename or not filename.startswith('mingw-w64-ucrt-x86_64-') or not filename.endswith('.pkg.tar.zst'):
                    raise ValueError('Nome de pacote inválido no índice')
                lock[filename] = expected
                source = cached(cache, name, filename, expected)
                if source:
                    shutil.copyfile(source, dll)
                    queue.append(dll)
                else:
                    missing[name.lower()] = (name, filename, expected)
        downloads = sorted({filename for _, filename, _ in missing.values() if not (DEPS/filename).exists()})
        if downloads:
            fetch_all([(msys2(filename), DEPS/filename) for filename in downloads], label='Dependência')
        for name, filename, expected in missing.values():
            unpack(filename, expected, name, cache)
            opened.add(filename)
            source = cached(cache, name, filename, expected)
            if not source:
                raise ValueError('DLL ausente no pacote: '+name)
            shutil.copyfile(source, folder/name)
            queue.append(folder/name)
    for filename in opened:
        (DEPS/filename).unlink(missing_ok=True)  # (its DLLs are in the cache now)
    lockfile = folder/'dll-packages.json'
    previous = json.loads(lockfile.read_text(encoding='utf-8')) if lockfile.exists() else {}
    previous.update(lock)
    lockfile.write_text(json.dumps(previous, indent=2), encoding='utf-8')
    print(f'Dependências verificadas: {len(seen)} executáveis/DLLs.', flush=True)
    return lock


def prefetch():
    """Fills the DLL cache for the libraries the runtime links, then removes archives no build will read again."""
    bin = DEPS/'packages/ucrt64/bin'
    linked = [p for pattern in LINKED for p in sorted(bin.glob(pattern))]
    if len(linked) < len(LINKED):
        raise ValueError('Bibliotecas do runtime ausentes em '+str(bin))
    # (an antivirus may still be scanning the copies when the folder is removed: the check itself is already done)
    with tempfile.TemporaryDirectory(prefix='mk8-dlls-', ignore_cleanup_errors=True) as temp:
        for dll in linked:
            shutil.copyfile(dll, Path(temp)/dll.name)
        resolve(Path(temp))
    for leftover in DEPS.glob('mingw-w64-ucrt-x86_64-*.pkg.tar.zst'):
        leftover.unlink()  # (already in the DLL cache or not needed by the runtime; downloaded again if ever needed)


if __name__ == '__main__':
    if sys.argv[1:] == ['--prefetch']:
        prefetch()
    else:
        resolve(Path(sys.argv[1]).resolve())
