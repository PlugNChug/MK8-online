"""Build a Windows x86-64 portable tree without MSVC/WebView2 installer tooling.

This is the cross-build packaging path used from Linux: the launcher and runtime are
compiled with the project's `x86_64-windows-gnu` toolchain, the launcher embeds a
source payload, and the result is a self-contained directory plus zip. It does not
create the MSVC setup.exe; that remains a Windows CI/release step.
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
from zipfile import ZIP_DEFLATED, ZipFile

from launcher_payload import FFMPEG_FILES, HOST_FILES, write_payload
from package_guard import audit_archive

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
    parser.add_argument('--host-dir', type=Path, default=ROOT / 'prebuilt/host')
    parser.add_argument('--importer-dir', type=Path, default=ROOT / 'tools/mk8-import')
    parser.add_argument('--ffmpeg-dir', type=Path, default=ROOT / 'prebuilt/ffmpeg')
    parser.add_argument('--deps-dir', type=Path, default=ROOT / 'local/windows-verify/run-win')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    subprocess.run([sys.executable, ROOT / 'scripts/fetch-upstream.py'], check=True)
    payload = output / 'launcher-source.zip'
    payload_id = write_payload(payload, True, importer=args.importer_dir, host=args.host_dir, ffmpeg=args.ffmpeg_dir)
    launcher, webview = build_launcher(payload, payload_id, args.target_dir.resolve())
    tree = output / f'MK8-online-{version()}-windows-x86_64-portable'
    inventory = {
        'Launcher.exe': launcher,
        'WebView2Loader.dll': webview,
        'launcher-source.zip': payload,
        'LICENSE': ROOT / 'LICENSE',
        **{f'runtime/{name}': args.host_dir / name for name in HOST_FILES},
        **{f'runtime/{name}': args.ffmpeg_dir / name for name in FFMPEG_FILES},
        **{f'runtime/{name}': args.deps_dir / name
           for name in ('SDL3.dll', 'vulkan-1.dll', 'libshaderc_shared.dll')},
    }
    for name, source in inventory.items():
        if not source.is_file():
            raise ValueError(f'Missing {name}: {source}')
    if tree.exists():
        shutil.rmtree(tree)
    (tree / 'runtime').mkdir(parents=True)
    for name, source in inventory.items():
        shutil.copy2(source, tree / name)
    (tree / 'portable.txt').write_text('MK8-online portable mode\n', encoding='utf-8')
    (tree / 'README.txt').write_text(
        'MK8-online Windows x86-64 portable build\n\n'
        'Run Launcher.exe to prepare your own game files. Data is stored in portable/.\n'
        'Launcher.exe --headless --port 0 starts the local API.\n', encoding='utf-8')
    archive = output / f'{tree.name}.zip'
    with ZipFile(archive, 'w', ZIP_DEFLATED) as bundle:
        for path in sorted(tree.rglob('*')):
            if path.is_file():
                bundle.write(path, path.relative_to(output).as_posix())
    audit_archive(archive)
    print(f'tree {tree}', flush=True)
    print(f'archive {archive} {archive.stat().st_size} bytes sha256={digest(archive)}', flush=True)


if __name__ == '__main__':
    main()
