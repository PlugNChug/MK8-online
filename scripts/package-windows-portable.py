"""Build a Windows x86-64 portable tree without MSVC/WebView2 installer tooling.

This is the cross-build packaging path used from Linux: the launcher and runtime are
compiled with the project's `x86_64-windows-gnu` toolchain, the launcher embeds a
source payload, and the result is a self-contained directory plus zip. It does not
create the MSVC setup.exe; that remains a Windows CI/release step.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def version():
    text = (ROOT / 'Cargo.toml').read_text(encoding='utf-8')
    match = re.search(r'\[workspace\.package\][\s\S]*?\nversion = "([^"]+)"', text)
    if not match:
        raise SystemExit('workspace version not found')
    return match.group(1)


def payload_files():
    yield ROOT / 'LICENSE'
    yield ROOT / 'upstream/manifests/smm2recomp.json'
    for folder, suffixes in (
        ('runtime', None),
        ('toolchain', {'.py', '.txt', '.html'}),
        ('games/mk8d', None),
        ('scripts', {'.py', '.ps1'}),
        ('docs/licenses', None),
    ):
        for path in sorted((ROOT / folder).rglob('*')):
            if not path.is_file() or '__pycache__' in path.parts:
                continue
            if suffixes is not None and path.suffix not in suffixes:
                continue
            yield path
    for name in (
        'build_fg_provider.py', 'gen_fg_blobs.py', 'build_upscaler_provider.py',
        'gen_upscaler_blobs.py', 'build_fsr4_provider.py', 'fetch_vendor_sdk.py',
        'extract_fg_spv.py', 'extract_mlfi.py', 'dlss_rtx_harness.cpp',
        'dlss_bridge_harness.cpp', 'xess_wine_harness.cpp', 'mlfi_probe.c',
    ):
        path = ROOT / 'tools' / name
        if path.is_file():
            yield path


def write_payload(path):
    with ZipFile(path, 'w', ZIP_DEFLATED) as archive:
        for source in payload_files():
            entry = ZipInfo(source.relative_to(ROOT).as_posix())
            entry.date_time = (2026, 11, 20, 0, 0, 0)
            archive.writestr(entry, source.read_bytes(), compress_type=ZIP_DEFLATED)
    return digest(path)


def build_launcher(payload, payload_id, target_dir):
    env = dict(os.environ)
    env['MK8_LAUNCHER_PAYLOAD'] = str(payload)
    env['MK8_LAUNCHER_PAYLOAD_ID'] = payload_id
    env['CARGO_TARGET_DIR'] = str(target_dir)
    subprocess.run(
        ['cargo', 'build', '--release', '--locked', '-p', 'mk8-online',
         '--features', 'tauri/custom-protocol', '--target', 'x86_64-pc-windows-gnu'],
        cwd=ROOT, env=env, check=True)
    release = target_dir / 'x86_64-pc-windows-gnu/release'
    return release / 'Launcher.exe', release / 'WebView2Loader.dll'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'local/windows-portable')
    parser.add_argument('--target-dir', type=Path, default=ROOT / 'local/windows-portable/target')
    parser.add_argument('--host-dir', type=Path, default=ROOT / 'local/windows-rtx2/host')
    parser.add_argument('--game-dir', type=Path, default=ROOT / 'local/windows-rtx2/native-all')
    parser.add_argument('--deps-dir', type=Path,
                        default=ROOT / 'local/windows-verify/run-win')
    parser.add_argument('--skip-build', action='store_true')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    payload = output / 'launcher-source.zip'
    payload_id = write_payload(payload)
    print(f'payload {payload} {payload.stat().st_size} bytes sha256={payload_id}', flush=True)
    launcher, webview = build_launcher(payload, payload_id, args.target_dir.resolve()) \
        if not args.skip_build else (
            args.target_dir.resolve() / 'x86_64-pc-windows-gnu/release/Launcher.exe',
            args.target_dir.resolve() / 'x86_64-pc-windows-gnu/release/WebView2Loader.dll')
    tree = output / f'MK8-online-{version()}-windows-x86_64-portable'
    if tree.exists():
        shutil.rmtree(tree)
    runtime = tree / 'runtime'
    runtime.mkdir(parents=True)
    shutil.copy2(launcher, tree / 'Launcher.exe')
    shutil.copy2(webview, tree / 'WebView2Loader.dll')
    shutil.copy2(payload, tree / 'launcher-source.zip')
    shutil.copy2(ROOT / 'LICENSE', tree / 'LICENSE')
    (tree / 'README.txt').write_text(
        'MK8-online Windows x86-64 portable build\n\n'
        'This tree was cross-built without the MSVC/WebView2 setup.exe path.\n'
        'Run Launcher.exe --headless --port 0 --data-dir <folder> for the local API, or\n'
        'run runtime/mk8drecomp.exe <extracted data folder> for the game. Game files,\n'
        'keys, saves and vendor DLLs are not included. The runtime needs SDL3, vulkan-1,\n'
        'libshaderc_shared and the FFmpeg DLLs in runtime/.\n', encoding='utf-8')
    for path in sorted(args.host_dir.glob('*')):
        if path.is_file() and path.suffix.lower() in ('.exe', '.dll', '.dll.a'):
            shutil.copy2(path, runtime / path.name)
    for path in sorted(args.game_dir.glob('*')):
        if path.is_file() and (path.suffix.lower() in ('.dll', '.id')):
            shutil.copy2(path, runtime / path.name)
    for path in sorted(args.deps_dir.glob('*.dll')):
        target = runtime / path.name
        if not target.is_file():
            shutil.copy2(path, target)
    archive = output / f'{tree.name}.zip'
    with ZipFile(archive, 'w', ZIP_DEFLATED) as bundle:
        for path in sorted(tree.rglob('*')):
            if path.is_file():
                bundle.write(path, path.relative_to(output).as_posix())
    print(f'tree {tree}', flush=True)
    print(f'archive {archive} {archive.stat().st_size} bytes sha256={digest(archive)}', flush=True)
    for label, path in (('Launcher.exe', tree / 'Launcher.exe'), ('mk8drecomp.exe', runtime / 'mk8drecomp.exe'),
                        ('mk8dgame.dll', runtime / 'mk8dgame.dll'), ('launcher-source.zip', payload)):
        print(f'{label} {path.stat().st_size} {digest(path)}', flush=True)


if __name__ == '__main__':
    main()
