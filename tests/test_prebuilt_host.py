"""The packaged runtime executable is used only for exactly the inputs it was built from."""
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'toolchain'))
import host
from native import digest


class PrebuiltHostTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory(prefix='mk8-prebuilt-')
        self.root = Path(self.folder.name)
        self.prebuilt = self.root / 'prebuilt'
        self.prebuilt.mkdir()
        (self.prebuilt / 'mk8drecomp.exe').write_bytes(b'MZ runtime')
        (self.prebuilt / 'mk8drecomp.dll.a').write_bytes(b'!<arch>\n')
        self.key = 'a' * 64
        self.write_manifest(self.key)
        self.output = self.root / 'job/host-optimized'
        self.output.mkdir(parents=True)
        self.saved = host.PREBUILT, os.environ.pop('MK8_HOST_FROM_SOURCE', None)
        host.PREBUILT = self.prebuilt

    def tearDown(self):
        host.PREBUILT = self.saved[0]
        if self.saved[1] is not None:
            os.environ['MK8_HOST_FROM_SOURCE'] = self.saved[1]
        self.folder.cleanup()

    def write_manifest(self, key, files=None):
        files = files or {name: digest(self.prebuilt / name) for name in host.HOST_FILES}
        (self.prebuilt / 'manifest.json').write_text(json.dumps({'key': key, 'files': files}), encoding='utf-8')

    def test_matching_key_copies_the_checked_files(self):
        exe = host.use_prebuilt(self.key, self.output)
        self.assertEqual(exe, self.output / 'mk8drecomp.exe')
        self.assertEqual(exe.read_bytes(), b'MZ runtime')
        self.assertEqual((self.output / 'mk8drecomp.dll.a').read_bytes(), b'!<arch>\n')
        self.assertEqual(json.loads((self.output / 'host-key.json').read_text(encoding='utf-8'))['key'], self.key)

    def test_other_inputs_changed_file_or_forced_build_compile_here(self):
        self.assertIsNone(host.use_prebuilt('b' * 64, self.output))
        os.environ['MK8_HOST_FROM_SOURCE'] = '1'
        try:
            self.assertIsNone(host.use_prebuilt(self.key, self.output))
        finally:
            del os.environ['MK8_HOST_FROM_SOURCE']
        (self.prebuilt / 'mk8drecomp.exe').write_bytes(b'MZ changed')
        self.assertIsNone(host.use_prebuilt(self.key, self.output))
        self.assertFalse((self.output / 'mk8drecomp.exe').exists())

    def test_incomplete_or_unreadable_manifest_is_not_used(self):
        self.write_manifest(self.key, {'mk8drecomp.exe': digest(self.prebuilt / 'mk8drecomp.exe')})
        self.assertIsNone(host.use_prebuilt(self.key, self.output))
        (self.prebuilt / 'manifest.json').write_text('{', encoding='utf-8')
        self.assertIsNone(host.use_prebuilt(self.key, self.output))

    def test_key_ignores_the_build_folders_but_not_sources_or_flags(self):
        def layout(name, text='int x;'):
            job = self.root / name
            source = job / 'runtime'
            output = job / 'host-optimized'
            (source / 'runtime').mkdir(parents=True, exist_ok=True)
            (output / 'runtime').mkdir(parents=True, exist_ok=True)
            (source / 'runtime/a.c').write_text(text, encoding='utf-8')
            (output / 'runtime/b.c').write_text('int y;', encoding='utf-8')
            flags = ['-O2', '-I' + str(output / 'runtime'), '-I' + str(source / 'third_party'),
                     '-ffile-prefix-map=' + str(output) + '=host']
            sources = [source / 'runtime/a.c', output / 'runtime/b.c']
            return host.host_key({'headers': {'x.h': '1'}}, flags, sources, source, output)

        first = layout('one')
        self.assertEqual(first, layout('elsewhere/two'))
        self.assertNotEqual(first, layout('three', 'int x = 1;'))
        self.assertNotEqual(first, host.host_key({'headers': {'x.h': '2'}}, ['-O2'], [], self.root, self.root))


class PrebuiltFFmpegTests(unittest.TestCase):
    NAMES = ('avcodec-63.dll', 'avformat-63.dll', 'avutil-61.dll', 'swscale-10.dll')

    def setUp(self):
        self.folder = tempfile.TemporaryDirectory(prefix='mk8-ffmpeg-')
        root = Path(self.folder.name)
        self.prebuilt, self.deps = root / 'prebuilt', root / 'deps'
        for folder in (self.prebuilt, self.deps / 'bin'):
            folder.mkdir(parents=True)
            for name in self.NAMES:
                (folder / name).write_bytes(folder.name.encode() + name.encode())
        (self.prebuilt / 'manifest.json').write_text(json.dumps(
            {'files': {name: digest(self.prebuilt / name) for name in self.NAMES}}), encoding='utf-8')
        self.saved = host.PREBUILT_FFMPEG, host.DEPS
        host.PREBUILT_FFMPEG, host.DEPS = self.prebuilt, self.deps

    def tearDown(self):
        host.PREBUILT_FFMPEG, host.DEPS = self.saved
        self.folder.cleanup()

    def test_checked_minimal_build_is_preferred(self):
        self.assertEqual(host.ffmpeg_dlls(), [self.prebuilt / name for name in self.NAMES])

    def test_changed_or_missing_file_falls_back_to_the_pinned_build(self):
        (self.prebuilt / 'avcodec-63.dll').write_bytes(b'changed')
        self.assertEqual(host.ffmpeg_dlls(), [self.deps / 'bin' / name for name in self.NAMES])
        (self.prebuilt / 'manifest.json').unlink()
        self.assertEqual(host.ffmpeg_dlls(), [self.deps / 'bin' / name for name in self.NAMES])

    def test_no_ffmpeg_at_all_stops_the_build(self):
        (self.prebuilt / 'manifest.json').unlink()
        for name in self.NAMES:
            (self.deps / 'bin' / name).unlink()
        with self.assertRaisesRegex(ValueError, 'FFmpeg ausente'):
            host.copy_runtime_dlls(Path(self.folder.name))

    def test_minimal_ffmpeg_lets_the_tools_step_drop_the_full_one(self):
        import runpy
        sys.path.insert(0, str(ROOT / 'scripts'))
        prune = runpy.run_path(str(ROOT / 'scripts/setup-runtime-deps.py'))['prune']
        ucrt64 = Path(self.folder.name) / 'ucrt64'
        (ucrt64 / 'bin').mkdir(parents=True)
        keep = ['SDL3.dll', 'libshaderc_shared.dll', 'libiconv-2.dll', 'vulkan-1.dll']
        drop = [*self.NAMES, 'avfilter-12.dll', 'avdevice-63.dll', 'swresample-7.dll', 'ffmpeg.exe', 'ffprobe.exe']
        for name in keep + drop:
            (ucrt64 / 'bin' / name).write_bytes(b'x')
        prune(ucrt64)
        self.assertEqual(len(list((ucrt64 / 'bin').iterdir())), len(keep + drop))  # (only with the minimal build)
        prune(ucrt64, minimal_ffmpeg=True)
        self.assertEqual(sorted(p.name for p in (ucrt64 / 'bin').iterdir()), sorted(keep))


if __name__ == '__main__':
    unittest.main()
