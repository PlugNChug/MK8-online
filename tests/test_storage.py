from pathlib import Path
from types import SimpleNamespace
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'toolchain'))
from storage import GIB, require_space, describe_io_error


class StorageTests(unittest.TestCase):
    def test_io_diagnostics_distinguish_locks_permissions_and_devices(self):
        for code, expected in ((32, 'Arquivo em uso'), (33, 'Arquivo em uso'),
                               (5, 'Acesso negado'), (21, 'Unidade indisponível'),
                               (55, 'Unidade indisponível'), (1117, 'Unidade indisponível'),
                               (1167, 'Unidade indisponível')):
            with self.subTest(code=code):
                error = PermissionError('blocked') if code == 5 else OSError('blocked')
                error.winerror = code
                self.assertIn(expected, describe_io_error(error))

    def test_unavailable_volume_stops_instead_of_looping(self):
        with patch('storage.Path.exists', return_value=False), patch('storage.shutil.disk_usage') as usage:
            with self.assertRaisesRegex(ValueError, 'indisponível'):
                require_space(Path(__file__).anchor)
            usage.assert_not_called()

    def test_low_disk_stops_before_compile(self):
        with patch('storage.shutil.disk_usage', return_value=SimpleNamespace(free=3*GIB)):
            with self.assertRaisesRegex(ValueError, 'preservar espaço'):
                require_space(Path(__file__).parent/'not-created'/'objects')
        with patch('storage.shutil.disk_usage', return_value=SimpleNamespace(free=4*GIB)):
            require_space(Path(__file__).parent)
