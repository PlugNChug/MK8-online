// MK8 amiibo (nfp:user). There was no implementation: the logging stub answered CreateUserInterface (cmd 0) with
// success and no object, and nn::nfp::Initialize then used a null interface (an access violation at
// nn::nfp::Initialize+0x50, in a menu of a 0.1.4 test). This build reads no amiibo: the interface exists, initializes,
// and reports no NFC device, as the console does with no NFC-capable controller (ListDevices: DeviceNotFound). The
// game's events are real kernel events that are never signalled.
#include "hle.h"
#include "ipc.h"
#include "kernel.h"
#include <stdlib.h>

#define NFP_DEVICE_NOT_FOUND MAKE_RESULT(115, 64)

enum { NFP_NON_INITIALIZED = 0, NFP_INITIALIZED = 1 };

static uint32_t nfp_event(void) {
    uint32_t writable, readable;
    kernel_create_event_pair(&writable, &readable);
    return readable;
}

static void nfp_user_close(Service *s) {
    free(s->state);
    s->state = NULL;
}

void mk8_nfp_user_interface(Service *s, IpcReq *req, IpcResp *resp) {
    int *state = s->state;
    switch (req->cmd) {
    case 0: *state = NFP_INITIALIZED; break;      // Initialize
    case 1: *state = NFP_NON_INITIALIZED; break;  // Finalize
    case 2:                                        // ListDevices: none
        OUT(int32_t, 0) = 0;
        resp_size(resp, 4);
        resp->result = NFP_DEVICE_NOT_FOUND;
        break;
    case 17: case 18: case 23:  // AttachActivateEvent, AttachDeactivateEvent, AttachAvailabilityChangeEvent
        resp->copy_handles[resp->ncopy++] = nfp_event();
        break;
    case 19:  // GetState
        OUT(uint32_t, 0) = (uint32_t)*state;
        resp_size(resp, 4);
        break;
    default: {  // device commands (StartDetection, Mount, GetTagInfo...): no device is ever listed
        static int logged;
        if (logged++ < 20) LOG("[nfp] IUser cmd %u: no amiibo reader in this build\n", req->cmd);
        resp->result = NFP_DEVICE_NOT_FOUND;
    }
    }
}

SERVICE("nfp:user", mk8_nfp_user) {
    (void)s;
    if (req->cmd == 0) {  // CreateUserInterface
        int *state = calloc(1, sizeof *state);
        if (!state) FATAL("nfp: out of host memory\n");
        Service *user = service_new("IUser", mk8_nfp_user_interface, state);
        user->on_close = nfp_user_close;
        ipc_return_object(resp, user);
        return;
    }
    LOG("[nfp] nfp:user cmd %u (ok)\n", req->cmd);
}
