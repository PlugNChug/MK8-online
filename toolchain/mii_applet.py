"""Adapt only the Mii editor applet in the pinned host source."""


def adapt(text):
    changes = [
        ('typedef struct { uint32_t id; bool started; } AmApplet;',
         'extern int mk8_no_mii_renderer(void);\nextern size_t mk8_mii_editor(uint32_t, uint8_t[0x80]);\n'
         'typedef struct { uint32_t id, mii_mode; bool started, mii_args, popped; size_t out_size; uint8_t out[0x80]; } AmApplet;'),
        ('    case 10: ap->started = true; break;',
         '''    case 10:
        if (!ap->started && ap->id == 0x12 && mk8_no_mii_renderer()) {
            ap->out_size = mk8_mii_editor(ap->mii_args ? ap->mii_mode : UINT32_MAX, ap->out);
        }
        ap->started = true; break;'''),
        ('        Service *st = ipc_in_object(req, 0);',
         '''        Service *st = ipc_in_object(req, 0);
        if (req->cmd == 100 && st && st->handler == am_storage && ap->id == 0x12 && mk8_no_mii_renderer()) {
            AmStorage *d = st->state;
            if (d->size == 0x100) {
                uint32_t version;
                memcpy(&version, d->data, 4);
                memcpy(&ap->mii_mode, d->data + 4, 4);
                ap->mii_args = version == 3 || version == 4;
                LOG("[mk8:mii-editor] input version=%u mode=%u\\n", version, ap->mii_mode);
            }
        }'''),
        ('    case 101: case 104: resp->result = AM_NO_DATA; break;',
         '''    case 101: case 104:
        if (req->cmd == 101 && ap->id == 0x12 && ap->started && ap->out_size && !ap->popped) {
            ipc_return_object(resp, new_am_storage(ap->out, ap->out_size));
            ap->popped = true;
        } else resp->result = AM_NO_DATA;
        break;'''),
    ]
    for old, new in changes:
        if text.count(old) != 1:
            raise ValueError('Mii applet host boundary changed: ' + old[:60])
        text = text.replace(old, new)
    return text
