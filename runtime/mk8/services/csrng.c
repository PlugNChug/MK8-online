// MK8's LAN session requests random identifiers through csrng, not only through libc.
#include <windows.h>
#include <bcrypt.h>
#undef IN
#undef OUT
#include "hle.h"
#include "ipc.h"

SERVICE("csrng", mk8_csrng) {
    (void)s;
    IpcBuf *buffer = out_buf(req, 0);
    if (req->cmd != 0 || !buffer || buffer->size > 1024 * 1024 ||
        (buffer->size && !buffer->addr)) {
        resp->result = MAKE_RESULT(1, 1);
        return;
    }
    if (buffer->size && BCryptGenRandom(NULL, (PUCHAR)(uintptr_t)buffer->addr,
                                       (ULONG)buffer->size, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        LOG("[csrng] Windows entropy source failed\n");
        resp->result = MAKE_RESULT(1, 1);
    }
}
