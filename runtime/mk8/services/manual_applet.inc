// MK8's OfflineWeb applet uses a local guide instead of a browser/document archive.
static bool mk8_manual_arguments(const uint8_t *data, size_t size) {
    if (size != 0x2000) return false;
    uint16_t count; uint32_t shim;
    memcpy(&count, data, 2); memcpy(&shim, data + 4, 4);
    if (!count || count > 128 || shim != 3) return false;
    bool path = false;
    size_t at = 8;
    for (unsigned i = 0; i < count; i++) {
        if (at > size - 8) return false;
        uint16_t type, length;
        memcpy(&type, data + at, 2); memcpy(&length, data + at + 2, 2);
        at += 8;
        if (length > size - at) return false;
        if (type == 6) {
            if (!length || !data[at] || !memchr(data + at, 0, length)) return false;
            path = true;
        }
        at += length;
    }
    return path;
}
static void mk8_manual_reply(uint8_t out[0x1010]) {
    memset(out, 0, 0x1010);
    uint32_t reason = 1; // BackButton; an actual guide was displayed and closed.
    uint64_t url_size = 1;
    memcpy(out, &reason, 4);
    memcpy(out + 0x1008, &url_size, 8);
}
extern int mk8_manual_run(void);
