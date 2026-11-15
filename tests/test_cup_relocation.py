"""MK8 4.0.1's cups in a larger area (toolchain/cup_relocation.py, runtime/mk8/support/cups.c): the generated-code
patch (synthetic units and, when a build is present, the verified ones) and the runtime's copy and entry lookup."""
from collections import Counter
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from native import DEFAULT_COMPILER, compiler_env
import cup_relocation as C

GENERATED = next(iter(sorted((ROOT/'local/builds').glob('*/generated/main/main_081.c'))), None)

READER = '''void f_7000aaaa00(Cpu *c) {
    FN_ENTER(0x7000aaaa00ull);
    (void)(subs32(c, (uint32_t)c->x[9], 0x19ull));
    c->x[9] = (uint64_t)((COND_CC(c)) ? c->x[9] : (uint64_t)(0ull));
    c->x[9] = (uint64_t)(c->x[21] + (c->x[9] << 7));
    (void)(subs32(c, (uint32_t)c->x[10], 0x19ull));
L_7000aaaa20:;
    c->x[10] = (uint64_t)((COND_CC(c)) ? c->x[10] : (uint64_t)(0ull));
    (void)(subs32(c, (uint32_t)c->x[8], (uint32_t)((uint32_t)c->x[9])));
    c->x[10] = (uint64_t)(c->x[0] + (c->x[10] << 7));
    (void)(subs32(c, (uint32_t)c->x[3], 0x29ull));
    return;
}
'''
BUILDER = '''void f_70003f96b0(Cpu *c) {
    FN_ENTER(0x70003f96b0ull);
    { uint64_t ea = c->x[0] + 0x1430ull; ST32(ea, 0u); }
    return;
}
'''


class CupPatchTests(unittest.TestCase):
    def setUp(self):
        self.units = dict(C.UNITS)
        self.addCleanup(lambda: C.UNITS.update(self.units) or [C.UNITS.pop(k) for k in list(C.UNITS) if k not in self.units])

    def test_reader_clamps_widen_and_entries_redirect(self):
        C.UNITS['main_test.c'] = ('', {'7000aaaa00': 2})
        out = C.patch('#include "cpu.h"\n' + READER, 'main_test.c')
        self.assertEqual(out.count('0x29ull));'), 3)  # (two widened, one that was 0x29 already)
        self.assertNotIn('0x19ull', out)
        self.assertIn('c->x[9] = (uint64_t)(mk8_cup_entry(c->x[21], c->x[9]));', out)
        self.assertIn('c->x[10] = (uint64_t)(mk8_cup_entry(c->x[0], c->x[10]));', out)
        self.assertIn('(void)(subs32(c, (uint32_t)c->x[8], (uint32_t)((uint32_t)c->x[9])));', out)  # (in between: kept)
        self.assertLess(out.index('uint64_t mk8_cup_entry('), out.index('void f_7000aaaa00'))
        C.UNITS['main_test.c'] = ('', {'7000aaaa00': 3})
        with self.assertRaises(ValueError):
            C.patch(READER, 'main_test.c')  # (a different number of clamps: refused)
        C.UNITS['main_test.c'] = ('', {'7000aaaa00': 2})
        with self.assertRaises(ValueError):  # (a clamp whose entry computation is not there: refused)
            C.patch(READER.replace('c->x[9] = (uint64_t)(c->x[21] + (c->x[9] << 7));', 'c->x[9] = 0;'), 'main_test.c')
        with self.assertRaises(ValueError):
            C.patch(C.patch(READER, 'main_test.c'), 'main_test.c')  # (applied once only)

    def test_builder_hands_its_object_over_at_its_return(self):
        out = C.patch(BUILDER, 'main_081.c')
        self.assertLess(out.index('const uint64_t mk8_cups_object = c->x[0];'), out.index('ST32(ea, 0u)'))
        self.assertIn('    mk8_cups_built(mk8_cups_object);\n    return;\n}', out)
        with self.assertRaises(ValueError):
            C.patch(BUILDER.replace('    return;\n', '    if (c->x[0]) return;\n    return;\n'), 'main_081.c')

    @unittest.skipUnless(GENERATED, 'A build of MK8 4.0.1 is required')
    def test_verified_units(self):
        folder = GENERATED.parent
        sites = 0
        for unit in C.UNITS:
            text = (folder/unit).read_bytes().decode('utf-8').replace('\r\n', '\n')
            out = C.patch(text, unit)
            sites += out.count('mk8_cup_entry(c->x')
            selects = lambda t: Counter(l for l in t.split('\n') if 'COND_CC(c)) ?' in l)
            self.assertEqual(selects(text), selects(out))
            for match in C._functions(out):
                if match.group(1) in C.UNITS[unit][1]:
                    self.assertNotIn('0x19ull', match.group(2))
        self.assertEqual(sites, 42)
        with tempfile.TemporaryDirectory() as tmp:  # (main_082.c as the build passes it: after the Mii dialog hook)
            from experimental import patch_no_mii_unit
            out = C.patch_unit(patch_no_mii_unit(folder/'main_082.c', Path(tmp)), Path(tmp)).read_text(encoding='utf-8')
            self.assertEqual(out.count('mk8_cup_entry(c->x'), 24)
            self.assertIn('if (mk8_no_mii_renderer() && (uint32_t)c->x[8] == 0x21)', out)
        with tempfile.TemporaryDirectory() as tmp:
            changed = Path(tmp)/'main'/'main_100.c'
            changed.parent.mkdir()
            changed.write_bytes((folder/'main_100.c').read_bytes() + b'\n')
            with self.assertRaises(ValueError):
                C.patch_unit(changed, Path(tmp)/'out')
            other = Path(tmp)/'main'/'main_101.c'
            other.write_text('x')
            self.assertEqual(C.patch_unit(other, Path(tmp)/'out'), other)


RT_H = '#include <stdint.h>\n#define LOG(...) ((void)0)\n'
HARNESS = r'''
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
void mk8_cups_built(uint64_t object);
uint64_t mk8_cup_entry(uint64_t base, uint64_t index);
static uint8_t object[0x1500];
int main(int argc, char **argv) {
    int off = argc > 1;  // (MK8_CUPS_RELOCATE=0 in the environment)
    for (int i = 0; i < 25 * 0x80; i++) object[i] = (uint8_t)i;
    uint64_t base = (uint64_t)(uintptr_t)object, other = 0x7200000000ull;
    if (mk8_cup_entry(base, 3) != base + 3 * 0x80) return 1;  // (before the builder: the original arithmetic)
    mk8_cups_built(base);
    uint8_t *cup0 = (uint8_t *)(uintptr_t)mk8_cup_entry(base, 0), *cup24 = (uint8_t *)(uintptr_t)mk8_cup_entry(base, 24);
    if (off) {
        if (cup0 != object || mk8_cup_entry(base, 30) != base) return 2;  // (off: original, with its limit of 25)
        return 0;
    }
    if (cup0 == object || memcmp(cup0, object, 25 * 0x80) || cup24 != cup0 + 24 * 0x80) return 3;
    uint8_t *cup40 = (uint8_t *)(uintptr_t)mk8_cup_entry(base, 40);
    if (cup40 != cup0 + 40 * 0x80) return 4;
    for (int k = 0; k < 8; k++)  // (an added cup starts empty: records {-1, -1, -1, 0})
        for (int b = 0; b < 16; b++)
            if (cup40[16 * k + b] != (b < 12 ? 0xff : 0)) return 5;
    if (mk8_cup_entry(base, 41) != base) return 6;  // (beyond the area: index 0 of the original, as its clamp did)
    if (mk8_cup_entry(other, 3) != other + 3 * 0x80 || mk8_cup_entry(other, 30) != other) return 7;  // (other objects)
    object[5] = 0xAB;
    mk8_cups_built(base);  // (built again: copied again)
    if (cup0[5] != 0xAB) return 8;
    return 0;
}
'''


@unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
class CupRuntimeTests(unittest.TestCase):
    def test_entries_and_switch(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            (out/'inc').mkdir()
            (out/'inc/rt.h').write_text(RT_H, encoding='utf-8')
            (out/'harness.c').write_text(HARNESS, encoding='utf-8')
            exe = out/'harness.exe'
            subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu', '-O1', '-I', str(out/'inc'),
                            str(out/'harness.c'), str(ROOT/'runtime/mk8/support/cups.c'), '-o', str(exe)],
                           env=compiler_env(), check=True, capture_output=True)
            self.assertEqual(subprocess.run([str(exe)], timeout=60).returncode, 0)
            env = dict(os.environ, MK8_CUPS_RELOCATE='0')
            self.assertEqual(subprocess.run([str(exe), 'off'], env=env, timeout=60).returncode, 0)


if __name__ == '__main__':
    unittest.main()
