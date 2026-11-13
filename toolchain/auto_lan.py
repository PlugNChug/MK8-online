"""Experimental automatic entry into the LAN room (runtime/mk8/net/auto_lan.inc), for rooms opened by the launcher.

Off unless MK8_AUTO_LAN is set (the launcher sets it only for Steam rooms). Three observation points feed it:
hle/movie.c (the menu movie opening: the title screen), net/vlan.c (the virtual LAN starting, and the host
runtime's room beacon, which the game never receives) and the per-frame input hook already used by frame_capture.inc.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

FRAME = '    mk8_capture_frame(frame);'
HOOK = 'static void host_test_frame(uint64_t frame) {'
MOVIE = '    LOG("[movie] open %s -> %s\\n", path ? path : "(null)", host);\n'
VLAN_UP = '    LOG("[vlan] virtual LAN: room \\"%s\\" on relay %s, this player 10.13.37.%d\\n", room, relay, slot + 1);\n'
VLAN_RX = '        if (dip != me && dip != 0xFFFFFFFFu && dip != (VLAN_NET | ~VLAN_MASK)) continue;\n'
# The host runtime's room beacon (auto_lan.inc): read here, never delivered to the game.
BEACON = ('        if (dport == 47999) {\n'
          '            extern void mk8_auto_lan_beacon(uint32_t, const uint8_t *, size_t);\n'
          '            if (sip != me) mk8_auto_lan_beacon(sip, h + 12, (size_t)n - 4 - 15);\n'
          '            continue;\n'
          '        }\n')


def replace_once(path, old, new, first=True):
    text = path.read_text(encoding='utf-8')
    if first and 'mk8_auto_lan' in text:
        raise ValueError(f'{path.name}: automatic LAN entry already adapted')
    if text.count(old) != 1:
        raise ValueError(f'{path.name}: boundary changed upstream: {old.strip()[:60]}')
    path.write_text(text.replace(old, new), encoding='utf-8')


def prepare(runtime):
    source = (ROOT/'runtime/mk8/net/auto_lan.inc').read_text(encoding='utf-8')
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
