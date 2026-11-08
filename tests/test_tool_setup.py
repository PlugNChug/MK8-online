"""The Tools step keeps only what the build reads, never deletes through a link, and reuses checked DLLs offline."""
import hashlib
import io
import json
import os
from pathlib import Path
import runpy
import subprocess
import sys
import tarfile
import tempfile
import unittest

import zstandard

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))  # (downloads.py, imported by the setup scripts)


def junction(link, target):
    made = subprocess.run(['cmd', '/d', '/c', 'mklink', '/J', str(link), str(target)], capture_output=True)
    if made.returncode:
        raise unittest.SkipTest('mklink /J indisponível')


def touch(path, data=b'x'):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return path


@unittest.skipUnless(os.name == 'nt', 'Windows tools')
class PruneTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory(prefix='mk8-tools-')
        self.root = Path(self.folder.name)
        self.outside = touch(self.root / 'user/keep.txt').parent

    def tearDown(self):
        self.folder.cleanup()

    def test_zig_keeps_the_windows_c_library_only(self):
        prune = runpy.run_path(str(ROOT / 'scripts/setup-native.py'))['prune']
        zig = self.root / 'zig'
        kept = [touch(zig / 'zig.exe'), touch(zig / 'lib/libc/mingw/crt/crtexe.c'),
                touch(zig / 'lib/libc/include/any-windows-any/windows.h'), touch(zig / 'lib/libcxx/include/vector'),
                touch(zig / 'lib/std/std.zig')]
        gone = [touch(zig / 'lib/libc/glibc/abilists', b'1' * 100), touch(zig / 'lib/libc/musl/src/a.c'),
                touch(zig / 'lib/libc/include/x86_64-linux-gnu/bits/a.h'), touch(zig / 'lib/libc/include/any-darwin-any/b.h')]
        junction(zig / 'lib/libc/include/linked-elsewhere', self.outside)
        self.assertGreaterEqual(prune(zig), 100)
        self.assertTrue(all(p.is_file() for p in kept))
        self.assertFalse(any(p.exists() for p in gone))
        self.assertTrue((self.outside / 'keep.txt').is_file())  # (the link is left alone, never followed)
        self.assertEqual(prune(zig), 0)

    def test_graphics_packages_keep_c_headers_import_libraries_and_dlls(self):
        prune = runpy.run_path(str(ROOT / 'scripts/setup-runtime-deps.py'))['prune']
        ucrt64 = self.root / 'ucrt64'
        kept = [touch(ucrt64 / 'include/vulkan/vulkan.h'), touch(ucrt64 / 'include/vulkan/vulkan_core.h'),
                touch(ucrt64 / 'include/SDL3/SDL.h'), touch(ucrt64 / 'lib/libSDL3.dll.a'),
                touch(ucrt64 / 'lib/libshaderc_shared.dll.a'), touch(ucrt64 / 'bin/SDL3.dll'),
                touch(ucrt64 / 'bin/libshaderc_shared.dll')]
        gone = [touch(ucrt64 / 'include/vulkan/vulkan.hpp'), touch(ucrt64 / 'include/vulkan/vulkan_raii.hpp'),
                touch(ucrt64 / 'share/vulkan/registry/vk.xml'), touch(ucrt64 / 'bin/glslc.exe'),
                touch(ucrt64 / 'lib/libSDL3.a'), touch(ucrt64 / 'lib/libshaderc_combined.a')]
        self.assertGreater(prune(ucrt64), 0)
        self.assertTrue(all(p.is_file() for p in kept))
        self.assertFalse(any(p.exists() for p in gone))
        self.assertEqual(prune(ucrt64), 0)


def package(members):
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode='w') as bundle:
        for name, data in members.items():
            info = tarfile.TarInfo(name)
            info.size = len(data)
            bundle.addfile(info, io.BytesIO(data))
    return zstandard.ZstdCompressor().compress(raw.getvalue())


@unittest.skipUnless(os.name == 'nt', 'Windows tools')
class DllCacheTests(unittest.TestCase):
    NAME = 'mingw-w64-ucrt-x86_64-libdemo-1.0-1-any.pkg.tar.zst'

    def setUp(self):
        self.folder = tempfile.TemporaryDirectory(prefix='mk8-dlls-')
        self.tools = Path(self.folder.name)
        previous = os.environ.get('MK8_NATIVE_TOOLS')
        os.environ['MK8_NATIVE_TOOLS'] = str(self.tools)
        try:
            self.script = runpy.run_path(str(ROOT / 'scripts/runtime-dlls.py'))
        finally:
            if previous is None:
                del os.environ['MK8_NATIVE_TOOLS']
            else:
                os.environ['MK8_NATIVE_TOOLS'] = previous
        self.data = package({'ucrt64/bin/libdemo-1.dll': b'MZ demo', 'ucrt64/bin/libother-2.dll': b'MZ other',
                             'ucrt64/share/doc/readme': b'not a dll', 'ucrt64/bin/sub/evil.dll': b'nested'})
        self.sha = hashlib.sha256(self.data).hexdigest()
        self.archive = touch(self.tools / 'deps' / self.NAME, self.data)

    def tearDown(self):
        self.folder.cleanup()

    def test_only_the_imported_dll_is_kept_from_a_checked_archive(self):
        cache = {}
        self.script['unpack'](self.NAME, self.sha, 'LIBDEMO-1.dll', cache)
        dlls = self.tools / 'deps/dlls'
        self.assertTrue(self.archive.exists())  # (resolve deletes it once all its DLLs were taken)
        self.assertEqual(sorted(p.name for p in dlls.iterdir()), ['index.json', 'libdemo-1.dll'])
        self.assertEqual(json.loads((dlls / 'index.json').read_text(encoding='utf-8')), cache)
        cached = self.script['cached']
        self.assertEqual(cached(cache, 'LIBDEMO-1.dll', self.NAME, self.sha), dlls / 'libdemo-1.dll')
        self.assertIsNone(cached(cache, 'libother-2.dll', self.NAME, self.sha))
        # A newer package in the repository index, or a changed file, is not served from the cache.
        self.assertIsNone(cached(cache, 'libdemo-1.dll', self.NAME, '0' * 64))
        self.assertIsNone(cached(cache, 'libdemo-1.dll', self.NAME.replace('1.0-1', '1.0-2'), self.sha))
        (dlls / 'libdemo-1.dll').write_bytes(b'MZ changed')
        self.assertIsNone(cached(cache, 'libdemo-1.dll', self.NAME, self.sha))

    def test_archive_with_another_hash_is_refused_and_not_used(self):
        cache = {}
        with self.assertRaisesRegex(ValueError, 'hash mismatch'):
            self.script['unpack'](self.NAME, '0' * 64, 'libdemo-1.dll', cache)
        self.assertEqual(cache, {})
        self.assertFalse((self.tools / 'deps/dlls/libdemo-1.dll').exists())


if __name__ == '__main__':
    unittest.main()
