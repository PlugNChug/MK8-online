"""The swapchain acquire is bounded by MK8_ACQUIRE_MS (toolchain/acquire_bound.py).

prepare_host_sources applies the patch; mk8_acquire_timeout_ns is extracted and run in a native harness with different
environments, so the static caching inside the function is exercised as in the runtime.
"""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from host import prepare_host_sources

HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
/*ACQ*/
int main(void) {
    printf("%llu\n", (unsigned long long)mk8_acquire_timeout_ns());
    return 0;
}
'''


class AcquireBoundTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cc'), 'Native C compiler required')
    def test_timeout_follows_the_environment(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', out/'host')
            text = (runtime/'nvn/vulkan/device.c').read_text(encoding='utf-8')
            self.assertEqual(text.count('static uint64_t mk8_acquire_timeout_ns'), 1)
            start = text.index('static uint64_t mk8_acquire_timeout_ns')
            end = text.index('\n}\n', start) + 3
            (out/'harness.c').write_text(HARNESS.replace('/*ACQ*/', text[start:end]), encoding='utf-8')
            exe = out/'harness'
            subprocess.run(['cc', '-std=gnu11', '-O1', str(out/'harness.c'), '-o', str(exe)],
                           check=True, capture_output=True)

            def timeout(cap=None):
                env = dict(os.environ)
                env.pop('MK8_ACQUIRE_MS', None)
                if cap is not None:
                    env['MK8_ACQUIRE_MS'] = str(cap)
                result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60, env=env)
                self.assertEqual(result.returncode, 0, result.stderr)
                return int(result.stdout.strip())

            self.assertEqual(timeout(), 100 * 1000 * 1000)          # (default 100 ms)
            self.assertEqual(timeout(0), 2**64 - 1)                 # (0: unbounded)
            self.assertEqual(timeout(250), 250 * 1000 * 1000)
            self.assertEqual(timeout(1), 1000 * 1000)


if __name__ == '__main__':
    unittest.main()
