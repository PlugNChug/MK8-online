"""Private layout derivation, archive bounds, and preservation of input assets."""
import hashlib
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'toolchain'))
import settings_layout as layout


def archive(items):
    table, names, payload = bytearray(), bytearray(), bytearray()
    for key, data in items:
        if isinstance(key, str):
            flags = 0x1000000 | len(names)//4
            names.extend(key.encode()+b'\0')
            names.extend(bytes(-len(names) % 4))
            key = 0
        else:
            flags = 0
        table.extend(struct.pack('<4I', key, flags, len(payload), len(payload)+len(data)))
        payload.extend(data)
    start = 20+12+len(table)+8+len(names)
    return (b'SARC\x14\x00\xff\xfe'+struct.pack('<III', start+len(payload), start, 0x100)
            + b'SFAT'+struct.pack('<HHI', 12, len(items), 0x65)+table
            + b'SFNT\x08\x00\x00\x00'+names+payload)


def button_layout():
    sections = []
    for i in (6,0,1,2,3,4,5):
        section = bytearray(84)
        section[:8] = b'prt1'+struct.pack('<I', len(section))
        section[12:36] = f'L_SubBtn_{i:02}'.encode().ljust(24, b'\0')
        struct.pack_into('<f', section, 44, -128.0 if i==6 else i*90.0-39)
        sections.append(bytes(section))
    body = b''.join(sections)
    return b'FLYT\xff\xfe\x14\x00'+struct.pack('<IIHH', 0x05050000, 20+len(body), 7, 0)+body


def page_layout():
    body=bytearray(84)
    body[:8]=b'pan1'+struct.pack('<I',84)
    body[12:36]=b'RootPane'.ljust(24,b'\0')
    return b'FLYT\xff\xfe\x14\x00'+struct.pack('<IIHH', 0x05050000, 20+len(body), 1, 0)+body


def bntx(name,width,height,size,pixel):
    out=bytearray(512+size); out[:4]=b'BNTX'
    struct.pack_into('<IQ',out,36,1,64); struct.pack_into('<Q',out,64,80)
    out[80:84]=b'BRTI'; struct.pack_into('<H',out,80+22,1)
    struct.pack_into('<III',out,80+28,0x1d01,0,width)
    struct.pack_into('<I',out,80+40,height); struct.pack_into('<I',out,80+80,size)
    struct.pack_into('<Q',out,80+96,224); struct.pack_into('<Q',out,80+112,496)
    out[226:226+len(name)+1]=name.encode()+b'\0'; struct.pack_into('<Q',out,496,512)
    struct.pack_into('<Q',out,56,320); out[320:328]=b'_DIC'+struct.pack('<I',1)
    struct.pack_into('<IHHQ',out,328,0xffffffff,1,0,0)
    struct.pack_into('<IHHQ',out,344,0,0,1,224)
    out[512:]=bytes([pixel])*size
    return bytes(out)


class SettingsLayoutTests(unittest.TestCase):
    def test_combined_pack_keeps_settings_and_cup_assets_and_preserves_inputs(self):
        import subprocess
        name = 'mn_L_CupSlctBtn_00.szs'
        settings = archive([(name, b'base-cups'), ('mn_TopMenuNX_00.szs', b'settings-menu'),
                            ('mn_L_SetBtn_00.szs', b'wrench')])
        pack = archive([(name, b'ctgp-cups'), ('mn_TopMenuNX_00.szs', b'base-menu')])
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root/'menu.sarc').write_bytes(settings)
            (root/'common.sarc').write_bytes(b'settings-common')
            (root/'pack.sarc').write_bytes(pack)
            subprocess.run([sys.executable, layout.__file__, str(root/'menu.sarc'),
                            str(root/'pack.sarc'), str(root/'combined')], check=True, capture_output=True)
            result = (root/'combined/UI/cmn/menu.sarc').read_bytes()
            self.assertEqual({k:v for k,_,v in layout.entries(result)},
                             {name:b'ctgp-cups','mn_TopMenuNX_00.szs':b'settings-menu','mn_L_SetBtn_00.szs':b'wrench'})
            self.assertEqual((root/'combined/UI/cmn/common.sarc').read_bytes(), b'settings-common')
            self.assertEqual((root/'menu.sarc').read_bytes(), settings)
            self.assertEqual((root/'pack.sarc').read_bytes(), pack)
        for invalid in (b'corrupt', archive([('other',b'x')]), archive([(name,b'x'),(name,b'y')])):
            with self.assertRaises(ValueError):
                layout.combine_pack_menu(settings, invalid)

    def test_adds_native_pane_only_to_private_copy_and_preserves_other_entries(self):
        original = button_layout()
        nested = archive([(layout.LAYOUT_HASH, original), (5, b'animation')])
        source = archive([('mn_TopMenuNX_00.szs', nested), ('other', b'keep'),
                          ('mn_L_SubBtn_00.szs', b'original-button'),
                          ('mn_CompeInfo_00.szs', archive([(4,b'BNTX-test')]))])
        common = archive([('cm_Page_Dummy.szs', archive([(0xdd954d60, page_layout())])),
                          ('cm_Page_Cursor.szs', archive([(0xaa119e16, page_layout())])), ('other', b'common-keep'),
                          ('cm_L_Scroll_ListTitle_00.szs',b'base-label'),('cm_L_WinBtn_00.szs',b'base-button')])
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            path = root/'romfs/UI/cmn/menu.sarc'
            path.parent.mkdir(parents=True)
            path.write_bytes(source)
            common_path=path.with_name('common.sarc'); common_path.write_bytes(common)
            output = root/'build'
            with patch.object(layout, 'MENU_SHA256', hashlib.sha256(source).hexdigest()):
                with patch.object(layout, 'COMMON_SHA256', hashlib.sha256(common).hexdigest()):
                    with patch.object(layout, 'settings_icon_button', return_value=b'private-wrench-button'):
                        with patch.object(layout, 'dialog_label',return_value=b'private-label'), patch.object(layout, 'dialog_button',return_value=b'private-dialog-button'):
                            target = layout.prepare(root/'romfs', output)
                            again = layout.prepare(root/'romfs', output).read_bytes()
            self.assertEqual(path.read_bytes(), source)
            self.assertEqual(common_path.read_bytes(), common)
            controller = output/'controller-menu/romfs/UI/cmn/common.sarc'
            self.assertEqual(controller.read_bytes(), (output/layout.COMMON_RELATIVE).read_bytes())
            self.assertFalse(controller.with_name('menu.sarc').exists())
            derived={k:v for k,_,v in layout.entries((output/layout.COMMON_RELATIVE).read_bytes())}
            self.assertEqual(derived['other'], b'common-keep')
            self.assertEqual(derived['cm_L_WinBtn_00.szs'],b'base-button')
            self.assertEqual(derived['cm_L_SetBtn_00.szs'],b'private-dialog-button')
            self.assertEqual(derived['cm_L_SetHints_00.szs'],b'private-label')
            originals = {k:v for k,_,v in layout.entries(common)}
            self.assertEqual(derived['cm_Page_Dummy.szs'], originals['cm_Page_Dummy.szs'])
            private = dict((k,v) for k,_,v in layout.entries(layout.yaz0(derived['cm_Page_SetUI.szs'])))
            cursor=dict((k,v) for k,_,v in layout.entries(layout.yaz0(derived['cm_Page_Cursor.szs'])))
            self.assertIn(b'N_Settings', private[layout.name_hash('blyt/cm_Page_SetUI.bflyt')])
            self.assertIn(b'L_SettingsRoot', cursor[0xaa119e16])
            self.assertIn(b'cm_Page_SetUI\0', cursor[0xaa119e16])
            self.assertEqual(target.read_bytes(), again)
            entries = {key: data for key, _, data in layout.entries(again)}
            self.assertEqual(entries['other'], b'keep')
            self.assertEqual(entries['mn_L_SubBtn_00.szs'],b'original-button')
            self.assertEqual(entries['mn_L_SetBtn_00.szs'],b'private-wrench-button')
            inner = {key: data for key, _, data in layout.entries(layout.yaz0(entries['mn_TopMenuNX_00.szs']))}
            self.assertEqual(inner[5], b'animation')
            result = inner[layout.LAYOUT_HASH]
            self.assertEqual(struct.unpack_from('<H', result, 16)[0], 8)
            self.assertEqual(result.count(b'L_SubBtn_07'), 1)
            panes={s[12:36].split(b'\0')[0]:s for _,s in layout.sections(result) if s[:4]==b'prt1'}
            self.assertEqual(struct.unpack_from('<f',panes[b'L_SubBtn_07'],44)[0],-128)
            for _,s in layout.sections(original):
                name=s[12:36].split(b'\0')[0]
                self.assertEqual(struct.unpack_from('<f',panes[name],44)[0],struct.unpack_from('<f',s,44)[0]+90)
            with self.assertRaises(ValueError):
                layout.prepare(root/'romfs', output)

    def test_page_tree_uses_shared_game_parts_and_starts_hidden(self):
        original = button_layout()
        pas, pae = b'pas1\x08\0\0\0', b'pae1\x08\0\0\0'
        body = pas+original[20:]+pae
        header = bytearray(original[:20])
        struct.pack_into('<IH',header,12,20+len(body),9)
        result = layout.add_page(bytes(header)+body)
        pos, depth, rows, headings = 20, 0, [], []
        while pos < len(result):
            size, = struct.unpack_from('<I',result,pos+4)
            section = result[pos:pos+size]
            if section[:4]==b'pas1': depth+=1
            if section[:4]==b'pae1': depth-=1
            self.assertGreaterEqual(depth,0)
            if section[:4]==b'pan1':
                self.assertIn(section[12:36].split(b'\0')[0],(b'N_Settings',b'N_SettingsPopup'))
                self.assertFalse(section[8]&1)
            if b'cm_L_Scroll_List_01\0' in section:
                rows.append(struct.unpack_from('<f',section,48)[0])
            if b'cm_L_Scroll_List_00\0' in section:
                headings.append(struct.unpack_from('<f',section,48)[0])
            pos+=size
        self.assertEqual(rows,[60,0,-60,-120,-180])
        self.assertEqual(headings,[150])
        self.assertEqual(depth,0)
        self.assertEqual(result.count(b'cm_L_BackBtn_00\0'),1)
        self.assertEqual(result.count(b'cm_L_SetBtn_00\0'),2)
        self.assertEqual(result.count(b'cm_L_BtnCursor_00\0'),1)
        with self.assertRaises(ValueError): layout.add_page(result)
        with self.assertRaises(ValueError): layout.add_page(original)

    def test_rejects_corrupt_sizes_counts_offsets_and_compressed_streams(self):
        source = archive([(4, b'keep')])
        for at, value in ((8, 0), (12, len(source)+1), (40, 0xffffff), (44, 0xffffff)):
            corrupt = bytearray(source)
            struct.pack_into('<I', corrupt, at, value)
            with self.assertRaises(ValueError):
                layout.entries(corrupt)
        for stream in (b'Yaz0', b'Yaz0'+struct.pack('>I', layout.LIMIT+1)+bytes(8),
                       b'Yaz0'+struct.pack('>I', 1)+bytes(8)+b'\0\0\0'):
            with self.assertRaises(ValueError):
                layout.yaz0(stream)
        with self.assertRaises(ValueError):
            layout.add_button(layout.add_button(button_layout()))
        with self.assertRaises(ValueError):
            layout.replace_entry(source, 5, b'absent')

    def test_wrench_clone_preserves_original_texture_names_pixels_and_animation_contract(self):
        old,new='mn_L_SubBtn_00','mn_L_SetBtn_00'
        image=bntx('SubBtnIconPlayRecord_00^t',170,105,1024,7)
        source=bntx('tc_SettingsIcon_Kart^s',40,40,256,123)
        pane=bytearray(136); pane[:8]=b'pic1'+struct.pack('<I',136); pane[12:36]=b'P_Icon_00'.ljust(24,b'\0')
        pane[84:100]=bytes((255,255,255,190))*4
        txl=bytearray(16);txl[:4]=b'txl1';struct.pack_into('<HI',txl,8,1,0);struct.pack_into('<I',txl,12,4)
        txl.extend(b'SubBtnIconPlayRecord_00^t\0');txl.extend(bytes(-len(txl)%4));struct.pack_into('<I',txl,4,len(txl))
        header=b'FLYT\xff\xfe\x14\x00'+struct.pack('<IIHH',0x05050000,156+len(txl),2,0)
        tag=b'pat1'+struct.pack('<I',16)+b'Decide\0\0'
        animation=b'FLAN\xff\xfe\x14\x00'+struct.pack('<IIHH',0x05050000,36,1,0)+tag
        original=archive([(layout.name_hash('blyt/'+old+'.bflyt'),header+txl+pane),
                          (layout.name_hash('anim/'+old+'_Decide.bflan'),animation),(8,image)])
        result={k:v for k,_,v in layout.entries(layout.yaz0(layout.settings_icon_button(original,source)))}
        self.assertEqual(result[layout.name_hash('anim/'+new+'_Decide.bflan')],animation)
        self.assertEqual(struct.unpack_from('<2f',result[layout.name_hash('blyt/'+new+'.bflyt')],20+len(txl)+76),(72,72))
        colors=result[layout.name_hash('blyt/'+new+'.bflyt')][20+len(txl)+84:20+len(txl)+100]
        self.assertEqual(colors,bytes((64,64,64,190))*2+bytes((40,40,40,190))*2)
        self.assertEqual(pane[84:100],bytes((255,255,255,190))*4)
        at,pixels,size=layout.texture(result[8],'tc_SettingsIcon_Kart^s')
        self.assertEqual(struct.unpack_from('<II',result[8],at+36),(40,40))
        self.assertEqual(result[8][pixels:pixels+size],bytes([123])*256)
        self.assertEqual(image[512:],bytes([7])*1024)
        corrupt=bytearray(source); struct.pack_into('<I',corrupt,80+80,0xffffffff)
        with self.assertRaises(ValueError):layout.settings_icon_button(original,bytes(corrupt))

    def test_dialog_label_has_room_for_the_question_and_no_title_background(self):
        pane=bytearray(196);pane[:8]=b'txt1'+struct.pack('<I',196)
        pane[8]=1;pane[12:36]=b'T_Title_00'.ljust(24,b'\0')
        window=bytearray(140);window[:8]=b'wnd1'+struct.pack('<I',140);window[8]=1
        header=b'FLYT\xff\xfe\x14\x00'+struct.pack('<IIHH',0x05050000,356,2,0)
        source=archive([(layout.name_hash('blyt/cm_L_Scroll_ListTitle_00.bflyt'),header+window+pane)])
        result={k:v for k,_,v in layout.entries(layout.yaz0(layout.dialog_label(source)))}
        out=result[layout.name_hash('blyt/cm_L_SetLabel_00.bflyt')]
        self.assertEqual(out[20:24],b'pan1')
        self.assertTrue(out[20+8]&1)
        at=20+84
        self.assertGreaterEqual(struct.unpack_from('<H',out,at+84)[0],2*(len('Some settings require a restart.\nRestart now?')+1))
        self.assertEqual(out[at+104:at+112],bytes((30,30,30,255))*2)
        self.assertEqual(out[at+92],4) # Center labels inside the native button frames.

    def test_hint_archive_reuses_stock_frame_for_both_rows_and_adds_hidden_key_action_groups(self):
        window=bytearray(layout.pane(b'wnd1','W_Title_00')+bytes(56))
        struct.pack_into('<I',window,4,len(window))
        struct.pack_into('<f',window,80,56)
        struct.pack_into('<4H',window,92,200,200,56,56)
        struct.pack_into('<IH',window,132,136,3)
        title=bytearray(layout.pane(b'txt1','T_Title_00')+bytes(112))
        struct.pack_into('<I',title,4,len(title))
        title[9]=4
        parts=[window,b'pas1\x08\0\0\0',title,b'pae1\x08\0\0\0']
        body=b''.join(parts)
        bflyt=b'FLYT\xff\xfe\x14\x00'+struct.pack('<IIHH',0x05050000,20+len(body),len(parts),0)+body
        source=archive([(layout.name_hash('blyt/cm_L_Scroll_ListTitle_00.bflyt'),bflyt)])
        result={k:v for k,_,v in layout.entries(layout.yaz0(layout.dialog_label(source,hints=True)))}
        derived=[s for _,s in layout.sections(result[layout.name_hash('blyt/cm_L_SetHints_00.bflyt')])]
        single,double=[s for s in derived if s[:4]==b'wnd1']
        self.assertEqual(single[36:],window[36:])
        self.assertEqual(double[8],single[8]&~1)
        self.assertEqual(single[12:36].rstrip(b'\0'),b'W_HintSingle')
        self.assertEqual(double[12:36].rstrip(b'\0'),b'W_HintDouble')
        self.assertEqual(struct.unpack_from('<4H',double,92),(200,200,112,112))
        self.assertEqual(double[100:136],window[100:136])
        self.assertEqual(struct.unpack_from('<H',single,136)[0],3)
        self.assertEqual(struct.unpack_from('<H',double,136)[0],4)
        self.assertEqual(derived[-2],title)
        for i in range(4):
            cap,key,verb=derived[4+3*i:7+3*i]
            self.assertEqual([s[12:36].rstrip(b'\0') for s in (cap,key,verb)],
                             [f'P_Key{i}'.encode(),f'T_Key{i}'.encode(),f'T_Verb{i}'.encode()])
            self.assertTrue(all(s[8]==title[8]&~1 and s[9]==4 for s in (cap,key,verb)))
            self.assertEqual(cap[:4],b'pic1')
            self.assertEqual(struct.unpack_from('<HBB',cap,100),(2,0,0))
            self.assertEqual(key[104:112],bytes((24,30,38,255))*2)
            self.assertEqual(verb[104:112],title[104:112])

    def test_hint_texture_has_straight_16_pixel_cells_and_a_private_name(self):
        from mii_resource_nx import deswizzle
        name='tc_Check_64x32^r'
        source=bytearray(bntx(name,32,64,1024,17))
        struct.pack_into('<I',source,80+52,1)
        source=bytes(source)
        texlist=bytearray(16);texlist[:8]=b'txl1'+struct.pack('<I',36)
        struct.pack_into('<HI',texlist,8,1,0);struct.pack_into('<I',texlist,12,4)
        texlist+=name.encode().ljust(20,b'\0')
        header=b'FLYT\xff\xfe\x14\x00'+struct.pack('<IIHH',0x05050000,56,1,0)
        path='blyt/cm_L_Scroll_ListTitle_00.bflyt'
        original=archive([(layout.name_hash(path),header+texlist),(8,source)])
        result={k:v for k,_,v in layout.entries(layout.yaz0(layout.dialog_label(original,hints=True)))}
        tex=result[8]
        self.assertIn(b'tc_HintGrid^r\0',result[layout.name_hash('blyt/cm_L_SetHints_00.bflyt')])
        _,pixels,size=layout.texture(tex,'tc_HintGrid^r')
        self.assertEqual(source[512:],bytes([17])*1024)
        blocks=deswizzle(tex[pixels:pixels+size],8,16,8,1)
        for y in range(64):
            for x in range(32):
                i=(y//4*8+x//4)*8
                self.assertEqual(blocks[i],255 if (x//16+y//16)%2==0 else 0)
                self.assertEqual(blocks[i+2:i+8],bytes(6)) # Every texel selects endpoint zero.
        with self.assertRaises(ValueError): layout.texture(tex,name)
        invalid=bytearray(source);struct.pack_into('<I',invalid,80+52,0)
        with self.assertRaises(ValueError): layout.square_hint_checker(invalid)

    def test_only_double_height_material_repeats_checker_vertically(self):
        frame=bytearray(60)
        frame[:28]=b'W_Title_00LT'.ljust(28,b'\0')
        frame[44:52]=bytes((0,0,4,4,1,0,5,4))
        section=b'mat1'+struct.pack('<IHH4I',100,4,0,28,32,36,40)+bytes(range(12))+frame
        result=layout.repeat_hint_checker(section)
        self.assertEqual(struct.unpack_from('<IH',result,4),(164,5))
        self.assertEqual(struct.unpack_from('<5I',result,12),(32,36,40,44,104))
        self.assertEqual(result[32:104],section[28:])
        self.assertEqual(result[104+28:104+51],frame[28:51])
        self.assertEqual(result[104+51],5)
        self.assertEqual(result[104+52:],frame[52:])
        corrupt=bytearray(section);corrupt[40+44]=1
        with self.assertRaises(ValueError): layout.repeat_hint_checker(corrupt)

    def test_dialog_label_is_inside_animated_button_after_its_background(self):
        old,new='cm_L_WinBtn_00','cm_L_SetBtn_00'
        parts=[layout.pane(b'pan1','N_WinBtn_00'),b'pas1\x08\0\0\0',
               layout.pane(b'wnd1','W_Btn_00'),layout.pane(b'txt1','T_BtnText_00'),b'pae1\x08\0\0\0']
        body=b''.join(parts)
        source=b'FLYT\xff\xfe\x14\x00'+struct.pack('<IIHH',0x05050000,20+len(body),len(parts),0)+body
        tag=b'pat1'+struct.pack('<I',16)+b'Decide\0\0'
        animation=b'FLAN\xff\xfe\x14\x00'+struct.pack('<IIHH',0x05050000,36,1,0)+tag
        original=archive([(layout.name_hash('blyt/'+old+'.bflyt'),source),
                          (layout.name_hash('anim/'+old+'_Decide.bflan'),animation),(42,b'texture')])
        result=dict((k,v) for k,_,v in layout.entries(layout.yaz0(layout.dialog_button(original))))
        derived=[s for _,s in layout.sections(result[layout.name_hash('blyt/'+new+'.bflyt')])]
        self.assertEqual(derived[-2][12:36].split(b'\0')[0],b'L_SettingsLabel')
        self.assertIn(b'cm_L_SetLabel_00\0',derived[-2])
        self.assertEqual(struct.unpack_from('<f',derived[-2],48)[0],25)
        self.assertEqual(derived[-1][:4],b'pae1')
        self.assertFalse(derived[3][8]&1)
        self.assertTrue(parts[3][8]&1)
        self.assertEqual(result[layout.name_hash('anim/'+new+'_Decide.bflan')],animation)
        self.assertEqual(result[42],b'texture')
        with self.assertRaises(ValueError):
            layout.dialog_button(archive([(layout.name_hash('blyt/'+old+'.bflyt'),page_layout())]))
        for broken in (parts[:-1], [parts[3],parts[0]]):
            body=b''.join(broken)
            invalid=source[:12]+struct.pack('<IHH',20+len(body),len(broken),0)+body
            with self.assertRaises(ValueError):
                layout.dialog_button(archive([(layout.name_hash('blyt/'+old+'.bflyt'),invalid)]))


if __name__ == '__main__':
    unittest.main()
