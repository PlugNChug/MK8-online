"""Packaged source must include the headers needed by a runtime rebuild."""
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'scripts'))
import cargo_target


class PackagePayloadTests(unittest.TestCase):
    def test_runtime_headers_and_adapters_are_packaged_without_private_data(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            required = (
                'runtime/mk8/windows.manifest',
                'runtime/mk8/windows.rc',
                'runtime/mk8/net/nextendo_ca.h',
                'runtime/mk8/ui/hub.h',
                'runtime/mk8/ui/settings_theme.inc',
                'runtime/mk8/support/settings_values.h',
                'runtime/mk8/support/settings_values.c',
                'runtime/mk8/support/settings_page.c',
                'runtime/mk8/support/ultrawide.c',
                'runtime/mk8/ui/settings_controls.inc',
                'toolchain/settings_layout.py',
                'toolchain/settings_menu.py',
                'toolchain/mii_assembly.html',
                'toolchain/ultrawide.py',
            )
            excluded = ('runtime/private.bin', 'runtime/private.sarc',
                        'local/prod.keys', 'local/save/progress.dat')
            for name in (*required, *excluded):
                path = root/name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('authored fixture', encoding='utf-8')
            manifest = root/'upstream/manifests/smm2recomp.json'
            manifest.parent.mkdir(parents=True)
            manifest.write_text('{"files": {}}', encoding='utf-8')
            ffmpeg = root/'prebuilt/ffmpeg/manifest.json'
            ffmpeg.parent.mkdir(parents=True)
            ffmpeg.write_text('{"files": {}}', encoding='utf-8')
            spec = importlib.util.spec_from_file_location('package_launcher', ROOT/'scripts/package-launcher.py')
            package = importlib.util.module_from_spec(spec)
            with patch.object(cargo_target, 'target_dir', return_value=root/'target'):
                spec.loader.exec_module(package)
            with patch.object(package, 'ROOT', root), patch.object(package, 'MANIFEST', manifest):
                selected = {path.relative_to(root).as_posix() for path in package.payload_files()}
            self.assertFalse(set(required) - selected, 'Required build sources omitted: '+repr(set(required) - selected))
            self.assertFalse(set(excluded) & selected, 'Private fixture data entered the payload')


if __name__ == '__main__':
    unittest.main()
