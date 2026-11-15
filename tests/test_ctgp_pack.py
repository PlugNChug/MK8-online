"""CTGP Deluxe as an optional course pack: slot mapping, names in message.sarc, overlay build/verify, the import,
and the runtime's second RomFS overlay. Synthetic files only: no game or CTGP data is used or needed."""
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
import ctgp_pack as P
import import_ctgp
from native import DEFAULT_COMPILER, compiler_env

TAG = '\x0e\x00\x02\x02Z'
END = '\x0e\x00\x02\x02d'


def msbt(entries):
    """A minimal MSBT (one LBL1 bucket, TXT2) as the game's: label -> text."""
    labels = list(entries)
    lbl = bytearray(struct.pack('<III', 1, len(labels), 12))
    for i, label in enumerate(labels):
        lbl += bytes([len(label)]) + label.encode() + struct.pack('<I', i)
    texts = [(entries[k] + '\0').encode('utf-16-le') for k in labels]
    txt = bytearray(struct.pack('<I', len(texts)))
    at = 4 + 4 * len(texts)
    for t in texts:
        txt += struct.pack('<I', at)
        at += len(t)
    for t in texts:
        txt += t
    # magic, BOM, 0, UTF-16, version 3, two sections, 0, file size (below), padding to 0x20
    out = bytearray(b'MsgStdBn\xff\xfe\x00\x00\x00\x03' + struct.pack('<H', 2) + bytes(16))
    for magic, body in ((b'LBL1', lbl), (b'TXT2', txt)):
        section = bytearray(magic + struct.pack('<I', len(body)) + bytes(8) + body)
        section += b'\xab' * (-len(section) % 16)
        out += section
    struct.pack_into('<I', out, 0x12, len(out))
    return bytes(out)


def sarc(files):
    names = bytearray()
    nodes = []
    for name in files:
        nodes.append((name, len(names) // 4))
        names += name.encode() + b'\0'
        names += bytes(-len(names) % 4)
    head = 0x14 + 0xC + 16 * len(files) + 8 + len(names)
    data_off = (head + 0x7F) & ~0x7F
    body, entries = bytearray(), []
    for name, name_off in nodes:
        body += bytes(-len(body) % 0x80)
        entries.append((0x1234 + name_off, 0x01000000 | name_off, len(body), len(body) + len(files[name])))
        body += files[name]
    out = bytearray(b'SARC' + struct.pack('<HHIIHH', 0x14, 0xFEFF, 0, data_off, 0x100, 0))
    out += b'SFAT' + struct.pack('<HHI', 0xC, len(files), 0x65)
    for entry in entries:
        out += struct.pack('<IIII', *entry)
    out += b'SFNT' + struct.pack('<HH', 8, 0) + names
    out += bytes(data_off - len(out)) + body
    struct.pack_into('<I', out, 8, len(out))
    return bytes(out)


def common_labels():
    values = {str(label): f'Cup {label}' for label, _, _ in P.BASE_CUPS + P.BCP_CUPS}
    for _, _, _, label, _, _ in P.slots():
        values[str(label)] = f'Course {label}'
        values[str(label + 100)] = f'{TAG}Course {label}{END}'
    for label in P.PLATFORM_LABELS:
        values[str(label)] = 'Wii'
    values['9999'] = 'Untouched'
    return values


def write(path, data=b'x'):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def fixture(root):
    """A 4.0.1-shaped RomFS with every slot, and a CTGP-shaped SD folder with its 64 courses."""
    romfs = root/'game/romfs'
    for ct, cup, folders, label, music, picture in P.slots():
        for folder in folders:
            for name in ('course_model.szs', 'course_kcl.szs', f'Course_{folder}.bars'):
                write(romfs/'Course'/folder/name, b'original')
        for base in music:
            for kind in ('N', 'F'):
                write(romfs/f'Audio/Resource/Stream/pBGM_{base}_{kind}.bfstm', b'og')
        write(romfs/f'UI/cmn/menu/ym_CoursePict_{picture}_00^u.bntx', b'og')
    write(romfs/'Audio/Resource/Stream/pBGM_WU_SNOW_MOUNTAIN_LAP2.bfstm', b'og')
    write(romfs/'Audio/Resource/Stream/pBGM_GC_BABYPARK_REPLAY.bfstm', b'og')
    for _, icon, _ in P.BASE_CUPS + P.BCP_CUPS:
        for variant in P.ICON_VARIANTS:
            write(romfs/f'UI/cmn/race/ym_CupIcon{icon}{variant}', b'og')
    for rel in P.EXTRAS:
        write(romfs/rel, b'og')
    write(romfs/'UI/USen/message.sarc', sarc({'Common.msbt': msbt(common_labels()), 'Menu.msbt': msbt({'1': 'Menu'})}))
    ctgp = root/'CTGPDX/atmosphere/contents'/P.TITLE/'romfs'
    for i in range(P.CT_COUNT):
        course = ctgp/f'Course/CT_{i:02X}'
        for name in ('course_model.szs', 'course_kcl.szs', 'course_muunt.byaml', f'Course_CT_{i:02X}.bars',
                     'Course.bars'):
            write(course/name, f'ct{i}-{name}'.encode())
        write(course/'MapObj/Thing/Thing.bfres', f'obj{i}'.encode())
        for kind in ('N', 'F'):
            write(ctgp/f'Audio/Resource/Stream/pBGM_CT_{i:02X}_{kind}.bfstm', f'music{i}{kind}'.encode())
        write(ctgp/f'UI/cmn/menu/ym_CoursePict_CT_{i:02X}_00^u.bntx', f'pict{i}'.encode())
    for c, icon in enumerate(P.CTGP_CUP_ICONS):
        for variant in P.ICON_VARIANTS:
            write(ctgp/f'UI/cmn/race/ym_CupIcon{icon}{variant}', f'icon{c}'.encode())
    for rel in P.EXTRAS:
        write(ctgp/rel, b'ctgp extra')
    write(ctgp/'UI/cmn/menu.sarc', b'never used')  # crashes 4.0.1: must stay out of the pack
    names = {str(P.CTGP_CUP_LABEL + c): f'CTGP cup {c}' for c in range(16)}
    names.update({str(n): f'Name {n}' for n, _ in P.CT_NAMES if n})
    names.update({'500000': 'GP', '500002': 'GPDX', '500004': 'DKR', '500005': 'RMX'})
    write(ctgp/'UI/USen/message.sarc', sarc({'CTGPText.msbt': msbt(names)}))
    return romfs, root/'CTGPDX'


def texts(sarc_bytes):
    files = {n: b for n, _, _, b, _ in P.sarc_read(sarc_bytes)['files']}
    labels, values = P.msbt_read(files['Common.msbt'])
    return {k: values[v].rstrip('\0') for k, v in labels.items()}, files


class CtgpPackTests(unittest.TestCase):
    def test_archives_round_trip_and_names_keep_their_tags(self):
        common = msbt({'1401': 'Mario Kart Stadium', '1501': f'{TAG}Mario Kart Stadium{END}', '7': 'x'})
        archive = sarc({'Common.msbt': common, 'Menu.msbt': msbt({'1': 'a'})})
        parsed = P.sarc_read(archive)
        self.assertEqual(P.sarc_write(parsed, {}), archive)
        self.assertEqual(P.msbt_write(common, P.msbt_read(common)[1]), common)
        changed = P.set_texts(common, {'1401': 'Omatsuri', '1501': 'Omatsuri'})
        labels, values = P.msbt_read(changed)
        self.assertEqual(values[labels['1401']], 'Omatsuri\0')
        self.assertEqual(values[labels['1501']], f'{TAG}Omatsuri{END}\0')
        self.assertEqual(values[labels['7']], 'x\0')
        rebuilt = P.sarc_write(parsed, {'Common.msbt': changed})
        files = {n: b for n, _, _, b, _ in P.sarc_read(rebuilt)['files']}
        self.assertEqual(files['Common.msbt'], changed)
        self.assertEqual(files['Menu.msbt'], msbt({'1': 'a'}))
        with self.assertRaises(P.PackError):
            P.set_texts(common, {'404': 'x'})

    def test_slots_cover_every_ctgp_course_once(self):
        all_slots, base = P.slots(), P.slots(False)
        self.assertEqual([s[0] for s in all_slots], list(range(64)))
        self.assertEqual(len(base), 48)
        folders = [f for s in all_slots for f in s[2]]
        self.assertEqual(len(folders), len(set(folders)))
        self.assertEqual(all_slots[0][2], ('Gu_FirstCircuit',))
        self.assertEqual([s[2][0] for s in all_slots[48:52]], ['Cnsw_11', 'Cnsw_12', 'Cnsw_13', 'Cnsw_14'])
        self.assertEqual(all_slots[56][2:], (('Cnsw_21',), 11709, ('Cnsw_21',), 'DLC09'))
        self.assertEqual(len(P.CT_NAMES), 64)

    def test_plan_maps_ctgp_courses_into_the_slots(self):
        with tempfile.TemporaryDirectory() as folder:
            romfs, sd = fixture(Path(folder))
            files = P.plan(P.locate(sd), romfs)
            src = lambda rel: files[rel].read_bytes() if isinstance(files[rel], Path) else files[rel]
            self.assertEqual(src('Course/Gu_FirstCircuit/course_model.szs'), b'ct0-course_model.szs')
            self.assertEqual(src('Course/Gu_FirstCircuit/MapObj/Thing/Thing.bfres'), b'obj0')
            self.assertNotIn('Course/Gu_FirstCircuit/Course_CT_00.bars', files)  # the slot's own sound bank stays
            self.assertEqual(src('Course/Gu_MarioCircuit/course_model.szs'), b'ct4-course_model.szs')
            for season in ('Spring', 'Summer', 'Autumn', 'Winter'):  # one course, four folders
                self.assertEqual(src(f'Course/Du_Animal_{season}/course_kcl.szs'), b'ct43-course_kcl.szs')
            self.assertEqual(src('Course/Cnsw_11/course_model.szs'), b'ct48-course_model.szs')
            self.assertEqual(src('Course/Cnsw_28/course_model.szs'), b'ct63-course_model.szs')
            self.assertNotIn('Course/Cnsw_31/course_model.szs', files)  # the other BCP cups stay original
            stream = 'Audio/Resource/Stream/'
            self.assertEqual(src(stream + 'pBGM_WU_FIRST_CIRCUIT_N.bfstm'), b'music0N')
            self.assertEqual(src(stream + 'pBGM_WU_FIRST_CIRCUIT_F.bfstm'), b'music0F')
            self.assertEqual(src(stream + 'pBGM_WU_SNOW_MOUNTAIN_LAP2.bfstm'), b'music11N')
            self.assertEqual(src(stream + 'pBGM_GC_BABYPARK_REPLAY.bfstm'), b'music40N')
            self.assertEqual(src(stream + 'pBGM_Cnsw_11_F.bfstm'), b'music48F')
            self.assertEqual(src('UI/cmn/menu/ym_CoursePict_Du_Animal_00^u.bntx'), b'pict43')
            self.assertEqual(src('UI/cmn/menu/ym_CoursePict_DLC16_00^u.bntx'), b'pict63')
            self.assertEqual(src('UI/cmn/race/ym_CupIconMushroom_Nml^l.bntx'), b'icon0')
            self.assertEqual(src('UI/cmn/race/ym_CupIconCharger03_Line^s.bntx'), b'icon15')
            self.assertEqual(src('RaceCommon/JugemObj/JugemObj.bfres'), b'ctgp extra')
            self.assertNotIn('UI/cmn/menu.sarc', files)
            self.assertFalse(any(rel.startswith('skyline/') for rel in files))
            names, archive = texts(src('UI/USen/message.sarc'))
            self.assertEqual(names['1301'], 'CTGP cup 0')
            self.assertEqual(names['11604'], 'CTGP cup 15')
            self.assertEqual(names['1401'], 'GPDX Name 500100')
            self.assertEqual(names['1501'], f'{TAG}GPDX Name 500100{END}')
            self.assertEqual(names['1405'], 'Name 500131')  # Toad Harbor's slot: CT_05, second in the Flower Cup
            self.assertEqual(names['1404'], 'Name 500103')
            self.assertEqual(names['11701'], 'Name 500143')
            self.assertEqual(names['1406'], 'Name 500105'.replace('Name', 'GP Name'))
            self.assertEqual(names['1461'], '')
            self.assertEqual(names['9999'], 'Untouched')
            self.assertEqual(archive['Menu.msbt'], msbt({'1': 'Menu'}))
            unresolved = [i for i, (n, _) in enumerate(P.CT_NAMES) if n is None]
            self.assertTrue(unresolved)
            label = next(s[3] for s in P.slots() if s[0] == unresolved[0])
            self.assertEqual(names[str(label)], f'CTGP {unresolved[0] + 1:02d}')

    def test_build_verify_and_import(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            romfs, sd = fixture(root)
            local = root/'data'
            manifest = import_ctgp.import_pack(sd, romfs, local)
            pack = local/'ctgp/pack'
            self.assertEqual(json.loads((local/'ctgp.json').read_text()), {'enabled': True})
            self.assertEqual((manifest['courses'], manifest['base_courses']), (64, 48))
            self.assertFalse(manifest['gameplay_verified'])
            self.assertEqual(len(manifest['identity']), 64)
            self.assertEqual(P.verify(pack)['identity'], manifest['identity'])
            model = pack/'romfs/Course/Gu_FirstCircuit/course_model.szs'
            self.assertEqual(model.read_bytes(), b'ct0-course_model.szs')
            self.assertEqual(os.stat(model).st_nlink, 2)  # same disk: a hardlink, no copy
            self.assertEqual((romfs/'Course/Gu_FirstCircuit/course_model.szs').read_bytes(), b'original')
            (pack/'romfs/extra.bin').write_bytes(b'x')
            with self.assertRaises(P.PackError):
                P.verify(pack)
            again = import_ctgp.import_pack(sd, romfs, local)  # a broken pack is rebuilt
            self.assertEqual(again['identity'], manifest['identity'])
            self.assertFalse((pack/'romfs/extra.bin').exists())
            (local/'ctgp.json').write_text('{"enabled": false}')
            import_ctgp.import_pack(sd, romfs, local)  # reused as is, and the preference is kept
            self.assertEqual(json.loads((local/'ctgp.json').read_text()), {'enabled': False})
            self.assertEqual(sorted(p.name for p in (local/'ctgp').iterdir()), ['pack'])
            with self.assertRaises(P.PackError):
                P.locate(root/'game')

    def test_incomplete_ctgp_is_refused(self):
        with tempfile.TemporaryDirectory() as folder:
            romfs, sd = fixture(Path(folder))
            (sd/'atmosphere/contents'/P.TITLE/'romfs/Course/CT_3F/course_kcl.szs').unlink()
            with self.assertRaises(P.PackError):
                P.plan(P.locate(sd), romfs)
            self.assertFalse((Path(folder)/'data').exists())

    @unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
    def test_runtime_serves_the_pack_as_a_second_overlay(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            (out/'inc').mkdir()
            (out/'inc/rt.h').write_text('extern int fatal_calls; extern char logged[2048];\n'
                                        '#define FATAL(...) (fatal_calls++)\n'
                                        '#define LOG(...) snprintf(logged, sizeof logged, __VA_ARGS__)\n')
            harness = out/'harness.c'
            harness.write_text(r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int fatal_calls; char logged[2048];
const char *mk8_romfs_overlay_from(const char *value);
const char *game_romfs_overlay(void);
int main(int argc, char **argv) {
    if (mk8_romfs_overlay_from(NULL) || mk8_romfs_overlay_from("")) return 1;          // no pack
    if (strcmp(mk8_romfs_overlay_from(argv[1]), argv[1])) return 2;                     // a folder
    if (*mk8_romfs_overlay_from(argv[2]) || *mk8_romfs_overlay_from("nowhere")) return 3;  // a file / missing
    _putenv_s("MK8_ROMFS_OVERLAY", argv[1]);
    if (strcmp(game_romfs_overlay(), argv[1]) || fatal_calls || !strstr(logged, "[mk8:pack]")) return 4;
    _putenv_s("MK8_ROMFS_OVERLAY", argv[2]);
    game_romfs_overlay();
    if (fatal_calls != 1) return 5;  // an invalid pack stops the game instead of opening the base game silently
    return 0;
}''', encoding='utf-8')
            exe = out/'harness.exe'
            subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu', '-O1', '-I', str(out/'inc'),
                            str(harness), str(ROOT/'runtime/mk8/support/romfs_overlay.c'), '-o', str(exe)],
                           env=compiler_env(), check=True, capture_output=True)
            (out/'pack').mkdir()
            (out/'file.bin').write_bytes(b'x')
            result = subprocess.run([str(exe), str(out/'pack'), str(out/'file.bin')], capture_output=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
