"""Run real adapters with synthetic callers. Never marks gameplay as verified."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tests'))
sys.path.insert(0, str(ROOT/'toolchain'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cycles', type=int, default=20000, help='Profile/query cycles, 1..1000000')
    parser.add_argument('--browser', action='store_true', help='Also test real launcher with Chromium')
    args = parser.parse_args()
    if not 1 <= args.cycles <= 1000000:
        parser.error('--cycles must be 1..1000000')
    os.environ['MK8_SIMULATION_CYCLES'] = str(args.cycles)
    report = {'gameplay_verified': False, 'gpu_rendering_verified': False,
              'nextendo_verified': False, 'cycles': args.cycles, 'passed': False,
              'scope': 'Synthetic callers of native adapters, not game execution'}
    started = time.monotonic()
    output = ROOT/'local/performance-validation/simulation.json'
    output.parent.mkdir(parents=True, exist_ok=True)
    try:
        # Includes upstream integrity/prerequisite checks and rejects skipped tests.
        subprocess.run([sys.executable, str(ROOT/'scripts/verify-graphics.py')], cwd=ROOT, check=True)
        modules = ['test_local_mii', 'test_experimental']
        if args.browser:
            modules.append('test_mii_launcher')
        suite = unittest.TestLoader().loadTestsFromNames(modules)
        result = unittest.TextTestRunner(verbosity=2).run(suite)
        report.update(tests=result.testsRun, failures=len(result.failures), errors=len(result.errors),
                      skipped=len(result.skipped), passed=result.wasSuccessful() and not result.skipped)
        report['adapter_sha256'] = hashlib.sha256((ROOT/'runtime/mk8/mii/local_mii.inc').read_bytes()).hexdigest()
    except (OSError, subprocess.SubprocessError) as error:
        report['error'] = str(error)
    finally:
        report['elapsed_seconds'] = round(time.monotonic()-started, 3)
        output.write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
        print(f'Diagnostic: {output}\nGameplay/GPU/Nextendo remain unverified.')
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
