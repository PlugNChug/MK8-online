"""Sharing violations and unavailable sources must never trigger destructive recovery."""
import errno
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
import build
import import_mk8
from windows_file_lock import locked_file


@unittest.skipUnless(os.name == 'nt', 'Windows sharing violations')
class FileAccessTests(unittest.TestCase):
    def test_device_disappearing_during_copy_preserves_destination(self):
        with tempfile.TemporaryDirectory(prefix='mk8-access-') as temporary:
            root = Path(temporary)
            src, dst = root/'source', root/'destination'
            src.mkdir(); dst.mkdir()
            (src/'file.bin').write_bytes(b'new bytes')
            (dst/'file.bin').write_bytes(b'previous bytes')
            (dst/'stale.bin').write_bytes(b'keep until success')
            with patch.object(build.os, 'link', side_effect=OSError(errno.ENODEV, 'device disconnected')):
                with self.assertRaises(OSError) as caught:
                    build.mirror(dst, src, copy_ok=True)
            self.assertEqual(caught.exception.errno, errno.ENODEV)
            self.assertEqual((dst/'file.bin').read_bytes(), b'previous bytes')
            self.assertTrue((dst/'stale.bin').exists())

    def test_locked_runtime_receipt_does_not_trigger_deletion(self):
        with tempfile.TemporaryDirectory(prefix='mk8-access-') as temporary:
            job = Path(temporary)
            runtime = job/'runtime'
            runtime.mkdir()
            (runtime/'MK8-ADOPTION.json').write_text('{}')
            (runtime/'keep.c').write_bytes(b'keep runtime')
            receipt = job/'runtime-hashes.json'
            receipt.write_text('{}')
            with locked_file(receipt, deny_read=True), patch.object(build, 'remove_owned') as remove:
                with self.assertRaises(PermissionError):
                    build.prepare_runtime(job, ROOT/'upstream/smm2recomp', ROOT/'games/mk8d/profiles/development.json')
                remove.assert_not_called()
            self.assertEqual((runtime/'keep.c').read_bytes(), b'keep runtime')

    def test_locked_import_is_not_treated_as_corrupt(self):
        with tempfile.TemporaryDirectory(prefix='mk8-access-') as temporary:
            local = Path(temporary)
            target = local/'imports/mk8-4.0.1/exefs'
            target.mkdir(parents=True)
            npdm = target/'main.npdm'
            npdm.write_bytes(b'fixture')
            config = {}
            for key in ('base_package', 'update_package', 'prod_keys'):
                path = local/key
                path.write_bytes(b'fixture')
                config[key] = str(path)
            with locked_file(npdm, deny_read=True), patch.object(import_mk8, 'remove_owned') as remove:
                with self.assertRaises(PermissionError):
                    import_mk8.prepare(config, local, lambda path: None)
                remove.assert_not_called()
            self.assertEqual(npdm.read_bytes(), b'fixture')

    def test_locked_destination_survives_until_a_successful_retry(self):
        with tempfile.TemporaryDirectory(prefix='mk8-access-') as temporary:
            root = Path(temporary)
            src, dst = root/'source', root/'destination'
            src.mkdir(); dst.mkdir()
            (src/'file.bin').write_bytes(b'new bytes')
            (dst/'file.bin').write_bytes(b'previous bytes')
            (dst/'stale.bin').write_bytes(b'keep until success')
            with locked_file(dst/'file.bin'):
                with self.assertRaises(PermissionError):
                    build.mirror(dst, src, copy_ok=True)
                self.assertEqual((dst/'file.bin').read_bytes(), b'previous bytes')
                self.assertTrue((dst/'stale.bin').exists())
            build.mirror(dst, src, copy_ok=True)
            self.assertEqual((dst/'file.bin').read_bytes(), b'new bytes')
            self.assertFalse((dst/'stale.bin').exists())

    def test_missing_or_unreadable_source_never_prunes_destination(self):
        with tempfile.TemporaryDirectory(prefix='mk8-access-') as temporary:
            root = Path(temporary)
            src, dst = root/'source', root/'destination'
            src.mkdir(); dst.mkdir()
            keep = dst/'keep.bin'
            keep.write_bytes(b'preserve')
            with self.assertRaises(FileNotFoundError):
                build.mirror(dst, root/'disconnected')
            def denied(*args, **kwargs):
                kwargs['onerror'](PermissionError(errno.EACCES, 'denied', str(src)))
                return iter(())
            with patch.object(build.os, 'walk', side_effect=denied):
                with self.assertRaises(PermissionError):
                    build.mirror(dst, src)
            self.assertEqual(keep.read_bytes(), b'preserve')
