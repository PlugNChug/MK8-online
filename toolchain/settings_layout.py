"""Derive the opt-in top-menu layout privately, without writing to game inputs."""
import hashlib
from pathlib import Path
import shutil
import struct

MENU_SHA256 = '88333fb16dc2a43e758b3aefca3e455b13a666b46f8229033ac2d111b9ec3b13'
COMMON_SHA256 = '8d371bdfe7235d5652857b4450437aef4b1661d79eb1d1210db7806f2b584902'
LAYOUT_HASH = 0x6ec7bb3c
RELATIVE = Path('settings-menu/romfs/UI/cmn/menu.sarc')
COMMON_RELATIVE = RELATIVE.with_name('common.sarc')
LIMIT = 32 * 1024 * 1024


def name_hash(name):
    value=0
    for c in name.encode('ascii'):
        value=(value*0x65+c)&0xffffffff
    return value


def pack_entries(items):
    """Repack private archives when adding an isolated component."""
    if not 1<=len(items)<=4096:
        raise ValueError('Invalid private archive count')
    names, payload, table = bytearray(), bytearray(), bytearray()
    ordered=sorted(items,key=lambda item:name_hash(item[0]) if isinstance(item[0],str) else item[0])
    for key,data in ordered:
        flags=0
        if isinstance(key,str):
            flags=0x1000000|len(names)//4
            names.extend(key.encode('ascii')+b'\0'); names.extend(bytes(-len(names)%4))
            key=name_hash(key)
        payload.extend(bytes(-len(payload)%0x1000)); lo=len(payload); payload.extend(data)
        table.extend(struct.pack('<4I',key,flags,lo,len(payload)))
    start=(40+len(table)+len(names)+0xfff)&~0xfff
    header=(b'SARC\x14\x00\xff\xfe'+struct.pack('<III',start+len(payload),start,0x100)
            +b'SFAT'+struct.pack('<HHI',12,len(items),0x65)+table+b'SFNT\x08\0\0\0'+names)
    result=header+bytes(start-len(header))+payload
    entries(result)
    return bytes(result)


def sections(data):
    if data[:4] not in (b'FLYT',b'FLAN') or len(data)<20:
        raise ValueError('Invalid Settings layout or animation')
    at=20
    while at<len(data):
        size,=struct.unpack_from('<I',data,at+4)
        if size<8 or at+size>len(data):
            raise ValueError('Invalid Settings layout section')
        yield at,data[at:at+size]
        at+=size


def texture(data, name):
    if data[:4]!=b'BNTX' or len(data)<64:
        raise ValueError('Settings icon requires BNTX')
    count,=struct.unpack_from('<I',data,36); table,=struct.unpack_from('<Q',data,40)
    if not 1<=count<=4096 or table+count*8>len(data):
        raise ValueError('Invalid Settings icon texture table')
    for i in range(count):
        at,=struct.unpack_from('<Q',data,table+i*8)
        if at+120>len(data) or data[at:at+4]!=b'BRTI':
            raise ValueError('Invalid Settings icon descriptor')
        text,=struct.unpack_from('<Q',data,at+96); text+=2
        if text>=len(data) or b'\0' not in data[text:text+128]:
            raise ValueError('Invalid Settings icon name')
        if data[text:data.index(0,text)]!=name.encode('ascii'):
            continue
        size,=struct.unpack_from('<I',data,at+80); mips,=struct.unpack_from('<Q',data,at+112)
        if mips+8>len(data) or struct.unpack_from('<H',data,at+22)[0]!=1:
            raise ValueError('Unexpected Settings icon mip layout')
        pixels,=struct.unpack_from('<Q',data,mips)
        if not size or pixels+size>len(data):
            raise ValueError('Invalid Settings icon pixel bounds')
        return at,pixels,size
    raise ValueError('Settings icon texture not found: '+name)


def rename_texture(data, at, name):
    """Keep the SDK's reversed-bit Patricia dictionary valid after naming the private icon."""
    dictionary,=struct.unpack_from('<Q',data,56)
    if dictionary+8>len(data) or data[dictionary:dictionary+4]!=b'_DIC':
        raise ValueError('Invalid Settings texture dictionary')
    count,=struct.unpack_from('<I',data,dictionary+4)
    if not 1<=count<=4096 or dictionary+8+(count+1)*16>len(data):
        raise ValueError('Invalid Settings texture dictionary size')
    pointer,=struct.unpack_from('<Q',data,at+96)
    old_end=data.index(0,pointer+2)
    encoded=name.encode('ascii')
    if len(encoded)>old_end-pointer-2:
        raise ValueError('Settings texture name exceeds its storage')
    struct.pack_into('<H',data,pointer,len(encoded))
    data[pointer+2:old_end+1]=(encoded+b'\0').ljust(old_end-pointer-1,b'\0')
    pointers=[struct.unpack_from('<Q',data,dictionary+16+i*16)[0] for i in range(count+1)]
    keys=[b'']+[bytes(data[p+2:data.index(0,p+2)])[::-1] for p in pointers[1:]]
    def bit(key,n): return (key[n//8]>>(n%8))&1 if n//8<len(key) else 0
    nodes=[[-1,0,0] for _ in keys]
    for i,key in enumerate(keys[1:],1):
        parent,child=0,nodes[0][1]
        while nodes[parent][0]<nodes[child][0]:
            parent,child=child,nodes[child][1+bit(key,nodes[child][0])]
        other=keys[child]
        different=next((n for n in range(max(len(key),len(other))*8) if bit(key,n)!=bit(other,n)),None)
        if different is None: raise ValueError('Duplicate Settings texture name')
        parent,child=0,nodes[0][1]
        while nodes[parent][0]<nodes[child][0] and nodes[child][0]<different:
            parent,child=child,nodes[child][1+bit(key,nodes[child][0])]
        nodes[i]=[different,child,i] if bit(key,different) else [different,i,child]
        nodes[parent][1 if not parent else 1+bit(key,nodes[parent][0])]=i
    for i,key in enumerate(keys[1:],1):
        parent,child=0,nodes[0][1]
        while nodes[parent][0]<nodes[child][0]:
            parent,child=child,nodes[child][1+bit(key,nodes[child][0])]
        if child!=i: raise ValueError('Settings texture dictionary lookup failed')
    for i,(reference,left,right) in enumerate(nodes):
        struct.pack_into('<IHHQ',data,dictionary+8+i*16,reference&0xffffffff,left,right,pointers[i])


def settings_icon_button(button, source):
    """Copy the game's wrench pixels into a private clone of its small menu button."""
    import re
    old,new='mn_L_SubBtn_00','mn_L_SetBtn_00'
    result=[]
    source_at,source_pixels,source_size=texture(source,'tc_SettingsIcon_Kart^s')
    if struct.unpack_from('<II',source,source_at+36)!=(40,40):
        raise ValueError('Unexpected wrench dimensions')
    for key,_,data in entries(yaz0(button)):
        out=bytearray(data)
        if data[:4]==b'BNTX':
            at,pixels,size=texture(data,'SubBtnIconPlayRecord_00^t')
            if source_size>size:
                raise ValueError('Wrench exceeds private icon storage')
            out[at+16:at+96]=source[source_at+16:source_at+96]
            out[pixels:pixels+source_size]=source[source_pixels:source_pixels+source_size]
            rename_texture(out,at,'tc_SettingsIcon_Kart^s')
        elif data[:4]==b'FLYT':
            if key!=name_hash('blyt/'+old+'.bflyt'):
                raise ValueError('Settings button layout name changed')
            key=name_hash('blyt/'+new+'.bflyt')
            original_name=b'SubBtnIconPlayRecord_00^t\0'
            if out.count(original_name)!=1:
                raise ValueError('Settings icon texture reference changed')
            out=out.replace(original_name,b'tc_SettingsIcon_Kart^s\0'.ljust(len(original_name),b'\0'))
            icons=[pos for pos,s in sections(data) if s[:4]==b'pic1' and s[12:36].split(b'\0')[0]==b'P_Icon_00']
            if len(icons)!=1:
                raise ValueError('Settings button icon pane changed')
            struct.pack_into('<2f',out,icons[0]+76,72,72)
            if len(next(s for pos,s in sections(data) if pos==icons[0]))<100:
                raise ValueError('Settings icon vertex colors missing')
            # pic1: superior esquerdo/direito, inferior esquerdo/direito; manter alpha.
            for corner,shade in enumerate((64,64,40,40)):
                out[icons[0]+84+corner*4:icons[0]+87+corner*4]=bytes((shade,shade,shade))
        elif data[:4]==b'FLAN':
            tags=[tag.decode() for _,s in sections(data) if s[:4]==b'pat1' for tag in re.findall(rb'[A-Za-z_][A-Za-z_0-9]{2,}',s[8:])]
            matches=[tag for tag in tags if name_hash('anim/'+old+'_'+tag+'.bflan')==key]
            if len(matches)!=1:
                raise ValueError('Settings button animation name changed')
            key=name_hash('anim/'+new+'_'+matches[0]+'.bflan')
        result.append((key,bytes(out)))
    packed=pack_entries(result)
    return b'Yaz0'+struct.pack('>I',len(packed))+bytes(8)+b''.join(b'\xff'+packed[i:i+8] for i in range(0,len(packed),8))


def dialog_label(source):
    """Use the working game-font title control as plain dark text in our native dialog."""
    old,new='cm_L_Scroll_ListTitle_00','cm_L_SetLabel_00'
    result=[]
    for key,_,data in entries(yaz0(source)):
        out=bytearray(data)
        if data[:4]==b'FLYT':
            if key!=name_hash('blyt/'+old+'.bflyt'): raise ValueError('Settings dialog label changed')
            key=name_hash('blyt/'+new+'.bflyt')
            panes=[]
            for at,s in sections(data):
                pane=bytearray(s)
                name=s[12:36].split(b'\0')[0]
                if s[:4]==b'wnd1':
                    # Keep the text's parent visible, with no title ribbon behind it.
                    pane=pane[:84]; pane[:8]=b'pan1'+struct.pack('<I',84); pane[10]=255
                if s[:4]==b'txt1' and name!=b'T_Title_00':
                    pane[8]&=~1
                if s[:4]==b'txt1' and name==b'T_Title_00':
                    struct.pack_into('<2f',pane,76,900,160)
                    struct.pack_into('<H',pane,84,160)
                    pane[92]=4
                    pane[104:112]=bytes((30,30,30,255))*2
                    struct.pack_into('<2f',pane,112,30,38)
                panes.append(pane)
            out=bytearray(data[:20])+b''.join(panes)
            struct.pack_into('<I',out,12,len(out))
        elif data[:4]==b'FLAN':
            if key!=name_hash('anim/'+old+'_Ptn.bflan'): raise ValueError('Settings dialog label animation changed')
            key=name_hash('anim/'+new+'_Ptn.bflan')
        result.append((key,bytes(out)))
    packed=pack_entries(result)
    return b'Yaz0'+struct.pack('>I',len(packed))+bytes(8)+b''.join(b'\xff'+packed[i:i+8] for i in range(0,len(packed),8))


def yaz0(data):
    if data[:4] != b'Yaz0':
        return data
    if len(data) < 16:
        raise ValueError('Truncated menu Yaz0')
    size, = struct.unpack_from('>I', data, 4)
    if not 0 < size <= LIMIT:
        raise ValueError('Menu Yaz0 exceeds budget')
    out, pos = bytearray(), 16
    try:
        while len(out) < size:
            flags = data[pos]
            pos += 1
            for bit in range(7, -1, -1):
                if len(out) == size:
                    break
                if flags & (1 << bit):
                    out.append(data[pos])
                    pos += 1
                else:
                    a, b = data[pos:pos+2]
                    pos += 2
                    distance, count = ((a & 15) << 8 | b) + 1, a >> 4
                    if count:
                        count += 2
                    else:
                        count = data[pos] + 18
                        pos += 1
                    if distance > len(out) or len(out) + count > size:
                        raise ValueError('Invalid menu Yaz0 backreference')
                    for _ in range(count):
                        out.append(out[-distance])
    except (IndexError, ValueError) as error:
        raise ValueError('Invalid menu Yaz0 stream') from error
    return bytes(out)


def entries(data):
    if len(data) < 40 or len(data) > LIMIT or data[:8] != b'SARC\x14\x00\xff\xfe':
        raise ValueError('Unsupported menu SARC')
    size, start = struct.unpack_from('<II', data, 8)
    count, = struct.unpack_from('<H', data, 26)
    if size != len(data) or data[20:26] != b'SFAT\x0c\x00' or not 1 <= count <= 4096 or 32+16*count+8 > start or start > size:
        raise ValueError('Invalid menu SARC table')
    names = 32 + 16*count + 8
    result = []
    for at in range(32, 32+16*count, 16):
        key, flags, lo, hi = struct.unpack_from('<4I', data, at)
        if not 0 <= lo <= hi <= size-start:
            raise ValueError('Invalid menu SARC entry')
        if flags >> 24:
            offset = names + (flags & 0xffffff)*4
            if not names <= offset < start or b'\0' not in data[offset:start]:
                raise ValueError('Invalid menu SARC name')
            key = data[offset:data.index(0, offset)].decode('ascii')
        result.append((key, at, data[start+lo:start+hi]))
    return result


def replace_entry(data, key, replacement):
    matches = [at for name, at, _ in entries(data) if name == key]
    if len(matches) != 1 or len(replacement) > LIMIT:
        raise ValueError('Menu SARC boundary changed')
    start, = struct.unpack_from('<I', data, 12)
    out = bytearray(data)
    out.extend(bytes((-len(out)) % 0x1000))
    lo = len(out) - start
    out.extend(replacement)
    struct.pack_into('<II', out, matches[0]+8, lo, lo+len(replacement))
    struct.pack_into('<I', out, 8, len(out))
    entries(out)
    return bytes(out)


def add_button(data):
    if data[:8] != b'FLYT\xff\xfe\x14\x00' or len(data) < 20:
        raise ValueError('Unsupported top-menu layout')
    size, count = struct.unpack_from('<IH', data, 12)
    if size != len(data):
        raise ValueError('Invalid top-menu layout size')
    pos, sections = 20, []
    while pos < size:
        if pos + 8 > size:
            raise ValueError('Truncated top-menu section')
        length, = struct.unpack_from('<I', data, pos+4)
        if length < 8 or pos+length > size:
            raise ValueError('Invalid top-menu section')
        sections.append(data[pos:pos+length])
        pos += length
    if len(sections) != count:
        raise ValueError('Invalid top-menu section count')
    buttons = [s for s in sections if s[:4] == b'prt1' and s[12:36].split(b'\0')[0].startswith(b'L_SubBtn_')]
    if len(buttons) != 7 or any(s[12:36].split(b'\0')[0] == b'L_SubBtn_07' for s in sections):
        raise ValueError('Unexpected top-menu buttons')
    template, = [s for s in buttons if s[12:36].split(b'\0')[0] == b'L_SubBtn_02']
    new = bytearray(template)
    new[12:36] = b'L_SubBtn_07'.ljust(24, b'\0')
    struct.pack_into('<f', new, 44, -128.0)
    if b'mn_L_SubBtn_00\0' in new:
        new=new.replace(b'mn_L_SubBtn_00\0',b'mn_L_SetBtn_00\0')
        if new[96:120].split(b'\0')[0]!=b'P_Icon_00' or struct.unpack_from('<I',new,124)[0]!=352:
            raise ValueError('Settings icon override changed')
        struct.pack_into('<I',new,124,0)
    for button in buttons:
        at=sections.index(button); shifted=bytearray(button)
        struct.pack_into('<f',shifted,44,struct.unpack_from('<f',button,44)[0]+90)
        sections[at]=bytes(shifted)
    last = next(i for i,s in enumerate(sections) if s[:4]==b'prt1' and s[12:36].split(b'\0')[0]==b'L_SubBtn_05')
    sections.insert(last+1, bytes(new))
    header = bytearray(data[:20])
    struct.pack_into('<IH', header, 12, 20+sum(map(len, sections)), count+1)
    return bytes(header) + b''.join(sections)


def pane(kind, name, x=0, y=0, reference=None, visible=True, scale=1):
    out = bytearray(84)
    out[:4] = kind
    out[8:12] = bytes((3 if visible else 2, 0, 255, 0))
    out[12:36] = name.encode().ljust(24, b'\0')
    struct.pack_into('<3f', out, 44, x, y, 0)
    struct.pack_into('<4f', out, 68, scale, scale, 1280, 720)
    if reference:
        out.extend(struct.pack('<I2f', 0, 1, 1)+reference.encode()+b'\0')
        out.extend(bytes(-len(out) % 4))
    struct.pack_into('<I', out, 4, len(out))
    return bytes(out)


def dialog_button(source):
    """O rótulo legível acompanha a escala/animação do botão nativo, na cópia privada."""
    import re
    old,new='cm_L_WinBtn_00','cm_L_SetBtn_00'
    result=[]
    for key,_,data in entries(yaz0(source)):
        if data[:4]==b'FLYT':
            if key!=name_hash('blyt/'+old+'.bflyt'): raise ValueError('Settings dialog button changed')
            parts=[bytearray(s) for _,s in sections(data)]
            parents=[i for i,s in enumerate(parts) if s[:4]==b'pan1' and s[12:36].split(b'\0')[0]==b'N_WinBtn_00']
            labels=[s for s in parts if s[:4]==b'txt1' and s[12:36].split(b'\0')[0]==b'T_BtnText_00']
            if len(parents)!=1 or len(labels)!=1 or parents[0]+1>=len(parts) or parts[parents[0]+1][:4]!=b'pas1':
                raise ValueError('Settings dialog button hierarchy changed')
            labels[0][8]&=~1
            depth=0
            for end in range(parents[0]+1,len(parts)):
                if parts[end][:4]==b'pas1': depth+=1
                if parts[end][:4]==b'pae1': depth-=1
                if depth==0: break
            if depth or parts[end][:4]!=b'pae1': raise ValueError('Settings dialog button tree unbalanced')
            parts.insert(end,pane(b'prt1','L_SettingsLabel',y=25,reference='cm_L_SetLabel_00'))
            out=bytearray(data[:20])+b''.join(parts)
            struct.pack_into('<IH',out,12,len(out),len(parts))
            data=bytes(out); key=name_hash('blyt/'+new+'.bflyt')
        elif data[:4]==b'FLAN':
            tags=[t.decode() for _,s in sections(data) if s[:4]==b'pat1' for t in re.findall(rb'[A-Za-z_][A-Za-z_0-9]{1,}',s[8:])]
            matches=[t for t in tags if name_hash('anim/'+old+'_'+t+'.bflan')==key]
            if len(matches)!=1: raise ValueError('Settings dialog button animation changed')
            key=name_hash('anim/'+new+'_'+matches[0]+'.bflan')
        result.append((key,data))
    packed=pack_entries(result)
    return b'Yaz0'+struct.pack('>I',len(packed))+bytes(8)+b''.join(b'\xff'+packed[i:i+8] for i in range(0,len(packed),8))


def add_page(data, container=False):
    """Own layout tree referencing the game's shared selector parts."""
    pos, sections, depth, root_end = 20, [], 0, None
    while pos < len(data):
        size, = struct.unpack_from('<I', data, pos+4)
        if size < 8 or pos+size > len(data):
            raise ValueError('Invalid Settings layout tree')
        section = data[pos:pos+size]
        if section[:4] == b'pas1':
            depth += 1
        elif section[:4] == b'pae1':
            depth -= 1
            if depth == 0:
                if root_end is not None:
                    raise ValueError('Multiple Settings layout roots')
                root_end = len(sections)
            if depth < 0:
                raise ValueError('Unbalanced Settings layout tree')
        sections.append(section)
        pos += size
    if not depth and root_end is None:
        roots = [i for i,s in enumerate(sections) if s[:4]==b'pan1' and s[12:36].split(b'\0')[0]==b'RootPane']
        if len(roots)==1 and not any(s[:4] in (b'pas1',b'pae1',b'prt1') for s in sections):
            at=roots[0]+1
            sections[at:at]=[b'pas1\x08\0\0\0',b'pae1\x08\0\0\0']
            root_end=at+1
    if depth or root_end is None or b'N_Settings' in data or b'L_SettingsRoot' in data:
        raise ValueError('Unexpected Settings layout root')
    own = [pane(b'pan1', 'N_Settings', visible=False), b'pas1\x08\0\0\0',
           pane(b'prt1', 'L_SettingsBG', reference='cm_L_Scroll_List_BG'),
           pane(b'prt1', 'L_SettingsTitle', y=270, reference='cm_L_Scroll_ListTitle_00'),
           pane(b'prt1', 'L_SettingsHint', y=-235, reference='cm_L_Scroll_ListTitle_00', scale=0.55)]
    own += [pane(b'prt1', 'L_SettingsRow0', y=150, reference='cm_L_Scroll_List_00')]
    own += [pane(b'prt1', f'L_SettingsRow{i}', y=60-(i-1)*60, reference='cm_L_Scroll_List_01') for i in range(1,6)]
    own += [pane(b'prt1', 'L_SettingsScroll', x=470, y=-60, reference='cm_L_ScrollBar_00', scale=0.5)]
    own += [pane(b'prt1', 'L_SettingsBack', x=-460, y=-280, reference='cm_L_BackBtn_00'), b'pae1\x08\0\0\0']
    own += [pane(b'pan1','N_SettingsPopup',visible=False),b'pas1\x08\0\0\0',
            pane(b'prt1','L_SettingsDialog',reference='cm_Window_00'),
            pane(b'prt1','L_SettingsQuestion',y=60,reference='cm_L_SetLabel_00'),
            pane(b'prt1','L_SettingsYes',x=-200,y=-195,reference='cm_L_SetBtn_00'),
            pane(b'prt1','L_SettingsNo',x=200,y=-195,reference='cm_L_SetBtn_00'),
            pane(b'prt1','L_SettingsCursor',reference='cm_L_BtnCursor_00'),b'pae1\x08\0\0\0']
    if container:
        own = [pane(b'prt1', 'L_SettingsRoot', reference='cm_Page_Dummy')]
    sections[root_end:root_end] = own
    header = bytearray(data[:20])
    struct.pack_into('<IH', header, 12, 20+sum(map(len, sections)), len(sections))
    return bytes(header)+b''.join(sections)


def prepare(romfs, output):
    original = Path(romfs)/'UI/cmn/menu.sarc'
    data = original.read_bytes()
    if hashlib.sha256(data).hexdigest() != MENU_SHA256:
        raise ValueError('Settings button requires verified MK8 4.0.1 menu assets')
    common = (Path(romfs)/'UI/cmn/common.sarc').read_bytes()
    if hashlib.sha256(common).hexdigest() != COMMON_SHA256:
        raise ValueError('Settings page requires verified MK8 4.0.1 common assets')
    target = Path(output)/RELATIVE
    if target.resolve() == original.resolve():
        raise ValueError('Settings layout must not overwrite the original')
    Path(output).mkdir(parents=True, exist_ok=True)
    if shutil.disk_usage(output).free < 4*1024**3:
        raise ValueError('Settings layout requires the 4 GiB free reserve')
    nested, = [value for key, _, value in entries(data) if key == 'mn_TopMenuNX_00.szs']
    nested = yaz0(nested)
    layout, = [value for key, _, value in entries(nested) if key == LAYOUT_HASH]
    nested = replace_entry(nested, LAYOUT_HASH, add_button(layout))
    packed = b'Yaz0' + struct.pack('>I', len(nested)) + bytes(8)
    packed += b''.join(b'\xff'+nested[i:i+8] for i in range(0, len(nested), 8))
    result = replace_entry(data, 'mn_TopMenuNX_00.szs', packed)
    original_button,=[v for k,_,v in entries(data) if k=='mn_L_SubBtn_00.szs']
    icons,=[v for k,_,v in entries(data) if k=='mn_CompeInfo_00.szs']
    icons,=[v for k,_,v in entries(yaz0(icons)) if v[:4]==b'BNTX']
    result=pack_entries([(k,v) for k,_,v in entries(result)]+[('mn_L_SetBtn_00.szs',settings_icon_button(original_button,icons))])
    target.parent.mkdir(parents=True, exist_ok=True)
    temp = target.with_suffix('.tmp')
    temp.write_bytes(result)
    temp.replace(target)
    for name, key, container in [('cm_Page_Dummy.szs', 0xdd954d60, False), ('cm_Page_Cursor.szs', 0xaa119e16, True)]:
        nested, = [v for k, _, v in entries(common) if k == name]
        nested = yaz0(nested)
        layout, = [v for k, _, v in entries(nested) if k == key]
        nested = replace_entry(nested, key, add_page(layout, container))
        packed = b'Yaz0' + struct.pack('>I', len(nested)) + bytes(8)
        packed += b''.join(b'\xff'+nested[i:i+8] for i in range(0, len(nested), 8))
        common = replace_entry(common, name, packed)
    label,=[v for k,_,v in entries(common) if k=='cm_L_Scroll_ListTitle_00.szs']
    button,=[v for k,_,v in entries(common) if k=='cm_L_WinBtn_00.szs']
    common=pack_entries([(k,v) for k,_,v in entries(common)]+[('cm_L_SetLabel_00.szs',dialog_label(label)),
                                                         ('cm_L_SetBtn_00.szs',dialog_button(button))])
    common_target = Path(output)/COMMON_RELATIVE
    temp = common_target.with_suffix('.tmp')
    temp.write_bytes(common)
    temp.replace(common_target)
    return target
