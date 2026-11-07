"""Resolve Windows runtime DLL imports from the official MSYS2 repository index.

Only required DLLs are copied. Archives are checked against repository SHA256 metadata.
The index and resolved package/hash list are retained locally for audit and reuse.
"""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile

import pefile
import zstandard

ROOT = Path(__file__).resolve().parents[1]
DEPS = Path(os.environ.get('MK8_NATIVE_TOOLS') or ROOT/'local/native-tools')/'deps'


def fetch(url, target):
    pending = target.with_suffix('.pending')
    subprocess.run(['curl.exe', '-L', '--fail', '--retry', '2', '--max-time', '180', '--silent', '--show-error', url, '--output', str(pending)], check=True)
    pending.replace(target)


def resolve(folder):
    index = DEPS/'ucrt64.files'
    if not index.exists():
        fetch('https://mirrors.tuna.tsinghua.edu.cn/msys2/mingw/ucrt64/ucrt64.files', index)
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
    lock = {}
    seen = set()
    queue = [*folder.glob('*.exe'), *folder.glob('*.dll')]
    while queue:
        path = queue.pop()
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
            if name.lower().startswith(('api-ms-', 'ext-ms-')) or (Path(os.environ['SystemRoot'])/'System32'/name).is_file():
                continue
            record = owners.get(name.lower())
            if not record:
                raise ValueError(f'DLL sem fornecedor conhecido: {name} (importada por {path.name})')
            filename, expected = record['FILENAME'][0], record['SHA256SUM'][0]
            if Path(filename).name != filename or not filename.startswith('mingw-w64-ucrt-x86_64-') or not filename.endswith('.pkg.tar.zst'):
                raise ValueError('Nome de pacote inválido no índice')
            package = DEPS/filename
            if not package.exists():
                print('Dependência: '+filename, flush=True)
                fetch('https://mirrors.tuna.tsinghua.edu.cn/msys2/mingw/ucrt64/'+filename, package)
            with package.open('rb') as f:
                if hashlib.file_digest(f, 'sha256').hexdigest() != expected:
                    raise ValueError('Package hash mismatch: '+filename)
            lock[filename] = expected
            with package.open('rb') as f, zstandard.ZstdDecompressor().stream_reader(f) as reader, tarfile.open(fileobj=reader, mode='r|') as archive:
                found = False
                for member in archive:
                    if member.isfile() and member.name.lower() == 'ucrt64/bin/'+name.lower():
                        dll.write_bytes(archive.extractfile(member).read()); found = True; break
                if not found:
                    raise ValueError('DLL ausente no pacote: '+name)
            queue.append(dll)
    lockfile = folder/'dll-packages.json'
    previous = json.loads(lockfile.read_text(encoding='utf-8')) if lockfile.exists() else {}
    previous.update(lock)
    lockfile.write_text(json.dumps(previous, indent=2), encoding='utf-8')
    print(f'Dependências verificadas: {len(seen)} executáveis/DLLs.', flush=True)


if __name__ == '__main__':
    resolve(Path(sys.argv[1]).resolve())
