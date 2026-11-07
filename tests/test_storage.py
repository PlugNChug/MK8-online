from pathlib import Path
from types import SimpleNamespace
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'toolchain'))
from storage import GIB, require_space


class StorageTests(unittest.TestCase):
    def test_low_disk_stops_before_compile(self):
        with patch('storage.shutil.disk_usage', return_value=SimpleNamespace(free=3*GIB)):
            with self.assertRaisesRegex(ValueError, 'preservar espaço'):
                require_space(Path(__file__).parent/'not-created'/'objects')
        with patch('storage.shutil.disk_usage', return_value=SimpleNamespace(free=4*GIB)):
            require_space(Path(__file__).parent)
