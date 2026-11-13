"""Exercise the launcher's native bootstrap with verified CPython and a hashed fixture wheel."""
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
LAUNCHER = Path(os.environ.get('MK8_LAUNCHER_EXE', ROOT / 'target/debug/Launcher.exe'))


@unittest.skipUnless(os.name == 'nt' and ARCHIVE.is_file() and LAUNCHER.is_file(), 'Windows, built launcher and private CPython test archive required')
class PythonSetupTests(unittest.TestCase):
    def test_download_repair_reuse_and_hash_failure(self):
        # Keep interpreter files as private evidence; never force cleanup of files held by a scanner.
        with tempfile.TemporaryDirectory(prefix='mk8-python-', delete=False) as folder:
            root = Path(folder)
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
            def run():
                return subprocess.run([str(LAUNCHER), '--setup-python', str(root)],
                    env=dict(os.environ, PIP_NO_BINARY=':all:', PIP_CONFIG_FILE=str(config),
                             PIP_INDEX_URL='https://invalid.invalid/simple', PYTHONHOME='Z:/missing-python',
                             HTTPS_PROXY='http://127.0.0.1:1', ALL_PROXY='http://127.0.0.1:1', NO_PROXY=''),
                    capture_output=True, text=True, encoding='utf-8', timeout=180)
            cached = root/'local/python-3.12.10.zip'
            cached.write_bytes(b'corrupted archive')
            bad = run()
            self.assertNotEqual(bad.returncode, 0)
            self.assertTrue((environment / 'user-marker').exists())
            self.assertFalse(list((root/'local').glob('cpython-*')))
            shutil.copyfile(ARCHIVE,cached)
            good = run()
            self.assertEqual(good.returncode, 0, good.stdout + good.stderr)
            self.assertEqual(len(list((root / 'local').glob('python-backup-*/user-marker'))), 1)
            python = environment / 'Scripts/python.exe'
            version = subprocess.check_output([python, '-c', 'import sys; print(sys.version_info[:2])'], text=True)
            self.assertIn('(3, 12)', version)
            self.assertEqual(subprocess.check_output([python, '-c', 'import mk8_probe; print(mk8_probe.VALUE)'], text=True).strip(), '42')
            before = python.stat().st_mtime_ns
            reused = run()
            self.assertEqual(reused.returncode, 0, reused.stdout + reused.stderr)
            self.assertEqual(before, python.stat().st_mtime_ns)
            custom = environment / 'Lib/site-packages/sitecustomize.py'
            custom.write_text('import sys; sys.version_info = (3, 13, 0, "final", 0)')
            active = subprocess.Popen([str(python),'-I','-c','import time; time.sleep(120)'])
            try:
                busy = run()
                self.assertNotEqual(busy.returncode,0)
                self.assertIn('Ambiente Python em uso',busy.stderr)
                self.assertTrue(custom.exists())
            finally:
                active.terminate();active.wait(timeout=10)
            repaired = run()
            self.assertEqual(repaired.returncode, 0, repaired.stdout + repaired.stderr)
            self.assertFalse(custom.exists())
            self.assertEqual(len(list((root / 'local').glob('python-backup-*'))), 2)
            # A failed pinned dependency must fail the step, never report success.
            requirement.write_text(requirement.read_text().replace('mk8-probe==1.0','mk8-probe==2.0'))
            failed = run()
            self.assertNotEqual(failed.returncode,0)
            self.assertIn('Preparação do Python falhou',failed.stderr)

    def test_repair_refuses_junctions_and_preserves_external_files(self):
        with tempfile.TemporaryDirectory(prefix='mk8-python-link-') as folder:
            root=Path(folder)
            outside=root/'outside';outside.mkdir()
            keep=outside/'keep.txt';keep.write_text('keep')
            local=root/'local';local.mkdir()
            link=local/'python'
            made=subprocess.run(['cmd.exe','/D','/C','mklink','/J',str(link),str(outside)],capture_output=True)
            if made.returncode:self.skipTest('Junction creation unavailable')
            try:
                result=subprocess.run([str(LAUNCHER),'--setup-python',str(root)],capture_output=True,text=True,encoding='utf-8',timeout=30)
                self.assertNotEqual(result.returncode,0)
                self.assertIn('junction',result.stderr)
                self.assertEqual(keep.read_text(),'keep')
            finally:
                os.rmdir(link)  # remove only the test junction, never its target


if __name__ == '__main__':
    unittest.main()

