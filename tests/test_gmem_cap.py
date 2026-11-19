"""The heap-import cap bounds what MK8_GMEM_MB imports (toolchain/gmem_cap.py).

prepare_host_sources applies the patch; mk8_gmem_cap is extracted and compiled in a native harness, then run with
different environments so the static caching inside the function is exercised exactly as in the runtime.
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
import gmem_cap

HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
/*CAP*/
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    uint64_t size = (uint64_t)atoll(argv[1]) << 20;
    printf("%llu\n", (unsigned long long)(mk8_gmem_cap(size) >> 20));
    return 0;
}
'''


class GmemCapTests(unittest.TestCase):
    def test_boundaries_are_checked(self):
        with tempfile.TemporaryDirectory() as folder:
            vulkan = Path(folder)/'nvn/vulkan'
            vulkan.mkdir(parents=True)
            (vulkan/'exec.c').write_text('int x;\n', encoding='utf-8')
            with self.assertRaises(ValueError):
                gmem_cap.prepare(Path(folder))

    @unittest.skipUnless(shutil.which('cc'), 'Native C compiler required')
    def test_cap_follows_the_environment(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', out/'host')
            text = (runtime/'nvn/vulkan/exec.c').read_text(encoding='utf-8')
            self.assertEqual(text.count('static uint64_t mk8_gmem_cap'), 1)
            self.assertEqual(text.count('gmem_want'), 3)  # (definition, import, gmem_info)
            start = text.index('static uint64_t mk8_gmem_cap')
            end = text.index('\n}\n', start) + 3
            harness = out/'harness.c'
            harness.write_text(HARNESS.replace('/*CAP*/', text[start:end]), encoding='utf-8')
            exe = out/'harness'
            subprocess.run(['cc', '-std=gnu11', '-O1', str(harness), '-o', str(exe)], check=True, capture_output=True)

            def cap_of(size_mb, cap_env=None):
                env = dict(os.environ)
                env.pop('MK8_GMEM_MB', None)
                if cap_env is not None:
                    env['MK8_GMEM_MB'] = str(cap_env)
                result = subprocess.run([str(exe), str(size_mb)], capture_output=True, text=True, timeout=60, env=env)
                self.assertEqual(result.returncode, 0, result.stderr)
                return int(result.stdout.strip())

            self.assertEqual(cap_of(3100), gmem_cap.DEFAULT_MB)
            self.assertEqual(cap_of(100), 100)
            self.assertEqual(cap_of(3100, 512), 512)
            self.assertEqual(cap_of(3100, 0), 3100)   # (0: the whole heap, the previous behaviour)
            self.assertEqual(cap_of(100, 1024), 100)
            self.assertEqual(cap_of(3100, 1024), 1024)


if __name__ == '__main__':
    unittest.main()
