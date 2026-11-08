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
