"""Build the runtime executable that the setup carries (packaging machine; no game file is read).

The runtime is the project's own code (upstream runtime, runtime/mk8, games/mk8d) prepared from the 4.0.1 profile, the
same way a build prepares it. `prebuilt/host/manifest.json` records the portable key of its inputs (toolchain/host.py
host_key) and the file hashes; a build uses these files only when its own key is identical, and compiles otherwise.
"""
import json
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'toolchain'))
sys.path.insert(0, str(ROOT / 'scripts'))
from cargo_target import target_dir  # noqa: E402

TARGET = target_dir()
WORK = TARGET / 'prebuilt-host'
PROFILE = ROOT / 'games/mk8d/profiles/4.0.1.json'


def main():
    os.environ['MK8_HOST_FROM_SOURCE'] = '1'
    from host import HOST_FILES, PREBUILT, build_host
    if WORK.exists():
        if WORK.is_symlink() or WORK.is_junction() or not WORK.resolve().is_relative_to(TARGET.resolve()):
            raise ValueError(f'Pasta de trabalho inesperada: {WORK}')
    WORK.mkdir(parents=True, exist_ok=True)
    # Keep host objects; source preparation is immutable and verified before reuse.
    source_inputs = [ROOT / 'runtime/prepare.py', PROFILE, ROOT / 'upstream/manifests/smm2recomp.json',
                     *sorted(path for path in (ROOT / 'games/mk8d').rglob('*') if path.is_file())]
    source_key = hashlib.sha256(b''.join(str(path.relative_to(ROOT)).encode() + b'\0' + path.read_bytes()
                                      for path in source_inputs)).hexdigest()[:16]
    source = WORK / ('runtime-' + source_key)
    receipt = WORK / ('runtime-' + source_key + '.json')
    if source.exists():
        if source.is_symlink() or source.is_junction():
            raise ValueError(f'Link na fonte preparada: {source}')
        recorded = json.loads(receipt.read_text(encoding='utf-8'))
        paths = list(source.rglob('*'))
        if any(path.is_symlink() or path.is_junction() for path in paths) or \
                {path.relative_to(source).as_posix() for path in paths if path.is_file()} != set(recorded):
            raise ValueError('Conjunto de fontes preparadas alterado')
        for name, expected in recorded.items():
            path = source / name
            if path.is_symlink() or not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
                raise ValueError(f'Fonte preparada alterada: {path}')
    # Same layout as a build job: <job>/runtime (prepared) and <job>/host-optimized.
    else:
        subprocess.run([sys.executable, ROOT / 'runtime/prepare.py', '--upstream', ROOT / 'upstream/smm2recomp',
                        '--output', source, '--profile', PROFILE], check=True)
        receipt.write_text(json.dumps({path.relative_to(source).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                                      for path in source.rglob('*') if path.is_file()}, sort_keys=True), encoding='utf-8')
    output = WORK / 'host-optimized'
    build_host(source, output)
    recorded = json.loads((output / 'host-key.json').read_text(encoding='utf-8'))
    PREBUILT.mkdir(parents=True, exist_ok=True)
    for name in HOST_FILES:
        shutil.copyfile(output / name, PREBUILT / name)
    recorded['profile'] = PROFILE.name
    (PREBUILT / 'manifest.json').write_text(json.dumps(recorded, indent=2), encoding='utf-8')
    print(f'Runtime pré-compilado: {PREBUILT} (chave {recorded["key"][:16]}…)', flush=True)


if __name__ == '__main__':
    main()
