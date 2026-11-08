"""Fetch the pinned Windows graphics dependencies into the local tool directory.

Each package archive is checked against its pinned SHA-256, extracted and then deleted: `<package>.done` keeps the hash
(the launcher checks it). Files the runtime build never reads are removed after extraction: the Vulkan C++ headers and
registry (the runtime is C), the glslc compiler (shaderc is used as a library) and the static libraries (the runtime
links the import libraries, *.dll.a). The runtime DLLs the game executable needs are then fetched ahead of the build
(scripts/runtime-dlls.py --prefetch), so the build itself downloads nothing.

While the packages download, Zig builds the MinGW C runtime and libc++ into its cache (~5 min of CPU, once per tool
folder). The build used to do that next to the game compilation; the same link commands then take seconds.
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

from downloads import fetch_all, msys2

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'toolchain'))

ROOT = Path(__file__).resolve().parents[1]
ZIG_VERSION = '0.17.0'  # (scripts/setup-native.py)
PROBES = {'probe.c': 'int mk8_link_probe(void) { return 0; }\n',
          'probe.cpp': '#include <string>\nint main(void) { std::string s("x"); return (int)s.size() - 1; }\n'}


def warm_compiler(tools):
    """Starts the two links whose libraries the build needs (a C DLL like the game modules, a C++ program like the
    runtime), with the build's cache folder. Returns the processes, or [] when Zig is not installed yet."""
    zig = tools / f'zig-x86_64-windows-{ZIG_VERSION}/zig.exe'
    if not zig.is_file():
        return []
    folder = tools / 'warmup'
    folder.mkdir(parents=True, exist_ok=True)
    for name, text in PROBES.items():
        (folder / name).write_text(text, encoding='ascii')
    env = {**os.environ, 'ZIG_GLOBAL_CACHE_DIR': str(tools / 'cache')}
    commands = [[zig, 'cc', '-target', 'x86_64-windows-gnu', '-shared', '-s', '-o', folder / 'probe.dll', folder / 'probe.c'],
                [zig, 'c++', '-target', 'x86_64-windows-gnu', '-o', folder / 'probe.exe', folder / 'probe.cpp']]
    print('Preparando o compilador (bibliotecas C e C++) enquanto baixa...', flush=True)
    return [(subprocess.Popen([str(a) for a in command], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env),
             command[1]) for command in commands]


def finish_warmup(started):
    for process, kind in started:
        output, _ = process.communicate()
        if process.returncode:
            raise ValueError(f'Falha ao preparar o compilador ({kind}):\n' + output.decode('utf-8', 'replace')[-3000:])
    if started:
        print('Compilador preparado.', flush=True)


def prune(ucrt64, minimal_ffmpeg=False):
    """Removes from the extracted packages what the runtime build never reads (idempotent). Returns bytes freed.

    With the setup's minimal FFmpeg (checked by toolchain/host.py ffmpeg_dlls), the full MSYS2 FFmpeg programs and
    DLLs are not used either; its headers and import libraries (same version) still are."""
    victims = [*(ucrt64 / 'include/vulkan').glob('*.hpp'), ucrt64 / 'share/vulkan/registry', ucrt64 / 'bin/glslc.exe',
               *(p for p in (ucrt64 / 'lib').glob('lib*.a') if not p.name.endswith('.dll.a'))]
    if minimal_ffmpeg:
        victims += [p for pattern in ('av*-*.dll', 'sw*-*.dll', 'postproc-*.dll', 'ffmpeg.exe', 'ffprobe.exe',
                                      'ffplay.exe') for p in (ucrt64 / 'bin').glob(pattern)]
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
    tools = Path(os.environ.get('MK8_NATIVE_TOOLS') or ROOT / 'local/native-tools')
    root = tools / 'deps'
    root.mkdir(parents=True, exist_ok=True)
    warming = warm_compiler(tools)
    try:

        def archive_of(package):
            return root / ('mingw-w64-ucrt-x86_64-' + package + '-any.pkg.tar.zst')

        def extracted(expected, package):
            done = root / (package + '.done')
            return done.is_file() and done.read_text(encoding='ascii').strip() == expected

        wanted = [(expected, package) for expected, package in pins if not extracted(expected, package)]
        fetch_all([(msys2(archive_of(package).name), archive_of(package))
                   for _, package in wanted if not archive_of(package).exists()], label='Downloading')
        for expected, package in wanted:
            archive = archive_of(package)
            with archive.open('rb') as f:
                if hashlib.file_digest(f, 'sha256').hexdigest() != expected:
                    raise ValueError('Package hash mismatch: ' + package)
            with archive.open('rb') as f, zstandard.ZstdDecompressor().stream_reader(f) as reader, \
                    tarfile.open(fileobj=reader, mode='r|') as bundle:
                bundle.extractall(root / 'packages', filter='data')
            (root / (package + '.done')).write_text(expected, encoding='ascii')
        # The DLLs the runtime and FFmpeg import, fetched now rather than in the middle of the build. It runs before the
        # pinned archives are deleted: some imported DLLs (FFmpeg's swresample) come from them.
        subprocess.run([sys.executable, str(ROOT / 'scripts/runtime-dlls.py'), '--prefetch'], check=True)
        from host import PREBUILT_FFMPEG, ffmpeg_dlls
        found = ffmpeg_dlls()
        minimal = bool(found) and all(p.is_relative_to(PREBUILT_FFMPEG) for p in found)
        freed = prune(root / 'packages/ucrt64', minimal_ffmpeg=minimal)
        for expected, package in pins:
            archive = archive_of(package)
            if archive.exists():
                freed += archive.stat().st_size
                archive.unlink()
        if freed:
            print(f'Bibliotecas: {freed / 1024**2:.0f} MB de arquivos não usados removidos.', flush=True)
    except BaseException:
        for process, _ in warming:
            process.kill()  # (a failed or cancelled step leaves no compiler running)
        raise
    finish_warmup(warming)
    print(root / 'packages/ucrt64', flush=True)


if __name__ == '__main__':
    setup()
