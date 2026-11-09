"""Real independent processes contend for the same preparation directory."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'toolchain'))
from build_lock import exclusive_build


class BuildLockTests(unittest.TestCase):
    def test_contention_and_release_after_process_exit(self):
        with tempfile.TemporaryDirectory(prefix='mk8-lock-') as temporary:
            folder = Path(temporary)
            protected = folder / 'config.json'
            protected.write_bytes(b'preserve')
            script = 'from build_lock import exclusive_build; import sys,os\nwith exclusive_build(sys.argv[1]):\n print("locked", flush=True)\n sys.stdin.readline()\n os._exit(9)'
            child = subprocess.Popen([sys.executable, '-c', script, str(folder)],
                                     cwd=ROOT/'toolchain', stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
            try:
                self.assertEqual(child.stdout.readline().strip(), 'locked')
                with self.assertRaisesRegex(RuntimeError, 'preparação'):
                    with exclusive_build(folder):
                        self.fail('Concurrent preparation entered')
                with exclusive_build(folder/'other'):
                    pass
            finally:
                child.stdin.write('exit\n')
                child.stdin.close()
                child.wait(timeout=10)
                child.stdout.close()
            with exclusive_build(folder):
                self.assertEqual(protected.read_bytes(), b'preserve')
            with self.assertRaises(ValueError):
                with exclusive_build(folder):
                    raise ValueError('failed build')
            with exclusive_build(folder):
                pass
