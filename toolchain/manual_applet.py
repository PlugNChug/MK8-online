"""OfflineWeb reply and local manual overlay, for the pinned MK8 host source."""
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]

def replace(text, old, new):
    if text.count(old) != 1:
        raise ValueError('Local manual boundary changed: '+old[:90])
    return text.replace(old, new)

def prepare(runtime):
    path = runtime/'services/am.c'
    text = path.read_text(encoding='utf-8')
    text = replace(text,'bool started, mii_args, popped;', 'bool started, mii_args, popped, manual_args;')
    boundary = 'static void applet_accessor(Service *s, IpcReq *req, IpcResp *resp) {'
    text = replace(text,boundary,(ROOT/'runtime/mk8/services/manual_applet.inc').read_text(encoding='utf-8')+'\n'+boundary)
    text = replace(text,'    case 10:\n        if (!ap->started', '''    case 10:
        if (ap->id == 0x17 && !ap->started) {
            if (!ap->manual_args || !mk8_manual_run()) { resp->result = AM_NO_DATA; break; }
        }
        if (!ap->started''')
    text = replace(text,'        Service *st = ipc_in_object(req, 0);', '''        Service *st = ipc_in_object(req, 0);
        if (req->cmd == 100 && st && st->handler == am_storage && ap->id == 0x17) {
            AmStorage *d = st->state;
            if (d->size != 0x20) {
                ap->manual_args = mk8_manual_arguments(d->data, d->size);
                LOG("[mk8:manual] arguments size=%llu valid=%d\\n", (unsigned long long)d->size, ap->manual_args);
            }
        }''')
    text = replace(text,'    case 101: case 104:\n', '''    case 101: case 104:
        if (req->cmd == 101 && ap->id == 0x17 && ap->started && !ap->popped) {
            uint8_t out[0x1010]; mk8_manual_reply(out);
            ipc_return_object(resp, new_am_storage(out, sizeof out)); ap->popped = true; break;
        }
''')
    path.write_text(text,encoding='utf-8')
