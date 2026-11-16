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

    def test_clamps_followed_through_labels_jumps_and_calls(self):
        reader = '''void f_7000bbbb00(Cpu *c) {
    (void)(subs32(c, (uint32_t)c->x[9], 0x19ull));
L_7000bbbb04:;
    c->x[9] = (uint64_t)((COND_CC(c)) ? c->x[9] : (uint64_t)(0ull));
    goto L_7000bbbb40;
    (void)(subs32(c, (uint32_t)c->x[9], 0x19ull));
    c->x[9] = (uint64_t)((COND_CC(c)) ? c->x[9] : (uint64_t)(0ull));
L_7000bbbb40:;
    c->x[8] = (uint64_t)(c->x[8] + (c->x[9] << 7));
    (void)(subs32(c, (uint32_t)c->x[8], 0x19ull));
    c->x[24] = (uint64_t)((COND_CC(c)) ? c->x[8] : (uint64_t)(0ull));
    c->x[30] = 0x7000bbbb58ull; f_70008bf0b0(c);
    c->x[8] = (uint64_t)(c->x[22] + (c->x[24] << 7));
    return;
}
'''
        C.UNITS['main_test.c'] = ('', {'7000bbbb00': (3, 2)})  # (two clamps meet at one computation)
        out = C.patch(reader, 'main_test.c')
        self.assertEqual(out.count('0x29ull'), 3)
        self.assertIn('c->x[8] = (uint64_t)(mk8_cup_entry(c->x[8], c->x[9]));', out)
        self.assertIn('c->x[8] = (uint64_t)(mk8_cup_entry(c->x[22], c->x[24]));', out)  # (x24 survives the call)
        clobbered = reader.replace('c->x[24]', 'c->x[10]')  # (x10 does not: refused)
        C.UNITS['main_test.c'] = ('', {'7000bbbb00': (3, 2)})
        with self.assertRaises(ValueError):
            C.patch(clobbered, 'main_test.c')

    def test_reader_clamps_widen_and_entries_redirect(self):
        C.UNITS['main_test.c'] = ('', {'7000aaaa00': (2, 2)})
        out = C.patch('#include "cpu.h"\n' + READER, 'main_test.c')
        self.assertEqual(out.count('0x29ull));'), 3)  # (two widened, one that was 0x29 already)
        self.assertNotIn('0x19ull', out)
        self.assertIn('c->x[9] = (uint64_t)(mk8_cup_entry(c->x[21], c->x[9]));', out)
        self.assertIn('c->x[10] = (uint64_t)(mk8_cup_entry(c->x[0], c->x[10]));', out)
        self.assertIn('(void)(subs32(c, (uint32_t)c->x[8], (uint32_t)((uint32_t)c->x[9])));', out)  # (in between: kept)
        self.assertLess(out.index('uint64_t mk8_cup_entry('), out.index('void f_7000aaaa00'))
        C.UNITS['main_test.c'] = ('', {'7000aaaa00': (3, 3)})
        with self.assertRaises(ValueError):
            C.patch(READER, 'main_test.c')  # (a different number of clamps: refused)
        C.UNITS['main_test.c'] = ('', {'7000aaaa00': (2, 2)})
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
            with tempfile.TemporaryDirectory() as tmp:  # (as the build passes it: some after toolchain/experimental.py)
                from experimental import patch_no_mii_unit
                built = patch_no_mii_unit(folder/unit, Path(tmp))
                text = (folder/unit).read_bytes().decode('utf-8').replace('\r\n', '\n')
                out = C.patch_unit(built, Path(tmp)).read_text(encoding='utf-8')
            sites += sum(out.count(call + '(c->x') for call in
                         ('mk8_cup_entry', 'mk8_cup_select', 'mk8_cup_by_course', 'mk8_cup_by_slot'))
            for match in C._functions(out):
                if match.group(1) in C.SELECTORS:
                    self.assertIn(C.SELECTORS[match.group(1)] + '(c->x', match.group(2))
            selects = lambda t: Counter(l for l in t.split('\n') if 'COND_CC(c)) ?' in l)
            self.assertEqual(selects(text), selects(out))
            for match in C._functions(out):
                if match.group(1) in C.UNITS[unit][1]:
                    self.assertIsNone(C.CLAMP.search(match.group(2)))  # (no clamp left at 25)
        self.assertEqual(sites, 56)
        sdk = GENERATED.parents[1]/'sdk'/'sdk_008.c'  # (nn::fs::OpenFile: the course music's path)
        with tempfile.TemporaryDirectory() as tmp:
            out = C.patch_unit(sdk, Path(tmp)).read_text(encoding='utf-8')
            self.assertEqual(out.count('    FN_ENTER(0x7004050080ull);\n    c->x[1] = mk8_stream_path(c->x[1]);'), 1)
            with self.assertRaises(ValueError):
                C.patch_sdk(out, 'sdk_008.c')
        hooks = Counter()  # (the cup screen: both button setups, both page turns, the state query)
        for unit in C.UI_UNITS:
            out = C.patch((folder/unit).read_bytes().decode('utf-8').replace('\r\n', '\n'), unit)
            for call in ('mk8_cup_page_id(c->x', 'mk8_cup_page_turn(c->x', 'mk8_cup_owned(LD32', 'mk8_cup_label(LD32'):
                hooks[call] += out.count(call)
        self.assertEqual(hooks, Counter({'mk8_cup_page_id(c->x': 2, 'mk8_cup_page_turn(c->x': 2, 'mk8_cup_owned(LD32': 1,
                                         'mk8_cup_label(LD32': 1}))
        with tempfile.TemporaryDirectory() as tmp:  # (the path builders: the folder hook once the name is ready)
            out = C.patch_unit(folder/'main_186.c', Path(tmp)).read_text(encoding='utf-8')
            self.assertIn('L_700089ece0:;\n    c->x[2] = (uint64_t)(mk8_course_folder(c->x[20], c->x[2]));', out)
            self.assertIn('L_700089ed48:;\n    c->x[20] = (uint64_t)(mk8_course_folder(c->x[21], c->x[20]));', out)
            with self.assertRaises(ValueError):
                C.patch_paths(out, 'main_186.c')
            with self.assertRaises(ValueError):
                C.patch_paths('void f_1(Cpu *c) {\n}\n', 'main_186.c')
        with tempfile.TemporaryDirectory() as tmp:  # (main_082.c as the build passes it: after the Mii dialog hook)
            from experimental import patch_no_mii_unit
            out = C.patch_unit(patch_no_mii_unit(folder/'main_082.c', Path(tmp)), Path(tmp)).read_text(encoding='utf-8')
            self.assertEqual(out.count('mk8_cup_entry(c->x'), 26)
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


RT_H = ('#include <stdint.h>\n#include <stdio.h>\n#include <stdlib.h>\n#define LOG(...) ((void)0)\n'
        '#define FATAL(...) (fprintf(stderr, __VA_ARGS__), exit(70))\n')
HARNESS = r'''
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
void mk8_cups_built(uint64_t object);
uint64_t mk8_cup_entry(uint64_t base, uint64_t index);
static uint8_t object[0x1500];
uint64_t mk8_cup_select(uint64_t base, uint64_t index);
uint64_t mk8_cup_by_course(uint64_t base, uint64_t index, uint64_t course);
uint64_t mk8_cup_by_slot(uint64_t base, uint64_t index);
uint64_t mk8_course_folder(uint64_t settings, uint64_t name);
int mk8_cups_parse(const char *spec, uint8_t *entries, int capacity, const uint8_t *reverse, int16_t ids[][4],
                   char folders[][4][32], int32_t labels[]);
int64_t mk8_cup_label(uint64_t id);
static uint32_t rd(uint64_t a) { uint32_t v; memcpy(&v, (const void *)(uintptr_t)a, 4); return v; }
static void game_cups(void) {  // (24 race cups with courses 16.., the reverse map, the battle cup's courses unmapped)
    memset(object, 0xff, 25 * 0x80);
    memset(object + 0xc80, 0xff, 123 * 8);
    for (uint32_t id = 16; id < 16 + 96; id++) {
        uint32_t where[2] = {(id - 16) / 4, (id - 16) % 4};
        memcpy(object + 0xc80 + id * 8, where, 8);
        memcpy(object + where[0] * 0x80 + where[1] * 16, &id, 4);
    }
}
static int added_cup(void) {  // (MK8_CUPS_EXTRA=27:20001:CTGP01,28:20002:CTGP02,19:20003:CTGP03,17:20004:CTGP04)
    game_cups();
    uint8_t reverse[123 * 8];
    memcpy(reverse, object + 0xc80, sizeof reverse);
    uint64_t base = (uint64_t)(uintptr_t)object;
    mk8_cups_built(base);
    if (memcmp(reverse, object + 0xc80, sizeof reverse)) return 10;  // (each course keeps its own cup)
    uint64_t cup = mk8_cup_entry(base, 25);
    const uint32_t ids[4] = {27, 28, 19, 17};
    for (int s = 0; s < 4; s++)
        if (rd(cup + 16 * s) != ids[s] || rd(cup + 16 * s + 4) != 0xffffffffu || rd(cup + 16 * s + 8) != 20001u + s)
            return 11;
    if (rd(cup + 64) != 0xffffffffu || rd(mk8_cup_entry(base, 26)) != 0xffffffffu) return 12;
    if (mk8_cup_label(25) != 30000 || mk8_cup_label(26) != -1 || mk8_cup_label(3) != -1) return 22;
    static uint8_t settings[0x200];
    const char *name = "Gu_FirstCircuit";
    uint64_t named = (uint64_t)(uintptr_t)name, at = (uint64_t)(uintptr_t)settings;
    memcpy(settings + 0x1a0, &ids[0], 4);
    if (mk8_course_folder(at, named) != named) return 13;  // (no cup chosen yet)
    mk8_cup_select(base, 3);
    if (mk8_course_folder(at, named) != named) return 14;  // (a game cup: its own course)
    if (mk8_cup_select(base, 25) != cup) return 15;
    if (strcmp((const char *)(uintptr_t)mk8_course_folder(at, named), "CTGP01")) return 16;
    memcpy(settings + 0x1a0, &ids[3], 4);
    if (strcmp((const char *)(uintptr_t)mk8_course_folder(at, named), "CTGP04")) return 17;
    uint32_t other = 40;  // (a course the added cup does not have)
    memcpy(settings + 0x1a0, &other, 4);
    if (mk8_course_folder(at, named) != named) return 18;
    mk8_cup_by_course(base, 6, 40);  // (a course set by its ID that the added cup does not have: a game cup's race)
    if (mk8_course_folder(at, named) != named) return 20;
    mk8_cup_select(base, 25);
    uint64_t record = mk8_cup_by_course(base, 2, 27);  // (course 27, game cup 2 position 3: the added cup's, first)
    if (rd(record + 3 * 16) != 27 || rd(record + 3 * 16 + 8) != 20001) return 23;
    memcpy(settings + 0x1a0, &ids[0], 4);
    if (strcmp((const char *)(uintptr_t)mk8_course_folder(at, named), "CTGP01")) return 21;
    mk8_cup_select(base, 25);
    uint64_t view = mk8_cup_by_slot(base, 2);  // (game cup 2, 24-27, seen by a reverse-map reader: 27 is the added cup's)
    if (rd(view + 3 * 16) != 27 || rd(view + 3 * 16 + 8) != 20001 || rd(view + 2 * 16) != 26
        || rd(view + 2 * 16 + 8) != rd(mk8_cup_entry(base, 2) + 2 * 16 + 8)) return 26;
    if (mk8_cup_by_slot(base, 6) != mk8_cup_entry(base, 6)) return 27;  // (a cup it borrows nothing from: as it is)
    if (rd(mk8_cup_entry(base, 2) + 3 * 16 + 8) == 20001) return 28;  // (the cup screen's reading stays the game's)
    if (mk8_cup_by_course(base, 2, 26) != mk8_cup_entry(base, 2)) return 24;  // (course 26 is not the added cup's)
    if (mk8_cup_by_slot(base, 2) != mk8_cup_entry(base, 2)) return 29;  // (a game cup chosen: as it is)
    if (mk8_course_folder(at, named) != named) return 25;
    mk8_cup_select(base, 0);  // (back to a game cup)
    memcpy(settings + 0x1a0, &ids[0], 4);
    if (mk8_course_folder(at, named) != named) return 19;
    return 0;
}
uint64_t mk8_cup_page_id(uint64_t id);
void mk8_cup_page_turn(uint64_t input);
int64_t mk8_cup_owned(uint64_t id);
static int page(int game, int expect) {  // (a button setup: the game's ids for its page; the cups it gets)
    for (int i = 0; i < 12; i++)
        if (mk8_cup_page_id((uint64_t)(game * 12 + i)) != (uint64_t)(expect + i)) return 1;
    return 0;
}
static int pages(int added) {  // (MK8_CUPS_EXTRA with one cup, or none)
    game_cups();
    uint64_t base = (uint64_t)(uintptr_t)object;
    mk8_cups_built(base);
    if (!added) {  // (no added cups: the game's pages as they are)
        mk8_cup_page_turn(1 << 12);
        return page(0, 0) || page(1, 12) || mk8_cup_owned(25) != -1 ? 40 : 0;
    }
    if (page(0, 0)) return 41;                              // (opened on the first page)
    mk8_cup_page_turn(1 << 12); if (page(1, 12)) return 42;  // (forward: DLC)
    mk8_cup_page_turn(1 << 12); if (page(0, 25)) return 43;  // (forward: the added cups, in the first page's layout)
    if (mk8_cup_owned(25) != 0 || mk8_cup_owned(26) != 1 || mk8_cup_owned(3) != -1 || mk8_cup_owned(41) != -1) return 44;
    mk8_cup_page_turn(1 << 12); if (page(1, 0)) return 45;   // (forward again: back to the base cups)
    mk8_cup_page_turn(1 << 11); if (page(0, 25)) return 46;  // (back: the added cups)
    mk8_cup_page_turn(1 << 11); if (page(1, 12)) return 47;  // (back: DLC)
    mk8_cup_select(base, 25);                                // (a race from the added cup; the screen opens again)
    if (page(1, 25)) return 48;
    if (mk8_cup_page_id(30) != 30) return 49;                // (a cup beyond the pages: unchanged)
    return 0;
}
static int pages16(void) {  // (16 added cups, as CTGP Deluxe's: pages base, DLC, added 1-12, added 13-16)
    game_cups();
    uint64_t base = (uint64_t)(uintptr_t)object;
    mk8_cups_built(base);
    if (page(0, 0)) return 50;
    mk8_cup_page_turn(1 << 12); if (page(1, 12)) return 51;
    mk8_cup_page_turn(1 << 12); if (page(0, 25)) return 52;
    mk8_cup_page_turn(1 << 12); if (page(1, 37)) return 53;  // (cups 13-16 and 8 empty places after them)
    if (mk8_cup_owned(40) != 0 || mk8_cup_owned(41) != -1) return 54;
    mk8_cup_page_turn(1 << 12); if (page(0, 0)) return 55;   // (and round to the base cups)
    mk8_cup_page_turn(1 << 11); if (page(1, 37)) return 56;
    mk8_cup_select(base, 39);                                // (a race from added cup 15: the screen opens on its page)
    if (page(0, 37)) return 57;
    return 0;
}
uint64_t mk8_stream_path(uint64_t path);
static int music(void) {  // (MK8_ROMFS_OVERLAY with only Audio/Resource/Stream/CTGP01_N.bfstm)
    game_cups();
    uint64_t base = (uint64_t)(uintptr_t)object;
    mk8_cups_built(base);
    static uint8_t settings[0x200];
    uint32_t id = 27;
    memcpy(settings + 0x1a0, &id, 4);
    uint64_t at = (uint64_t)(uintptr_t)settings, name = (uint64_t)(uintptr_t)"Gu_FirstCircuit";
    const char *n = "content://Audio/Resource/Stream/pBGM_WU_FIRST_CIRCUIT_N.bfstm";
    const char *f = "content://Audio/Resource/Stream/pBGM_WU_FIRST_CIRCUIT_F.bfstm";
    const char *menu = "content://Audio/Resource/Stream/BGM_MENU_SINGLE_MULTI.bfstm";
    uint64_t pn = (uint64_t)(uintptr_t)n, pf = (uint64_t)(uintptr_t)f, pm = (uint64_t)(uintptr_t)menu;
    if (mk8_stream_path(pn) != pn) return 60;  // (no race of an added cup's course)
    mk8_cup_select(base, 25);
    mk8_course_folder(at, name);
    const char *got = (const char *)(uintptr_t)mk8_stream_path(pn);
    if (strcmp(got, "content://Audio/Resource/Stream/CTGP01_N.bfstm")) return 61;
    if (mk8_stream_path(pf) != pf) return 62;  // (no _F for it in the overlay: the course's own)
    if (mk8_stream_path(pm) != pm) return 63;  // (not a course's music)
    id = 40;  // (the next race is a course the added cup does not have)
    memcpy(settings + 0x1a0, &id, 4);
    mk8_course_folder(at, name);
    if (mk8_stream_path(pn) != pn) return 64;
    return 0;
}
static int refused(void) {
    game_cups();
    static uint8_t entries[16 * 0x80];
    int16_t ids[16][4];
    char folders[16][4][32];
    int32_t labels[16];
    const uint8_t *reverse = object + 0xc80;
    if (mk8_cups_parse("27:1:A,28:2:B,19:3:C,17:4:D;30001=16:5:E,18:6:F,20:7:G,21:8:H", entries, 16, reverse, ids,
                       folders, labels) != 2 || ids[1][3] != 21 || strcmp(folders[1][3], "H") || labels[0] != -1
        || labels[1] != 30001) return 20;
    const char *bad[] = {"27:1:A,28:2:B,19:3:C", "27:1:A,28:2:B,19:3:C,17:4:D,16:5:E", "27:1:A,27:2:B,19:3:C,17:4:D",
                         "27:1:A,28:2:B,19:3:C,5:4:D", "27:1:A,28:2:B,19:3:C,115:4:D", "27:0:A,28:2:B,19:3:C,17:4:D",
                         "27:1,28:2:B,19:3:C,17:4:D", "27:1:A/B,28:2:B,19:3:C,17:4:D", "27:1:,28:2:B,19:3:C,17:4:D", "x"};
    for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++)
        if (mk8_cups_parse(bad[i], entries, 16, reverse, ids, folders, labels) >= 0) return 21 + (int)i;
    if (mk8_cups_parse("0=27:1:A,28:2:B,19:3:C,17:4:D", entries, 16, reverse, ids, folders, labels) >= 0) return 39;
    return 0;
}
int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "added")) return added_cup();
    if (argc > 1 && !strcmp(argv[1], "refused")) return refused();
    if (argc > 1 && !strcmp(argv[1], "pages")) return pages(getenv("MK8_CUPS_EXTRA") != NULL);
    if (argc > 1 && !strcmp(argv[1], "pages16")) return pages16();
    if (argc > 1 && !strcmp(argv[1], "music")) return music();
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
            env = dict(os.environ, MK8_CUPS_EXTRA='30000=27:20001:CTGP01,28:20002:CTGP02,19:20003:CTGP03,17:20004:CTGP04')
            self.assertEqual(subprocess.run([str(exe), 'added'], env=env, timeout=60).returncode, 0)
            self.assertEqual(subprocess.run([str(exe), 'refused'], timeout=60).returncode, 0)
            self.assertEqual(subprocess.run([str(exe), 'pages'], env=env, timeout=60).returncode, 0)
            self.assertEqual(subprocess.run([str(exe), 'pages'], timeout=60).returncode, 0)
            cups16 = ';'.join(','.join(f'{16 + 4 * c + s}:{20001 + 4 * c + s}:CT{c:02}{s}' for s in range(4))
                              for c in range(16))
            env16 = dict(os.environ, MK8_CUPS_EXTRA=cups16)
            self.assertEqual(subprocess.run([str(exe), 'pages16'], env=env16, timeout=60).returncode, 0)
            overlay = out/'overlay'
            (overlay/'Audio/Resource/Stream').mkdir(parents=True)
            (overlay/'Audio/Resource/Stream/CTGP01_N.bfstm').write_bytes(b'FSTM')
            env = dict(os.environ, MK8_CUPS_EXTRA='30000=27:20001:CTGP01,28:20002:CTGP02,19:20003:CTGP03,17:20004:CTGP04',
                       MK8_ROMFS_OVERLAY=str(overlay))
            self.assertEqual(subprocess.run([str(exe), 'music'], env=env, timeout=60).returncode, 0)
            env = dict(os.environ, MK8_CUPS_EXTRA='27:20001:CTGP01,5:20002:CTGP02,19:20003:CTGP03,17:20004:CTGP04')
            run = subprocess.run([str(exe), 'added'], env=env, timeout=60, capture_output=True)
            self.assertEqual(run.returncode, 70)  # (a reserved ID: the game stops with the reason)


if __name__ == '__main__':
    unittest.main()
