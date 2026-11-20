"""Real launcher startup without a window, using isolated folders and no game assets."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest

EXE = Path(os.environ.get('MK8_LAUNCHER_EXE', 'target/debug/Launcher.exe'))


@unittest.skipUnless(EXE.is_file(), 'Set MK8_LAUNCHER_EXE to a built launcher')
class LauncherStartupTests(unittest.TestCase):
    def test_direct_play_fails_without_prepared_game_and_never_starts_server(self):
        with tempfile.TemporaryDirectory(prefix='mk8-direct-') as folder:
            exe = Path(folder) / 'Launcher.exe'
            shutil.copy2(EXE, exe)
            (Path(folder) / 'portable.txt').touch()
            result = subprocess.run([exe, '--play', '--data-dir', folder],
                                    capture_output=True, text=True, timeout=90)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse((Path(folder) / 'launcher.json').exists())
            self.assertFalse((Path(folder) / 'candidate.json').exists())

    def test_portable_marker_and_explicit_data_override(self):
        with tempfile.TemporaryDirectory(prefix='mk8 portable ') as folder:
            base = Path(folder)
            exe = base / 'Launcher.exe'
            shutil.copy2(EXE, exe)
            (base / 'portable.txt').touch()
            # A marker takes precedence over any installed-home metadata.
            (base / 'mk8-online.json').write_text('{"home":"not an absolute path"}')
            for override in (False, True):
                local = base / 'explicit data' if override else base / 'portable/data'
                args = [exe, '--headless']
                if override:
                    args += ['--data-dir', local]
                child = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                try:
                    deadline = time.monotonic() + 90
                    while not (local / 'launcher.json').is_file():
                        self.assertIsNone(child.poll(), 'Launcher exited before publishing its server')
                        self.assertLess(time.monotonic(), deadline)
                        time.sleep(.1)
                    receipt = json.loads((local / 'launcher.json').read_text())
                    self.assertEqual(receipt['pid'], child.pid)
                finally:
                    child.terminate()
                    child.wait(timeout=15)


if __name__ == '__main__':
    unittest.main()
