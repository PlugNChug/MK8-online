"""Keep MK8 account progress separate from device and temporary save cleanup."""


def prepare(runtime):
    path = runtime/'services/fs.c'
    text = path.read_text(encoding='utf-8')
    old = '''    case 51: case 53: {  // OpenSaveDataFileSystem(space, attribute)
        uint8_t type = IN(uint8_t, 0x8 + 0x18);  // attribute.type
        char sub[64];
        snprintf(sub, sizeof sub, "save_%u", type);
        ipc_return_object(resp, open_host_fs(sub));
        break;
    }
'''
    new = '''    case 51: case 53: {  // OpenSaveDataFileSystem(space, attribute)
        if (!req->data || req->data_size < 0x48) {
            resp->result = MAKE_RESULT(2, 4000);
            break;
        }
        // Attribute +0x18 is the system-save ID; its type follows that u64 at +0x20.
        uint8_t type = IN(uint8_t, 0x8 + 0x20);
        if (type > 6) {
            resp->result = MAKE_RESULT(2, 4000);
            break;
        }
        char sub[64];
        // Preserve the existing account save; CommonSave/temporary cleanup must never reach it.
        if (type == 1) snprintf(sub, sizeof sub, "save_0");
        else snprintf(sub, sizeof sub, "save_type_%u", type);
        ipc_return_object(resp, open_host_fs(sub));
        break;
    }
'''
    if text.count(old) != 1:
        raise ValueError('Save-data mount layout changed upstream')
    path.write_text(text.replace(old, new), encoding='utf-8')
