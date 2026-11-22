"""Exercise the native Linux linker, real loader ABI, and MK8 dispatch tables."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_pipeline import ROOT, adapter, generator, synthetic_nso
from host import build_host, link_game
from native import compile_modules


@unittest.skipUnless(os.name == 'posix' and shutil.which('clang'), 'Requires Linux native tools')
class LinuxElfTests(unittest.TestCase):
    def test_link_and_load_synthetic_modules(self):
        import json

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root/'runtime'
            exefs = root/'exefs'
            exefs.mkdir()
            profile = json.loads((ROOT/'games/mk8d/profiles/development.json').read_text())
            adapter.prepare(ROOT/'upstream/smm2recomp', source)
            for name in profile['modules']:
                synthetic_nso(exefs/name)
            generator.generate(source, exefs, root/'generated', profile)
            result = compile_modules(source, root/'generated', root/'native', profile['modules'],
                                     cache=root/'cache')
            output = root/'host'
            host = build_host(source, output)
            library = link_game(result['archive'], output, source, result['modules'])
            loaded = subprocess.run([host, '--check-game-library', library], check=True,
                                    capture_output=True, text=True)
            self.assertIn('[gamelib]', loaded.stdout + loaded.stderr)
            bad = output/'invalid.so'
            bad.write_bytes(b'invalid game library')
            failed = subprocess.run([host, '--check-game-library', bad], capture_output=True, text=True)
            self.assertNotEqual(failed.returncode, 0)
            self.assertIn('Game library:', failed.stderr)
            before = library.stat().st_mtime_ns
            rebuilt = build_host(source, output)
            subprocess.run([rebuilt, '--check-game-library', library], check=True)
            self.assertEqual(library.stat().st_mtime_ns, before)


if __name__ == '__main__':
    unittest.main()
