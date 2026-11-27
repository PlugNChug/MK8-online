"""Build the minimal FFmpeg that the setup carries (Linux or Windows with WSL; scripts/build-ffmpeg.sh).

The runtime only plays the game's movies: H.264 + AAC in MP4 and VP8 in WebM. The MSYS2 FFmpeg (all codecs) pulls
~100 DLLs (140 MB); this build has the four the runtime imports (~5 MB), with the same version and file names
(avcodec-63, avformat-63, avutil-61, swscale-10), so the runtime keeps linking the pinned MSYS2 headers and import
libraries. `prebuilt/ffmpeg/manifest.json` records each DLL's SHA-256; a build uses them only while they match.
Kept as is when the recorded build script is unchanged. Use `--require-source` to verify the
release source files alongside the DLLs, or `--force` to rebuild.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'scripts/build-ffmpeg.sh'
OUTPUT = ROOT / 'prebuilt/ffmpeg'
sys.path.insert(0, str(ROOT / 'scripts'))
from cargo_target import target_dir  # noqa: E402

SOURCE = target_dir() / 'prebuilt-ffmpeg'  # (source archive for the release, LGPL)
SOURCE_ARCHIVE = 'ffmpeg-9.0.2.tar.xz'
DLLS = ('avcodec-63.dll', 'avformat-63.dll', 'avutil-61.dll', 'swscale-10.dll')
NOTICES = ('LICENSE.md', 'COPYING.LGPLv2.1', 'configuration.txt')


def sha256(path):
    with Path(path).open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def script_digest():
    """The build script's hash with LF line endings (the same whatever line endings the checkout uses)."""
    return hashlib.sha256(SCRIPT.read_bytes().replace(b'\r\n', b'\n')).hexdigest()


def current(require_source=False):
    try:
        recorded = json.loads((OUTPUT / 'manifest.json').read_text(encoding='utf-8'))
        digest = script_digest()
        if not isinstance(recorded, dict) or recorded.get('script') != digest:
            return False
        files = recorded.get('files')
        if not isinstance(files, dict) or set(files) != set(DLLS):
            return False
        if any(sha256(OUTPUT / name) != expected for name, expected in files.items()):
            return False
        if not all((OUTPUT / name).is_file() for name in NOTICES):
            return False
        return not require_source or (
            sha256(SOURCE / SCRIPT.name) == digest
            and sha256(SOURCE / SOURCE_ARCHIVE) == recorded.get('source_sha256')
        )
    except (OSError, ValueError):
        return False


def windows_to_wsl(path):
    drive, rest = str(path).split(':', 1)
    return f'/mnt/{drive.lower()}' + rest.replace('\\', '/')


def main():
    if '--force' not in sys.argv and current(require_source='--require-source' in sys.argv):
        print(f'FFmpeg mínimo já preparado: {OUTPUT}', flush=True)
        return
    # (the script travels with LF line endings, whatever the checkout uses)
    script = SOURCE / 'build-ffmpeg.sh'
    SOURCE.mkdir(parents=True, exist_ok=True)
    script.write_bytes(SCRIPT.read_bytes().replace(b'\r\n', b'\n'))
    if os.name == "nt":
        subprocess.run(['wsl.exe', '-e', 'bash', windows_to_wsl(script)], check=True)
        subprocess.run(['wsl.exe', '-e', 'bash', '-c', 'cp /tmp/mk8-ffmpeg/out/* "$1/"',
                        'bash', windows_to_wsl(SOURCE)], check=True)
    else:
        subprocess.run(['bash', script], check=True)
        for path in Path('/tmp/mk8-ffmpeg/out').iterdir():
            shutil.copyfile(path, SOURCE / path.name)
    if OUTPUT.exists():
        shutil.rmtree(OUTPUT)
    OUTPUT.mkdir(parents=True)
    for name in (*DLLS, *NOTICES):
        shutil.copyfile(SOURCE / name, OUTPUT / name)
    manifest = {'version': '9.0.2', 'source': 'https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz',
                'source_sha256': sha256(SOURCE / SOURCE_ARCHIVE), 'script': script_digest(),
                'files': {name: sha256(OUTPUT / name) for name in DLLS}}
    (OUTPUT / 'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    print(f'FFmpeg mínimo: {OUTPUT} ({sum((OUTPUT / n).stat().st_size for n in DLLS) / 2**20:.1f} MB)', flush=True)


if __name__ == '__main__':
    main()
