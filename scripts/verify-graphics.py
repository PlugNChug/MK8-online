"""Run native graphics regression tests without game data; prerequisites and skips are checked."""
import hashlib
import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
sys.path.insert(0, str(ROOT/'tests'))
from host import DEPS
from native import DEFAULT_COMPILER

MODULES = [
    'test_descriptor_lifetime', 'test_texture_lifetime', 'test_texture_pool',
    'test_texture_memo', 'test_texture_init', 'test_pool_release', 'test_shader_budget',
    'test_runtime_stalls', 'test_compute_flush', 'test_gpu_sync', 'test_async_submit',
    'test_shader_storage', 'test_frame_stats',
]


def main():
    missing = [p for p in (DEFAULT_COMPILER, DEPS/'include/SDL3/SDL.h',
                           DEPS/'include/vulkan/vulkan.h') if not p.is_file()]
    if missing:
        raise SystemExit('Native prerequisites missing. Run scripts/setup-tools.ps1, then scripts/setup-native.py '
                         'and scripts/setup-runtime-deps.py with local/python/Scripts/python.exe:\n' +
                         '\n'.join(str(p) for p in missing))
    manifest = json.loads((ROOT/'upstream/manifests/smm2recomp.json').read_text(encoding='utf-8'))
    for relative, expected in manifest['files'].items():
        path = ROOT/'upstream/smm2recomp'/relative
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise SystemExit(f'Upstream absent or changed: {relative}. Run scripts/fetch-upstream.py and inspect errors.')
    result = unittest.TextTestRunner(verbosity=2).run(unittest.TestLoader().loadTestsFromNames(MODULES))
    if result.skipped:
        print('Incomplete verification: skipped native tests are not accepted.', file=sys.stderr)
    return 0 if result.wasSuccessful() and not result.skipped else 1


if __name__ == '__main__':
    sys.exit(main())
