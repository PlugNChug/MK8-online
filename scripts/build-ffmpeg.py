"""Build the minimal FFmpeg that the setup carries (packaging machine with WSL; scripts/build-ffmpeg.sh).

The runtime only plays the game's movies: H.264 + AAC in MP4 and VP8 in WebM. The MSYS2 FFmpeg (all codecs) pulls
~100 DLLs (140 MB); this build has the four the runtime imports (~5 MB), with the same version and file names
(avcodec-63, avformat-63, avutil-61, swscale-10), so the runtime keeps linking the pinned MSYS2 headers and import
libraries. `prebuilt/ffmpeg/manifest.json` records each DLL's SHA-256; a build uses them only while they match.
Kept as is when the recorded build script is unchanged; `--force` builds again.
"""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'scripts/build-ffmpeg.sh'
OUTPUT = ROOT / 'prebuilt/ffmpeg'
SOURCE = ROOT / 'target/prebuilt-ffmpeg'  # (source archive for the release, LGPL)
DLLS = ('avcodec-63.dll', 'avformat-63.dll', 'avutil-61.dll', 'swscale-10.dll')


def sha256(path):
    with Path(path).open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def script_digest():
    """The build script's hash with LF line endings (the same whatever line endings the checkout uses)."""
    return hashlib.sha256(SCRIPT.read_bytes().replace(b'\r\n', b'\n')).hexdigest()


def current():
    manifest = OUTPUT / 'manifest.json'
    if not manifest.is_file():
        return False
    recorded = json.loads(manifest.read_text(encoding='utf-8'))
    return recorded.get('script') == script_digest() and all(
        (OUTPUT / name).is_file() and sha256(OUTPUT / name) == digest for name, digest in recorded['files'].items())


def windows_to_wsl(path):
    drive, rest = str(path).split(':', 1)
    return f'/mnt/{drive.lower()}' + rest.replace('\\', '/')


def main():
    if current() and '--force' not in sys.argv:
        print(f'FFmpeg mínimo já preparado: {OUTPUT}', flush=True)
        return
    # (the script travels with LF line endings, whatever the checkout uses)
    script = SOURCE / 'build-ffmpeg.sh'
    SOURCE.mkdir(parents=True, exist_ok=True)
    script.write_bytes(SCRIPT.read_bytes().replace(b'\r\n', b'\n'))
    subprocess.run(['wsl.exe', '-e', 'bash', windows_to_wsl(script)], check=True)
    subprocess.run(['wsl.exe', '-e', 'bash', '-c', f'cp /tmp/mk8-ffmpeg/out/* "{windows_to_wsl(SOURCE)}/"'], check=True)
    if OUTPUT.exists():
        shutil.rmtree(OUTPUT)
    OUTPUT.mkdir(parents=True)
    for name in (*DLLS, 'LICENSE.md', 'COPYING.LGPLv2.1', 'configuration.txt'):
        shutil.copyfile(SOURCE / name, OUTPUT / name)
    manifest = {'version': '9.0.2', 'source': 'https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz',
                'source_sha256': sha256(SOURCE / 'ffmpeg-9.0.2.tar.xz'), 'script': script_digest(),
                'files': {name: sha256(OUTPUT / name) for name in DLLS}}
    (OUTPUT / 'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    print(f'FFmpeg mínimo: {OUTPUT} ({sum((OUTPUT / n).stat().st_size for n in DLLS) / 2**20:.1f} MB)', flush=True)


if __name__ == '__main__':
    main()
