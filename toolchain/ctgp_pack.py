"""CTGP Deluxe as an optional track pack for MK8 4.0.1 (phase A: no game code).

CTGP Deluxe 1.1.1 targets the 32-bit MK8 3.0.3: its Skyline plugin registers 64 extra courses (CT_00..CT_3F) in
16 cups. That code cannot run here (4.0.1 is AArch64, recompiled). Its course files, however, use the same formats as
4.0.1 (FRES 0.5.0.3, KCL, AAMP, VFXB, BNTX, BYAML, BARS) and a CTGP course raced at 60 FPS in the slot of
Mario Kart Stadium (docs/STATUS.md, 15/11/2026). So the pack is built as a RomFS overlay that puts CTGP courses into
existing course slots:

- layout 'base': the 48 base-game slots take CTGP cups 1-12 (CT_00..CT_2F), cup by cup, and the first 16 Booster
  Course Pass slots take CTGP cups 13-16 (CT_30..CT_3F). Those only show with the DLC enabled (its RomFS holds just
  contents.dat and version.txt; the courses are in the base RomFS), so the DLC stays optional;
- layout 'dlc': the 12 Booster Course Pass cups take CTGP cups 1-12 and the base game stays original: with the DLC
  enabled, the 48 original courses and 48 CTGP courses at once (CTGP cups 13-16 are left out);
- per slot: the course folder (minus CTGP's own sound bank, the slot's stays), its music (N/F and variants), its
  course picture; the cup icons; and the names in every language's message.sarc (the 4.0.1 file with CTGP's course
  and cup names written into the slot's labels; CTGP's own message.sarc, built for 3.0.3, is not used);
- CTGP's title/menu music, menu sounds and Lakitu model (verified to load on 4.0.1).

CTGP's UI/cmn/common.sarc and menu.sarc crash the 4.0.1 menu and are never used. The pack is read from the player's own
copy and stays in their data folder: CTGP is for personal use, not for redistribution. Nothing from it is in this
repository; the only CTGP-specific data here are numbers (which name label each CT course uses, read from the plugin
of version 1.1.1) and file names.
"""
import hashlib
import json
import os
import shutil
import struct
from pathlib import Path

TITLE = '0100152000022000'
CT_COUNT = 64
# CTGPDX.nro 1.1.1 (sha256 of the plugin): CT index -> CTGPText.msbt course-name label (5001xx) and origin prefix
# (50000x: GP, GP2, GPDX, -, DKR, RMX), recovered by reading the plugin's ARM code statically (docs/STATUS.md).
# None: the plugin builds that label in a way the static read did not resolve; a neutral name is used.
PLUGIN_111 = '66771c3c609330649e95c30226528880e060ad649eadaf37db0356044d351345'
CT_NAMES = [
    (500100, 500002), (500102, None), (500141, None), (500103, None), (500105, 500000), (500131, None),
    (500106, None), (500152, None), (None, 500002), (500108, None), (None, None), (500110, 500004), (500125, None),
    (None, None), (500113, None), (500151, None), (500148, None), (500118, None), (500115, None), (500117, None),
    (500134, None), (500119, None), (500120, None), (500116, 500004), (500121, None), (500122, None), (500123, None),
    (500124, None), (500107, None), (500127, 500005), (500140, None), (500142, None), (500130, None), (500147, None),
    (500101, 500002), (500137, None), (500133, None), (500149, None), (500128, None), (500126, None),
    (500111, 500004), (500146, None), (500112, None), (500104, 500005), (500135, None), (500136, None),
    (500150, 500000), (500145, None), (500143, None), (500144, 500002), (500114, None), (500101, None),
    (500139, 500002), (500132, None), (500101, None), (None, None), (500127, None), (500129, 500004), (500101, None),
    (None, None), (500153, None), (500107, None), (500154, None), (500109, None),
]
CTGP_CUP_LABEL = 700000  # 16 cups, 700000..700015, four CT courses each in order
CTGP_CUP_ICONS = ['Boo', 'Blooper', 'Penguin', 'Packun', 'CloudFlower', 'SuperCrown', 'GrandStar', 'MultiMoon',
                  'SuperLeaf', 'Squid', 'Heart', 'ShineSprite', 'ExtraMushroom', 'IceFlower', 'PranksterComet', 'Coin']
ICON_VARIANTS = ['_130x130^h.bntx', '_Line128x128^s.bntx', '_Line^s.bntx', '_Nml^l.bntx']

# The 4.0.1 slots in cup order: (cup name label, cup icon, [(course folders, name label, music, picture)]).
# Name label n: the short name; n + 100 the tagged one shown at the race intro (BCP: n and n + 100 as well).
BASE_CUPS = [
    (1301, 'Mushroom', [('Gu_FirstCircuit', 1401, 'WU_FIRST_CIRCUIT'), ('Gu_WaterPark', 1402, 'WU_WATER_PARK'),
                        ('Gu_Cake', 1403, 'WU_CAKE'), ('Gu_DossunIseki', 1404, 'WU_DOSSUN')]),
    (1302, 'Flower', [('Gu_MarioCircuit', 1406, 'WU_CIRCUIT'), ('Gu_City', 1405, 'WU_CITY'),
                      ('Gu_HorrorHouse', 1408, 'WU_HAUNTED'), ('Gu_Expert', 1411, 'WU_EXPERT')]),
    (1303, 'Star', [('Gu_Airport', 1409, 'WU_AIRPORT'), ('Gu_Ocean', 1410, 'WU_OCEAN'), ('Gu_Techno', 1407, 'WU_TECHNO'),
                    ('Gu_SnowMountain', 1412, 'WU_SNOW_MOUNTAIN')]),
    (1304, 'Special', [('Gu_Cloud', 1414, 'WU_CLOUD'), ('Gu_Desert', 1413, 'WU_DESERT'),
                       ('Gu_BowserCastle', 1415, 'WU_BOWSER_CASTLE'), ('Gu_RainbowRoad', 1416, 'WU_RAINBOW_ROAD')]),
    (1305, 'Shell', [('Gwii_MooMooMeadows', 1441, 'WII_MOO_MOO_MEADOWS'), ('Gagb_MarioCircuit', 1442, 'AGB_MARIO_CIRCUIT'),
                     ('Gds_PukupukuBeach', 1443, 'DS_PUKUPUKU_BEACH'), ('G64_KinopioHighway', 1445, 'N64_KINOPIO_HIGHWAY')]),
    (1306, 'Banana', [('Ggc_DryDryDesert', 1447, 'GC_DRY_DRY_DESERT'), ('Gsfc_DonutsPlain3', 1446, 'SFC_DONUTS_PLAIN3'),
                      ('G64_PeachCircuit', 1451, 'N64_PEACH_CIRCUIT'), ('G3ds_DKJungle', 1448, '3DS_DK_JUNGLE')]),
    (1307, 'Leaf', [('Gds_WarioStadium', 1454, 'DS_WARIO_STADIUM'), ('Ggc_SherbetLand', 1449, 'GC_SHERBET_LAND'),
                    ('G3ds_MusicPark', 1452, '3DS_MUSIC_PARK'), ('G64_YoshiValley', 1453, 'N64_YOSHI_VALLEY')]),
    (1308, 'Thunder', [('Gds_TickTockClock', 1450, 'DS_TICK_TOCK_CLOCK'), ('G3ds_PackunSlider', 1444, '3DS_PACKUN_SLIDER'),
                       ('Gwii_GrumbleVolcano', 1455, 'WII_GRUMBLE_VOLCANO'), ('G64_RainbowRoad', 1456, 'N64_RAINBOW_ROAD')]),
    (1309, 'DLC02', [('Dgc_YoshiCircuit', 1485, 'GC_YOSHICIRCUIT'), ('Du_ExciteBike', 1482, 'WU_EXCITEBIKE'),
                     ('Du_DragonRoad', 1483, 'WU_DRAGONROAD'), ('Du_MuteCity', 1484, 'WU_MUTECITY')]),
    (1310, 'DLC03', [('Dwii_WariosMine', 1481, 'WII_WARIOSMINE'), ('Dsfc_RainbowRoad', 1486, 'SFC_RAINBOWROAD'),
                     ('Du_IcePark', 1487, 'WU_ICEPARK'), ('Du_Hyrule', 1488, 'WU_HYRULE')]),
    (1311, 'DLC04', [('Dgc_BabyPark', 1490, 'GC_BABYPARK'), ('Dagb_CheeseLand', 1489, 'GBA_CHEESELAND'),
                     ('Du_Woods', 1491, 'WU_WOOD'),
                     (('Du_Animal_Spring', 'Du_Animal_Summer', 'Du_Animal_Autumn', 'Du_Animal_Winter'), 1492,
                      ('WU_ANIMALSPRING', 'WU_ANIMALSUMMER', 'WU_ANIMALAUTUMN', 'WU_ANIMALWINTER'), 'Du_Animal')]),
    (1312, 'DLC05', [('D3ds_NeoBowserCity', 1493, '3DS_NEOBOWSERCITY'), ('Dagb_RibbonRoad', 1494, 'GBA_RIBBONROAD'),
                     ('Du_Metro', 1495, 'WU_METRO'), ('Du_BigBlue', 1496, 'WU_BIGBLUE')]),
]
# Booster Course Pass, in cup order: the course folder of pictures DLC01..DLC48 (the game's own table, as the
# CTGP plugin repeats it). Names 11701.. and cups 11601.. (icons Charger00..11) follow the same order.
BCP_FOLDERS = ['Cnsw_' + n for n in (
    '11', '12', '13', '14', '15', '16', '17', '18', '21', '22', '23', '24', '25', '26', '27', '28',
    '31', '33', '34', '62', '35', '32', '37', '38', '41', '47', '42', '44', '55', '43', '36', '45',
    '65', '46', '63', '58', '48', '53', '52', '61', '54', '56', '66', '64', '51', '67', '57', '68')]
BCP_CUPS = [(11601 + c, f'Charger{c:02d}',
             [(BCP_FOLDERS[4 * c + k], 11701 + 4 * c + k, BCP_FOLDERS[4 * c + k], f'DLC{4 * c + k + 1:02d}')
              for k in range(4)])
            for c in range(12)]
# Where CTGP's cups go, in CTGP cup order.
LAYOUTS = {'base': BASE_CUPS + BCP_CUPS[:4], 'dlc': BCP_CUPS}
PLATFORM_LABELS = range(1461, 1469)  # Wii, GBA, DS, 3DS, N64, SNES, GCN, Tour: retro prefixes shown before a name
# CTGP files verified to load on 4.0.1 outside the course slots (race-time-trial-ssw00gbi, -lzz6p1u3)
EXTRAS = ['Audio/Resource/Stream/BGM_MENU_LOCAL.bfstm', 'Audio/Resource/Stream/BGM_MENU_SINGLE_MULTI.bfstm',
          'Audio/Resource/Stream/BGM_TITLE.bfstm', 'Audio/Resource/Stream/BGM_TITLE_SHORT.bfstm',
          'Audio/Static/StaticSysMenu.bars', 'RaceCommon/JugemObj/JugemObj.bfres']
LANGUAGES = ['CNzh', 'EUde', 'EUen', 'EUes', 'EUfr', 'EUit', 'EUnl', 'EUpt', 'EUru', 'JPja', 'KRko', 'TWzh', 'USen',
             'USes', 'USfr']


class PackError(ValueError):
    pass


# ------------------------------------------------------------------ SARC / MSBT (little endian, as on the Switch)
def sarc_read(data):
    if data[:4] != b'SARC' or data[6:8] != b'\xff\xfe':
        raise PackError('SARC inválido')
    data_off = struct.unpack_from('<I', data, 0xC)[0]
    if data[0x14:0x18] != b'SFAT':
        raise PackError('SARC inválido')
    count = struct.unpack_from('<H', data, 0x14 + 6)[0]
    nodes = [struct.unpack_from('<IIII', data, 0x20 + 16 * i) for i in range(count)]
    names_at = 0x20 + 16 * count + 8
    files = []
    for name_hash, attr, start, end in nodes:
        at = names_at + (attr & 0xFFFF) * 4
        name = data[at:data.index(b'\0', at)].decode('utf-8')
        files.append((name, name_hash, attr, data[data_off + start:data_off + end], start))
    return {'header': data[:0x14], 'sfat': data[0x14:0x20], 'names': data[names_at - 8:data_off], 'files': files,
            'data_off': data_off}


def _align_of(offset):
    a = 1
    while a < 0x2000 and offset % (a * 2) == 0:
        a *= 2
    return max(a, 4)


def sarc_write(parsed, replace):
    """The same archive (node order, hashes, names, data alignment) with some files' contents replaced."""
    out_nodes, blobs, cursor = [], [], 0
    for name, name_hash, attr, blob, start in parsed['files']:
        blob = replace.get(name, blob)
        align = _align_of(start) if start else 0x80
        cursor = (cursor + align - 1) & ~(align - 1)
        out_nodes.append((name_hash, attr, cursor, cursor + len(blob)))
        blobs.append((cursor, blob))
        cursor += len(blob)
    head = bytearray(parsed['header'] + parsed['sfat'])
    for node in out_nodes:
        head += struct.pack('<IIII', *node)
    head += parsed['names']
    data_off = parsed['data_off']
    if len(head) > data_off:
        raise PackError('SARC: tabela maior que o original')
    head += bytes(data_off - len(head))
    body = bytearray(cursor)
    for at, blob in blobs:
        body[at:at + len(blob)] = blob
    out = head + body
    struct.pack_into('<I', out, 8, len(out))
    return bytes(out)


def _sections(data):
    if data[:8] != b'MsgStdBn' or data[8:10] != b'\xff\xfe':
        raise PackError('MSBT inválido')
    count = struct.unpack_from('<H', data, 0xE)[0]
    at, out = 0x20, []
    for _ in range(count):
        size = struct.unpack_from('<I', data, at + 4)[0]
        out.append((data[at:at + 4], at, size))
        at += 0x10 + ((size + 15) & ~15)
    return out


def msbt_read(data):
    labels, texts = {}, []
    for magic, at, size in _sections(data):
        body = data[at + 0x10:at + 0x10 + size]
        if magic == b'LBL1':
            for i in range(struct.unpack_from('<I', body, 0)[0]):
                count, o = struct.unpack_from('<II', body, 4 + 8 * i)
                for _ in range(count):
                    n = body[o]
                    labels[body[o + 1:o + 1 + n].decode('ascii')] = struct.unpack_from('<I', body, o + 1 + n)[0]
                    o += 1 + n + 4
        elif magic == b'TXT2':
            n = struct.unpack_from('<I', body, 0)[0]
            offsets = [struct.unpack_from('<I', body, 4 + 4 * i)[0] for i in range(n)] + [size]
            texts = [body[offsets[i]:offsets[i + 1]].decode('utf-16-le') for i in range(n)]
    return labels, texts


def msbt_write(data, texts):
    """The same MSBT with TXT2 rebuilt from texts (each ends with its NUL); every other section unchanged."""
    out = bytearray(data[:0x20])
    for magic, at, size in _sections(data):
        if magic == b'TXT2':
            encoded = [t.encode('utf-16-le') for t in texts]
            table = 4 + 4 * len(encoded)
            body = bytearray(struct.pack('<I', len(encoded)))
            offset = table
            for blob in encoded:
                body += struct.pack('<I', offset)
                offset += len(blob)
            for blob in encoded:
                body += blob
            section = bytearray(data[at:at + 0x10])
            struct.pack_into('<I', section, 4, len(body))
            section += body
        else:
            section = bytearray(data[at:at + 0x10 + size])
        section += b'\xab' * (-len(section) % 16)
        out += section
    struct.pack_into('<I', out, 0x12, len(out))
    return bytes(out)


def set_texts(data, values):
    """values: label -> text (without the final NUL). Keeps a tagged text's control tags around the new name."""
    labels, texts = msbt_read(data)
    texts = list(texts)
    for label, new in values.items():
        if label not in labels:
            raise PackError(f'Rótulo {label} ausente')
        i = labels[label]
        old = texts[i].rstrip('\0')
        # A control tag: 0x0E, group, type, parameter size in bytes, parameters. "<tag>Name<tag>" keeps both tags.
        head = 4 + ord(old[3]) // 2 if len(old) >= 4 and old[0] == '\x0e' else 0
        tail = old.rfind('\x0e')
        if head and head <= tail and len(old) >= tail + 4 and len(old) == tail + 4 + ord(old[tail + 3]) // 2:
            texts[i] = old[:head] + new + old[tail:] + '\0'
        else:
            texts[i] = new + '\0'
    return msbt_write(data, texts)


# ------------------------------------------------------------------ the pack
def locate(source):
    """The CTGP title folder in a selection: the SD root, atmosphere/, contents/ or the title folder itself."""
    source = Path(source)
    for candidate in (source, source / TITLE, source / 'contents' / TITLE, source / 'atmosphere/contents' / TITLE):
        if (candidate / 'romfs/Course/CT_00').is_dir():
            return candidate
    raise PackError('Pasta do CTGP Deluxe não encontrada. Selecione a pasta extraída do ZIP (com atmosphere/).')


def sha256(path):
    with open(path, 'rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def slots(layout='base'):
    """[(ct index, cup index, folders, name label, music bases, picture)], cup by cup."""
    if layout not in LAYOUTS:
        raise PackError(f'Disposição desconhecida: {layout}')
    out = []
    for c, (_, _, courses) in enumerate(LAYOUTS[layout]):
        for k, course in enumerate(courses):
            folders, label, music = course[0], course[1], course[2]
            picture = course[3] if len(course) > 3 else folders
            folders = folders if isinstance(folders, tuple) else (folders,)
            music = music if isinstance(music, tuple) else (music,)
            out.append((4 * c + k, c, folders, label, music, picture))
    return out


def plan(title, romfs, layout='base'):
    """Every overlay file: (overlay path, source path or bytes). Validates both trees first."""
    title, romfs = Path(title), Path(romfs)
    ct_romfs = title / 'romfs'
    for i in range(CT_COUNT):
        course = ct_romfs / f'Course/CT_{i:02X}'
        for name in ('course_model.szs', 'course_kcl.szs', 'course_muunt.byaml'):
            if not (course / name).is_file():
                raise PackError(f'CTGP incompleto: Course/CT_{i:02X}/{name}')
    if not (romfs / 'Course/Gu_FirstCircuit/course_model.szs').is_file():
        raise PackError('RomFS do MK8 4.0.1 não encontrado.')
    files = {}
    stream = romfs / 'Audio/Resource/Stream'
    for ct, cup, folders, label, music, picture in slots(layout):
        src = ct_romfs / f'Course/CT_{ct:02X}'
        for folder in folders:
            if not (romfs / 'Course' / folder).is_dir():
                raise PackError(f'Vaga ausente no 4.0.1: {folder}')
            for path in sorted(src.rglob('*')):
                if path.is_file() and path.name not in (f'Course_CT_{ct:02X}.bars', f'Course_CT_{ct:02X}.barslist'):
                    files[f'Course/{folder}/{path.relative_to(src).as_posix()}'] = path
        for base in music:
            # the slot's every music stream (N, F, LAP2, REPLAY...) plays CTGP's normal or final-lap theme
            for target in sorted(stream.glob(f'pBGM_{base}_*.bfstm')):
                kind = 'F' if target.stem.endswith('_F') else 'N'
                ct_music = ct_romfs / f'Audio/Resource/Stream/pBGM_CT_{ct:02X}_{kind}.bfstm'
                if ct_music.is_file():
                    files[f'Audio/Resource/Stream/{target.name}'] = ct_music
        pict = ct_romfs / f'UI/cmn/menu/ym_CoursePict_CT_{ct:02X}_00^u.bntx'
        if pict.is_file() and (romfs / f'UI/cmn/menu/ym_CoursePict_{picture}_00^u.bntx').is_file():
            files[f'UI/cmn/menu/ym_CoursePict_{picture}_00^u.bntx'] = pict
    for c, (_, icon, _) in enumerate(LAYOUTS[layout]):
        for variant in ICON_VARIANTS:
            src = ct_romfs / f'UI/cmn/race/ym_CupIcon{CTGP_CUP_ICONS[c]}{variant}'
            if src.is_file() and (romfs / f'UI/cmn/race/ym_CupIcon{icon}{variant}').is_file():
                files[f'UI/cmn/race/ym_CupIcon{icon}{variant}'] = src
    for rel in EXTRAS:
        if (ct_romfs / rel).is_file() and (romfs / rel).is_file():
            files[rel] = ct_romfs / rel
    for lang in LANGUAGES:
        target = romfs / f'UI/{lang}/message.sarc'
        if target.is_file():
            files[f'UI/{lang}/message.sarc'] = messages(target, ct_romfs / f'UI/{lang}/message.sarc',
                                                        ct_romfs / 'UI/USen/message.sarc', layout)
    return files


def ctgp_names(ctgp_sarc):
    files = {name: blob for name, _, _, blob, _ in sarc_read(Path(ctgp_sarc).read_bytes())['files']}
    if 'CTGPText.msbt' not in files:
        raise PackError('CTGPText.msbt ausente no message.sarc do CTGP')
    labels, texts = msbt_read(files['CTGPText.msbt'])
    text = {int(k): texts[v].rstrip('\0') for k, v in labels.items() if k.isdigit()}
    names = []
    for i, (name, prefix) in enumerate(CT_NAMES):
        value = text.get(name) if name else None
        if not value or value.startswith('-'):
            value = f'CTGP {i + 1:02d}'
        elif prefix in (500000, 500001, 500002, 500004, 500005) and text.get(prefix):
            value = f'{text[prefix]} {value}'
        names.append(value)
    cups = [text.get(CTGP_CUP_LABEL + c) or f'CTGP {c + 1}' for c in range(16)]
    return names, cups


def messages(target, ctgp_sarc, fallback, layout='base'):
    """The 4.0.1 message.sarc of one language with CTGP's course and cup names in the slots' labels."""
    names, cups = ctgp_names(ctgp_sarc if Path(ctgp_sarc).is_file() else fallback)
    parsed = sarc_read(Path(target).read_bytes())
    common = next(blob for name, _, _, blob, _ in parsed['files'] if name == 'Common.msbt')
    # (the retro prefixes, Wii, GBA..., belong to the base game's slots: emptied only when CTGP takes them)
    values = {str(label): '' for label in PLATFORM_LABELS} if layout == 'base' else {}
    for c, (cup_label, _, _) in enumerate(LAYOUTS[layout]):
        values[str(cup_label)] = cups[c]
    for ct, _, _, label, _, _ in slots(layout):
        values[str(label)] = names[ct]
        values[str(label + 100)] = names[ct]
    return sarc_write(parsed, {'Common.msbt': set_texts(common, values)})


def build(title, romfs, output, link=True, layout='base'):
    """Writes the overlay (romfs/ + manifest.json) into output, which must not exist. Returns the manifest."""
    title, output = locate(title), Path(output)
    files = plan(title, romfs, layout)
    stage = output.with_name(output.name + '.stage')
    if output.exists() or stage.exists():
        raise PackError(f'Destino já existe: {output}')
    record = {}
    try:
        for rel, source in sorted(files.items()):
            target = stage / 'romfs' / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            if isinstance(source, bytes):
                target.write_bytes(source)
            else:
                linked = False
                if link:
                    try:
                        os.link(source, target)
                        linked = True
                    except OSError:
                        pass
                if not linked:
                    shutil.copyfile(source, target)
            record[rel] = {'bytes': target.stat().st_size}
        plugin = title / 'romfs/skyline/plugins/CTGPDX.nro'
        manifest = {
            'schema': 1, 'kind': 'ctgp-deluxe', 'title': TITLE, 'source': str(title),
            'plugin_sha256': sha256(plugin) if plugin.is_file() else None,
            'names_verified': sha256(plugin) == PLUGIN_111 if plugin.is_file() else False,
            'layout': layout, 'courses': len(slots(layout)),
            'base_courses': sum(1 for s in slots(layout) if not s[2][0].startswith('Cnsw_')),
            'files': record, 'gameplay_verified': False,
        }
        manifest['identity'] = hashlib.sha256(json.dumps({k: manifest[k] for k in ('plugin_sha256', 'layout', 'files')},
                                                         sort_keys=True).encode()).hexdigest()
        (stage / 'manifest.json').write_text(json.dumps(manifest, indent=1), encoding='utf-8')
        stage.rename(output)
        return manifest
    except BaseException:
        shutil.rmtree(stage, ignore_errors=True)
        raise


def verify(output):
    """The overlay as recorded: every file present with its size, nothing extra. Returns the manifest."""
    output = Path(output)
    manifest = json.loads((output / 'manifest.json').read_text(encoding='utf-8'))
    if manifest.get('kind') != 'ctgp-deluxe' or manifest.get('schema') != 1:
        raise PackError('Manifesto do CTGP inválido')
    romfs = output / 'romfs'
    for rel, entry in manifest['files'].items():
        path = romfs / rel
        if not path.is_file() or path.stat().st_size != entry['bytes']:
            raise PackError(f'CTGP: arquivo ausente ou alterado: {rel}')
    present = sum(1 for p in romfs.rglob('*') if p.is_file())
    if present != len(manifest['files']):
        raise PackError('CTGP: arquivos inesperados na sobreposição')
    return manifest
