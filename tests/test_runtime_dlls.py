"""Regression tests for the Windows DLL preparation helpers (scripts/runtime-dlls.py).

The real Tools step reads MSYS2 packages; here a tiny archive exercises the two
shape assumptions that a fresh Windows machine hits: a packaged DLL may be a
hardlink to another member, and a cross-check SystemRoot may keep another case.
"""
import importlib.util
import io
import os
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest

import zstandard

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'scripts'))


def load_script():
    spec = importlib.util.spec_from_file_location('runtime_dlls_under_test', ROOT/'scripts/runtime-dlls.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class RuntimeDllsTest(unittest.TestCase):
    def test_packaged_hardlink_is_resolved(self):
        module = load_script()
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            package = folder/'mingw-w64-ucrt-x86_64-zlib-ng-compat-test-any.pkg.tar.zst'
            raw = folder/'package.tar'
            payload = b'DLL payload' * 4096
            with tarfile.open(raw, 'w') as archive:
                member = tarfile.TarInfo('ucrt64/bin/libz-1.dll')
                member.size = len(payload)
                archive.addfile(member, io.BytesIO(payload))
                link = tarfile.TarInfo('ucrt64/bin/zlib1.dll')
                link.type = tarfile.LNKTYPE
                link.linkname = 'ucrt64/bin/libz-1.dll'
                archive.addfile(link)
            package.write_bytes(zstandard.ZstdCompressor().compress(raw.read_bytes()))
            module.DEPS = folder
            module.CACHE = folder/'cache'
            cache = {}
            module.unpack(package.name, module.digest(package), 'zlib1.dll', cache)
            self.assertEqual((folder/'cache'/'zlib1.dll').read_bytes(), payload)
            self.assertEqual(cache['zlib1.dll']['package'], package.name)

    def test_system_root_case_on_a_case_sensitive_host(self):
        module = load_script()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root/'System32').mkdir()
            (root/'System32/kernel32.dll').write_bytes(b'')
            previous = os.environ.get('SystemRoot')
            os.environ['SystemRoot'] = str(root)
            try:
                self.assertTrue(module.system_library('kernel32.dll'))
                # Windows resolves both spellings on its own; a case-sensitive
                # cross-check host must do the same deliberately.
                self.assertTrue(module.system_library('KERNEL32.dll'))
            finally:
                if previous is None:
                    del os.environ['SystemRoot']
                else:
                    os.environ['SystemRoot'] = previous


if __name__ == '__main__':
    unittest.main()
