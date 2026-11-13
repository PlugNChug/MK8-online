// Conteúdo adicional importado: manifesto versionado e hashes dos arquivos reais, sem ativação por variável.
#include "hle.h"
#include "ipc.h"
#include <errno.h>
#include <limits.h>
#include <mbedtls/sha256.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DLC_APPLICATION 0x0100152000022000ull
#define DLC_TITLE       0x0100152000023001ull
#define DLC_INVALID MAKE_RESULT(2, 6001)
#define DLC_MISSING MAKE_RESULT(2, 1001)

static uint64_t dlc_u64(const uint8_t *p, unsigned count) {
    uint64_t value = 0;
    for (unsigned i = 0; i < count; i++) value |= (uint64_t)p[i] << (8 * i);
    return value;
}

static int dlc_read(const char *path, uint8_t *bytes, size_t size) {
    FILE *file = fopen(path, "rb");
    if (!file) return errno == ENOENT ? 0 : -1;
    int valid = fread(bytes, 1, size, file) == size && fgetc(file) == EOF && !ferror(file);
    fclose(file);
    return valid ? 1 : -1;
}

static int dlc_path(char *path, size_t size, const char *leaf) {
    const char *root = getenv("MK8_DLC_ROOT");
    if (root) {
        if (!*root) return 0;
        int length = snprintf(path, size, "%s/%s", root, leaf);
        return length >= 0 && (size_t)length < size;
    }
    if (!g_romfs_dir) return 0;
    int length = snprintf(path, size, "%s/../dlc/%s", g_romfs_dir, leaf);
    return length >= 0 && (size_t)length < size;
}

// 0 = ausente, 1 = instalado e íntegro, -1 = instalação inválida.
static int dlc_installed(void) {
    const char *root = getenv("MK8_DLC_ROOT");
    if (root && !*root) return 0;
    char path[16384];
    uint8_t manifest[112], content[4096], version[5], hash[32];
    if (!dlc_path(path, sizeof path, "installed.bin")) return -1;
    int found = dlc_read(path, manifest, sizeof manifest);
    if (found != 1) return found;
    if (memcmp(manifest, "MK8DLC1\0", 8) || dlc_u64(manifest + 8, 8) != DLC_APPLICATION ||
        dlc_u64(manifest + 16, 8) != DLC_TITLE || dlc_u64(manifest + 24, 4) != 65536 ||
        dlc_u64(manifest + 28, 4) != 1 || dlc_u64(manifest + 32, 8) != sizeof content ||
        dlc_u64(manifest + 40, 8) != sizeof version) return -1;
    if (!dlc_path(path, sizeof path, "0100152000023001/romfs/contents.dat") ||
        dlc_read(path, content, sizeof content) != 1 ||
        mbedtls_sha256(content, sizeof content, hash, 0) || memcmp(hash, manifest + 48, 32)) return -1;
    if (!dlc_path(path, sizeof path, "0100152000023001/romfs/version.txt") ||
        dlc_read(path, version, sizeof version) != 1 ||
        mbedtls_sha256(version, sizeof version, hash, 0) || memcmp(hash, manifest + 80, 32)) return -1;
    return 1;
}

int mk8_dlc_storage_path(uint64_t title, char *path, size_t size) {
    if ((title & ~0xfffull) != (DLC_APPLICATION + 0x1000)) return 0;
    if (title != DLC_TITLE || dlc_installed() != 1 ||
        !dlc_path(path, size, "0100152000023001/romfs")) return -1;
    LOG("[dlc] mounting %016llx\n", (unsigned long long)title);
    return 1;
}

void mk8_add_on_content(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    int installed = dlc_installed();
    static _Atomic unsigned logged;
    if (atomic_fetch_add(&logged, 1) < 64)
        LOG("[aoc] cmd=%u installed=%d input=%u\n", req->cmd, installed, req->data_size);
    if (installed < 0) { resp->result = DLC_INVALID; return; }
    unsigned needed = req->cmd == 3 || req->cmd == 7 ? 16 : req->cmd == 8 ? 0 : 8;
    if (needed && (!req->data || req->data_size < needed)) { resp->result = DLC_INVALID; return; }
    switch (req->cmd) {
    case 2:  // CountAddOnContent(pid)
        OUT(uint32_t, 0) = installed;
        resp_size(resp, 4);
        break;
    case 3: {  // ListAddOnContent(offset, count, pid)
        uint32_t offset = IN(uint32_t, 0), count = IN(uint32_t, 4);
        IpcBuf *buffer = out_buf(req, 0);
        if (offset > INT_MAX || count > INT_MAX || (count && (!buffer || !buffer->addr ||
            buffer->addr < GUEST_LO || buffer->addr > GUEST_HI - 4 || count > buffer->size / 4))) {
            resp->result = DLC_INVALID;
            break;
        }
        uint32_t copied = installed && offset == 0 && count != 0;
        if (copied) { uint32_t index = 1; memcpy((void *)(uintptr_t)buffer->addr, &index, sizeof index); }
        OUT(uint32_t, 0) = copied;
        resp_size(resp, 4);
        break;
    }
    case 5:  // GetAddOnContentBaseId(pid)
        OUT(uint64_t, 0) = DLC_APPLICATION + 0x1000;
        resp_size(resp, 8);
        break;
    case 7:  // PrepareAddOnContent(index, pid)
        if (!installed || IN(uint32_t, 0) != 1) resp->result = DLC_MISSING;
        break;
    case 8: case 10: {  // Evento real; a instalação não muda durante a execução do jogo.
        if (resp->ncopy >= 8) { resp->result = DLC_INVALID; break; }
        KEvent *event = calloc(1, sizeof *event);
        if (!event) { resp->result = DLC_INVALID; break; }
        event->type = KOBJ_EVENT;
        resp->copy_handles[resp->ncopy++] = kernel_new_handle(event);
        break;
    }
    default:
        LOG("[aoc] unsupported command %u\n", req->cmd);
        resp->result = MAKE_RESULT(1, 33);
    }
}
