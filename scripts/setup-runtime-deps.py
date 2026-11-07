"""Fetch the pinned Windows graphics dependencies into the local tool directory."""
import hashlib
import os
from pathlib import Path
import re
import shutil
import tarfile
import subprocess
import zstandard

ROOT=Path(__file__).resolve().parents[1]

def setup():
    script=(ROOT/'upstream/smm2recomp/recomp/tools/fetch_deps.sh').read_text(encoding='utf-8')
    pins=re.findall(r'^([a-f0-9]{64}) (\S+)$',script,re.M)
    # Official MSYS2 package metadata; MK8's movie HLE also needs FFmpeg.
    pins.append(('35e9c314af3d80ba14ded8a7950ea2cb66195c9ec8b78a3983f64c4c36e4c083', 'ffmpeg-9.0.2-2'))
    root=Path(os.environ.get('MK8_NATIVE_TOOLS') or ROOT/'local/native-tools')/'deps';root.mkdir(parents=True,exist_ok=True)
    for expected,package in pins:
        name='mingw-w64-ucrt-x86_64-'+package+'-any.pkg.tar.zst'
        archive=root/name
        if not archive.exists():
            print('Downloading '+package,flush=True)
            pending=archive.with_suffix('.pending')
            subprocess.run(['curl.exe', '-L', '--fail', '--retry', '2', '--max-time', '180',
                            '--silent', '--show-error',
                            'https://mirrors.tuna.tsinghua.edu.cn/msys2/mingw/ucrt64/'+name,
                            '--output', str(pending)], check=True)
            pending.replace(archive)
        with archive.open('rb') as f:
            if hashlib.file_digest(f,'sha256').hexdigest()!=expected:raise ValueError('Package hash mismatch: '+package)
        if not (root/(package+'.done')).exists():
            with archive.open('rb') as f, zstandard.ZstdDecompressor().stream_reader(f) as reader, tarfile.open(fileobj=reader,mode='r|') as bundle:
                bundle.extractall(root/'packages',filter='data')
            (root/(package+'.done')).write_text(expected)
    print(root/'packages/ucrt64',flush=True)

if __name__=='__main__':setup()
