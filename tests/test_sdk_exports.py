import hashlib
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'toolchain'))
import sdk_exports


class SdkExportsTests(unittest.TestCase):
    def test_sized_assembly_export_is_added_without_editing_original_table(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root/'runtime'
            nso = root/'inputs/exefs/sdk'
            nso.parent.mkdir(parents=True)
            nso.write_bytes(b'authored fixture')
            table = root/'generated/sdk/sdk_table.c'
            table.parent.mkdir(parents=True)
            original = 'void f_7004000080(Cpu *c);\n'
            table.write_text(original)
            class Module:
                base = 0x7004000000
                def __init__(self, *args, **kwargs):
                    self.nso = SimpleNamespace(exports=lambda: [
                        ('nndetailCryptoBignumFixture', 16, 1, 0x20, 4),
                        ('data', 16, 1, 0x40, 4),
                        ('nndetailCryptoBignumExisting', 16, 1, 0x80, 4)])
                def in_text(self, a): return True
                def scan(self, a, end): return {a}, set(), {}
                def emit_function(self, a, *args): return [f'void f_{a:x}(Cpu *c) {{ return; }}']
            with self.assertRaises(ValueError):
                sdk_exports.supplement(source, root/'generated', root/'out')
            old_path = sys.path[:]
            try:
                with patch.object(sdk_exports, 'SDK_HASH', hashlib.sha256(nso.read_bytes()).hexdigest()), patch.dict(sys.modules, {'gen': SimpleNamespace(Module=Module)}):
                    code, result = sdk_exports.supplement(source, root/'generated', root/'out')
                text = result.read_text()
                self.assertIn('sdk_fn_count = 2;', text)
                self.assertLess(text.index('0x7004000020ull'), text.index('0x7004000080ull'))
                self.assertNotIn('0x7004000040ull', text)
                self.assertIn('f_7004000020', code.read_text())
                self.assertEqual(table.read_text(), original)
            finally:
                sys.path[:] = old_path
