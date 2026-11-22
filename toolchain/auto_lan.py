"""Experimental automatic entry into the LAN room (runtime/mk8/net/auto_lan.inc), for rooms opened by the launcher.

Off unless MK8_AUTO_LAN is set (the launcher sets it only for Steam rooms). Three observation points feed it:
hle/movie.c (the menu movie opening: the title screen), net/vlan.c (the virtual LAN starting, and the host
runtime's room beacon, which the game never receives) and the per-frame input hook already used by frame_capture.inc.
"""
from guest_source import verified_bytes
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

FRAME = '    mk8_capture_frame(frame);'
HOOK = 'static void host_test_frame(uint64_t frame) {'
MOVIE = '    LOG("[movie] open %s -> %s\\n", path ? path : "(null)", host);\n'
VLAN_UP = '    LOG("[vlan] virtual LAN: room \\"%s\\" on relay %s, this player 10.13.37.%d\\n", room, relay, slot + 1);\n'
VLAN_RX = '        if (dip != me && dip != 0xFFFFFFFFu && dip != (VLAN_NET | ~VLAN_MASK)) continue;\n'
# The host runtime's room beacon (auto_lan.inc): read here, never delivered to the game.
BEACON = '        if (dport == 47999) continue;  // Reserved for older automatic LAN clients.\n'


def patch_guest(source, original, output):
    if original.parent.name != 'main' or original.name not in ('main_099.c', 'main_189.c'):
        return source
    if original.name == 'main_099.c':
        checksum = 'eecbee2a09ecc4bc86992e72e9d00b2723a4e5c082f2aa11e76591d0971c5322'
        marker = ('    c->x[30] = 0x7000490720ull; f_70008ad8e8(c);\n'
                  '    { uint64_t ea = c->x[19] + 0x388ull; ST64(ea, c->x[0]); }')
        declaration = 'extern int mk8_guest_lan_hide_boot_logo(void);'
        hook = (ROOT/'runtime/mk8/net/guest_boot_logo.inc').read_text(encoding='utf-8').rstrip()
    else:
        checksum = 'f5095ebd497d0917faba15246c4d662b3a9a733020d76042e1016bae9e83bd2e'
        marker = '    c->x[30] = 0x70008b2f9cull; f_70008bdc8c(c);'
        declaration = 'extern void mk8_guest_lan_start(unsigned long long, unsigned long long);'
        hook = '    mk8_guest_lan_start(c->x[19], c->x[0]);'
    verified_bytes(original, checksum, 'Guest LAN boot requires the verified MK8 4.0.1 unit')
    text = source.read_text(encoding='utf-8')
    if original.name == 'main_099.c':
        departure = 'void f_7000490e18(Cpu *c) {'
        if text.count(departure) != 1:
            raise ValueError('Guest LAN boot readiness boundary changed')
        declaration += '\nextern int mk8_guest_lan_hold_boot(unsigned long long);'
        text = text.replace(departure, departure+'\n    if (mk8_guest_lan_hold_boot(c->x[0])) return;')
    if text.count(marker) != 1:
        raise ValueError('Guest LAN boot page activation changed')
    target = output / 'experimental' / original.name
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(declaration+'\n'+text.replace(marker, marker+'\n'+hook), encoding='utf-8', newline='\r\n')
    return target


def replace_once(path, old, new, first=True):
    text = path.read_text(encoding='utf-8')
    if first and 'mk8_auto_lan' in text:
        raise ValueError(f'{path.name}: automatic LAN entry already adapted')
    if text.count(old) != 1:
        raise ValueError(f'{path.name}: boundary changed upstream: {old.strip()[:60]}')
    path.write_text(text.replace(old, new), encoding='utf-8')


def prepare(runtime):
    source = (ROOT/'runtime/mk8/net/auto_lan.inc').read_text(encoding='utf-8')
    (runtime/'net/auto_lan_menu.h').write_bytes((ROOT/'runtime/mk8/net/auto_lan_menu.h').read_bytes())
    input_c = runtime/'hle/input.c'
    text = input_c.read_text(encoding='utf-8')
    if text.count(FRAME) != 1 or text.count(HOOK) != 1 or 'mk8_auto_lan' in text:
        raise ValueError('hle/input.c: frame hook changed (frame_capture must be applied first)')
    text = text.replace(HOOK, source + '\n' + HOOK).replace(FRAME, FRAME + '\n    mk8_auto_lan_frame(frame);')
    input_c.write_text(text, encoding='utf-8')
    replace_once(runtime/'hle/movie.c', MOVIE,
                 MOVIE + '    { extern void mk8_auto_lan_movie(const char *); mk8_auto_lan_movie(path); }\n')
    replace_once(runtime/'net/vlan.c', VLAN_UP,
                 VLAN_UP + '    { extern void mk8_auto_lan_vlan_up(void); mk8_auto_lan_vlan_up(); }\n')
    replace_once(runtime/'net/vlan.c', VLAN_RX, VLAN_RX + BEACON, first=False)
