"""Exercise the PowerShell bootstrap with a verified private CPython archive."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
ARCHIVE = Path(os.environ.get('MK8_TEST_PYTHON_ARCHIVE', ROOT / 'local/python-bootstrap-check/python.3.12.10.zip'))


@unittest.skipUnless(os.name == 'nt' and ARCHIVE.is_file(), 'Windows and private CPython test archive required')
class PythonSetupTests(unittest.TestCase):
    def test_download_repair_reuse_and_hash_failure(self):
        with tempfile.TemporaryDirectory(prefix='mk8-python-') as folder:
            root = Path(folder)
            scripts = root / 'scripts'
            scripts.mkdir()
            for name in ('setup-tools.ps1', 'setup-python.ps1'):
                shutil.copyfile(ROOT / 'scripts' / name, scripts / name)
            for name in ('upstream/smm2recomp/recomp/tools/requirements-gen.txt', 'toolchain/requirements-host.txt'):
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('')
            environment = root / 'local/python'
            environment.mkdir(parents=True)
            (environment / 'user-marker').write_text('preserve')
            # A broken/wrong environment must be backed up, not reused or deleted.
            (environment / 'pyvenv.cfg').write_text('version = 3.13.0')
            def run(download):
                command = download + "\n& '" + str(scripts / 'setup-tools.ps1').replace("'", "''") + "'"
                return subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive', '-Command', command],
                    env=dict(os.environ, PIP_NO_INDEX='1', PIP_DISABLE_PIP_VERSION_CHECK='1'),
                    capture_output=True, text=True, timeout=180)
            bad = run("function Invoke-WebRequest { param([switch]$UseBasicParsing, $Uri, $OutFile); Set-Content -LiteralPath $OutFile -Value bad }")
            self.assertNotEqual(bad.returncode, 0)
            self.assertIn('SHA-256 mismatch', bad.stderr)
            self.assertTrue((environment / 'user-marker').exists())
            archive = str(ARCHIVE.resolve()).replace("'", "''")
            good = run("function Invoke-WebRequest { param([switch]$UseBasicParsing, $Uri, $OutFile); Copy-Item -LiteralPath '" + archive + "' -Destination $OutFile }")
            self.assertEqual(good.returncode, 0, good.stdout + good.stderr)
            self.assertEqual(len(list((root / 'local').glob('python-backup-*/user-marker'))), 1)
            python = environment / 'Scripts/python.exe'
            version = subprocess.check_output([python, '-c', 'import sys; print(sys.version_info[:2])'], text=True)
            self.assertIn('(3, 12)', version)
            before = python.stat().st_mtime_ns
            reused = run('function Invoke-WebRequest { throw "Unexpected download" }')
            self.assertEqual(reused.returncode, 0, reused.stdout + reused.stderr)
            self.assertEqual(before, python.stat().st_mtime_ns)
            custom = environment / 'Lib/site-packages/sitecustomize.py'
            custom.write_text('import sys; sys.version_info = (3, 13, 0, "final", 0)')
            repaired = run('function Invoke-WebRequest { throw "Must reuse private CPython" }')
            self.assertEqual(repaired.returncode, 0, repaired.stdout + repaired.stderr)
            self.assertFalse(custom.exists())
            self.assertEqual(len(list((root / 'local').glob('python-backup-*'))), 2)


if __name__ == '__main__':
    unittest.main()

