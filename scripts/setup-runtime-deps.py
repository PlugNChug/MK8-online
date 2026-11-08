"""Fetch the pinned Windows graphics dependencies into the local tool directory.

Each package archive is checked against its pinned SHA-256, extracted and then deleted: `<package>.done` keeps the hash
(the launcher checks it). Files the runtime build never reads are removed after extraction: the Vulkan C++ headers and
registry (the runtime is C), the glslc compiler (shaderc is used as a library) and the static libraries (the runtime
links the import libraries, *.dll.a). The runtime DLLs the game executable needs are then fetched ahead of the build
(scripts/runtime-dlls.py --prefetch), so the build itself downloads nothing.
"""
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile

import zstandard

ROOT = Path(__file__).resolve().parents[1]


def prune(ucrt64):
    """Removes from the extracted packages what the runtime build never reads (idempotent). Returns bytes freed."""
    victims = [*(ucrt64 / 'include/vulkan').glob('*.hpp'), ucrt64 / 'share/vulkan/registry', ucrt64 / 'bin/glslc.exe',
               *(p for p in (ucrt64 / 'lib').glob('lib*.a') if not p.name.endswith('.dll.a'))]
    freed = 0
    for path in victims:
        if path.is_symlink() or os.path.isjunction(path) or not path.exists():
            continue
        if path.is_dir():
            freed += sum(f.stat().st_size for f in path.rglob('*') if f.is_file())
            shutil.rmtree(path)
        else:
            freed += path.stat().st_size
            path.unlink()
    return freed


def setup():
    script = (ROOT / 'upstream/smm2recomp/recomp/tools/fetch_deps.sh').read_text(encoding='utf-8')
    pins = re.findall(r'^([a-f0-9]{64}) (\S+)$', script, re.M)
    # Official MSYS2 package metadata; MK8's movie HLE also needs FFmpeg.
    pins.append(('35e9c314af3d80ba14ded8a7950ea2cb66195c9ec8b78a3983f64c4c36e4c083', 'ffmpeg-9.0.2-2'))
    root = Path(os.environ.get('MK8_NATIVE_TOOLS') or ROOT / 'local/native-tools') / 'deps'
    root.mkdir(parents=True, exist_ok=True)
    for expected, package in pins:
        name = 'mingw-w64-ucrt-x86_64-' + package + '-any.pkg.tar.zst'
        archive = root / name
        done = root / (package + '.done')
        if not (done.is_file() and done.read_text(encoding='ascii').strip() == expected):
            if not archive.exists():
                print('Downloading ' + package, flush=True)
                pending = archive.with_suffix('.pending')
                subprocess.run(['curl.exe', '-L', '--fail', '--retry', '2', '--max-time', '180',
                                '--silent', '--show-error',
                                'https://mirrors.tuna.tsinghua.edu.cn/msys2/mingw/ucrt64/' + name,
                                '--output', str(pending)], check=True)
                pending.replace(archive)
            with archive.open('rb') as f:
                if hashlib.file_digest(f, 'sha256').hexdigest() != expected:
                    raise ValueError('Package hash mismatch: ' + package)
            with archive.open('rb') as f, zstandard.ZstdDecompressor().stream_reader(f) as reader, \
                    tarfile.open(fileobj=reader, mode='r|') as bundle:
                bundle.extractall(root / 'packages', filter='data')
            done.write_text(expected, encoding='ascii')
    # The DLLs the runtime and FFmpeg import, fetched now rather than in the middle of the build. It runs before the
    # pinned archives are deleted: some imported DLLs (FFmpeg's swresample) come from them.
    subprocess.run([sys.executable, str(ROOT / 'scripts/runtime-dlls.py'), '--prefetch'], check=True)
    freed = prune(root / 'packages/ucrt64')
    for expected, package in pins:
        archive = root / ('mingw-w64-ucrt-x86_64-' + package + '-any.pkg.tar.zst')
        if archive.exists():
            freed += archive.stat().st_size
            archive.unlink()
    if freed:
        print(f'Bibliotecas: {freed / 1024**2:.0f} MB de arquivos não usados removidos.', flush=True)
    print(root / 'packages/ucrt64', flush=True)


if __name__ == '__main__':
    setup()
