"""Exercise the PowerShell bootstrap with a verified private CPython archive."""
import os
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import zipfile

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
            wheel = root / 'mk8_probe-1.0-py3-none-any.whl'
            with zipfile.ZipFile(wheel, 'w') as archive:
                archive.writestr('mk8_probe.py', 'VALUE = 42\n')
                archive.writestr('mk8_probe-1.0.dist-info/METADATA', 'Metadata-Version: 2.1\nName: mk8-probe\nVersion: 1.0\n')
                archive.writestr('mk8_probe-1.0.dist-info/WHEEL', 'Wheel-Version: 1.0\nRoot-Is-Purelib: true\nTag: py3-none-any\n')
                archive.writestr('mk8_probe-1.0.dist-info/RECORD', '')
            requirement = root / 'upstream/smm2recomp/recomp/tools/requirements-gen.txt'
            requirement.write_text('--no-index\n--find-links ' + root.as_uri() + '\nmk8-probe==1.0 --hash=sha256:'
                                   + hashlib.sha256(wheel.read_bytes()).hexdigest() + '\n')
            config = root / 'bad-pip.ini'
            config.write_text('[global]\nno-binary = :all:\nindex-url = https://invalid.invalid/simple\n')
            environment = root / 'local/python'
            environment.mkdir(parents=True)
            (environment / 'user-marker').write_text('preserve')
            # A broken/wrong environment must be backed up, not reused or deleted.
            (environment / 'pyvenv.cfg').write_text('version = 3.13.0')
            def run(download):
                command = download + "\n& '" + str(scripts / 'setup-tools.ps1').replace("'", "''") + "'"
                return subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive', '-Command', command],
                    env=dict(os.environ, PIP_NO_BINARY=':all:', PIP_CONFIG_FILE=str(config),
                             PIP_INDEX_URL='https://invalid.invalid/simple', PYTHONHOME='Z:/missing-python'),
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
            self.assertEqual(subprocess.check_output([python, '-c', 'import mk8_probe; print(mk8_probe.VALUE)'], text=True).strip(), '42')
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

