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

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'toolchain'))

ROOT = Path(__file__).resolve().parents[1]
DEPS = Path(os.environ.get('MK8_NATIVE_TOOLS') or ROOT/'local/native-tools')/'deps'
CACHE = DEPS/'dlls'

# SDL loads the Vulkan loader at runtime; it is absent from the PE import table.
DYNAMIC_IMPORTS = {'sdl3.dll': ('vulkan-1.dll',)}


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


def resolve(folder, roots=None):
    """Copies into `folder` every DLL its programs import, from the cache or checked MSYS2 archives. Without `roots`, the
    programs are the executables and the DLLs no MSYS2 package provides (runtime and game modules); an MSYS2 DLL
    that none of them reaches any more (left by an earlier build) is removed."""
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
    if roots is None:
        roots = [*folder.glob('*.exe'), *(p for p in folder.glob('*.dll') if p.name.lower() not in owners)]
        prune_unreached = True
    else:
        prune_unreached = False
    queue = list(roots)
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
            imports.extend(DYNAMIC_IMPORTS.get(path.name.lower(), ()))
            pe.close()
            for name in imports:
                dll = folder/name
                if dll.exists():
                    entry = cache.get(name.lower())
                    if entry and digest(dll) == entry['sha256']:
                        lock[entry['package']] = entry['package_sha256']  # (placed earlier from this archive)
                    queue.append(dll); continue
                # Bundle the loader even when the build PC has a driver-provided copy.
                system = name.lower() != 'vulkan-1.dll' and (Path(os.environ['SystemRoot'])/'System32'/name).is_file()
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
        downloads = sorted({(filename, expected) for _, filename, expected in missing.values()})
        if downloads:
            fetch_all([(msys2(filename), DEPS/filename, expected) for filename, expected in downloads], label='Dependência')
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
    for stale in folder.glob('*.dll'):
        if prune_unreached and stale.name.lower() in owners and stale.name.lower() not in seen:
            stale.unlink()  # (left by an earlier build, e.g. the full FFmpeg's dependencies; nothing imports it now)
            print('DLL não usada removida: '+stale.name, flush=True)
    (folder/'dll-packages.json').write_text(json.dumps(lock, indent=2, sort_keys=True), encoding='utf-8')
    print(f'Dependências verificadas: {len(seen)} executáveis/DLLs.', flush=True)
    return lock


def prefetch():
    """Fills the DLL cache for the libraries the runtime links, then removes archives no build will read again."""
    from host import ffmpeg_dlls  # (the same libraries the build copies next to the runtime)
    bin = DEPS/'packages/ucrt64/bin'
    linked = [*(bin/name for name in ('SDL3.dll', 'libshaderc_shared.dll')), *ffmpeg_dlls()]
    if len(linked) != 6 or not all(p.is_file() for p in linked):
        raise ValueError('Bibliotecas do runtime ausentes em '+str(bin))
    # (an antivirus may still be scanning the copies when the folder is removed: the check itself is already done)
    with tempfile.TemporaryDirectory(prefix='mk8-dlls-', ignore_cleanup_errors=True) as temp:
        for dll in linked:
            shutil.copyfile(dll, Path(temp)/dll.name)
        resolve(Path(temp), roots=list(Path(temp).glob('*.dll')))
        # The cache keeps what the runtime imports now (e.g. no longer the full FFmpeg's ~95 dependencies).
        used = {p.name.lower() for p in Path(temp).glob('*.dll')}
        index = CACHE/'index.json'
        cache = json.loads(index.read_text(encoding='utf-8')) if index.is_file() else {}
        unused = sorted(set(cache) - used)
        for name in unused:
            (CACHE/name).unlink(missing_ok=True)
            del cache[name]
        if unused:
            index.write_text(json.dumps(cache, indent=2, sort_keys=True), encoding='utf-8')
            print(f'Cache de DLLs: {len(unused)} não usadas removidas.', flush=True)
    for leftover in DEPS.glob('mingw-w64-ucrt-x86_64-*.pkg.tar.zst'):
        leftover.unlink()  # (already in the DLL cache or not needed by the runtime; downloaded again if ever needed)


if __name__ == '__main__':
    if sys.argv[1:] == ['--prefetch']:
        prefetch()
    else:
        resolve(Path(sys.argv[1]).resolve())
