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


def all_slots():
    return P.slots('base') + P.slots('dlc')


def bntx(textures):
    """A minimal Switch BNTX: textures = {name: (format, width, height, block height log2, data)}."""
    names = list(textures)
    out = bytearray(b'BNTX' + bytes(28))
    nx = len(out)
    out += b'NX  ' + struct.pack('<I', len(names)) + bytes(8 + 0x20)
    infos = len(out)
    out += bytes(8 * len(names))
    struct.pack_into('<Q', out, nx + 8, infos)
    for i, name in enumerate(names):
        fmt, width, height, layout, data = textures[name]
        brti = len(out)
        struct.pack_into('<Q', out, infos + 8 * i, brti)
        out += bytes(0x90)
        out[brti:brti + 4] = b'BRTI'
        struct.pack_into('<H', out, brti + 0x16, 1)
        struct.pack_into('<IIII', out, brti + 0x1C, fmt, 0, width, height)
        struct.pack_into('<I', out, brti + 0x34, layout)
        struct.pack_into('<I', out, brti + 0x50, len(data))
        name_at = len(out)
        out += struct.pack('<H', len(name)) + name.encode() + b'\0'
        out += bytes(-len(out) % 8)
        ptrs = len(out)
        out += bytes(8)
        data_at = len(out)
        out += data
        struct.pack_into('<QQQ', out, brti + 0x60, name_at, 0, ptrs)
        struct.pack_into('<Q', out, ptrs, data_at)
    return bytes(out)


def unnamed_sarc(blobs):
    """A SARC of hashed, unnamed files (like the layout archives inside menu.sarc)."""
    data_off = (0x20 + 16 * len(blobs) + 8 + 0x7F) & ~0x7F
    out = bytearray(b'SARC' + struct.pack('<HHIIHH', 0x14, 0xFEFF, 0, data_off, 0x100, 0))
    out += b'SFAT' + struct.pack('<HHI', 0xC, len(blobs), 0x65)
    body, nodes = bytearray(), []
    for i, blob in enumerate(blobs):
        body += bytes(-len(body) % 0x80)
        nodes.append((0x1000 + i, 0, len(body), len(body) + len(blob)))
        body += blob
    for node in nodes:
        out += struct.pack('<IIII', *node)
    out += b'SFNT' + struct.pack('<HH', 8, 0)
    out += bytes(data_off - len(out)) + body
    struct.pack_into('<I', out, 8, len(out))
    return bytes(out)


SLOT = 512  # (8x8 texels in one 64x8-byte GOB: RGB565 rows of 16 bytes; BC3 rows of 2 blocks, 32 bytes)
RED_BC3 = bytes((255, 255, 0, 0, 0, 0, 0, 0)) + struct.pack('<HHI', 0xF800, 0xF800, 0)


def red_icon():
    """CTGP's Nml^l icons are RGBA8: a 16x16 one, red and opaque (scaled down 2:1 into the 8x8 slots)."""
    data = bytearray(1024)
    for y in range(16):
        for x in range(16):
            at = P._gob(x, y, 64, 4, 1)
            data[at:at + 4] = bytes((255, 0, 0, 255))
    return bntx({'icon_Nml^l': (0xB06, 16, 16, 0, bytes(data))})


def common_labels():
    values = {str(label): f'Cup {label}' for label, _, _ in P.BASE_CUPS + P.BCP_CUPS}
    for _, _, _, label, _, _ in all_slots():
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
    for ct, cup, folders, label, music, picture in all_slots():
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
    icons = bntx({f'ym_CupIcon{icon}^h': (0x701, 8, 8, 0, bytes(SLOT)) for _, icon, _ in P.BASE_CUPS + P.BCP_CUPS})
    layout = P.yaz0_store(unnamed_sarc([b'FLYT layout', icons, b'FLAN animation']), 0x80)
    write(romfs/'UI/cmn/menu.sarc', sarc({'mn_L_CupSlctBtn_00.szs': layout, 'other.szs': b'untouched'}))
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
    for icon in P.CTGP_CUP_ICONS:  # (the menu's icons are decoded and converted from these)
        write(ctgp/f'UI/cmn/race/ym_CupIcon{icon}_130x130^h.bntx', red_icon())
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
    def test_texture_dictionary_and_pattern_frames(self):
        names = [b'ym_CupIconMushroom^h', b'ym_CupIconFlower^h', b'ym_CupIconStar^h', b'ym_CupIconCT00^h', b'a', b'ab']
        nodes = P._dic_build(names)
        self.assertEqual(len(nodes), len(names) + 1)
        for name in names:  # (each name is found by walking the trie as the game's ResDic does)
            p, x = 0, nodes[0][1]
            while nodes[p][0] < nodes[x][0]:
                p, x = x, nodes[x][2] if P._dic_bit(name, nodes[x][0]) else nodes[x][1]
            self.assertEqual(nodes[x][3], name)
        # a BFLAN as the game's: one pai1 with a texture list and a pane's FLTP step curve last
        def flan(textures, keys):
            table = b''.join(struct.pack('<I', 4 * len(textures) + sum(len(n) + 1 for n in textures[:i]))
                             for i in range(len(textures)))
            blob = table + b''.join(n + b'\0' for n in textures)
            blob += b'\0' * (-(0x14 + len(blob)) % 4)
            anim_table = 0x14 + len(blob)
            anim = bytearray(b'P_CupIcon_00'.ljust(28, b'\0') + struct.pack('<BBHI', 1, 1, 0, 0x24))
            anim += b'FLTP' + struct.pack('<BBHI', 1, 0, 0, 0xC)
            anim += struct.pack('<HBBHHI', 0, 1, 0, len(keys), 0, 0xC)
            anim += b''.join(struct.pack('<fHH', float(f), i, 0) for f, i in keys)
            pai = bytearray(b'pai1' + struct.pack('<IHHHHI', 0, len(keys), 0, len(textures), 1, anim_table))
            pai += blob + struct.pack('<I', anim_table + 4) + anim
            struct.pack_into('<I', pai, 4, len(pai))
            out = bytearray(b'FLAN\xff\xfe\x14\x00\x00\x00\x00\x08' + struct.pack('<IHH', 0, 1, 0) + pai)
            struct.pack_into('<I', out, 0xC, len(out))
            return bytes(out)
        base = flan([b'ym_CupIconMushroom^h', b'ym_CupIconBack^h'], [(0, 0), (1, 1)])
        self.assertEqual(P.flan_add_pattern(base, 'P_CupIcon_00', [], 25), base)
        out = P.flan_add_pattern(base, 'P_CupIcon_00', ['ym_CupIconCT00^h', 'ym_CupIconBack^h'], 25)
        pai = out.find(b'pai1')
        frames, _, textures, anims, _ = struct.unpack_from('<HHHHI', out, pai + 8)
        self.assertEqual((frames, textures, anims), (26, 3, 1))
        self.assertEqual(out[-16:], struct.pack('<fHHfHH', 25.0, 2, 0, 26.0, 1, 0))
        self.assertEqual(struct.unpack_from('<I', out, 0xC)[0], len(out))
        with self.assertRaises(P.PackError):
            P.flan_add_pattern(base, 'P_Other_00', ['x'], 25)

    @unittest.skipUnless((ROOT/'local/imports/mk8-4.0.1/romfs/UI/cmn/menu.sarc').is_file(), 'MK8 4.0.1 RomFS required')
    def test_cup_icon_textures_are_added_to_the_games_bntx(self):
        menu = P.sarc_read((ROOT/'local/imports/mk8-4.0.1/romfs/UI/cmn/menu.sarc').read_bytes())
        archive = next(b for n, _, _, b, _ in menu['files'] if n == 'mn_L_CupSlctBtn_00.szs')
        bntx = next(f[3] for f in P.sarc_read(P.yaz0_decode(archive))['files'] if f[3][:4] == b'BNTX')
        same = P.bntx_add_textures(bntx, [])  # (the same file: only the _RLT's encoding may differ)
        rlt = struct.unpack_from('<I', bntx, 0x18)[0]
        self.assertEqual(same[0x20:rlt], bntx[0x20:rlt])
        self.assertEqual(sorted(P._bntx_slots(same)[1]), sorted(P._bntx_slots(bntx)[1]))
        textures = P.bntx_textures(bntx)
        size = textures['ym_CupIconMushroom^h'][4]
        image = bytes(range(256)) * (size // 256) + bytes(size % 256)
        out = P.bntx_add_textures(bntx, [('ym_CupIconCT00^h', 'ym_CupIconMushroom^h', image)])
        added = P.bntx_textures(out)
        self.assertEqual(len(added), len(textures) + 1)
        self.assertEqual(added['ym_CupIconCT00^h'][:6], textures['ym_CupIconMushroom^h'][:6])
        at = added['ym_CupIconCT00^h'][6]
        self.assertEqual(out[at:at + size], image)
        for name, (fmt, w, h, mips, length, layout, start) in textures.items():
            self.assertEqual(added[name][:6], (fmt, w, h, mips, length, layout))
            self.assertEqual(out[added[name][6]:added[name][6] + length], bntx[start:start + length])
        self.assertTrue(all(struct.unpack_from('<Q', out, o)[0] < len(out) for _, o in P._bntx_slots(out)[1]))
        with self.assertRaises(P.PackError):
            P.bntx_add_textures(bntx, [('ym_CupIconMushroom^h', 'ym_CupIconMushroom^h', image)])

    def test_new_labels_are_added_in_their_buckets(self):
        common = msbt({'1401': 'Mario Kart Stadium', '1501': f'{TAG}Mario Kart Stadium{END}'})
        self.assertEqual(P.msbt_add(common, {}), common)
        added = P.msbt_add(common, {'30000': 'Boo Cup', '31000': 'Omatsuri', '31100': f'{TAG}Omatsuri{END}'})
        labels, values = P.msbt_read(added)
        self.assertEqual(values[labels['30000']], 'Boo Cup\0')
        self.assertEqual(values[labels['31100']], f'{TAG}Omatsuri{END}\0')
        self.assertEqual(values[labels['1401']], 'Mario Kart Stadium\0')
        for bad in ({'1401': 'x'}, {'': 'x'}, {'é': 'x'}):
            with self.assertRaises(P.PackError):
                P.msbt_add(common, bad)
        # (the game's files have 101 buckets: each label goes to the one of its hash, after the ones there)
        empty = bytearray(msbt({}))
        lbl = struct.pack('<I', 7) + struct.pack('<II', 0, 4 + 8 * 7) * 7
        empty[0x24:0x28] = struct.pack('<I', len(lbl))
        body = bytes(empty[:0x30]) + lbl + b'\xab' * (-(0x10 + len(lbl)) % 16) + bytes(empty[0x30 + 16:])
        empty = bytearray(body)
        struct.pack_into('<I', empty, 0x12, len(empty))
        out = P.msbt_add(bytes(empty), {str(n): f't{n}' for n in range(30000, 30020)})
        at = 0x20 + 0x10
        slots = struct.unpack_from('<I', out, at)[0]
        self.assertEqual(slots, 7)
        for i in range(slots):
            count, offset = struct.unpack_from('<II', out, at + 4 + 8 * i)
            for _ in range(count):
                n = out[at + offset]
                self.assertEqual(P._label_slot(out[at + offset + 1:at + offset + 1 + n].decode(), 7), i)
                offset += 1 + n + 4
        labels, values = P.msbt_read(out)
        self.assertEqual(values[labels['30019']], 't30019\0')
        self.assertEqual(P.msbt_add(out, {}), out)

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
        base, dlc = P.slots('base'), P.slots('dlc')
        self.assertEqual([s[0] for s in base], list(range(64)))
        self.assertEqual([s[0] for s in dlc], list(range(48)))
        for layout in (base, dlc):
            folders = [f for s in layout for f in s[2]]
            self.assertEqual(len(folders), len(set(folders)))
        self.assertEqual(base[0][2], ('Gu_FirstCircuit',))
        self.assertEqual([s[2][0] for s in base[48:52]], ['Cnsw_11', 'Cnsw_12', 'Cnsw_13', 'Cnsw_14'])
        self.assertEqual(base[56][2:], (('Cnsw_21',), 11709, ('Cnsw_21',), 'DLC09'))
        self.assertEqual(sorted(P.BCP_FOLDERS), sorted(f'Cnsw_{w}{n}' for w in range(1, 7) for n in range(1, 9)))
        self.assertEqual(dlc[16][2:], (('Cnsw_31',), 11717, ('Cnsw_31',), 'DLC17'))  # Rock Cup: London Loop's slot
        self.assertEqual(dlc[19][2][0], 'Cnsw_62')  # (the cups do not follow the folder numbers)
        self.assertFalse(any(f.startswith('Cnsw_') for s in base[:48] for f in s[2]))
        self.assertTrue(all(s[2][0].startswith('Cnsw_') for s in dlc))
        with self.assertRaises(P.PackError):
            P.slots('other')
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
            self.assertEqual(src('UI/cmn/race/ym_CupIconMushroom_Line^s.bntx'), b'icon0')
            self.assertEqual(src('UI/cmn/race/ym_CupIconCharger03_Line^s.bntx'), b'icon15')
            menu = {n: b for n, _, _, b, _ in P.sarc_read(src('UI/cmn/menu.sarc'))['files']}
            self.assertEqual(menu['other.szs'], b'untouched')
            inner = P.sarc_read(P.yaz0_decode(menu['mn_L_CupSlctBtn_00.szs']))['files']
            self.assertEqual([f[3] for f in (inner[0], inner[2])], [b'FLYT layout', b'FLAN animation'])
            textures = P.bntx_textures(inner[1][3])
            fmt, w, h, mips, size, layout, at = textures['ym_CupIconMushroom^h']
            texels = {struct.unpack_from('<H', inner[1][3], at + P._gob(x, y, 16, 2, 1))[0]
                      for y in range(8) for x in range(8)}
            self.assertEqual(texels, {0xF800})  # Boo's (red) icon, decoded and scaled, as RGB565 in every texel
            fmt, w, h, mips, size, layout, at = textures['ym_CupIconCharger04^h']
            self.assertEqual(inner[1][3][at:at + size], bytes(SLOT))  # (a cup this layout leaves alone: unchanged)
            self.assertEqual(src('RaceCommon/JugemObj/JugemObj.bfres'), b'ctgp extra')
            self.assertNotEqual(src('UI/cmn/menu.sarc'), b'never used')  # (4.0.1's, with icons; never CTGP's)
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

    def test_dlc_layout_keeps_the_base_game_original(self):
        with tempfile.TemporaryDirectory() as folder:
            romfs, sd = fixture(Path(folder))
            files = P.plan(P.locate(sd), romfs, 'dlc')
            src = lambda rel: files[rel].read_bytes() if isinstance(files[rel], Path) else files[rel]
            self.assertFalse([rel for rel in files if rel.startswith('Course/') and not rel.startswith('Course/Cnsw_')])
            self.assertFalse(any('WU_FIRST_CIRCUIT' in rel or 'CupIconMushroom' in rel for rel in files))
            self.assertEqual(src('Course/Cnsw_11/course_model.szs'), b'ct0-course_model.szs')
            self.assertEqual(src('Course/Cnsw_62/course_model.szs'), b'ct19-course_model.szs')
            self.assertEqual(src('Audio/Resource/Stream/pBGM_Cnsw_62_F.bfstm'), b'music19F')
            self.assertEqual(src('UI/cmn/menu/ym_CoursePict_DLC20_00^u.bntx'), b'pict19')
            self.assertEqual(src('UI/cmn/race/ym_CupIconCharger11_Line^s.bntx'), b'icon11')
            names, _ = texts(src('UI/USen/message.sarc'))
            self.assertEqual(names['1301'], 'Cup 1301')  # the base game's cups and courses keep their names
            self.assertEqual(names['1401'], 'Course 1401')
            self.assertEqual(names['1461'], 'Wii')
            self.assertEqual(names['11601'], 'CTGP cup 0')
            self.assertEqual(names['11612'], 'CTGP cup 11')
            self.assertEqual(names['11701'], 'GPDX Name 500100')
            self.assertEqual(names['11820'], f'{TAG}Name 500117{END}')  # (slot 20: CT_13)

    def test_bc3_blocks_decode(self):
        data = bytearray(SLOT)
        for by in range(2):
            for bx in range(2):
                at = P._gob(bx, by, 32, 16, 1)
                data[at:at + 16] = RED_BC3
        rgba = P.bc3_rgba(bytes(data), 8, 8, 1)
        self.assertEqual(set(rgba[i:i + 4] for i in range(0, len(rgba), 4)), {bytes((255, 0, 0, 255))})
        half = P.icon_rgb565(bytes((0, 0, 255, 255)) * 4, 2, 2, SLOT, 1, 8, 8)  # (and an upscale stays exact)
        self.assertEqual(struct.unpack_from('<H', half, 0)[0], 0x001F)

    def test_bc7_blocks_decode(self):
        def bits(*fields):  # (value, width) pairs, least significant first
            value = pos = 0
            for v, n in fields:
                value |= v << pos
                pos += n
            return value.to_bytes(16, 'little')
        # mode 6: endpoints all 127 with p-bit 1 -> 255, alpha too: every texel white and opaque
        white = bits((1 << 6, 7), *[(127, 7)] * 8, (1, 1), (1, 1), (0, 63))
        self.assertEqual(set(P.bc7_block(white)), {(255, 255, 255, 255)})
        # mode 1 (2 subsets, partition 0: the right two columns are subset 1): subset 0 black, subset 1 red
        red = bits((1 << 1, 2), (0, 6), (0, 6), (0, 6), (63, 6), (63, 6), (0, 6), (0, 6), (0, 6), (0, 6), (0, 6), (0, 6),
                   (0, 6), (0, 6), (1, 1), (1, 1), *[(0, 3)] * 14, (0, 2), (0, 2))
        texels = P.bc7_block(red)
        self.assertEqual(texels[0], (2, 2, 2, 255))  # (6-bit 0 + p-bit 1 = 7-bit 1, expanded to 2)
        self.assertEqual(texels[3], (255, 2, 2, 255))  # (subset 1, index 0: its red endpoint 63 + p-bit -> 255)
        self.assertEqual([P.BC7_P2[0] >> i & 1 for i in range(4)], [0, 0, 1, 1])
        with self.assertRaises(P.PackError):
            P.bc7_block(bits((1, 1), (0, 127)))  # mode 0: 3 subsets, refused

    def test_late_dlc_layout_places_ctgp_cups_5_to_16(self):
        late = P.slots('dlc-late')
        self.assertEqual([s[0] for s in late], list(range(16, 64)))
        self.assertEqual(late[0][2:], (('Cnsw_11',), 11701, ('Cnsw_11',), 'DLC01'))
        with tempfile.TemporaryDirectory() as folder:
            romfs, sd = fixture(Path(folder))
            files = P.plan(P.locate(sd), romfs, 'dlc-late')
            src = lambda rel: files[rel].read_bytes() if isinstance(files[rel], Path) else files[rel]
            self.assertEqual(src('Course/Cnsw_11/course_model.szs'), b'ct16-course_model.szs')
            self.assertEqual(src('Course/Cnsw_68/course_model.szs'), b'ct63-course_model.szs')
            self.assertEqual(src('UI/cmn/race/ym_CupIconCharger00_Line^s.bntx'), b'icon4')
            names, _ = texts(src('UI/USen/message.sarc'))
            self.assertEqual(names['11601'], 'CTGP cup 4')
            self.assertEqual(names['11612'], 'CTGP cup 15')
            self.assertEqual(names['1301'], 'Cup 1301')

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
            other = import_ctgp.import_pack(sd, romfs, local, layout='dlc')  # another layout: built again
            self.assertEqual((other['layout'], other['courses'], other['base_courses']), ('dlc', 48, 0))
            self.assertNotEqual(other['identity'], manifest['identity'])
            self.assertFalse((pack/'romfs/Course/Gu_FirstCircuit').exists())
            self.assertEqual(manifest['layout'], 'base')
            with self.assertRaises(P.PackError):
                P.locate(root/'game')

    def test_added_layout_gives_ct_courses_cups_of_their_own(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            romfs, sd = fixture(root)
            (romfs/'UI/cmn/menu.sarc').unlink()  # (its icons: with the game's own file, test_cup_icon_textures_...)
            names = [b'Course_Du_Animal_Spring', b'Course_Du_Animal_Spring.bars', b'Course_Du_Animal.bars', b'Course.bars']
            offsets = [len(names[0]) + 1, len(names[0]) + len(names[1]) + 2,
                       len(names[0]) + len(names[1]) + len(names[2]) + 3]
            write(romfs/'Course/Du_Animal_Spring/Course_Du_Animal_Spring.barslist',  # (three banks, as Animal's)
                  b'ARSL\xff\xfe' + struct.pack('<HIIIII', 1, 0, 3, *offsets) + b''.join(n + b'\0' for n in names))
            manifest = import_ctgp.import_pack(sd, romfs, root/'data', layout='added')
            pack = root/'data/ctgp/pack/romfs'
            self.assertEqual((manifest['layout'], manifest['courses']), ('added', 64))
            self.assertEqual(manifest['cups_extra'], P.cups_extra())
            cups = [c.split('=') for c in manifest['cups_extra'].split(';')]
            self.assertEqual([int(label) for label, _ in cups], list(range(30000, 30016)))
            for _, courses in cups:  # (four courses each, on IDs of the game's race courses, none twice in a cup)
                ids = [int(c.split(':')[0]) for c in courses.split(',')]
                self.assertEqual(len(set(ids)), 4)
                self.assertTrue(all(16 <= i < 123 and P.COURSE_ORDER[i - 16][0] != 'B' for i in ids))
            first = pack/'Course/CTGP01'  # (CT 0 over Mario Kart Stadium's files, its sound bank under its name)
            self.assertEqual((first/'course_model.szs').read_bytes(), b'ct0-course_model.szs')
            self.assertEqual((first/'Course_Gu_FirstCircuit.bars').read_bytes(), b'original')
            self.assertEqual((first/'Course_CTGP01.bars').read_bytes(), b'original')
            self.assertEqual((first/'Course_CTGP01.barslist').read_bytes(), P.barslist('CTGP01'))
            self.assertTrue((first/'MapObj/Thing/Thing.bfres').is_file())
            self.assertFalse((first/'Course_CT_00.bars').exists())
            animal = next(P.added_folder(ct) for ct, _, f, *_ in P.slots('added') if f[0] == 'Du_Animal_Spring')
            listed = (pack/'Course'/animal/f'Course_{animal}.barslist').read_bytes()
            self.assertEqual(struct.unpack_from('<I', listed, 12)[0], 3)
            self.assertTrue(listed.endswith(f'Course_{animal}\0Course_{animal}.bars\0Course_Du_Animal.bars\0'
                                            'Course.bars\0'.encode()))
            self.assertFalse((pack/'Course/Gu_FirstCircuit').exists())  # (no game course replaced)
            self.assertFalse(any(p.name.startswith('pBGM_') for p in (pack/'Audio/Resource/Stream').iterdir()))
            self.assertEqual((pack/'Audio/Resource/Stream/CTGP01_N.bfstm').read_bytes(), b'music0N')
            self.assertEqual((pack/'Audio/Resource/Stream/CTGP64_F.bfstm').read_bytes(), b'music63F')
            self.assertEqual((pack/'UI/cmn/menu/ym_CoursePict_CTGP01_00^u.bntx').read_bytes(), b'pict0')
            values, _ = texts((pack/'UI/USen/message.sarc').read_bytes())
            names, cup_names = P.ctgp_names(sd/'atmosphere/contents'/P.TITLE/'romfs/UI/USen/message.sarc')
            self.assertEqual((values['30000'], values['30015']), (cup_names[0], cup_names[15]))
            self.assertEqual((values['31000'], values['31063']), (names[0], names[63]))
            self.assertIn(names[0], values['31100'])
            self.assertEqual(values['1401'], common_labels()['1401'])  # (the game's own texts as they are)
            self.assertEqual(P.verify(root/'data/ctgp/pack')['identity'], manifest['identity'])

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
static const char *settings;
const char *mk8_settings_romfs_overlay(void) { return settings; }
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
    fatal_calls = 0;
    _putenv_s("MK8_ROMFS_OVERLAY", "");
    if (game_romfs_overlay()) return 6;
    settings = "combined-settings";
    if (strcmp(game_romfs_overlay(), settings)) return 7;
    _putenv_s("MK8_ROMFS_OVERLAY", argv[1]);
    _putenv_s("MK8_SETTINGS_ROMFS", settings);
    char expected[8192]; snprintf(expected, sizeof expected, "%s\n%s", argv[1], settings);
    if (strcmp(game_romfs_overlay(), expected) || fatal_calls) return 8;
    _putenv_s("MK8_SETTINGS_ROMFS", "");
    game_romfs_overlay();
    if (fatal_calls != 1) return 9;
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
