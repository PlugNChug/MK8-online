"""Adapta descoberta e armazenamento do DLC na cópia verificada do runtime."""

STORAGE_PATH = '''static bool mk8_data_storage_path(uint64_t data_id, char *dir, size_t capacity) {
    int dlc = mk8_dlc_storage_path(data_id, dir, capacity);
    if (dlc) return dlc > 0;  // a rejected DLC path must not fall back to system data
    const char *mii = data_id == 0x0100000000000802ull ? getenv("MK8_MII_RESOURCE_DIR") : NULL;
    int length = mii && *mii ? snprintf(dir, capacity, "%s", mii)
        : snprintf(dir, capacity, "sysdata/%016llx", (unsigned long long)data_id);
    return length >= 0 && (size_t)length < capacity;
}
'''


def prepare(runtime):
    misc = runtime / "services/misc.c"
    text = misc.read_text(encoding="utf-8")
    original = '''// ------------------------------------------------------------------ aoc:u (no DLC)
SERVICE("aoc:u", aoc_u) {
    (void)s;
    switch (req->cmd) {
    case 2: OUT(uint32_t, 0) = 0; resp_size(resp, 4); break;   // CountAddOnContent
    case 3: OUT(uint32_t, 0) = 0; resp_size(resp, 4); break;   // ListAddOnContent
    case 5: OUT(uint64_t, 0) = GAME_TITLE_ID + 0x1000; resp_size(resp, 8); break;  // GetAddOnContentBaseId
    case 8: resp->copy_handles[resp->ncopy++] = new_event_handle(); break;
    default: ok_logger(s, req, resp);
    }
}
'''
    if text.count(original) != 1:
        raise ValueError("DLC: serviço aoc:u não corresponde ao upstream verificado.")
    misc.write_text(text.replace(original, '''void mk8_add_on_content(Service *, IpcReq *, IpcResp *);
SERVICE("aoc:u", aoc_u) { mk8_add_on_content(s, req, resp); }
'''), encoding="utf-8")
    fs = runtime / "services/fs.c"
    text = fs.read_text(encoding="utf-8")
    anchor = 'static Service *open_data_storage(uint64_t data_id) {\n'
    path = '''    char dir[64], why[256];
    snprintf(dir, sizeof dir, "sysdata/%016llx", (unsigned long long)data_id);'''
    if text.count(anchor) != 1 or text.count(path) != 1:
        raise ValueError("DLC: armazenamento por título não corresponde ao upstream verificado.")
    text = text.replace(anchor, 'extern int mk8_dlc_storage_path(uint64_t, char *, size_t);\n' + STORAGE_PATH + anchor)
    text = text.replace(path, '''    char dir[16384], why[256];
    if (!mk8_data_storage_path(data_id, dir, sizeof dir)) return NULL;''')
    fs.write_text(text, encoding="utf-8")
