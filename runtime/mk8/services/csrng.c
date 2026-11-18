// MK8's LAN session requests random identifiers through csrng, not only through libc.
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#undef IN
#undef OUT
#else
#include <errno.h>
#include <sys/random.h>
#endif
#include "hle.h"
#include "ipc.h"

bool mk8_random(void *buffer, size_t size) {
#ifdef _WIN32
    return BCryptGenRandom(NULL, buffer, (ULONG)size, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    uint8_t *out = buffer;
    while (size) {
        ssize_t count = getrandom(out, size, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        out += count;
        size -= (size_t)count;
    }
    return true;
#endif
}

SERVICE("csrng", mk8_csrng) {
    (void)s;
    IpcBuf *buffer = out_buf(req, 0);
    if (req->cmd != 0 || !buffer || buffer->size > 1024 * 1024 ||
        (buffer->size && !buffer->addr)) {
        resp->result = MAKE_RESULT(1, 1);
        return;
    }
    if (buffer->size && !mk8_random((void *)(uintptr_t)buffer->addr, buffer->size)) {
        LOG("[csrng] system entropy source failed\n");
        resp->result = MAKE_RESULT(1, 1);
    }
}
