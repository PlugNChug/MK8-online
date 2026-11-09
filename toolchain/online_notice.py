"""The notice MK8's Online Play shows without a network: how this build plays with others.

Upstream's notice (runtime/ui/online_rows.mm) asks SMM2 players to sign in to its community service (Nextendo
Network). MK8 4.0.1's Online Play talks to Nintendo's servers, which this build does not reach; races with friends go
through a room of the MK8-online launcher and the game's own LAN Play. The notice says so instead.
"""

OLD = '''        snprintf(t, sizeof t, "You're offline. The game needs its online service, even for Course Practice: open %s, "
                 "Online, and sign in to Nextendo Network.", menu);
'''
NEW = '''        snprintf(t, sizeof t, "Online Play needs Nintendo's servers, which this build doesn't use (%s has no MK8 service). "
                 "To race friends: join a room in the MK8-online launcher (Multiplayer), then in Wireless Play hold L + R "
                 "and press the left stick for LAN Play.", menu);  // MK8 (toolchain/online_notice.py)
'''


def prepare(runtime):
    source = runtime/'ui/online_rows.mm'
    text = source.read_text(encoding='utf-8')
    if text.count(OLD) != 1:
        raise ValueError('Online notice boundary changed upstream')
    source.write_text(text.replace(OLD, NEW), encoding='utf-8')

    source = runtime/'services/am.c'
    text = source.read_text(encoding='utf-8')
    old = '''    else snprintf(full, sizeof full, "%s If it's an online error: check that you're online (Course World's courses come "
                  "from TGRCode or your own server; TGRCode sometimes asks to wait a few seconds).", t);'''
    new = '''    else snprintf(full, sizeof full, "%s For launcher rooms, use LAN Play, not Wireless Play. "
                  "Join the launcher room, select Play in room, then hold L + R and click the left stick "
                  "at the game's main menu. For VPN invitations, use the host's VPN IP and allow UDP 47820.", t);'''
    if text.count(old) != 1:
        raise ValueError('Game error notice boundary changed upstream')
    source.write_text(text.replace(old, new), encoding='utf-8')
