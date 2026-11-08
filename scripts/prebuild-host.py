"""Build the runtime executable that the setup carries (packaging machine; no game file is read).

The runtime is the project's own code (upstream runtime, runtime/mk8, games/mk8d) prepared from the 4.0.1 profile, the
same way a build prepares it. `prebuilt/host/manifest.json` records the portable key of its inputs (toolchain/host.py
host_key) and the file hashes; a build uses these files only when its own key is identical, and compiles otherwise.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'toolchain'))
WORK = ROOT / 'target/prebuilt-host'
PROFILE = ROOT / 'games/mk8d/profiles/4.0.1.json'


def main():
    os.environ['MK8_HOST_FROM_SOURCE'] = '1'
    from host import HOST_FILES, PREBUILT, build_host
    if WORK.exists():
        if WORK.is_symlink() or WORK.is_junction() or not WORK.resolve().is_relative_to((ROOT / 'target').resolve()):
            raise ValueError(f'Pasta de trabalho inesperada: {WORK}')
        shutil.rmtree(WORK)
    WORK.mkdir(parents=True)
    # Same layout as a build job: <job>/runtime (prepared) and <job>/host-optimized.
    subprocess.run([sys.executable, ROOT / 'runtime/prepare.py', '--upstream', ROOT / 'upstream/smm2recomp',
                    '--output', WORK / 'runtime', '--profile', PROFILE], check=True)
    output = WORK / 'host-optimized'
    build_host(WORK / 'runtime', output)
    recorded = json.loads((output / 'host-key.json').read_text(encoding='utf-8'))
    PREBUILT.mkdir(parents=True, exist_ok=True)
    for name in HOST_FILES:
        shutil.copyfile(output / name, PREBUILT / name)
    recorded['profile'] = PROFILE.name
    (PREBUILT / 'manifest.json').write_text(json.dumps(recorded, indent=2), encoding='utf-8')
    print(f'Runtime pré-compilado: {PREBUILT} (chave {recorded["key"][:16]}…)', flush=True)


if __name__ == '__main__':
    main()
