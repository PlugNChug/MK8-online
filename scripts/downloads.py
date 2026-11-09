"""Parallel downloads for the Tools step (curl.exe ships with Windows 10 1803+).

One connection to these servers runs at well under the line speed (0.6-0.9 MB/s measured), so packages are fetched
several at a time and a large file is fetched as byte ranges in parallel. Nothing here trusts what it downloads: each
caller checks the pinned or repository SHA-256 before using the file.
"""
import concurrent.futures
import hashlib
import os
from pathlib import Path
import subprocess
import threading

# Official MSYS2 mirrors with the same repository layout; the first one is what upstream pins.
MSYS2 = ['https://mirrors.tuna.tsinghua.edu.cn/msys2/mingw/ucrt64/', 'https://repo.msys2.org/mingw/ucrt64/']
CURL = ['curl.exe', '-L', '--fail', '--retry', '2', '--connect-timeout', '30', '--max-time', '600', '--silent',
        '--show-error']
_lock = threading.Lock()


def _size(url):
    """Content length when the server accepts byte ranges, else None."""
    result = subprocess.run([*CURL, '--head', url], capture_output=True, text=True)
    if result.returncode:
        return None
    headers = {}
    for line in result.stdout.splitlines():  # (after redirects, the last response wins)
        name, _, value = line.partition(':')
        if value:
            headers[name.strip().lower()] = value.strip()
    if headers.get('accept-ranges', '').lower() != 'bytes' or not headers.get('content-length', '').isdigit():
        return None
    return int(headers['content-length'])


def _ranged(url, pending, size, parts):
    step = -(-size // parts)
    pieces = [pending.with_name(f'{pending.name}.{i}') for i in range(parts)]

    def one(i):
        first, last = i * step, min(size, (i + 1) * step) - 1
        subprocess.run([*CURL, '--range', f'{first}-{last}', url, '--output', str(pieces[i])], check=True)
        if pieces[i].stat().st_size != last - first + 1:
            raise ValueError('Download incompleto: ' + url)

    try:
        with concurrent.futures.ThreadPoolExecutor(parts) as pool:
            list(pool.map(one, range(parts)))
        with pending.open('wb') as out:
            for piece in pieces:
                with piece.open('rb') as data:
                    while chunk := data.read(1 << 20):
                        out.write(chunk)
    finally:
        for piece in pieces:
            piece.unlink(missing_ok=True)


def fetch(urls, target, parts=1, sha256=None):
    """Downloads the first URL that works into `target` (atomically). `parts` > 1 splits a large file into ranges."""
    target = Path(target)
    def matches(path):
        with path.open('rb') as stream:
            return hashlib.file_digest(stream, 'sha256').hexdigest() == sha256
    if sha256 is not None and target.is_file() and matches(target):
        return target
    pending = target.with_name(target.name + '.pending')
    errors = []
    for url in [urls] if isinstance(urls, str) else urls:
        try:
            size = _size(url) if parts > 1 else None
            if size and size > 8 << 20:
                _ranged(url, pending, size, parts)
            else:
                subprocess.run([*CURL, url, '--output', str(pending)], check=True)
            if sha256 is not None and not matches(pending):
                raise ValueError('SHA-256 mismatch: ' + target.name)
        except (OSError, subprocess.CalledProcessError, ValueError) as error:
            errors.append(f'{url}: {error}')
            pending.unlink(missing_ok=True)
            continue
        # Local publication failures are not mirror failures. Keep the verified
        # download and previous destination; never delete the destination to retry.
        try:
            os.replace(pending, target)
        except OSError as error:
            raise OSError(error.errno, f'Não foi possível publicar o download em {target}: {error}. '
                          f'Confira permissões, arquivos em uso e a conexão da unidade. Arquivo baixado preservado em {pending}.', str(target)) from error
        return target
    raise ValueError('Falha no download:\n' + '\n'.join(errors))


def fetch_all(jobs, workers=8, label='Baixando'):
    """Downloads [(urls, target[, sha256])] in parallel; verified cached files are reused."""
    def one(job):
        urls, target, *expected = job
        with _lock:
            print(f'{label}: {Path(target).name}', flush=True)
        return fetch(urls, target, sha256=expected[0] if expected else None)

    with concurrent.futures.ThreadPoolExecutor(max(1, min(workers, len(jobs)))) as pool:
        return list(pool.map(one, jobs))


def msys2(filename):
    return [base + filename for base in MSYS2]
